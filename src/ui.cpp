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
    M5.M5Ink.waitDisplay();  // block until EPD physical refresh completes
}

// ── Primitives ────────────────────────────────────────────────────────────────

void UI::_hline(int y) {
    _sprite->FillRect(0, y, DISPLAY_W, 1, 1);
}

void UI::_fill(int x, int y, int w, int h) {
    // FillRect passes pixBit=1 raw to lgfx fillRect, but the M5CoreInk panel
    // driver inverts the buffer, so ink colour is 0xFFFF not 1.
    // Using M5Canvas::fillRect directly avoids Ink_Sprite's truncation to uint8_t.
    static_cast<M5Canvas*>(_sprite)->fillRect(x, y, w, h, 0xFFFF);
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
    _small(TEXT_LEFT, 8, name);

    _hline(HEADER_H - 2);

    // Small indicator bottom-left
    char ind[6];
    if (!wifiOk && offlineCount > 0)
        snprintf(ind, sizeof(ind), "Q%d", offlineCount);
    else
        snprintf(ind, sizeof(ind), "%s", wifiOk ? "W" : "X");
    _small(TEXT_LEFT, DISPLAY_H - SMALL_H - 2, ind);
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

void UI::drawConnectingDots(int n) {
    if (!_sprite) return;
    _sprite->clear();
    // Three-frame dot cycle — large enough to see clearly on e-ink
    static const char* frames[] = {".", "..", "..."};
    _bigCenter((DISPLAY_H - BIG_H) / 2, frames[n % 3]);
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

void UI::_drawIcon(int x, int y, int type) {
    if (!_sprite) return;
    switch (type) {
        case 0:  // Bottle — solid chunky silhouette
            _fill(x+7,  y+0,  6,  5);   // narrow spout (solid)
            _fill(x+4,  y+5,  12, 2);   // shoulder
            _fill(x+2,  y+7,  16, 13);  // wide body (solid)
            break;
        case 1:  // Diaper — top/bottom bands + crotch bridge
            _fill(x+0,  y+0,  20, 4);   // top waistband
            _fill(x+0,  y+4,  4,  12);  // left panel
            _fill(x+16, y+4,  4,  12);  // right panel
            _fill(x+4,  y+8,  12, 4);   // crotch bridge
            _fill(x+0,  y+16, 20, 4);   // bottom band
            break;
        case 2:  // Crescent moon — clear C-shape, no thin lines, no fillCircle
            _fill(x+3, y+0,  9, 3);    // wide top arc
            _fill(x+1, y+3,  5, 2);    // upper-left arm
            _fill(x+0, y+5,  4, 10);   // thick left body
            _fill(x+1, y+15, 5, 2);    // lower-left arm
            _fill(x+3, y+17, 9, 3);    // wide bottom arc
            break;
        case 3:  // Hourglass — tummy time
            _fill(x+0, y+0,  20, 2);
            _fill(x+2, y+2,  16, 2);
            _fill(x+4, y+4,  12, 2);
            _fill(x+6, y+6,  8,  2);
            _fill(x+8, y+8,  4,  4);   // waist
            _fill(x+6, y+12, 8,  2);
            _fill(x+4, y+14, 12, 2);
            _fill(x+2, y+16, 16, 2);
            _fill(x+0, y+18, 20, 2);
            break;
        case 4:  // Droplet — pumping
            _fill(x+8, y+0,  4,  2);   // tip
            _fill(x+7, y+2,  6,  2);
            _fill(x+6, y+4,  8,  2);
            _fill(x+5, y+6,  10, 2);
            _fill(x+4, y+8,  12, 6);   // widest section
            _fill(x+5, y+14, 10, 2);
            _fill(x+6, y+16, 8,  2);
            _fill(x+7, y+18, 6,  2);
            break;
        case 6:  // Thermometer — temperature
            _fill(x+8, y+0,  4, 12);   // tube (solid, 4px wide)
            _fill(x+7, y+12, 6,  1);   // shoulder top
            _fill(x+6, y+13, 8,  5);   // bulb body
            _fill(x+7, y+18, 6,  1);   // shoulder bottom
            break;
        case 5:  // Pill/capsule — solid left half, outlined right half
            // Left half (solid)
            _fill(x+2,  y+5,  8,  1);   // top edge   (x+2..x+9)
            _fill(x+1,  y+6,  9,  8);   // solid body (x+1..x+9)
            _fill(x+2,  y+14, 8,  1);   // bot edge   (x+2..x+9)
            // Right half (outline only — interior stays paper/white)
            _fill(x+10, y+5,  8,  1);   // top edge   (x+10..x+17)
            _fill(x+17, y+6,  2,  8);   // right cap
            _fill(x+10, y+14, 8,  1);   // bot edge   (x+10..x+17)
            break;
    }
}

void UI::drawSummary(const char* babyName, const char* clockStr,
                     const SummaryRecord* records, int count) {
    if (!_sprite) return;
    _sprite->clear();

    // Header: baby name left, current clock right
    _small(TEXT_LEFT, 8, babyName);
    int cw = _smallWidth(clockStr);
    _small(DISPLAY_W - cw - TEXT_LEFT, 8, clockStr);
    _hline(HEADER_H - 2);

    if (count == 0) {
        _bigCenter(CONTENT_Y + (DISPLAY_H - CONTENT_Y - BIG_H) / 2, "No data");
        return;
    }

    // 3 equal rows in the content area
    // Each row: [20px icon] [18pt relative time] / [12pt absolute time beneath]
    static const int ICON_W    = 20;
    static const int ICON_GAP  = 6;
    static const int TEXT_GAP  = 4;   // between rel and abs lines
    static const int ROW_H     = (DISPLAY_H - CONTENT_Y) / MAX_ROWS;

    for (int i = 0; i < count && i < MAX_ROWS; i++) {
        int rowY = CONTENT_Y + i * ROW_H;

        // Icon centred vertically in row (icon height ≈ 20px)
        _drawIcon(TEXT_LEFT, rowY + (ROW_H - 20) / 2, records[i].iconType);

        // Text block: relTime (18pt) above absTime (12pt)
        int textX     = TEXT_LEFT + ICON_W + ICON_GAP;
        int blockH    = BIG_H + TEXT_GAP + SMALL_H;
        int textY     = rowY + (ROW_H - blockH) / 2;
        _big(textX,  textY,            records[i].relTime);
        _small(textX, textY + BIG_H + TEXT_GAP, records[i].absTime);
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
