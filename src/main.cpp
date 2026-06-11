#include <Arduino.h>
#include <M5CoreInk.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>

#include "config.h"
#include "ui.h"
#include "api.h"

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
    ST_ERROR,
    ST_SLEEPING,
};

// ── Persistent timer state ────────────────────────────────────────────────────
static const char TIMER_NS[]      = "bb_timer";
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
static bool          _diaperWet      = true;
static bool          _diaperSolid    = false;
static char          _errorMsg[80]   = "";
static unsigned long _lastActivityMs = 0;
static unsigned long _timerRefreshMs = 0;

// ── Menu definitions (≤8 chars per item for AsciiFont24x48) ──────────────────
static const char* MAIN_ITEMS[] = {
    "Feeding", "Diaper", "Sleep", "Tummy", "Pumping", "Settings"
};
static const int MAIN_COUNT = 6;

static const char* FEED_ITEMS[] = {
    "Left", "Right", "Both", "Formula", "Pumped", "Back"
};
static const int FEED_COUNT = 6;
static const char* FEED_METHODS[] = { "bl", "br", "bo", "f", "p" };

static const char* DIAPER_ITEMS[] = { "Wet", "Dirty", "Both", "Back" };
static const int DIAPER_COUNT = 4;

static const char* SETTINGS_ITEMS[] = {
    "WiFi", "Offline", "Replay", "Clear Q", "Back"
};
static const int SETTINGS_COUNT = 5;

// ── Button reading ────────────────────────────────────────────────────────────
static unsigned long _midPressStart = 0;
static bool          _midHeld       = false;

enum BtnEvent { BTN_NONE, BTN_UP, BTN_MID, BTN_DOWN, BTN_MID_LONG };

static BtnEvent readButton() {
    M5.update();
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

// ── Deep sleep with summary screen ───────────────────────────────────────────
static void goToSleep() {
    _state = ST_SLEEPING;

    BBRecentRecord recs[3] = {};
    bool gotRecs = _wifiOk && api.getRecentRecords(recs);

    UI::SummaryRecord srecs[3] = {};
    int recCount = 0;
    if (gotRecs) {
        const char* labels[] = {"F", "D", "S"};
        for (int i = 0; i < 3; i++) {
            strncpy(srecs[i].label, labels[i],        sizeof(srecs[i].label) - 1);
            strncpy(srecs[i].time,  recs[i].timeStr,  sizeof(srecs[i].time)  - 1);
        }
        recCount = 3;
    }

    char clockBuf[8] = "--:--";
    time_t now = time(nullptr);
    if (now > 1000000) {
        struct tm* t = localtime(&now);
        strftime(clockBuf, sizeof(clockBuf), "%H:%M", t);
    }

    display.drawSummary(_childName, clockBuf, srecs, recCount);
    display.refresh(true);

    // Wake on BtnMID press (GPIO38, active LOW — pressed = 0)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
    delay(100);
    esp_deep_sleep_start();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 30000UL) {
        delay(500);
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
    display.drawConnecting(WIFI_SSID);
    display.refresh(true);

    _wifiOk = connectWiFi();
    api.begin(BB_BASE_URL, BB_AUTH_TOKEN);

    if (_wifiOk) {
        configTzTime(BB_TIMEZONE, NTP_SERVER);
        delay(1500);

        int q = api.offlineCount();
        if (q > 0) {
            display.drawProgress("Syncing...", 0, q);
            display.refresh();
            api.replayOffline();
        }
        _childId = BB_CHILD_ID > 0 ? BB_CHILD_ID : api.getChildId();
        if (_childId > 0) {
            api.fetchChildName(_childId);
            strncpy(_childName, api.getChildName(), sizeof(_childName) - 1);
            if (_childName[0] == '\0') strncpy(_childName, "Baby", sizeof(_childName));
        }
    } else {
        _childId = BB_CHILD_ID > 0 ? BB_CHILD_ID : -1;
        // Use cached name loaded from NVS in api.begin()
        const char* cached = api.getChildName();
        if (cached[0] != '\0') strncpy(_childName, cached, sizeof(_childName) - 1);
    }

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
        case 0:  // Feeding
            _subSel = 2;
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
        case 5:  // Settings
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

    if (_wifiOk) {
        BBResult r = api.logFeeding(_childId, start, end, _feedMethod);
        finishWithStatus("Saved!", r.ok, r.httpCode);
    } else {
        char startBuf[24], endBuf[24], body[256];
        struct tm* tmS = localtime(&start); strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%S", tmS);
        struct tm* tmE = localtime(&end);   strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%S", tmE);
        const char* type = (strcmp(_feedMethod,"f")==0)?"fo":(strcmp(_feedMethod,"s")==0)?"so":"br";
        snprintf(body, sizeof(body),
                 "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\","
                 "\"method\":\"%s\",\"type\":\"%s\"}",
                 _childId, startBuf, endBuf, _feedMethod, type);
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
    if (btn == BTN_MID_LONG || (btn == BTN_MID && _subSel == 4)) {
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
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    M5.begin();
    Serial.begin(115200);
    handleBoot();
}

void loop() {
    BtnEvent btn = readButton();

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
