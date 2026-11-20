# Corner Walk on the CoreS3 K128

Tap **Talk** on the CoreS3 and wait for **Listening**. Ask a question and keep talking for follow-ups. For a visual question, point the small lens on the screen side at the subject and ask what it sees. Each new look takes one 320 × 240 photo. Large print and nearby objects are good first subjects. Tap **End chat** when finished. A conversation also closes after ten minutes, or when the hotspot disconnects. Each new chat starts with fresh context. The local journal saves speaker-labeled chat captions for the walk recap. Only **Name corner** creates game signs. Camera images are sent for the visual answer and are not written to flash.

Tap **Name corner** to record a sign. Say the name, wait for the reply and tap **End and save corner**. This mode closes after one minute. The logger writes the words and reply to its local journal and attaches coordinates if a fresh fix was available at the first tap. Wait for **Recording** before naming a corner that should appear on the map. GPS messages can arrive indoors before a position is available.

## Startup on the Mac

Run the following from the project root. Keep the Mac awake and online while walking. This example uses a temporary Cloudflare tunnel and three terminal windows. The session server prompts privately for a project API key and checks access to `gpt-live-1.1`.

```sh
mkdir -p -m 700 "$HOME/.corner-walk"
caffeinate -i python3 relay/live.py --token-file "$HOME/.corner-walk/device-token"
```

```sh
cloudflared tunnel --protocol http2 --url http://127.0.0.1:8765
```

Set the CoreS3 USB port and the HTTPS address printed by the tunnel. Provision once per new address or board token. The device stores its broker settings and hotspot profile across reboots.

```sh
python3 tools/connect_voice.py --port "$CORE_PORT" --url "$TUNNEL_URL/session" \
  --token-file "$HOME/.corner-walk/device-token"
python3 tools/export_walk.py --port "$CORE_PORT" --status
```

The Mac authorizes each new GPT-Live-1.1 session. The CoreS3 exchanges audio and requested images with OpenAI over WebRTC. The delegated Responses model, `gpt-6-luna`, has the camera tool and web search for questions that need current information. Leave both terminal processes running while away. GPS logging continues offline.

## Record and build a walk

Tap **Start Walk** to start a new journal entry without rebooting. Begin moving once the GPS status reads **Recording**. Tap **End Walk & Install** when finished. Corner Walk closes any live chat, saves the walk locally, and sends it through the authenticated HTTPS tunnel. Leave the Mac awake and the selected Chromatic connected by data USB with the rewritable cartridge inserted.

The Mac uses GPT-6.2 Sol Ultrafast for the title and dialogue for the start and finish screens. It builds the map and conversation markers from the received journal and compiles the ROM in a separate directory for that walk. Finishing the walk requests one cartridge write. The Core shows the build and install status. If Wi-Fi drops before upload, the Core keeps the finished walk and retries with the same ID once it reconnects.

Tap **Indoor Walk** for the GPS-free demo. The walk starts at stop 1 and enables the Stick's automatic photo trigger. Tap **Next Stop** after moving somewhere new, up to twelve stops. **Talk**, **Aim camera** and **Name corner** also work during an indoor walk. At each stop you can let an object trigger a photo chat or start one yourself. Tap **End & Install** to send the journal. The game lays out an indoor route in the recorded stop order and puts the photo observations and chats at their corresponding stops.

### USB export

```sh
python3 tools/export_walk.py --port "$CORE_PORT"
python3 tools/make_walk_game.py "$WALK_FILE"
```

Choose the JSONL file for this outing as `WALK_FILE`. The generator needs two fixes and a route spanning at least five meters. It updates the local GB Studio project and places each geotagged spoken note on its nearest route tile. Notes made without a fix are still in the exported journal.

The game marks camera observations with a camera and conversations with a speech bubble. Stand on a marker and press **A** to read it. Press **A** to advance the dialogue. Entries at the same tile share a marker. Each caption uses the position at the beginning of that utterance, so a conversation can continue around a corner. New logs use the Core's elapsed clock across GPS, the shutter, and live audio. For older logs the builder can use the session's UTC start plus the caption offset. Time-based placement requires a GPS point within three seconds. The build reports how many entries had no matching position.

## Rebuild the firmware

Use the ESP-IDF 5.3.4 environment with its ESP32-S3 toolchain and activate its `export.sh`. Use Python 3.12 or later for the SDK dependency preparation. Set these paths in the shell before running the build commands.

| Variable | Directory |
| --- | --- |
| `WALK_LIVE_SDK` | OpenAI's `openai-live-esp32` at `6520b96840493c285ecbb7c72cace3b8116b619d` |
| `ARDUINO_CORE` | Espressif's `arduino-esp32` at `dbfde15b6ac25720c9132ebd33decf6b34d5e2af` |
| `LOGGER_LIBS` | Directory containing M5Unified 0.3.0, M5GFX 0.3.0 and TinyGPSPlus 1.1.0 in their respective subdirectories |
| `LIVE_PEER_DEPS_DIR` | A writable directory for the SDK's pinned crypto, SRTP and SCTP libraries |
| `WALK_COMPONENTS` | A separate writable directory for the staged IDF components |

```sh
cd firmware/cores3/live
export WALK_COMPONENTS LIVE_PEER_DEPS_DIR WALK_LIVE_SDK
export IDF_TARGET=esp32s3
python3 "$WALK_LIVE_SDK/tools/prepare_peer.py" "$LIVE_PEER_DEPS_DIR"
python3 stage.py "$WALK_COMPONENTS" "$ARDUINO_CORE" "$WALK_LIVE_SDK" "$LOGGER_LIBS" "$LIVE_PEER_DEPS_DIR"
idf.py build
```

The build produces `build/corner_walk_live.bin`. This project's existing K128 logger uses app0 at `0x10000` and a LittleFS journal at `0xc90000`. For an update on that layout, verify the partition table and active app slot, back up the journal, then write just the app image to the active slot. A full erase destroys the saved walks and network settings.

## Start a chat by approaching an object

Fit the ToF HAT in the StickS3's legacy HAT row and aim its sensor window in the same direction as the CoreS3's screen-side camera. The paired boards communicate directly using encrypted ESP-NOW. The CoreS3 uses its hotspot for the photo and voice session.

Tap **Aim camera** on the Core to see a local preview. Center the object in that frame, point the Stick in the same direction, then tap **Done**. Automatic triggers pause while you aim. During a sensor-initiated chat, the Core displays the exact snapshot used for the question.

With **Auto** on, aim at open space for at least half a second, then hold an object 10–55 cm from the ToF for about one second. The CoreS3 takes a single snapshot, and the relay uses Responses to produce an observation and question. Live speaks first and accepts a spoken follow-up. Tap **End chat** to finish, or tap **Auto** to toggle future triggers. With no spoken reply, an automatic chat ends after about a minute of active connection. Each new approach requires the view to clear past 75 cm or go out of range. The Core also spaces automatic starts at least 30 seconds apart.

The walk journal stores the opening text as a `scene` record and the conversation as `chat` records. If a current GPS fix is available, the scene also has coordinates. The JPEG is held in memory and sent for analysis.

Build the Stick app with `pio run -d firmware/sticks3`. After installing both apps, run the following from the repository root using the corresponding USB ports. Pairing creates a fresh per-kit key and restarts both devices.

```sh
python3 tools/pair_sensors.py --stick "$STICK_PORT" --core "$CORE_PORT"
```
