# M5BabyBuddy

A [Baby Buddy](https://github.com/babybuddy/babybuddy) remote input device for the [M5Stack CoreInk](https://docs.m5stack.com/en/core/coreink), built with PlatformIO and Arduino.

Inspired by [babypod-software](https://github.com/skjdghsdjgsdj/babypod-software).

## Features

- **Feeding** — log left breast, right breast, both, formula, or pumped milk with a running timer
- **Diaper** — log wet, dirty, or both
- **Sleep** — start/stop a sleep timer; saved to Baby Buddy on stop
- **Tummy Time** — start/stop a tummy time timer
- **Pumping** — timer + amount entry (0–400 ml in 5 ml steps)
- **Offline queue** — events logged while offline are stored in NVS and replayed automatically on reconnect (up to 20 events)
- **Timer persistence** — active timers survive deep sleep and device restarts
- **Summary screen** — last feeding, diaper, and sleep shown on the e-ink display while the device sleeps (image persists without power)
- **Deep sleep** — device sleeps after 30 s inactivity; wakes on multifunction button press

## Hardware

| Item | Detail |
|------|--------|
| Board | M5Stack CoreInk |
| Display | 200×200 e-ink (EPD) |
| Input | Single multifunction button (UP / CENTER / DOWN) |
| MCU | ESP32 (WiFi) |
| RTC | RV3028 (built-in) |
| Battery | ~390 mAh built-in |

## Controls

| Press | Action |
|-------|--------|
| UP | Navigate up / increment value |
| DOWN | Navigate down / decrement value |
| CENTER | Select / confirm / stop timer |
| CENTER (hold 1.5 s) | Back / cancel |

Power on/off via the hardware power button on the side.

## Quick Start

See [INSTALL.md](INSTALL.md) for full setup instructions.

1. Clone this repo
2. Copy `src/config.h.example` → `src/config.h` and fill in your WiFi and Baby Buddy credentials
3. Flash with PlatformIO: `pio run --target upload`
4. Monitor serial output: `pio device monitor`

## Baby Buddy API

This device uses the Baby Buddy REST API with the following endpoints:

- `GET /api/children/` — auto-detect child ID
- `GET|POST|DELETE /api/timers/` — timer management
- `POST /api/feedings/` — log feedings
- `POST /api/changes/` — log diaper changes
- `POST /api/sleep/` — log sleep sessions
- `POST /api/tummy-times/` — log tummy time
- `POST /api/pumping/` — log pumping sessions
- `GET /api/feedings/?limit=1` etc. — recent records for summary screen

## License

Requirements, architecture and design by Elliot Alfirevich. Code writing with the assistance of Claude Code.

MIT — see [LICENSE](LICENSE).
