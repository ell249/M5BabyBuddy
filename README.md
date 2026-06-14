# M5BabyBuddy

A [Baby Buddy](https://github.com/babybuddy/babybuddy) remote input device for the [M5Stack CoreInk](https://docs.m5stack.com/en/core/coreink), built with PlatformIO and Arduino.

Inspired by [babypod-software](https://github.com/skjdghsdjgsdj/babypod-software).

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buymeacoffee.com/ell249)

## Features

### Activity logging

- **Feeding** — log left breast, right breast, both, formula, or pumped milk with a running timer; menu automatically defaults to the opposite breast (or repeats the last method for non-breast options)
- **Diaper** — log wet, dirty (poo), wet+dirty, or dry
- **Sleep** — start/stop a sleep timer; saved to Baby Buddy on stop
- **Tummy Time** — start/stop a tummy time timer
- **Pumping** — timer + amount entry (0–400 ml in 5 ml steps)
- **Medication** — log a medication by name and dosage; medications pre-configured in `config.h` and supplemented automatically from recent Baby Buddy history
- **Temperature** — log a temperature reading (30–45°C in 0.1° steps); defaults to the last recorded temperature

### Summary screen

Shown on the e-ink display while the device sleeps; image persists without power.

- **3 most recent activities** across all tracked types (feeding, diaper, sleep, tummy time, pumping, medication, temperature)
- **Distinct icon** for each activity type: bottle, diaper, moon, hourglass, droplet, two-tone pill, thermometer
- **Activity badge** shown next to the time for quick glance:

  | Activity | Badge |
  |----------|-------|
  | Feeding | `L` `R` `B` `F` `P` (left / right / both / formula / pumped) |
  | Diaper | `W` `P` `W+P` `D` (wet / poo / both / dry) |
  | Sleep | duration, e.g. `1h30m` |
  | Tummy time | duration, e.g. `20m` |
  | Pumping | amount, e.g. `150ml` (or duration if no amount recorded) |
  | Medication | medication name, e.g. `Panadol` |
  | Temperature | reading, e.g. `37.5` |

- **Battery percentage** and current time shown in the footer
- **Child-filtered** — only shows records for the active child

### Multi-child support

- **Switch child** via Settings → Child; fetches all children from Baby Buddy and lets you pick
- Active child is persisted to NVS and remembered across deep sleep and restarts

### Reliability

- **Offline queue** — events logged while offline are stored in NVS and replayed automatically on next WiFi connection (up to 20 events)
- **Timer persistence** — active timers survive deep sleep and device restarts via NVS
- **BM8563 RTC** — hardware real-time clock keeps the time correct across power cycles and deep sleep without needing WiFi; NTP syncs and updates the RTC whenever WiFi is available

### Performance

- **Instant boot** — menu appears immediately using cached NVS data; WiFi connects in the background and updates the status indicator when ready
- **Fast summary screen** — all activity queries reuse a single SSL connection (one handshake instead of seven), dramatically reducing loading time
- **Smart defaults** — last feed method, last temperature, and recent medication names are fetched from Baby Buddy on WiFi connect (one SSL session for all three) and persisted to NVS for offline use

## Hardware

| Item | Detail |
|------|--------|
| Board | M5Stack CoreInk |
| Display | 200×200 e-ink (EPD) |
| Input | Single multifunction button (UP / CENTER / DOWN) + PWR button |
| MCU | ESP32-PICO-D4 (WiFi) |
| RTC | BM8563 (built-in, I²C) |
| Charger | TP4057 linear Li-ion charger |
| Battery | ~390 mAh built-in LiPo |

## Controls

| Press | Action |
|-------|--------|
| UP | Navigate up / increment value |
| DOWN | Navigate down / decrement value |
| CENTER | Select / confirm / stop timer |
| CENTER (hold 1.5 s) | Back / cancel |
| PWR | Go to summary screen / deep sleep |
| PWR (on sleep screen) | Wake device |

## Configuration

Copy `src/config.h.example` → `src/config.h` and fill in:

```cpp
// WiFi
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"

// Baby Buddy server (no trailing slash)
#define BB_BASE_URL   "https://your-babybuddy-server"
#define BB_AUTH_TOKEN "your-api-token"

// Child ID — set to 0 for auto-detect via API, or force a specific ID
#define BB_CHILD_ID   0

// POSIX timezone string (see https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html)
#define BB_TIMEZONE   "AEST-10AEDT,M10.1.0,M4.1.0/3"

// Inactivity timeout before deep sleep (seconds)
#define SLEEP_TIMEOUT_S  5

// Medications shown in the Medication menu
// Set MEDICINE_COUNT to 0 to hide the Medication menu item entirely
#define MEDICINE_COUNT 2
static const char* const MEDICINE_NAMES[]   = { "Panadol",  "Nurofen" };
static const float       MEDICINE_AMOUNTS[] = {  10.0f,       8.0f    };
static const char* const MEDICINE_UNITS[]   = {  "mL",       "mL"     };
```

## Quick Start

1. Clone this repo
2. Copy `src/config.h.example` → `src/config.h` and fill in your credentials
3. Flash with PlatformIO: `pio run --target upload`
4. Monitor serial output: `pio device monitor`

## Baby Buddy API

This device uses the Baby Buddy REST API with token authentication:

| Method | Endpoint | Purpose |
|--------|----------|---------|
| `GET` | `/api/children/` | Fetch child list for child switcher |
| `GET\|POST\|DELETE` | `/api/timers/` | Timer management |
| `POST` | `/api/feedings/` | Log feeding |
| `POST` | `/api/changes/` | Log diaper change |
| `POST` | `/api/sleep/` | Log sleep session |
| `POST` | `/api/tummy-times/` | Log tummy time |
| `POST` | `/api/pumping/` | Log pumping session |
| `POST` | `/api/medication/` | Log medication |
| `POST` | `/api/temperature/` | Log temperature |
| `GET` | `/api/feedings/?limit=1&child=N` | Last feed method (smart default) + summary screen |
| `GET` | `/api/temperature/?limit=1&child=N` | Last temperature (smart default) + summary screen |
| `GET` | `/api/medication/?limit=5&child=N` | Recent medication names (dynamic menu) |
| `GET` | `/api/changes/?limit=1&child=N` | Recent records for summary screen |
| `GET` | `/api/sleep/?limit=1&child=N` | ↑ |
| `GET` | `/api/tummy-times/?limit=1&child=N` | ↑ |
| `GET` | `/api/pumping/?limit=1&child=N` | ↑ |

## License

Requirements, architecture and design by Elliot Alfirevich. Code writing with the assistance of Claude Code.

MIT — see [LICENSE](LICENSE).
