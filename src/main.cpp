#include <Arduino.h>
#include <M5CoreInk.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "config.h"
#include "ui.h"
#include "api.h"

// Defaults if not defined in config.h
#ifndef MEDICINE_COUNT
#  define MEDICINE_COUNT 0
static const char* const MEDICINE_NAMES[1]   = { nullptr };
static const float       MEDICINE_AMOUNTS[1] = { 0.0f };
static const char* const MEDICINE_UNITS[1]   = { nullptr };
#endif

// ── App states ────────────────────────────────────────────────────────────────
enum AppState {
    ST_BOOT,
    ST_MAIN_MENU,
    ST_FEEDING_METHOD,
    ST_FEEDING_TIMER,
    ST_FEEDING_SAVE,
    ST_DIAPER_TYPE,
    ST_DIAPER_SAVE,
    ST_SLEEP_TIMER,
    ST_SLEEP_SAVE,
    ST_TUMMY_TIMER,
    ST_TUMMY_SAVE,
    ST_PUMP_TIMER,
    ST_PUMP_AMOUNT,
    ST_PUMP_SAVE,
    ST_SETTINGS,
    ST_SETTINGS_CHILD,
    ST_MEDICATION_SELECT,
    ST_MEDICATION_AMOUNT,
    ST_MEDICATION_SAVE,
    ST_TEMP_VALUE,
    ST_TEMP_SAVE,
    ST_ERROR,
    ST_SLEEPING,
};

// ── Persistent timer state ────────────────────────────────────────────────────
static const char TIMER_NS[]      = "bb_timer";
static const char SREC_NS[]       = "bb_srec";
static const char MTIME_NS[]      = "bb_mtime";
static const char TIMER_ACT_KEY[] = "act";
static const char TIMER_T_KEY[]   = "start";
static const char TIMER_ID_KEY[]  = "bbid";
static const char TIMER_CH_KEY[]  = "child";

struct ActiveTimer {
    bool   valid;
    char   activity[16];
    time_t startTime;
    int    bbTimerId;
    int    childId;
};

static ActiveTimer _timer;

static void saveTimer() {
    Preferences p;
    p.begin(TIMER_NS, false);
    p.putString(TIMER_ACT_KEY, _timer.activity);
    p.putLong(TIMER_T_KEY,     (long)_timer.startTime);
    p.putInt(TIMER_ID_KEY,     _timer.bbTimerId);
    p.putInt(TIMER_CH_KEY,     _timer.childId);
    p.end();
}

static void loadTimer() {
    Preferences p;
    p.begin(TIMER_NS, true);
    String act = p.getString(TIMER_ACT_KEY, "");
    long   ts  = p.getLong(TIMER_T_KEY, 0);
    int    id  = p.getInt(TIMER_ID_KEY, -1);
    int    ch  = p.getInt(TIMER_CH_KEY, -1);
    p.end();
    if (act.length() > 0 && ts > 0) {
        _timer.valid = true;
        strncpy(_timer.activity, act.c_str(), sizeof(_timer.activity) - 1);
        _timer.startTime = (time_t)ts;
        _timer.bbTimerId = id;
        _timer.childId   = ch;
    } else {
        _timer.valid = false;
    }
}

static void clearTimer() {
    Preferences p;
    p.begin(TIMER_NS, false);
    p.clear();
    p.end();
    _timer.valid = false;
}

// ── Global state ──────────────────────────────────────────────────────────────
static AppState      _state          = ST_BOOT;
static bool          _wifiOk         = false;
static int           _childId        = -1;
static char          _childName[32]  = "Baby";
static int           _menuSel        = 0;
static int           _subSel         = 0;
static float         _pumpAmount     = 100.0f;
static char          _feedMethod[4]  = "bo";
static BBChild       _children[4]    = {};
static int           _childCount     = 0;
static int           _childSel       = 0;
static const char*   _childNamePtrs[5] = {};  // up to 4 children + "Back"
static int           _medicationIdx    = 0;
static float         _medicationAmount = 0.0f;
static const int     MED_MAX           = 8;
static BBMedication  _meds[8]          = {};   // merged config.h + API meds
static int           _medCount         = 0;
static const char*   _medicationPtrs[9] = {};  // MED_MAX + 1 (names + "Back")
static char          _lastFeedMethod[4] = "";
static float         _lastTemp          = 0.0f;
static float         _tempValue         = 37.0f;
static bool          _diaperWet      = true;
static bool          _diaperSolid    = false;
static char          _errorMsg[80]   = "";
static unsigned long _lastActivityMs = 0;
static unsigned long _timerRefreshMs = 0;
static bool          _wifiPending    = false;
static unsigned long _wifiStartMs    = 0;

// ── Menu definitions (≤8 chars per item for AsciiFont24x48) ──────────────────
static const char* MAIN_ITEMS[] = {
    "Feeding", "Diaper", "Sleep", "Tummy", "Pumping", "Medication", "Temp", "Settings"
};
static const int MAIN_COUNT = 8;

static const char* FEED_ITEMS[] = {
    "Left", "Right", "Both", "Formula", "Pumped", "Back"
};
static const int FEED_COUNT = 6;
static const char* FEED_METHODS[] = { "bl", "br", "bo", "f", "p" };

static const char* DIAPER_ITEMS[] = { "Wet", "Dirty", "Both", "Back" };
static const int DIAPER_COUNT = 4;

static const char* SETTINGS_ITEMS[] = {
    "WiFi", "Offline", "Replay", "Clear Q", "Child", "Back"
};
static const int SETTINGS_COUNT = 6;

// ── Button reading ────────────────────────────────────────────────────────────
static const int     PWR_BTN_PIN    = 27;
static unsigned long _midPressStart = 0;
static bool          _midHeld       = false;
static bool          _pwrBtnDown    = false;

enum BtnEvent { BTN_NONE, BTN_UP, BTN_MID, BTN_DOWN, BTN_MID_LONG, BTN_EXT };

static BtnEvent readButton() {
    M5.update();

    // PWR button (GPIO27) — edge-detect falling edge as sleep trigger
    bool pwrPressed = (digitalRead(PWR_BTN_PIN) == LOW);
    if (pwrPressed && !_pwrBtnDown) { _pwrBtnDown = true;  return BTN_EXT; }
    if (!pwrPressed)                  _pwrBtnDown = false;

    if (M5.BtnUP.wasPressed())   return BTN_UP;
    if (M5.BtnDOWN.wasPressed()) return BTN_DOWN;

    if (M5.BtnMID.isPressed()) {
        if (_midPressStart == 0) _midPressStart = millis();
        if (!_midHeld && (millis() - _midPressStart) > 1500UL) {
            _midHeld = true;
            return BTN_MID_LONG;
        }
    } else {
        if (_midPressStart > 0 && !_midHeld) {
            _midPressStart = 0;
            return BTN_MID;
        }
        _midPressStart = 0;
        _midHeld = false;
    }
    return BTN_NONE;
}

static void touch() { _lastActivityMs = millis(); }

// ── Battery ───────────────────────────────────────────────────────────────────
static int getBatPercent() {
    analogSetPinAttenuation(35, ADC_11db);
    esp_adc_cal_characteristics_t* chars =
        (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 3600, chars);
    uint32_t mv = esp_adc_cal_raw_to_voltage(analogRead(35), chars);
    free(chars);
    float v = float(mv) * 25.1f / 5.1f / 1000.0f;
    int pct = (int)((v - 3.2f) / (4.2f - 3.2f) * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void formatAgo(time_t ts, time_t now, char* buf, size_t len) {
    if (ts <= 0 || now <= ts) { strncpy(buf, "--", len); return; }
    uint32_t sec = (uint32_t)(now - ts);
    uint32_t min = sec / 60;
    uint32_t hr  = min / 60;
    uint32_t day = hr  / 24;
    if (day > 0)     snprintf(buf, len, "%dd%dh", (int)day,  (int)(hr  % 24));
    else if (hr > 0) snprintf(buf, len, "%dh%dm", (int)hr,   (int)(min % 60));
    else             snprintf(buf, len, "%dm",     (int)min);
}

// ── RTC helpers (BM8563) ─────────────────────────────────────────────────────

// Wait up to timeoutMs for SNTP to complete; returns true if synced.
static bool waitNtp(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (millis() - start > timeoutMs) return false;
        delay(100);
    }
    return true;
}

// Read BM8563 (stores local time) and apply to the system clock.
static void syncSystemFromRtc() {
    RTC_TimeTypeDef ts; RTC_DateTypeDef ds;
    M5.Rtc.GetTime(&ts);
    M5.Rtc.GetDate(&ds);
    if (ds.Year < 2020 || ds.Year > 2100) return;
    struct tm t = {};
    t.tm_year  = ds.Year - 1900;
    t.tm_mon   = ds.Month - 1;
    t.tm_mday  = ds.Date;
    t.tm_hour  = ts.Hours;
    t.tm_min   = ts.Minutes;
    t.tm_sec   = ts.Seconds;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch < 0) return;
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
}

// Write current system local time to BM8563.
static void syncRtcFromSystem() {
    time_t now = time(nullptr);
    if (now < 1000000000UL) return;
    struct tm* t = localtime(&now);
    RTC_TimeTypeDef ts;
    ts.Hours   = t->tm_hour;
    ts.Minutes = t->tm_min;
    ts.Seconds = t->tm_sec;
    M5.Rtc.SetTime(&ts);
    RTC_DateTypeDef ds;
    ds.Year  = t->tm_year + 1900;
    ds.Month = t->tm_mon + 1;
    ds.Date  = t->tm_mday;
    M5.Rtc.SetDate(&ds);
}

static void saveMedLastTimes();  // forward — defined after goToSleep

// ── Async WiFi ────────────────────────────────────────────────────────────────

static void startWiFiAsync() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    _wifiPending = true;
    _wifiStartMs = millis();
}

// Called from loop() the first time WiFi becomes connected
static void onWiFiConnected() {
    _wifiOk = true;
    // NTP has been running since configTzTime() at boot; usually syncs within 1s of WiFi up
    if (waitNtp(3000)) syncRtcFromSystem();
    if (api.offlineCount() > 0) api.replayOffline();
    // Fetch child info from API only if not already cached in NVS
    if (_childId <= 0) {
        _childId = api.getChildId();
        if (_childId > 0) {
            api.fetchChildName(_childId);
            strncpy(_childName, api.getChildName(), sizeof(_childName) - 1);
            if (_childName[0] == '\0') strncpy(_childName, "Baby", sizeof(_childName));
        }
    }
    // Fetch last feed method, last temperature, and recent API medications
    {
        BBMedication apiMeds[4];
        char apiFeedMethod[4] = "";
        float apiLastTemp = 0.0f;
        int apiMedCount = api.fetchStartupData(_childId,
                                               apiFeedMethod, sizeof(apiFeedMethod),
                                               &apiLastTemp, apiMeds, 4);
        if (apiFeedMethod[0]) strncpy(_lastFeedMethod, apiFeedMethod, sizeof(_lastFeedMethod) - 1);
        if (apiLastTemp > 0.0f) _lastTemp = apiLastTemp;

        Preferences sp;
        sp.begin("bb_state", false);
        if (_lastFeedMethod[0]) sp.putString("lastfeed", _lastFeedMethod);
        if (_lastTemp > 0.0f)   sp.putFloat("lasttemp", _lastTemp);
        sp.end();

        // Merge API meds into _meds[] — config.h entries take priority, API adds extras
        for (int i = 0; i < apiMedCount && _medCount < MED_MAX; i++) {
            bool found = false;
            for (int j = 0; j < _medCount; j++) {
                if (strcmp(_meds[j].name, apiMeds[i].name) == 0) {
                    _meds[j].lastTime    = apiMeds[i].lastTime;
                    _meds[j].intervalSec = apiMeds[i].intervalSec;
                    found = true; break;
                }
            }
            if (!found) {
                strncpy(_meds[_medCount].name, apiMeds[i].name, sizeof(_meds[0].name) - 1);
                _meds[_medCount].amount      = apiMeds[i].amount;
                strncpy(_meds[_medCount].unit, apiMeds[i].unit, sizeof(_meds[0].unit) - 1);
                _meds[_medCount].lastTime    = apiMeds[i].lastTime;
                _meds[_medCount].intervalSec = apiMeds[i].intervalSec;
                _medCount++;
            }
        }
        for (int i = 0; i < _medCount; i++) _medicationPtrs[i] = _meds[i].name;
        _medicationPtrs[_medCount] = "Back";
        saveMedLastTimes();
    }

    // Refresh whichever screen shows the WiFi indicator
    if (_state == ST_MAIN_MENU) {
        display.drawMenu(_childName, MAIN_ITEMS, MAIN_COUNT, _menuSel,
                         _wifiOk, api.offlineCount());
        display.refresh(true);
        touch();
    }
}

// ── Medication last-dose time NVS cache ───────────────────────────────────────

static void saveMedLastTimes() {
    Preferences p;
    p.begin(MTIME_NS, false);
    int cnt = 0;
    for (int i = 0; i < _medCount; i++) {
        if (_meds[i].lastTime <= 0 && _meds[i].intervalSec == 0) continue;
        char kn[5], kt[5], ki[5];
        snprintf(kn, sizeof(kn), "nm%d", cnt);
        snprintf(kt, sizeof(kt), "lt%d", cnt);
        snprintf(ki, sizeof(ki), "iv%d", cnt);
        p.putString(kn, _meds[i].name);
        p.putLong(kt, (long)_meds[i].lastTime);
        p.putUInt(ki, _meds[i].intervalSec);
        cnt++;
    }
    p.putInt("cnt", cnt);
    p.end();
}

static void loadMedLastTimes() {
    Preferences p;
    p.begin(MTIME_NS, true);
    int cnt = p.getInt("cnt", 0);
    for (int i = 0; i < cnt; i++) {
        char kn[5], kt[5], ki[5];
        snprintf(kn, sizeof(kn), "nm%d", i);
        snprintf(kt, sizeof(kt), "lt%d", i);
        snprintf(ki, sizeof(ki), "iv%d", i);
        char name[33] = "";
        p.getString(kn, name, sizeof(name));
        if (!name[0]) continue;
        long t  = p.getLong(kt, 0);
        uint32_t iv = p.getUInt(ki, 0);
        for (int j = 0; j < _medCount; j++) {
            if (strcmp(_meds[j].name, name) == 0) {
                if (t  > 0)  _meds[j].lastTime    = (time_t)t;
                if (iv > 0)  _meds[j].intervalSec = iv;
                break;
            }
        }
    }
    p.end();
}

// ── Recent-record NVS cache (lets sleep summary show advancing times offline) ─
static void saveRecentRecords(BBRecentRecord* recs, int count) {
    Preferences p;
    p.begin(SREC_NS, false);
    p.putInt("cnt", count);
    for (int i = 0; i < count; i++) {
        char k[5];
        snprintf(k, sizeof(k), "ts%d", i); p.putLong(k,   (long)recs[i].timestamp);
        snprintf(k, sizeof(k), "ic%d", i); p.putInt(k,    recs[i].iconType);
        snprintf(k, sizeof(k), "tm%d", i); p.putString(k, recs[i].timeStr);
        snprintf(k, sizeof(k), "me%d", i); p.putString(k, recs[i].method);
    }
    p.end();
}

static int loadRecentRecords(BBRecentRecord* recs) {
    Preferences p;
    p.begin(SREC_NS, true);
    int count = p.getInt("cnt", 0);
    for (int i = 0; i < count; i++) {
        char k[5];
        snprintf(k, sizeof(k), "ts%d", i); recs[i].timestamp = (time_t)p.getLong(k, 0);
        snprintf(k, sizeof(k), "ic%d", i); recs[i].iconType  = p.getInt(k, 0);
        snprintf(k, sizeof(k), "tm%d", i); p.getString(k, recs[i].timeStr, sizeof(recs[i].timeStr));
        snprintf(k, sizeof(k), "me%d", i); p.getString(k, recs[i].method,  sizeof(recs[i].method));
    }
    p.end();
    return count;
}

// ── Deep sleep with summary screen ───────────────────────────────────────────
static void goToSleep() {
    _state = ST_SLEEPING;
    if (_wifiPending) { WiFi.disconnect(true); _wifiPending = false; }

    // Give immediate visual feedback before potentially-slow API calls
    display.drawStatus("...");
    display.refresh();   // partial update — fast, no clear phase

    BBRecentRecord recs[3] = {};
    int recCount = _wifiOk ? api.getRecentRecords(recs, _childId) : 0;
    if (recCount > 0) {
        saveRecentRecords(recs, recCount);   // keep a copy so offline wakeups can show advancing times
    } else {
        recCount = loadRecentRecords(recs);  // fall back to cached records; formatAgo uses current RTC
    }

    int batPct = getBatPercent();
    time_t now = time(nullptr);

    UI::SummaryRecord srecs[3] = {};
    for (int i = 0; i < recCount; i++) {
        srecs[i].iconType = recs[i].iconType;
        formatAgo(recs[i].timestamp, now, srecs[i].relTime, sizeof(srecs[i].relTime));
        if (recs[i].method[0]) {
            snprintf(srecs[i].absTime, sizeof(srecs[i].absTime),
                     "%s %s", recs[i].timeStr, recs[i].method);
        } else {
            strncpy(srecs[i].absTime, recs[i].timeStr, sizeof(srecs[i].absTime) - 1);
        }
    }

    char clockBuf[16] = "--:--";
    if (now > 1000000) {
        struct tm* t = localtime(&now);
        snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d %d%%", t->tm_hour, t->tm_min, batPct);
    }

    display.drawSummary(_childName, clockBuf, srecs, recCount);
    display.refresh(true);

    // Put the EPD controller into its own deep sleep — latches the image and
    // powers down the HV supply.  Without this the controller keeps running after
    // SPI goes quiet and the image degrades.  display.begin() re-inits on wakeup.
    M5.M5Ink.deepSleep();

    // Wake on MID (GPIO38) press OR after 5-minute timer
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
    esp_sleep_enable_timer_wakeup(5ULL * 60 * 1000000);
    esp_deep_sleep_start();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static bool connectWiFi(bool showProgress = false) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t = millis();
    int frame = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t < 30000UL) {
        if (showProgress) {
            display.drawConnectingDots(frame++);
            display.refresh();   // partial — no clear flash, natural pacing via waitDisplay
        } else {
            delay(500);
        }
    }
    return WiFi.status() == WL_CONNECTED;
}

// ── Navigation helpers ────────────────────────────────────────────────────────
static void enterMainMenu() {
    _state = ST_MAIN_MENU;
    display.drawMenu(_childName, MAIN_ITEMS, MAIN_COUNT, _menuSel,
                     _wifiOk, api.offlineCount());
    display.refresh(true);
    touch();
}

static void startTimer(const char* activity, const char* apiName) {
    _timer.startTime = time(nullptr);
    _timer.childId   = _childId;
    _timer.bbTimerId = -1;
    strncpy(_timer.activity, activity, sizeof(_timer.activity) - 1);
    _timer.valid = true;
    saveTimer();
    if (_wifiOk) {
        BBTimer bt;
        api.startTimer(apiName, _childId, bt);
        _timer.bbTimerId = bt.id;
        saveTimer();
    }
}

static void showTimerScreen() {
    uint32_t elapsed = (uint32_t)(time(nullptr) - _timer.startTime);
    display.drawTimer(_timer.activity, elapsed, _wifiOk);
    display.refresh(true);
    _timerRefreshMs = millis();
}

// ── Save helpers ──────────────────────────────────────────────────────────────
static void finishWithStatus(const char* msg, bool ok, int httpCode) {
    if (ok) {
        display.drawStatus(msg);
        display.refresh(true);
        delay(1500);
        goToSleep();
    } else {
        snprintf(_errorMsg, sizeof(_errorMsg), "HTTP %d", httpCode);
        _state = ST_ERROR;
        display.drawError("Failed", _errorMsg);
        display.refresh(true);
    }
}

static void offlineLog(const char* path, const char* body, const char* successMsg) {
    api.enqueueOffline("POST", path, body);
    display.drawStatus(successMsg);
    display.refresh(true);
    delay(1500);
    goToSleep();
}

// ── Boot ──────────────────────────────────────────────────────────────────────
static void handleBoot() {
    display.begin();
    pinMode(PWR_BTN_PIN, INPUT_PULLUP);

    // TZ must be set before syncSystemFromRtc so mktime() uses the correct local offset.
    // configTzTime also starts the SNTP daemon; it will sync once WiFi is up.
    configTzTime(BB_TIMEZONE, NTP_SERVER);
    syncSystemFromRtc();  // clock is immediately correct from RTC, even without WiFi

    // Timer wakeup: silent background refresh — blocking WiFi is fine, user isn't watching
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        api.begin(BB_BASE_URL, BB_AUTH_TOKEN);
        const char* cached = api.getChildName();
        if (cached[0] != '\0') strncpy(_childName, cached, sizeof(_childName) - 1);
        _childId = BB_CHILD_ID > 0 ? BB_CHILD_ID : api.getChildId();
        _wifiOk = connectWiFi();
        if (_wifiOk && waitNtp(5000)) syncRtcFromSystem();
        goToSleep();
        return;
    }

    // Normal (button) wakeup: load from NVS and show UI immediately — no waiting for WiFi
    api.begin(BB_BASE_URL, BB_AUTH_TOKEN);

    // Build medication list from config.h, then merge API meds on WiFi connect
    _medCount = 0;
    for (int i = 0; i < MEDICINE_COUNT && _medCount < MED_MAX; i++) {
        strncpy(_meds[_medCount].name, MEDICINE_NAMES[i], sizeof(_meds[0].name) - 1);
        _meds[_medCount].amount = MEDICINE_AMOUNTS[i];
        strncpy(_meds[_medCount].unit, MEDICINE_UNITS[i], sizeof(_meds[0].unit) - 1);
        _medCount++;
    }
    for (int i = 0; i < _medCount; i++) _medicationPtrs[i] = _meds[i].name;
    _medicationPtrs[_medCount] = "Back";
    loadMedLastTimes();  // restore last-dose times from NVS (available before WiFi)

    // Load persisted last-feed method and last temperature from NVS
    {
        Preferences sp;
        sp.begin("bb_state", true);
        sp.getString("lastfeed", _lastFeedMethod, sizeof(_lastFeedMethod));
        _lastTemp = sp.getFloat("lasttemp", 0.0f);
        sp.end();
    }

    _childId = BB_CHILD_ID > 0 ? BB_CHILD_ID : api.getChildId();  // NVS read, no network
    const char* cached = api.getChildName();
    if (cached[0] != '\0') strncpy(_childName, cached, sizeof(_childName) - 1);
    if (_childName[0] == '\0') strncpy(_childName, "Baby", sizeof(_childName));

    startWiFiAsync();  // connect in background; onWiFiConnected() fires from loop()

    loadTimer();
    if (_timer.valid) {
        showTimerScreen();
        if (strcmp(_timer.activity, "Feeding") == 0)    _state = ST_FEEDING_TIMER;
        else if (strcmp(_timer.activity, "Sleep") == 0)  _state = ST_SLEEP_TIMER;
        else if (strcmp(_timer.activity, "Tummy") == 0)  _state = ST_TUMMY_TIMER;
        else if (strcmp(_timer.activity, "Pumping") == 0) _state = ST_PUMP_TIMER;
        else enterMainMenu();
    } else {
        enterMainMenu();
    }
}

static void _medSubLabel(int idx, char* buf, size_t len);   // forward — defined before handleMedicationSelect
static void _medLastLabel(int idx, char* buf, size_t len);  // forward

// ── State handlers ────────────────────────────────────────────────────────────
static void handleMainMenu(BtnEvent btn) {
    if (btn == BTN_UP   && _menuSel > 0)            _menuSel--;
    if (btn == BTN_DOWN && _menuSel < MAIN_COUNT-1) _menuSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawMenu(_childName, MAIN_ITEMS, MAIN_COUNT, _menuSel,
                         _wifiOk, api.offlineCount());
        display.refresh(); touch(); return;
    }
    if (btn != BTN_MID) return;
    touch();
    switch (_menuSel) {
        case 0:  // Feeding — default to opposite breast, or repeat last method
            if      (strcmp(_lastFeedMethod, "bl") == 0) _subSel = 1;  // Last Left  → Right
            else if (strcmp(_lastFeedMethod, "br") == 0) _subSel = 0;  // Last Right → Left
            else if (strcmp(_lastFeedMethod, "f")  == 0) _subSel = 3;  // Last Formula → Formula
            else if (strcmp(_lastFeedMethod, "p")  == 0) _subSel = 4;  // Last Pumped → Pumped
            else                                          _subSel = 2;  // default: Both
            _state = ST_FEEDING_METHOD;
            display.drawMenu("Feeding", FEED_ITEMS, FEED_COUNT, _subSel);
            display.refresh(true);
            break;
        case 1:  // Diaper
            _subSel = 0;
            _state = ST_DIAPER_TYPE;
            display.drawMenu("Diaper", DIAPER_ITEMS, DIAPER_COUNT, _subSel);
            display.refresh(true);
            break;
        case 2:  // Sleep
            startTimer("Sleep", "sleep");
            _state = ST_SLEEP_TIMER;
            showTimerScreen();
            break;
        case 3:  // Tummy
            startTimer("Tummy", "tummy-time");
            _state = ST_TUMMY_TIMER;
            showTimerScreen();
            break;
        case 4:  // Pumping
            startTimer("Pumping", "pumping");
            _state = ST_PUMP_TIMER;
            showTimerScreen();
            break;
        case 5:  // Medication
            _subSel = 0;
            _state = ST_MEDICATION_SELECT;
            {
                char sub1[20], sub2[24];
                _medSubLabel(0, sub1, sizeof(sub1));
                _medLastLabel(0, sub2, sizeof(sub2));
                display.drawMenu("Medication", _medicationPtrs, _medCount + 1, _subSel,
                                 true, 0, sub1[0] ? sub1 : nullptr, sub2[0] ? sub2 : nullptr);
            }
            display.refresh(true);
            break;
        case 6:  // Temp
            _tempValue = (_lastTemp > 0.0f) ? _lastTemp : 37.0f;
            _state = ST_TEMP_VALUE;
            display.drawNumericSelector("Temp", _tempValue, 0.1f, 30.0f, 45.0f, "C", _wifiOk);
            display.refresh(true);
            break;
        case 7:  // Settings
            _subSel = 0;
            _state = ST_SETTINGS;
            display.drawMenu("Settings", SETTINGS_ITEMS, SETTINGS_COUNT, _subSel,
                             _wifiOk, api.offlineCount());
            display.refresh(true);
            break;
    }
}

static void handleFeedingMethod(BtnEvent btn) {
    if (btn == BTN_UP   && _subSel > 0)             _subSel--;
    if (btn == BTN_DOWN && _subSel < FEED_COUNT-1)  _subSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawMenu("Feeding", FEED_ITEMS, FEED_COUNT, _subSel);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    if (_subSel == FEED_COUNT - 1) { enterMainMenu(); return; }  // Back
    strncpy(_feedMethod, FEED_METHODS[_subSel], sizeof(_feedMethod) - 1);
    startTimer("Feeding", "feeding");
    _state = ST_FEEDING_TIMER;
    showTimerScreen();
}

static void handleTimerScreen(BtnEvent btn) {
    if (millis() - _timerRefreshMs > 60000UL) {
        _timerRefreshMs = millis();
        uint32_t elapsed = (uint32_t)(time(nullptr) - _timer.startTime);
        display.drawTimer(_timer.activity, elapsed, _wifiOk);
        display.refresh();
    }
    if (btn == BTN_MID) {
        touch();
        if (strcmp(_timer.activity, "Feeding") == 0)    _state = ST_FEEDING_SAVE;
        else if (strcmp(_timer.activity, "Sleep") == 0)  _state = ST_SLEEP_SAVE;
        else if (strcmp(_timer.activity, "Tummy") == 0)  _state = ST_TUMMY_SAVE;
        else if (strcmp(_timer.activity, "Pumping") == 0) {
            _state = ST_PUMP_AMOUNT;
            display.drawNumericSelector("Pumped", _pumpAmount,
                                        5.0f, 0.0f, 400.0f, "ml", _wifiOk);
            display.refresh(true);
        }
    }
    if (btn == BTN_DOWN || btn == BTN_MID_LONG) {
        touch();
        if (_timer.bbTimerId > 0 && _wifiOk) api.stopTimer(_timer.bbTimerId);
        clearTimer();
        enterMainMenu();
    }
}

static void handleFeedingSave() {
    time_t start = _timer.startTime;
    time_t end   = time(nullptr);
    if (_timer.bbTimerId > 0 && _wifiOk) api.stopTimer(_timer.bbTimerId);
    clearTimer();

    // Persist feed method so next menu open shows the smart default
    strncpy(_lastFeedMethod, _feedMethod, sizeof(_lastFeedMethod) - 1);
    {
        Preferences sp;
        sp.begin("bb_state", false);
        sp.putString("lastfeed", _lastFeedMethod);
        sp.end();
    }

    if (_wifiOk) {
        BBResult r = api.logFeeding(_childId, start, end, _feedMethod);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char startBuf[24], endBuf[24], body[256];
        struct tm* tmS = localtime(&start); strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%S", tmS);
        struct tm* tmE = localtime(&end);   strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%S", tmE);
        const char* apiMethod = "bottle";
        const char* apiType   = "breast milk";
        if (strcmp(_feedMethod,"bl")==0)      { apiMethod="left breast";  }
        else if (strcmp(_feedMethod,"br")==0) { apiMethod="right breast"; }
        else if (strcmp(_feedMethod,"bo")==0) { apiMethod="both breasts"; }
        else if (strcmp(_feedMethod,"f")==0)  { apiMethod="bottle"; apiType="formula"; }
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\","
                 "\"method\":\"%s\",\"type\":\"%s\"}",
                 _childId, startBuf, endBuf, apiMethod, apiType);
        offlineLog("/api/feedings/", body, "Saved!");
    }
}

static void handleDiaperType(BtnEvent btn) {
    if (btn == BTN_UP   && _subSel > 0)               _subSel--;
    if (btn == BTN_DOWN && _subSel < DIAPER_COUNT-1)  _subSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawMenu("Diaper", DIAPER_ITEMS, DIAPER_COUNT, _subSel);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    if (_subSel == DIAPER_COUNT - 1) { enterMainMenu(); return; }  // Back
    _diaperWet   = (_subSel == 0 || _subSel == 2);
    _diaperSolid = (_subSel == 1 || _subSel == 2);
    _state = ST_DIAPER_SAVE;
}

static void handleDiaperSave() {
    time_t now = time(nullptr);
    if (_wifiOk) {
        BBResult r = api.logDiaper(_childId, now, _diaperWet, _diaperSolid);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char timeBuf[24], body[128];
        struct tm* tm = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", tm);
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"time\":\"%s\",\"wet\":%s,\"solid\":%s}",
                 _childId, timeBuf,
                 _diaperWet?"true":"false", _diaperSolid?"true":"false");
        offlineLog("/api/changes/", body, "Saved!");
    }
}

static void handleGenericSave() {
    time_t start = _timer.startTime;
    time_t end   = time(nullptr);
    if (_timer.bbTimerId > 0 && _wifiOk) api.stopTimer(_timer.bbTimerId);
    const char* act = _timer.activity;
    clearTimer();

    const char* path = "/api/sleep/";
    if (strcmp(act, "Tummy") == 0) path = "/api/tummy-times/";

    if (_wifiOk) {
        BBResult r;
        if (strcmp(act, "Sleep") == 0) r = api.logSleep(_childId, start, end);
        else                            r = api.logTummyTime(_childId, start, end);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char startBuf[24], endBuf[24], body[128];
        struct tm* tmS = localtime(&start); strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%S", tmS);
        struct tm* tmE = localtime(&end);   strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%S", tmE);
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\"}",
                 _childId, startBuf, endBuf);
        offlineLog(path, body, "Saved!");
    }
}

static void handlePumpAmount(BtnEvent btn) {
    if (btn == BTN_UP   && _pumpAmount < 400.0f) _pumpAmount += 5.0f;
    if (btn == BTN_DOWN && _pumpAmount > 0.0f)   _pumpAmount -= 5.0f;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawNumericSelector("Pumped", _pumpAmount,
                                    5.0f, 0.0f, 400.0f, "ml", _wifiOk);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { clearTimer(); enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    _state = ST_PUMP_SAVE;
}

static void handlePumpSave() {
    time_t start = _timer.startTime;
    time_t end   = time(nullptr);
    if (_timer.bbTimerId > 0 && _wifiOk) api.stopTimer(_timer.bbTimerId);
    clearTimer();

    if (_wifiOk) {
        BBResult r = api.logPumping(_childId, start, end, _pumpAmount);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char startBuf[24], endBuf[24], body[192];
        struct tm* tmS = localtime(&start); strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%S", tmS);
        struct tm* tmE = localtime(&end);   strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%S", tmE);
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\",\"amount\":%.1f}",
                 _childId, startBuf, endBuf, _pumpAmount);
        offlineLog("/api/pumping/", body, "Saved!");
    }
}

static void handleSettings(BtnEvent btn) {
    if (btn == BTN_UP   && _subSel > 0)                _subSel--;
    if (btn == BTN_DOWN && _subSel < SETTINGS_COUNT-1) _subSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawMenu("Settings", SETTINGS_ITEMS, SETTINGS_COUNT, _subSel,
                         _wifiOk, api.offlineCount());
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG || (btn == BTN_MID && _subSel == 5)) {
        touch(); enterMainMenu(); return;
    }
    if (btn != BTN_MID) return;
    touch();
    switch (_subSel) {
        case 0: {  // WiFi
            display.drawError(_wifiOk ? "WiFi OK" : "No WiFi",
                              _wifiOk ? "Connctd" : "Offline");
            display.refresh();
            break;
        }
        case 1: {  // Offline count
            char msg[12];
            snprintf(msg, sizeof(msg), "%d events", api.offlineCount());
            display.drawError("Queue", msg);
            display.refresh();
            break;
        }
        case 2:  // Replay
            if (_wifiOk) {
                int q = api.offlineCount();
                display.drawProgress("Replaying", 0, q);
                display.refresh();
                int done = api.replayOffline();
                char msg[12];
                snprintf(msg, sizeof(msg), "%d/%d ok", done, q);
                display.drawError("Replayed", msg);
                display.refresh();
            } else {
                display.drawError("No WiFi", "Offline");
                display.refresh();
            }
            break;
        case 3: {  // Clear queue
            Preferences p; p.begin("bb_queue", false); p.clear(); p.end();
            display.drawError("Queue", "Cleared");
            display.refresh();
            break;
        }
        case 4: {  // Child
            if (!_wifiOk) {
                display.drawError("No WiFi", "Offline");
                display.refresh();
                break;
            }
            _childCount = api.fetchChildren(_children, 4);
            if (_childCount == 0) {
                display.drawError("Child", "Not found");
                display.refresh();
                break;
            }
            for (int i = 0; i < _childCount; i++) _childNamePtrs[i] = _children[i].name;
            _childNamePtrs[_childCount] = "Back";
            _childSel = 0;
            _state = ST_SETTINGS_CHILD;
            display.drawMenu("Child", _childNamePtrs, _childCount + 1, _childSel);
            display.refresh(true);
            break;
        }
    }
}

static void handleSettingsChild(BtnEvent btn) {
    int count = _childCount + 1;
    if (btn == BTN_UP   && _childSel > 0)          _childSel--;
    if (btn == BTN_DOWN && _childSel < count - 1)  _childSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawMenu("Child", _childNamePtrs, count, _childSel);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG || (btn == BTN_MID && _childSel == count - 1)) {
        touch();
        _subSel = 4;  // return focus to "Child" in settings
        _state = ST_SETTINGS;
        display.drawMenu("Settings", SETTINGS_ITEMS, SETTINGS_COUNT, _subSel,
                         _wifiOk, api.offlineCount());
        display.refresh(true);
        return;
    }
    if (btn != BTN_MID) return;
    touch();
    _childId = _children[_childSel].id;
    strncpy(_childName, _children[_childSel].name, sizeof(_childName) - 1);
    api.setActiveChild(_childId, _childName);
    display.drawStatus("Saved!");
    display.refresh(true);
    delay(1000);
    _subSel = 4;
    _state = ST_SETTINGS;
    display.drawMenu("Settings", SETTINGS_ITEMS, SETTINGS_COUNT, _subSel,
                     _wifiOk, api.offlineCount());
    display.refresh(true);
    touch();
}

static void _medSubLabel(int idx, char* buf, size_t len) {
    buf[0] = '\0';
    if (idx >= _medCount || _meds[idx].intervalSec == 0) return;
    uint32_t sec = _meds[idx].intervalSec;
    uint32_t h   = sec / 3600;
    uint32_t m   = (sec % 3600) / 60;
    if (h > 0 && m > 0) snprintf(buf, len, "every %uh%um", h, m);
    else if (h > 0)     snprintf(buf, len, "every %uh", h);
    else                snprintf(buf, len, "every %um", m);
}

static void _medLastLabel(int idx, char* buf, size_t len) {
    buf[0] = '\0';
    if (idx >= _medCount || _meds[idx].lastTime <= 0) return;
    time_t now = time(nullptr);
    if (now <= _meds[idx].lastTime) return;
    char ago[12];
    formatAgo(_meds[idx].lastTime, now, ago, sizeof(ago));
    bool due = (_meds[idx].intervalSec > 0) &&
               ((uint32_t)(now - _meds[idx].lastTime) >= _meds[idx].intervalSec);
    snprintf(buf, len, due ? "%s ago +" : "%s ago", ago);
}

static void handleMedicationSelect(BtnEvent btn) {
    int count = _medCount + 1;
    if (btn == BTN_UP   && _subSel > 0)          _subSel--;
    if (btn == BTN_DOWN && _subSel < count - 1)  _subSel++;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        char sub1[20], sub2[24];
        _medSubLabel(_subSel, sub1, sizeof(sub1));
        _medLastLabel(_subSel, sub2, sizeof(sub2));
        display.drawMenu("Medication", _medicationPtrs, count, _subSel,
                         true, 0, sub1[0] ? sub1 : nullptr, sub2[0] ? sub2 : nullptr);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    if (_subSel == count - 1) { enterMainMenu(); return; }  // Back
    _medicationIdx    = _subSel;
    _medicationAmount = _meds[_medicationIdx].amount;
    _state = ST_MEDICATION_AMOUNT;
    {
        char intStr[20], lstStr[24];
        _medSubLabel(_medicationIdx, intStr, sizeof(intStr));
        _medLastLabel(_medicationIdx, lstStr, sizeof(lstStr));
        display.drawNumericSelector(_meds[_medicationIdx].name, _medicationAmount,
                                    0.5f, 0.0f, 50.0f, _meds[_medicationIdx].unit, _wifiOk,
                                    intStr[0] ? intStr : nullptr,
                                    lstStr[0] ? lstStr : nullptr);
    }
    display.refresh(true);
}

static void handleMedicationAmount(BtnEvent btn) {
    if (btn == BTN_UP   && _medicationAmount < 50.0f) _medicationAmount += 0.5f;
    if (btn == BTN_DOWN && _medicationAmount > 0.0f)  _medicationAmount -= 0.5f;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        char intStr[20], lstStr[24];
        _medSubLabel(_medicationIdx, intStr, sizeof(intStr));
        _medLastLabel(_medicationIdx, lstStr, sizeof(lstStr));
        display.drawNumericSelector(_meds[_medicationIdx].name, _medicationAmount,
                                    0.5f, 0.0f, 50.0f, _meds[_medicationIdx].unit, _wifiOk,
                                    intStr[0] ? intStr : nullptr,
                                    lstStr[0] ? lstStr : nullptr);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    _state = ST_MEDICATION_SAVE;
}

static void handleMedicationSave() {
    time_t now    = time(nullptr);
    const char* name = _meds[_medicationIdx].name;
    const char* unit = _meds[_medicationIdx].unit;

    // Record dose time immediately so the next visit shows the correct interval
    _meds[_medicationIdx].lastTime = now;
    saveMedLastTimes();

    if (_wifiOk) {
        BBResult r = api.logMedication(_childId, now, name, _medicationAmount, unit);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char timeBuf[24], body[256];
        struct tm* tm = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", tm);
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"name\":\"%s\",\"time\":\"%s\","
                 "\"dosage\":%.2f,\"dosage_unit\":\"%s\"}",
                 _childId, name, timeBuf, _medicationAmount, unit);
        offlineLog("/api/medication/", body, "Saved!");
    }
}

static void handleTempValue(BtnEvent btn) {
    if (btn == BTN_UP   && _tempValue < 45.0f)
        _tempValue = roundf((_tempValue + 0.1f) * 10.0f) / 10.0f;
    if (btn == BTN_DOWN && _tempValue > 30.0f)
        _tempValue = roundf((_tempValue - 0.1f) * 10.0f) / 10.0f;
    if (btn == BTN_UP || btn == BTN_DOWN) {
        display.drawNumericSelector("Temp", _tempValue, 0.1f, 30.0f, 45.0f, "C", _wifiOk);
        display.refresh(); touch(); return;
    }
    if (btn == BTN_MID_LONG) { enterMainMenu(); return; }
    if (btn != BTN_MID) return;
    touch();
    _state = ST_TEMP_SAVE;
}

static void handleTempSave() {
    time_t now = time(nullptr);

    // Persist last temperature so next entry defaults to it
    _lastTemp = _tempValue;
    {
        Preferences sp;
        sp.begin("bb_state", false);
        sp.putFloat("lasttemp", _lastTemp);
        sp.end();
    }

    if (_wifiOk) {
        BBResult r = api.logTemperature(_childId, now, _tempValue);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char timeBuf[24], body[128];
        struct tm* tmP = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", tmP);
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"temperature\":%.1f,\"time\":\"%s\"}",
                 _childId, _tempValue, timeBuf);
        offlineLog("/api/temperature/", body, "Saved!");
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    M5.begin();
    Serial.begin(115200);
    handleBoot();
}

void loop() {
    // Background WiFi — poll until connected or timed out
    if (_wifiPending) {
        if (WiFi.status() == WL_CONNECTED) {
            _wifiPending = false;
            onWiFiConnected();
        } else if (millis() - _wifiStartMs > 30000UL) {
            _wifiPending = false;
            WiFi.disconnect(true);  // give up; _wifiOk stays false
        }
    }

    BtnEvent btn = readButton();

    // PWR button — go to sleep screen from anywhere
    if (btn == BTN_EXT) { goToSleep(); return; }

    switch (_state) {
        case ST_MAIN_MENU:
            if (btn != BTN_NONE) handleMainMenu(btn);
            break;
        case ST_FEEDING_METHOD:
            if (btn != BTN_NONE) handleFeedingMethod(btn);
            break;
        case ST_FEEDING_TIMER:
            handleTimerScreen(btn);
            if (_state == ST_FEEDING_SAVE) handleFeedingSave();
            break;
        case ST_DIAPER_TYPE:
            if (btn != BTN_NONE) handleDiaperType(btn);
            break;
        case ST_DIAPER_SAVE:
            handleDiaperSave();
            break;
        case ST_SLEEP_TIMER:
            handleTimerScreen(btn);
            if (_state == ST_SLEEP_SAVE) handleGenericSave();
            break;
        case ST_TUMMY_TIMER:
            handleTimerScreen(btn);
            if (_state == ST_TUMMY_SAVE) handleGenericSave();
            break;
        case ST_PUMP_TIMER:
            handleTimerScreen(btn);
            break;
        case ST_PUMP_AMOUNT:
            if (btn != BTN_NONE) handlePumpAmount(btn);
            if (_state == ST_PUMP_SAVE) handlePumpSave();
            break;
        case ST_PUMP_SAVE:
            handlePumpSave();
            break;
        case ST_SETTINGS:
            if (btn != BTN_NONE) handleSettings(btn);
            break;
        case ST_SETTINGS_CHILD:
            if (btn != BTN_NONE) handleSettingsChild(btn);
            break;
        case ST_MEDICATION_SELECT:
            if (btn != BTN_NONE) handleMedicationSelect(btn);
            break;
        case ST_MEDICATION_AMOUNT:
            if (btn != BTN_NONE) handleMedicationAmount(btn);
            if (_state == ST_MEDICATION_SAVE) handleMedicationSave();
            break;
        case ST_MEDICATION_SAVE:
            handleMedicationSave();
            break;
        case ST_TEMP_VALUE:
            if (btn != BTN_NONE) handleTempValue(btn);
            if (_state == ST_TEMP_SAVE) handleTempSave();
            break;
        case ST_TEMP_SAVE:
            handleTempSave();
            break;
        case ST_ERROR:
            if (btn == BTN_MID || btn == BTN_MID_LONG) enterMainMenu();
            break;
        default:
            break;
    }

    // Inactivity sleep — skip during active timers (they resume after wake)
    bool timerActive = (_state == ST_FEEDING_TIMER || _state == ST_SLEEP_TIMER ||
                        _state == ST_TUMMY_TIMER   || _state == ST_PUMP_TIMER);
    if (!timerActive &&
        (millis() - _lastActivityMs) > ((unsigned long)SLEEP_TIMEOUT_S * 1000UL)) {
        goToSleep();
    }
}
