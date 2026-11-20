"""Stage the pinned Live adapter and the logger's Arduino libraries for ESP-IDF."""
import argparse
from pathlib import Path


def link_entries(source, destination, names):
    destination.mkdir(exist_ok=True, parents=True)
    for name in names:
        target = destination / name
        if not target.exists():
            target.symlink_to((source / name).resolve())


def stage(root, arduino, sdk, libraries, peer):
    # The upstream port passes NULL (the default attributes) to every rwlock.
    # IDF 5.3 has working rwlocks but no optional rwlockattr lifecycle routines.
    locks = peer / "usrsctp/usrsctplib/netinet/sctp_process_lock.h"
    if "NULL /* ESP-IDF default rwlock attributes */" not in locks.read_text():
        raise ValueError("Run the pinned SDK's prepare_peer.py first")
    pcb = peer / "usrsctp/usrsctplib/netinet/sctp_pcb.c"
    original = pcb.read_text()
    patched = original
    for op in ("init", "destroy"):
        patched = patched.replace(f"pthread_rwlockattr_{op}(&SCTP_BASE_VAR(rwlock_attr));", "/* default lock attributes */")
    if patched != original:
        pcb.write_text(patched)
    link_entries(arduino, root / "arduino",
                 ["Kconfig.projbuild", "CMakeLists.txt", "cores", "libraries", "variants"])
    # Use the Arduino network, filesystem and board libraries selected in sdkconfig.
    # The full Arduino manifest would pull in Matter, Zigbee and speech recognition.
    (root / "arduino/idf_component.yml").write_text(
        'dependencies:\n  joltwallet/littlefs: "1.20.2"\n  espressif/mdns: "^1.2.3"\n')
    for name in ("M5Unified", "M5GFX"):
        link_entries(libraries / name, root / name, ["CMakeLists.txt", "src"])
    link_entries(libraries / "TinyGPSPlus", root / "TinyGPSPlus", ["src"])
    (root / "TinyGPSPlus/CMakeLists.txt").write_text(
        'idf_component_register(SRCS "src/TinyGPS++.cpp" INCLUDE_DIRS src REQUIRES arduino)\n')
    source = sdk / "components/live_esp32"
    adapter = root / "live_esp32"
    link_entries(source, adapter, [p.name for p in source.iterdir() if p.name not in {"live_esp32.cpp", "https_transport.cpp"}])
    warm = adapter / "https_transport.cpp"
    if warm.is_symlink():
        warm.unlink()
    warm.symlink_to(Path(__file__).resolve().with_name("broker_transport.cpp"))
    text = (source / "live_esp32.cpp").read_text()
    text = text.replace(": live_audio::Prebuffer::DELAY_MS)", ": (unsigned)live_audio::Prebuffer::DELAY_MS)")
    text = text.replace("? config.playout_buffer_ms", "? (unsigned)config.playout_buffer_ms")
    text = text.replace("t.usStackHighWaterMark);", "(unsigned)t.usStackHighWaterMark);")
    old = 'http.url = "https://api.openai.com/v1/live/sessions";'
    if text.count(old) != 1:
        raise ValueError("Unexpected Live adapter version")
    changes = {
        old: 'http.url = walk_session_url();',
        '  esp_http_client_config_t http{};': (
            '  if (walk_photo_capacity()) {\n'
            '    body.resize(body.size() + walk_photo_capacity());\n'
            '    if (!walk_append_photo(body.data(), body.size())) { error("Photo offer failed"); return false; }\n'
            '  }\n  esp_http_client_config_t http{};'),
        'http.timeout_ms = 30000;': 'http.timeout_ms = walk_photo_capacity() ? 60000 : 30000;',
        '  if (live_session_answer(session, code, answer.data(), total, sdp.data(),': (
            '  if (!walk_read_opener(answer.data(), total)) { error("Missing camera observation"); return false; }\n'
            '  if (live_session_answer(session, code, answer.data(), total, sdp.data(),'),
        'config.greeting, [](const char *text)': 'walk_scene_greeting(), [](const char *text)',
    }
    for before, after in changes.items():
        if text.count(before) != 1:
            raise ValueError(f"Unexpected Live adapter marker {before}")
        text = text.replace(before, after)
    hooks = """#include <cstddef>
extern "C" const char *walk_session_url();
extern "C" size_t walk_photo_capacity();
extern "C" bool walk_append_photo(char*, size_t);
extern "C" bool walk_read_opener(const char*, size_t);
extern "C" const char *walk_scene_greeting();
"""
    (adapter / "live_esp32.cpp").write_text(hooks + text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage", "arduino", "sdk", "libraries", "peer"):
        parser.add_argument(name, type=Path)
    a = parser.parse_args()
    stage(a.stage, a.arduino, a.sdk, a.libraries, a.peer)
