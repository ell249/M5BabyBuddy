# Installation Guide

## Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) (VS Code extension)
- A running [Baby Buddy](https://github.com/babybuddy/babybuddy) server accessible from your WiFi network
- M5Stack CoreInk connected via USB-C

## Step 1 — Clone the repository

```bash
git clone https://github.com/your-username/M5BabyBuddy.git
cd M5BabyBuddy
```

## Step 2 — Create your config file

```bash
cp src/config.h.example src/config.h
```

Edit `src/config.h` and fill in:

```cpp
#define WIFI_SSID       "your_wifi_name"
#define WIFI_PASSWORD   "your_wifi_password"
#define BB_BASE_URL     "http://192.168.1.50"   // Baby Buddy server IP or hostname
#define BB_AUTH_TOKEN   "your-token-here"        // Baby Buddy Settings → API Keys
#define BB_CHILD_ID     0                        // 0 = auto-detect first child
#define BB_TIMEZONE     "AEST-10AEDT,M10.1.0,M4.1.0/3"  // your POSIX TZ string
```

> **Note:** `config.h` is listed in `.gitignore` and will never be committed.

### Getting your Baby Buddy API token

1. Log in to Baby Buddy
2. Go to **Settings → API Keys**
3. Create a new key and copy the token

### Finding your timezone string

- Australia/Sydney: `AEST-10AEDT,M10.1.0,M4.1.0/3`
- Australia/Perth: `AWST-8`
- Australia/Brisbane: `AEST-10`
- UTC: `UTC0`
- US Eastern: `EST5EDT,M3.2.0,M11.1.0`
- US Pacific: `PST8PDT,M3.2.0,M11.1.0`

## Step 3 — Build and flash

With PlatformIO CLI:

```bash
pio run --target upload
```

Or in VS Code with the PlatformIO extension: click the **Upload** button (→) in the PlatformIO toolbar.

On first build, PlatformIO will automatically download:
- `m5stack/M5CoreInk`
- `bblanchon/ArduinoJson`

This may take a few minutes.

## Step 4 — Monitor serial output (optional)

```bash
pio device monitor
```

Serial output (115200 baud) shows WiFi connection status, child ID fetched, NTP sync, and any API errors.

## Step 5 — Use the device

1. Press the **power button** (side) to turn on
2. The device connects to WiFi and syncs the time
3. If offline events are queued, they are replayed automatically
4. Navigate the menu with the **multifunction button** (UP / CENTER / DOWN)
5. After saving an activity, the device shows a summary of recent records and enters deep sleep
6. Press any direction on the multifunction button to wake

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| "Connecting…" hangs | Check SSID/password in `config.h`; ensure 2.4 GHz network |
| HTTP 401 errors | Check `BB_AUTH_TOKEN` |
| HTTP 404 errors | Check `BB_BASE_URL` — no trailing slash |
| Child ID -1 | Set `BB_CHILD_ID` manually, or ensure Baby Buddy has at least one child |
| Time wrong | Check `BB_TIMEZONE` string |
| Build fails: M5CoreInk.h not found | Run `pio run` once to download libraries; VS Code squiggles will clear |
