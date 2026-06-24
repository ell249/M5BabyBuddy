#pragma once
#include <M5CoreInk.h>

// Layout constants
// _big  → FreeSansBold18pt7b (yAdvance=42, cap height≈26px) — headers, menus
// _huge → FreeSansBold24pt7b (yAdvance=56, cap height≈34px) — timer/value readout
// _small→ FreeSansBold12pt7b (yAdvance=29, cap height≈17px) — indicators
static const int DISPLAY_W   = 200;
static const int DISPLAY_H   = 200;
static const int HEADER_H    = 34;   // 18pt cap height + margin + separator
static const int CONTENT_Y   = 36;   // first content row y
static const int BIG_H       = 26;   // approximate cap height for 18pt bold
static const int HUGE_H      = 34;   // approximate cap height for 24pt bold
static const int SMALL_H     = 17;   // approximate cap height for 12pt bold
static const int TEXT_LEFT   = 8;
static const int MAX_ROWS    = 3;    // max records in summary screen

class UI {
public:
    void begin();
    void clear();
    void refresh(bool full = false);

    void drawConnecting(const char* ssid);       // kept for reference, no longer called on boot
    void drawConnectingDots(int n);              // minimal partial-refresh connecting indicator
    void drawStatus(const char* msg);
    void drawError(const char* title, const char* msg);
    void drawHeader(const char* name, bool wifiOk, int offlineCount = 0);
    void drawMenu(const char* title, const char** items, int count, int selected,
                  bool wifiOk = true, int offlineCount = 0,
                  const char* sub1 = nullptr, const char* sub2 = nullptr);
    void drawTimer(const char* activity, uint32_t elapsedSec, bool wifiOk = true);
    void drawNumericSelector(const char* label, float value, float step,
                             float minVal, float maxVal, const char* unit,
                             bool wifiOk = true,
                             const char* hintLeft  = nullptr,
                             const char* hintRight = nullptr);

    struct SummaryRecord {
        int  iconType;    // 0=feeding, 1=diaper, 2=sleep
        char relTime[12]; // "45m" or "1h5m" (relative)
        char absTime[12]; // "10:30" or "10:30 B" (absolute + optional method)
    };
    void drawSummary(const char* babyName, const char* clockStr,
                     const SummaryRecord* records, int count);

    void drawProgress(const char* msg, int done, int total);

private:
    Ink_Sprite* _sprite = nullptr;

    void _hline(int y);
    void _fill(int x, int y, int w, int h);
    void _rect(int x, int y, int w, int h);
    void _big(int x, int y, const char* s);          // 18pt — nav/labels
    void _bigCenter(int y, const char* s);
    int  _bigWidth(const char* s);
    void _huge(int x, int y, const char* s);         // 24pt — timer/values
    void _hugeCenter(int y, const char* s);
    void _small(int x, int y, const char* s);        // 12pt — indicators
    void _smallCenter(int y, const char* s);
    int  _smallWidth(const char* s);
    void _formatElapsed(uint32_t sec, char* buf, size_t len);
    void _drawIcon(int x, int y, int type);          // 20×20 icon: 0=bottle,1=diaper,2=moon,3=hourglass,4=droplet,5=pill,6=thermometer
};

extern UI display;
