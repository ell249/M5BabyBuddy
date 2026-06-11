#include "ui.h"
#include <stdio.h>
#include <string.h>

UI display;

// ── Init / refresh ────────────────────────────────────────────────────────────

void UI::begin() {
    _sprite = new Ink_Sprite(&M5.M5Ink);
    _sprite->creatSprite(0, 0, DISPLAY_W, DISPLAY_H, true);
    _sprite->setTextDatum(lgfx::datum_t::top_left);
    _sprite->setTextColor(0xFFFF, 0x0000);  // white=ink, black=paper (e-ink)
}

void UI::clear() {
    if (!_sprite) return;
    M5.M5Ink.clear();
    _sprite->clear();
    _sprite->pushSprite();
}

void UI::refresh(bool full) {
    if (!_sprite) return;
    if (full) M5.M5Ink.clear();
    _sprite->pushSprite();
}

// ── Primitives ────────────────────────────────────────────────────────────────

void UI::_hline(int y) {
    _sprite->FillRect(0, y, DISPLAY_W, 1, 1);
}

void UI::_fill(int x, int y, int w, int h) {
    _sprite->FillRect(x, y, w, h, 1);
}

void UI::_rect(int x, int y, int w, int h) {
    _fill(x,     y,     w, 1);
    _fill(x,     y+h-1, w, 1);
    _fill(x,     y,     1, h);
    _fill(x+w-1, y,     1, h);
}

// Ink_Sprite::drawString hides M5Canvas overloads (C++ name hiding).
// Cast to M5Canvas to reach the lgfx version that works with GFX fonts via setFont().

void UI::_big(int x, int y, const char* s) {
    _sprite->setFont(&fonts::FreeSansBold18pt7b);
    static_cast<M5Canvas*>(_sprite)->drawString(s, x, y);
}

void UI::_bigCenter(int y, const char* s) {
    int w = _bigWidth(s);
    int x = (DISPLAY_W - w) / 2;
    if (x < TEXT_LEFT) x = TEXT_LEFT;
    _big(x, y, s);
}

int UI::_bigWidth(const char* s) {
    _sprite->setFont(&fonts::FreeSansBold18pt7b);
    return (int)_sprite->textWidth(s);
}

void UI::_huge(int x, int y, const char* s) {
    _sprite->setFont(&fonts::FreeSansBold24pt7b);
    static_cast<M5Canvas*>(_sprite)->drawString(s, x, y);
}

void UI::_hugeCenter(int y, const char* s) {
    _sprite->setFont(&fonts::FreeSansBold24pt7b);
    int w = (int)_sprite->textWidth(s);
    int x = (DISPLAY_W - w) / 2;
    if (x < TEXT_LEFT) x = TEXT_LEFT;
    _huge(x, y, s);
}

void UI::_small(int x, int y, const char* s) {
    _sprite->setFont(&fonts::FreeSansBold12pt7b);
    static_cast<M5Canvas*>(_sprite)->drawString(s, x, y);
}

int UI::_smallWidth(const char* s) {
    _sprite->setFont(&fonts::FreeSansBold12pt7b);
    return (int)_sprite->textWidth(s);
}

void UI::_formatElapsed(uint32_t sec, char* buf, size_t len) {
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (h > 0) snprintf(buf, len, "%uh%02um", (unsigned)h, (unsigned)m);
    else        snprintf(buf, len, "%um",       (unsigned)m);
}

// ── Header ────────────────────────────────────────────────────────────────────

void UI::drawHeader(const char* name, bool wifiOk, int offlineCount) {
    _sprite->clear();
    _big(TEXT_LEFT, 2, name);

    // Small indicator top-right — use small font so it fits alongside long names
    char ind[6];
    if (!wifiOk && offlineCount > 0)
        snprintf(ind, sizeof(ind), "Q%d", offlineCount);
    else
        snprintf(ind, sizeof(ind), "%s", wifiOk ? "W" : "X");

    int rx = DISPLAY_W - _smallWidth(ind) - 3;
    _small(rx, 6, ind);

    _hline(HEADER_H - 2);
}

// ── Menu — one item per page ──────────────────────────────────────────────────

void UI::drawMenu(const char* title, const char** items, int count, int selected,
                  bool wifiOk, int offlineCount) {
    if (!_sprite) return;
    drawHeader(title, wifiOk, offlineCount);

    // Item centered horizontally and vertically in content area
    // Reserve SMALL_H+4 at bottom for position indicator
    int availH = DISPLAY_H - CONTENT_Y - SMALL_H - 6;
    int itemY  = CONTENT_Y + (availH - BIG_H) / 2;
    _bigCenter(itemY, items[selected]);

    // Position indicator bottom-right in small font
    char pg[8];
    snprintf(pg, sizeof(pg), "%d/%d", selected + 1, count);
    int pgW = _smallWidth(pg);
    _small(DISPLAY_W - pgW - 4, DISPLAY_H - SMALL_H - 2, pg);
}

// ── Utility screens ───────────────────────────────────────────────────────────

void UI::drawConnecting(const char* ssid) {
    if (!_sprite) return;
    _sprite->clear();
    _big(TEXT_LEFT, 2, "Connect");
    _hline(HEADER_H - 2);
    _big(TEXT_LEFT, CONTENT_Y, "to WiFi");
    char buf[32];
    snprintf(buf, sizeof(buf), "%.30s", ssid);
    _small(TEXT_LEFT, CONTENT_Y + BIG_H + 10, buf);
}

void UI::drawStatus(const char* msg) {
    if (!_sprite) return;
    _sprite->clear();
    _hline(HEADER_H - 2);
    _bigCenter(CONTENT_Y + (DISPLAY_H - CONTENT_Y - BIG_H) / 2, msg);
}

void UI::drawError(const char* title, const char* msg) {
    if (!_sprite) return;
    drawHeader(title, false, 0);
    _bigCenter(CONTENT_Y + (DISPLAY_H - CONTENT_Y - BIG_H) / 2, msg);
}

// ── Timer screen ──────────────────────────────────────────────────────────────

void UI::drawTimer(const char* activity, uint32_t elapsedSec, bool wifiOk) {
    if (!_sprite) return;
    drawHeader(activity, wifiOk, 0);

    char elapsed[12];
    _formatElapsed(elapsedSec, elapsed, sizeof(elapsed));

    // Elapsed time in large 24pt font, centred in content area
    int availH = DISPLAY_H - CONTENT_Y - SMALL_H - 6;
    int y = CONTENT_Y + (availH - HUGE_H) / 2;
    _hugeCenter(y, elapsed);

    _small(TEXT_LEFT, DISPLAY_H - SMALL_H - 2, "MID=Stop  DN=Cancel");
}

// ── Numeric selector ──────────────────────────────────────────────────────────

void UI::drawNumericSelector(const char* label, float value,
                              float step, float minVal, float maxVal,
                              const char* unit, bool wifiOk) {
    if (!_sprite) return;
    drawHeader(label, wifiOk, 0);

    char valBuf[12];
    if (step >= 1.0f) snprintf(valBuf, sizeof(valBuf), "%d%s", (int)value, unit);
    else               snprintf(valBuf, sizeof(valBuf), "%.1f%s", value, unit);

    int availH = DISPLAY_H - CONTENT_Y - SMALL_H - 6;
    int y = CONTENT_Y + (availH - HUGE_H) / 2;
    _hugeCenter(y, valBuf);

    char rangeBuf[24];
    snprintf(rangeBuf, sizeof(rangeBuf), "%.0f-%.0f %s", minVal, maxVal, unit);
    _small(TEXT_LEFT, DISPLAY_H - SMALL_H - 2, rangeBuf);
}

// ── Sleep summary screen ──────────────────────────────────────────────────────

void UI::drawSummary(const char* babyName, const char* clockStr,
                     const SummaryRecord* records, int count) {
    if (!_sprite) return;
    _sprite->clear();

    // Header: baby name (18pt) left, clock (12pt) right — small font avoids overlap
    _big(TEXT_LEFT, 2, babyName);
    int cw = _smallWidth(clockStr);
    _small(DISPLAY_W - cw - TEXT_LEFT, 8, clockStr);
    _hline(HEADER_H - 2);

    if (count == 0) {
        _bigCenter(CONTENT_Y + (DISPLAY_H - CONTENT_Y - BIG_H) / 2, "No data");
        return;
    }

    // Evenly space up to 3 rows — label left, time right, both in 18pt
    int availH = DISPLAY_H - CONTENT_Y;
    int rowSpacing = availH / count;
    for (int i = 0; i < count && i < MAX_ROWS; i++) {
        int y = CONTENT_Y + i * rowSpacing + (rowSpacing - BIG_H) / 2;
        _big(TEXT_LEFT, y, records[i].label);
        int tw = _bigWidth(records[i].time);
        _big(DISPLAY_W - tw - TEXT_LEFT, y, records[i].time);
    }
}

// ── Progress screen ───────────────────────────────────────────────────────────

void UI::drawProgress(const char* msg, int done, int total) {
    if (!_sprite) return;
    _sprite->clear();
    _big(TEXT_LEFT, 2, "Syncing");
    _hline(HEADER_H - 2);

    _small(TEXT_LEFT, CONTENT_Y + 4, msg);

    int barX = TEXT_LEFT, barY = CONTENT_Y + SMALL_H + 10;
    int barW = DISPLAY_W - TEXT_LEFT * 2, barH = 14;
    _rect(barX, barY, barW, barH);
    if (total > 0 && done > 0) {
        int filled = (barW - 2) * done / total;
        if (filled > 0) _fill(barX + 1, barY + 1, filled, barH - 2);
    }

    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d / %d", done, total);
    _small(TEXT_LEFT, barY + barH + 6, countBuf);
}
