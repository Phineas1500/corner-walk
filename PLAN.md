# Hackathon plan: a walk that becomes a Game Boy game

OpenAI NY HQ, Thursday Nov 19. Kickoff 1:30 pm, build 2:00–6:00 pm, showcase 6:00 pm.

## The idea

Wear the CoreS3 on a walk through the city. It records where you went and what the air was like, collects a character from anyone it passes who is also wearing a board, and talks with you along the way. When you get back, Codex turns the walk into a Game Boy game, writes it to the cartridge, and you play it on the Chromatic.

Showcase pitch: "Go for a walk, come back to a Game Boy game about it, starring the people you passed."

## What is decided

| Part | Decision |
|---|---|
| Wearable | CoreS3 on its battery base, with the GPS and the temperature/humidity/pressure sensor. Distance sensor only if there is time. |
| Internet on the walk | iPhone hotspot. |
| Encounters | Boards swap characters over Bluetooth when they pass. The StickS3 plays "the other person" for testing. |
| Companion | Push-to-talk, plus short unprompted remarks based on a photo. |
| Game build | Codex with the ModRetro Chromatic plugin, on GPT-6.2 Sol Ultrafast, written to the rewritable cartridge. This path already works. |
| Model access | The Codex subscription on the Mac, for the game and the companion. No API key. |
| Team | Solo or pair, decided at the event. The schedule works for both. |
| Not used | The Pi, QR reader, RFID reader, gesture sensor, live speech-to-speech, continuous video, Wi-Fi transfer to the Chromatic. |

## How it fits together

```
StickS3 ──Bluetooth──▶ CoreS3 (worn) ──iPhone hotspot──▶ relay on the Mac
                                                            │
                                   walk log (files in walks/)
                                        │                │
                                      Codex          map page (iPad)
                                        │
                                   cartridge ──▶ Chromatic
```

The **relay** is a small web server on the Mac, reachable from the street through a public tunnel (cloudflared or ngrok). It does all the thinking through the Codex login on the Mac, so there is no API key anywhere. The board knows only the relay's address and a shared token.

Two rules that keep the walk safe:

- **The board saves first.** Every log line goes to the board's own storage before any upload. If the signal or the relay drops, the walk still records and uploads later.
- **The Mac stays awake at the table.** It has to be online for the whole walk (`caffeinate -dims`).

## The five parts

### 1. Wearable (CoreS3)

- **Logger:** position, temperature, humidity and pressure every one to two seconds, saved locally and uploaded in batches.
- **Encounters:** scan for other boards over Bluetooth; on a new one, chime, show the character, log it.
- **Push-to-talk:** hold the screen to record, send to the relay, play the reply, show it as a caption.
- **Unprompted remarks:** on a trigger, take one photo and send it; play a remark only if the relay returns one.
- **Screen:** GPS status, signal, encounter count, last caption.

### 2. Beacon (StickS3)

Advertises one character over Bluetooth and nothing else. Its button can cycle between a few characters.

### 3. Relay (Mac)

| Address | Board sends | Relay returns |
|---|---|---|
| `POST /log` | A batch of log lines | OK |
| `POST /talk` | A recording and the latest position | Spoken reply and its caption |
| `POST /look` | A photo and what triggered it | A note for the log; sometimes a spoken remark and caption |
| `GET /walk` | — | A summary of the walk for Codex |
| `GET /` | — | The map page for the iPad |

Codex takes text and images but not audio, so speech stays on the Mac. A companion turn runs in three steps:

1. **Transcribe** the recording with a local Whisper model.
2. **Ask Codex** with `codex exec -m <Sol Ultrafast>`, passing the walk context and, for `/look`, the photo with `--image`. `--output-schema` makes `/look` return a note plus an optional remark in a fixed shape.
3. **Speak** the reply with the Mac's built-in `say` command, which already produces the format the board needs.

Starting Codex fresh for every turn may be slow. Time one turn early; if it takes more than a few seconds, keep one Codex session running and send turns to it instead.

- **Audio format:** replies come back as plain uncompressed audio (16-bit mono WAV at 16 kHz) so the board can play them without a decoder.
- **Photos:** never stored. Only the one-line note is kept.
- **Place names:** the relay looks up streets and landmarks for the GPS points when it writes the summary.
- **Profiles:** a Bluetooth advertisement only carries about 20 bytes of our data, so a board sends an ID, a look and a short name. The relay keeps the rest (greeting, full name), editable from the iPad page.

### 4. Game build (Codex and the Chromatic plugin)

Prepare one template game early: title screen, a walkable route, characters you can talk to. Each walk then only changes the content, which keeps builds fast and reliable.

| From the walk | In the game |
|---|---|
| Route and turns | The path through the level, one scene per block |
| Street and landmark names | Signs and scene titles |
| Temperature | Colour palette and weather (cold reads blue) |
| Humidity | Rain or fog |
| Pressure change | Stairs, hills, going underground |
| Encounters | Characters standing where you met them, with their name and greeting |
| Things you said | Signs, dialogue, special events ("that building looks haunted") |
| Photo notes | Scenery and objects to find |
| The companion | Your guide character |

Keep the game finishable in two minutes: walk the route, talk to everyone you met, find a few things the companion noticed.

### 5. Map page (iPad)

The route on a map with readings, encounters and what was said, shown beside the level it produced. This is what explains the project at an indoor showcase.

## Walk log

One line per event, saved as files in `walks/` where Codex can read them.

| Type | Contents |
|---|---|
| `fix` | time, latitude, longitude, speed |
| `env` | time, temperature, humidity, pressure |
| `encounter` | time, position, the other board's ID, look, name, signal strength |
| `said` | time, position, what you said, what the companion replied |
| `remark` | time, position, what triggered it, the line spoken |
| `scene` | time, position, one-line note from a photo |

## Companion rules

- **Triggers to build first:** you stop walking, and an encounter. Later: new street, sudden change in a reading, a slow timer.
- **Minimum gap:** one unprompted remark every two to three minutes, none right after a conversation.
- **No comments on people:** places, signs and objects only.
- **Silence when unsure:** a dark or blurred photo gets no remark.
- **Shush:** a double tap mutes unprompted remarks for ten minutes.
- **Short:** one or two sentences, with a chime first and a caption on screen.

## Encounter rules

- **Close range only:** count an encounter only when the signal is strong, so it means walking up to someone.
- **No repeats:** remember IDs already met, with a cooldown.
- **If Bluetooth fights the hotspot:** fall back to ESP-NOW with both boards offline on the same channel.

## Before you arrive

- **Time a game build.** Codex building a game and writing it to the cartridge already works. Time one build and write on Ultrafast, because that number sets how live the showcase can be.
- **Install two tools on the Mac.** A local transcription tool (`brew install whisper-cpp`, plus a small model file) and a tunnel (`brew install cloudflared`). Neither is installed yet.
- **Time one companion turn.** Recording in, spoken reply out, through Codex.
- **Record a sample walk.** Track your route in on your phone, so there is real data to build from before the wearable works.
- **Scaffold what needs no hardware.** The relay, the game template and the map page.
- **iPhone hotspot.** Turn on "Maximize Compatibility"; the boards only use 2.4 GHz.
- **Pack.** Laptop, charger, data-capable USB-C cables, iPad, Chromatic and cartridge, iPhone, Pixel, a power bank, and something to wear the CoreS3 with (lanyard, clip or tape).

## Check first at the event

- [ ] **CoreS3 variant.** The full version has a camera and motion sensor; the cheaper "SE" has neither, which removes photos and the "stopped walking" trigger.
- [ ] **Sensor ports.** Can the GPS and environment sensor plug in at the same time, and is there a hub for more?
- [ ] **Storage.** Is there a microSD card, or do we log to the board's built-in storage?
- [ ] **Framework.** Use whichever the kit walkthrough uses.
- [ ] **StickS3.** Confirm what it has; we only need Bluetooth and a button.
- [ ] **Cartridge.** Is there only the one? If so, the last good game file on the Mac is the backup.
- [ ] **Hotspot and Bluetooth together.** Does the CoreS3 hold the hotspot while scanning for boards?
- [ ] **Tunnel.** Does venue Wi-Fi allow it? If not, put the Mac on the Pixel's hotspot.
- [ ] **GPS near the venue.** How long to a first fix, and how far does it drift? Phone location is the fallback.
- [ ] **Ultrafast allowance.** Game builds and every companion turn now draw on the same plan. How does Ultrafast count against it, so it doesn't run out mid-demo?

## Schedule

Sunset is about 4:35 pm, so the first walk has to be in the bag by about 4:10.

| Time | Wearable | Game and relay |
|---|---|---|
| 1:30–2:00 | Kickoff. Work through "Check first". | Same. |
| 2:00–2:30 | Run a test program. Read the GPS and environment sensor. Join the hotspot. | Build and write one game; time it. Start the relay and tunnel. |
| 2:30–3:20 | Logger: save locally, upload in batches, status on screen. | `/log`, the walk summary with place names, the game template, a first game from the sample walk. |
| 3:20–3:40 | Encounters: StickS3 advertises, CoreS3 detects, chimes and logs. | Map page. |
| 3:40–4:10 | **Walk 1**, in daylight. Film 20 seconds of it. | Watch the log arrive; fix what breaks. |
| 4:10–4:50 | Push-to-talk. | Build the real game from walk 1, write it, play it, fix the mapping. |
| 4:50–5:20 | Unprompted remarks: the two first triggers. | "Add a visitor": new encounter, rebuild, rewrite. |
| 5:20–5:40 | Full run-through. Short second walk if the companion works. | Save the last good game file. |
| 5:40–6:00 | Stop changing things. Set up the table. | Same. |

If you are solo, follow the game column first in each row and skip the companion unless the first real game is on the cartridge by 4:50.

## Priorities

1. **Must:** logger, walk log, a game on the cartridge built from a real walk.
2. **Should:** encounters, place names, the map page.
3. **Stretch:** push-to-talk, then unprompted remarks, then the distance sensor.

Cut from the bottom.

## At the showcase

1. Show the walk on the iPad: route, readings, who you met, what was said.
2. Hand over the Chromatic to play the game built from it.
3. Add the visitor: they name a character on the iPad and walk up holding the StickS3. The CoreS3 chimes, Codex rebuilds on Ultrafast, and they find themselves in the game.
4. Let them talk to the companion.

If a live build fails, rewrite the last good game file and show the visitor added on the iPad instead.

## Suggested layout

```
hardware_hack/
  PLAN.md
  firmware/cores3/
  firmware/sticks3/
  relay/
  game/      template game for the Chromatic plugin
  walks/     logs and summaries
```
