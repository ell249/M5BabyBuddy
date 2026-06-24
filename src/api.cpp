#include "api.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>

BabyBuddyAPI api;

// Offline queue stored as indexed NVS keys to avoid JSON serialisation issues.
// Keys: "count", "m0","p0","b0", "m1","p1","b1", ...
static const char NVS_QUEUE_NS[] = "bb_queue";
static const char NVS_CHILD_NS[] = "bb_child";
static const char NVS_CHILD_KEY[]= "id";
static const int  OFFLINE_MAX    = 20;

static bool _isHttps(const char* url) {
    return strncmp(url, "https://", 8) == 0;
}

void BabyBuddyAPI::begin(const char* baseUrl, const char* token) {
    strncpy(_baseUrl, baseUrl, sizeof(_baseUrl) - 1);
    strncpy(_token,   token,   sizeof(_token)   - 1);
    snprintf(_authHeader, sizeof(_authHeader), "Token %s", token);
    _childName[0] = '\0';

    // Load cached child name from NVS
    Preferences p;
    p.begin(NVS_CHILD_NS, true);
    p.getString("name", _childName, sizeof(_childName));
    p.end();
}

void BabyBuddyAPI::_isoTime(time_t t, char* buf, size_t len) {
    struct tm* tm = localtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", tm);
}

void BabyBuddyAPI::_formatDuration(time_t start, time_t end, char* buf, size_t len) {
    uint32_t sec = (uint32_t)(end - start);
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (h > 0) snprintf(buf, len, "%uh%02um", (unsigned)h, (unsigned)m);
    else        snprintf(buf, len, "%um",       (unsigned)m);
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

String BabyBuddyAPI::_get(const char* path, int& httpCode) {
    char url[160];
    snprintf(url, sizeof(url), "%s%s", _baseUrl, path);

    HTTPClient http;
    if (_isHttps(url)) {
        WiFiClientSecure* client = new WiFiClientSecure();
        client->setInsecure();  // skip cert verification for home server
        http.begin(*client, url);
        http.addHeader("Authorization", _authHeader);
        http.addHeader("Content-Type", "application/json");
        httpCode = http.GET();
        String body = (httpCode > 0) ? http.getString() : "";
        http.end();
        delete client;
        return body;
    } else {
        http.begin(url);
        http.addHeader("Authorization", _authHeader);
        http.addHeader("Content-Type", "application/json");
        httpCode = http.GET();
        String body = (httpCode > 0) ? http.getString() : "";
        http.end();
        return body;
    }
}

BBResult BabyBuddyAPI::_post(const char* path, const char* body) {
    char url[160];
    snprintf(url, sizeof(url), "%s%s", _baseUrl, path);

    BBResult r; r.error[0] = '\0';

    HTTPClient http;
    auto _do = [&](HTTPClient& h) {
        h.addHeader("Authorization", _authHeader);
        h.addHeader("Content-Type", "application/json");
        r.httpCode = h.POST((uint8_t*)body, strlen(body));
        r.ok = (r.httpCode == 200 || r.httpCode == 201);
        if (!r.ok) {
            strncpy(r.error, h.getString().c_str(), sizeof(r.error) - 1);
        }
        h.end();
    };

    if (_isHttps(url)) {
        WiFiClientSecure* client = new WiFiClientSecure();
        client->setInsecure();
        http.begin(*client, url);
        _do(http);
        delete client;
    } else {
        http.begin(url);
        _do(http);
    }
    return r;
}

BBResult BabyBuddyAPI::_del(const char* path) {
    char url[160];
    snprintf(url, sizeof(url), "%s%s", _baseUrl, path);

    BBResult r; r.error[0] = '\0';

    HTTPClient http;
    auto _do = [&](HTTPClient& h) {
        h.addHeader("Authorization", _authHeader);
        r.httpCode = h.sendRequest("DELETE");
        r.ok = (r.httpCode == 204 || r.httpCode == 200);
        if (!r.ok) strncpy(r.error, h.getString().c_str(), sizeof(r.error)-1);
        h.end();
    };

    if (_isHttps(url)) {
        WiFiClientSecure* client = new WiFiClientSecure();
        client->setInsecure();
        http.begin(*client, url);
        _do(http);
        delete client;
    } else {
        http.begin(url);
        _do(http);
    }
    return r;
}

// ── Child info ────────────────────────────────────────────────────────────────

void BabyBuddyAPI::fetchChildName(int childId) {
    int code;
    String body = _get("/api/children/", code);
    if (code != 200 || body.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;
    JsonArray results = doc["results"].as<JsonArray>();
    for (JsonObject child : results) {
        if ((int)(child["id"] | -1) == childId) {
            const char* name = child["first_name"] | "";
            strncpy(_childName, name, sizeof(_childName) - 1);
            Preferences p;
            p.begin(NVS_CHILD_NS, false);
            p.putString("name", _childName);
            p.end();
            return;
        }
    }
}

// ── Child ID ──────────────────────────────────────────────────────────────────

int BabyBuddyAPI::getChildId() {
    Preferences prefs;
    prefs.begin(NVS_CHILD_NS, true);
    int cached = prefs.getInt(NVS_CHILD_KEY, 0);
    prefs.end();
    if (cached > 0) return cached;

    int code;
    String body = _get("/api/children/", code);
    if (code != 200 || body.isEmpty()) return -1;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return -1;
    JsonArray results = doc["results"].as<JsonArray>();
    if (results.size() == 0) return -1;

    int id = results[0]["id"] | -1;
    if (id > 0) {
        prefs.begin(NVS_CHILD_NS, false);
        prefs.putInt(NVS_CHILD_KEY, id);
        prefs.end();
    }
    return id;
}

int BabyBuddyAPI::fetchChildren(BBChild* out, int maxCount) {
    int code;
    String body = _get("/api/children/", code);
    if (code != 200 || body.isEmpty()) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 0;
    JsonArray results = doc["results"].as<JsonArray>();

    int count = 0;
    for (JsonObject child : results) {
        if (count >= maxCount) break;
        out[count].id = child["id"] | -1;
        const char* first = child["first_name"] | "";
        strncpy(out[count].name, first, sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = '\0';
        count++;
    }
    return count;
}

void BabyBuddyAPI::setActiveChild(int id, const char* name) {
    strncpy(_childName, name, sizeof(_childName) - 1);
    _childName[sizeof(_childName) - 1] = '\0';
    Preferences p;
    p.begin(NVS_CHILD_NS, false);
    p.putInt(NVS_CHILD_KEY, id);
    p.putString("name", name);
    p.end();
}

// ── Timer management ──────────────────────────────────────────────────────────

BBTimer BabyBuddyAPI::getActiveTimer(const char* name) {
    BBTimer t; t.id = -1; t.start = 0; t.name[0] = '\0';
    char path[80];
    snprintf(path, sizeof(path), "/api/timers/?name=%s", name);
    int code;
    String body = _get(path, code);
    if (code != 200 || body.isEmpty()) return t;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return t;
    JsonArray results = doc["results"].as<JsonArray>();
    if (results.size() == 0) return t;

    t.id = results[0]["id"] | -1;
    strncpy(t.name, name, sizeof(t.name) - 1);
    const char* startStr = results[0]["start"] | "";
    struct tm tm = {};
    if (sscanf(startStr, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        t.start = mktime(&tm);
    }
    return t;
}

BBResult BabyBuddyAPI::startTimer(const char* name, int childId, BBTimer& out) {
    char startBuf[24]; _isoTime(time(nullptr), startBuf, sizeof(startBuf));
    char body[128];
    snprintf(body, sizeof(body),
             "{\"child\":%d,\"name\":\"%s\",\"start\":\"%s\"}",
             childId, name, startBuf);

    BBResult r = _post("/api/timers/", body);
    if (r.ok) out = getActiveTimer(name);
    else      out.id = -1;
    return r;
}

BBResult BabyBuddyAPI::stopTimer(int timerId) {
    char path[32];
    snprintf(path, sizeof(path), "/api/timers/%d/", timerId);
    return _del(path);
}

// ── Activity logging ──────────────────────────────────────────────────────────

BBResult BabyBuddyAPI::logFeeding(int child, time_t start, time_t end,
                                   const char* method) {
    char s[24], e[24];
    _isoTime(start, s, sizeof(s)); _isoTime(end, e, sizeof(e));
    // Convert internal abbreviations to the full strings the API requires
    const char* apiMethod = "bottle";
    const char* apiType   = "breast milk";
    if (strcmp(method,"bl")==0)      { apiMethod="left breast";  }
    else if (strcmp(method,"br")==0) { apiMethod="right breast"; }
    else if (strcmp(method,"bo")==0) { apiMethod="both breasts"; }
    else if (strcmp(method,"f")==0)  { apiMethod="bottle"; apiType="formula"; }
    // "p" (pumped) stays as bottle + breast milk
    char body[256];
    snprintf(body, sizeof(body),
             "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\","
             "\"method\":\"%s\",\"type\":\"%s\"}", child, s, e, apiMethod, apiType);
    return _post("/api/feedings/", body);
}

BBResult BabyBuddyAPI::logDiaper(int child, time_t when, bool wet, bool solid) {
    char t[24]; _isoTime(when, t, sizeof(t));
    char body[128];
    snprintf(body, sizeof(body),
             "{\"child\":%d,\"time\":\"%s\",\"wet\":%s,\"solid\":%s}",
             child, t, wet?"true":"false", solid?"true":"false");
    return _post("/api/changes/", body);
}

BBResult BabyBuddyAPI::logSleep(int child, time_t start, time_t end) {
    char s[24], e[24]; _isoTime(start,s,sizeof(s)); _isoTime(end,e,sizeof(e));
    char body[128];
    snprintf(body,sizeof(body),
             "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\"}", child,s,e);
    return _post("/api/sleep/", body);
}

BBResult BabyBuddyAPI::logTummyTime(int child, time_t start, time_t end) {
    char s[24], e[24]; _isoTime(start,s,sizeof(s)); _isoTime(end,e,sizeof(e));
    char body[128];
    snprintf(body,sizeof(body),
             "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\"}", child,s,e);
    return _post("/api/tummy-times/", body);
}

BBResult BabyBuddyAPI::logPumping(int child, time_t start, time_t end,
                                   float amountMl) {
    char s[24], e[24]; _isoTime(start,s,sizeof(s)); _isoTime(end,e,sizeof(e));
    char body[192];
    snprintf(body,sizeof(body),
             "{\"child\":%d,\"start\":\"%s\",\"end\":\"%s\",\"amount\":%.1f}",
             child, s, e, amountMl);
    return _post("/api/pumping/", body);
}

BBResult BabyBuddyAPI::logMedication(int child, time_t when, const char* name,
                                    float amount, const char* unit) {
    char t[24]; _isoTime(when, t, sizeof(t));
    char body[256];
    snprintf(body, sizeof(body),
             "{\"child\":%d,\"name\":\"%s\",\"time\":\"%s\","
             "\"dosage\":%.2f,\"dosage_unit\":\"%s\"}",
             child, name, t, amount, unit);
    return _post("/api/medication/", body);
}

BBResult BabyBuddyAPI::logTemperature(int childId, time_t when, float temp) {
    char t[24]; _isoTime(when, t, sizeof(t));
    char body[128];
    snprintf(body, sizeof(body),
             "{\"child\":%d,\"temperature\":%.1f,\"time\":\"%s\"}",
             childId, temp, t);
    return _post("/api/temperature/", body);
}

// Fetches last feed method, last temperature, and recent unique medications
// using a single SSL session (one TLS handshake for 3 requests).
int BabyBuddyAPI::fetchStartupData(int childId,
                                    char* feedMethod, size_t feedLen,
                                    float* lastTemp,
                                    BBMedication* meds, int maxMeds) {
    feedMethod[0] = '\0';
    *lastTemp = 0.0f;
    int medCount = 0;

    struct { const char* base; int tag; } queries[] = {
        { "/api/feedings/?limit=1",   0 },
        { "/api/temperature/?limit=1",1 },
        { "/api/medication/?limit=5", 2 },
    };

    bool isHttps = _isHttps(_baseUrl);
    WiFiClientSecure secureClient;
    if (isHttps) secureClient.setInsecure();
    HTTPClient http;
    http.setReuse(true);

    for (int qi = 0; qi < 3; qi++) {
        char path[80];
        if (childId > 0)
            snprintf(path, sizeof(path), "%s&child=%d", queries[qi].base, childId);
        else
            strncpy(path, queries[qi].base, sizeof(path) - 1);

        char url[192];
        snprintf(url, sizeof(url), "%s%s", _baseUrl, path);
        bool begun = isHttps ? http.begin(secureClient, url) : http.begin(url);
        if (!begun) continue;
        http.addHeader("Authorization", _authHeader);
        int code = http.GET();
        String body = (code > 0) ? http.getString() : "";
        http.end();
        if (code != 200 || body.isEmpty()) continue;

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) continue;
        JsonArray results = doc["results"].as<JsonArray>();
        if (results.size() == 0) continue;

        if (queries[qi].tag == 0) {  // last feeding → method abbreviation
            const char* method = results[0]["method"] | "";
            const char* type   = results[0]["type"]   | "";
            if      (strcmp(method, "left breast")  == 0) strncpy(feedMethod, "bl", feedLen);
            else if (strcmp(method, "right breast") == 0) strncpy(feedMethod, "br", feedLen);
            else if (strcmp(method, "both breasts") == 0) strncpy(feedMethod, "bo", feedLen);
            else if (strcmp(method, "bottle") == 0)
                strncpy(feedMethod, strcmp(type, "formula") == 0 ? "f" : "p", feedLen);

        } else if (queries[qi].tag == 1) {  // last temperature
            float t = results[0]["temperature"] | 0.0f;
            if (t > 0.0f) *lastTemp = t;

        } else {  // recent medications — collect unique names
            for (JsonObject entry : results) {
                if (medCount >= maxMeds) break;
                const char* name = entry["name"] | "";
                if (!name[0]) continue;
                bool found = false;
                for (int i = 0; i < medCount; i++)
                    if (strcmp(meds[i].name, name) == 0) { found = true; break; }
                if (found) continue;
                strncpy(meds[medCount].name, name, sizeof(meds[0].name) - 1);
                meds[medCount].amount   = entry["dosage"] | 0.0f;
                strncpy(meds[medCount].unit, entry["dosage_unit"] | "", sizeof(meds[0].unit) - 1);
                meds[medCount].lastTime    = 0;
                meds[medCount].intervalSec = 0;

                const char* tStr = entry["time"] | "";
                if (tStr[0]) {
                    struct tm tmE = {};
                    if (sscanf(tStr, "%d-%d-%dT%d:%d:%d",
                               &tmE.tm_year, &tmE.tm_mon, &tmE.tm_mday,
                               &tmE.tm_hour, &tmE.tm_min, &tmE.tm_sec) == 6) {
                        tmE.tm_year -= 1900; tmE.tm_mon -= 1; tmE.tm_isdst = -1;
                        meds[medCount].lastTime = mktime(&tmE);
                    }
                }

                // next_dose_interval: string "HH:MM:SS" or null
                {
                    uint32_t iv = 0;
                    JsonVariant ivField = entry["next_dose_interval"];
                    if (!ivField.isNull()) {
                        if (ivField.is<const char*>()) {
                            const char* ivStr = ivField.as<const char*>();
                            if (ivStr && ivStr[0]) {
                                int d = 0, h = 0, m = 0, s = 0;
                                const char* comma = strstr(ivStr, ", ");
                                if (comma) {
                                    sscanf(ivStr, "%d day", &d);
                                    sscanf(comma + 2, "%d:%d:%d", &h, &m, &s);
                                } else {
                                    sscanf(ivStr, "%d:%d:%d", &h, &m, &s);
                                }
                                iv = (uint32_t)(d * 86400 + h * 3600 + m * 60 + s);
                            }
                        } else {
                            iv = (uint32_t)(ivField.as<float>());  // numeric seconds
                        }
                    }
                    meds[medCount].intervalSec = iv;
                }

                medCount++;
            }
        }
    }
    return medCount;
}

// ── Recent records for summary screen ────────────────────────────────────────

int BabyBuddyAPI::getRecentRecords(BBRecentRecord records[3], int childId) {
    struct { const char* base; const char* type; int icon; } queries[] = {
        { "/api/feedings/?limit=1",     "Feed",  0 },  // bottle
        { "/api/changes/?limit=1",      "Diap",  1 },  // diaper
        { "/api/sleep/?limit=1",        "Sleep", 2 },  // moon
        { "/api/tummy-times/?limit=1",  "Tummy", 3 },  // hourglass
        { "/api/pumping/?limit=1",      "Pump",  4 },  // droplet
        { "/api/medication/?limit=1",   "Meds",  5 },  // pill
        { "/api/temperature/?limit=1",  "Temp",  6 },  // thermometer
    };
    const int NQUERIES = 7;

    BBRecentRecord all[7];
    memset(all, 0, sizeof(all));
    int count = 0;

    // Single SSL client reused across all requests — only one handshake for all 7 GETs
    bool isHttps = _isHttps(_baseUrl);
    WiFiClientSecure secureClient;
    if (isHttps) secureClient.setInsecure();
    HTTPClient http;
    http.setReuse(true);

    for (int qi = 0; qi < NQUERIES; qi++) {
        char path[80];
        if (childId > 0)
            snprintf(path, sizeof(path), "%s&child=%d", queries[qi].base, childId);
        else
            strncpy(path, queries[qi].base, sizeof(path) - 1);

        char url[192];
        snprintf(url, sizeof(url), "%s%s", _baseUrl, path);
        bool begun = isHttps ? http.begin(secureClient, url) : http.begin(url);
        if (!begun) continue;
        http.addHeader("Authorization", _authHeader);
        http.addHeader("Content-Type", "application/json");
        int code = http.GET();
        String body = (code > 0) ? http.getString() : "";
        http.end();
        if (code != 200 || body.isEmpty()) continue;

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) continue;
        JsonArray results = doc["results"].as<JsonArray>();
        if (results.size() == 0) continue;

        JsonObject entry = results[0].as<JsonObject>();

        // Safety net: skip if the returned entry belongs to a different child
        if (childId > 0) {
            int entrychild = entry["child"] | -1;
            if (entrychild > 0 && entrychild != childId) continue;
        }

        BBRecentRecord& r = all[count];
        strncpy(r.type, queries[qi].type, sizeof(r.type) - 1);
        r.iconType = queries[qi].icon;
        const char* startStr = entry["start"] | entry["time"] | "";

        if (strlen(startStr) >= 16) {
            int h, mi;
            if (sscanf(startStr + 11, "%d:%d", &h, &mi) == 2)
                snprintf(r.timeStr, sizeof(r.timeStr), "%02d:%02d", h, mi);
            struct tm tmP = {};
            if (sscanf(startStr, "%d-%d-%dT%d:%d:%d",
                       &tmP.tm_year, &tmP.tm_mon, &tmP.tm_mday,
                       &tmP.tm_hour, &tmP.tm_min, &tmP.tm_sec) == 6) {
                tmP.tm_year -= 1900; tmP.tm_mon -= 1; tmP.tm_isdst = -1;
                r.timestamp = mktime(&tmP);
            }
        }

        if (strcmp(queries[qi].type, "Feed") == 0) {
            const char* method = entry["method"] | "?";
            const char* endStr = entry["end"] | startStr;
            struct tm tmS = {}, tmE = {};
            sscanf(startStr, "%d-%d-%dT%d:%d:%d",
                   &tmS.tm_year, &tmS.tm_mon, &tmS.tm_mday,
                   &tmS.tm_hour, &tmS.tm_min, &tmS.tm_sec);
            sscanf(endStr, "%d-%d-%dT%d:%d:%d",
                   &tmE.tm_year, &tmE.tm_mon, &tmE.tm_mday,
                   &tmE.tm_hour, &tmE.tm_min, &tmE.tm_sec);
            tmS.tm_year -= 1900; tmS.tm_mon -= 1;
            tmE.tm_year -= 1900; tmE.tm_mon -= 1;
            char dur[12];
            _formatDuration(mktime(&tmS), mktime(&tmE), dur, sizeof(dur));
            const char* mabbr = "?", *mname = "?";
            if      (strcmp(method, "left breast")  == 0) { mabbr = "L"; mname = "Left"; }
            else if (strcmp(method, "right breast") == 0) { mabbr = "R"; mname = "Right"; }
            else if (strcmp(method, "both breasts") == 0) { mabbr = "B"; mname = "Both"; }
            else if (strcmp(method, "bottle") == 0) {
                const char* feedType = entry["type"] | "";
                if (strcmp(feedType, "formula") == 0) { mabbr = "F"; mname = "Formula"; }
                else                                   { mabbr = "P"; mname = "Pumped"; }
            }
            strncpy(r.method, mabbr, sizeof(r.method) - 1);
            snprintf(r.detail, sizeof(r.detail), "%s  %s", dur, mname);

        } else if (strcmp(queries[qi].type, "Diap") == 0) {
            bool wet   = entry["wet"]   | false;
            bool solid = entry["solid"] | false;
            if      (wet && solid) { strncpy(r.detail, "Wet+Dirty", sizeof(r.detail)-1); strncpy(r.method, "W+P", sizeof(r.method)-1); }
            else if (wet)          { strncpy(r.detail, "Wet",       sizeof(r.detail)-1); strncpy(r.method, "W",   sizeof(r.method)-1); }
            else if (solid)        { strncpy(r.detail, "Dirty",     sizeof(r.detail)-1); strncpy(r.method, "P",   sizeof(r.method)-1); }
            else                   { strncpy(r.detail, "Dry",       sizeof(r.detail)-1); strncpy(r.method, "D",   sizeof(r.method)-1); }

        } else if (strcmp(queries[qi].type, "Pump") == 0) {
            float amount = entry["amount"] | 0.0f;
            const char* endStr = entry["end"] | startStr;
            struct tm tmS = {}, tmE = {};
            sscanf(startStr, "%d-%d-%dT%d:%d:%d",
                   &tmS.tm_year, &tmS.tm_mon, &tmS.tm_mday,
                   &tmS.tm_hour, &tmS.tm_min, &tmS.tm_sec);
            sscanf(endStr, "%d-%d-%dT%d:%d:%d",
                   &tmE.tm_year, &tmE.tm_mon, &tmE.tm_mday,
                   &tmE.tm_hour, &tmE.tm_min, &tmE.tm_sec);
            tmS.tm_year -= 1900; tmS.tm_mon -= 1;
            tmE.tm_year -= 1900; tmE.tm_mon -= 1;
            char dur[12];
            _formatDuration(mktime(&tmS), mktime(&tmE), dur, sizeof(dur));
            if (amount > 0.0f) {
                snprintf(r.detail,  sizeof(r.detail),  "%s  %.0fml", dur, amount);
                snprintf(r.method,  sizeof(r.method),  "%.0fml", amount);
            } else {
                strncpy(r.detail, dur, sizeof(r.detail) - 1);
                strncpy(r.method, dur, sizeof(r.method) - 1);
            }

        } else if (strcmp(queries[qi].type, "Meds") == 0) {
            const char* name = entry["name"] | "";
            float dosage = entry["dosage"] | 0.0f;
            const char* unit = entry["dosage_unit"] | "";
            if (dosage > 0.0f && unit[0])
                snprintf(r.detail, sizeof(r.detail), "%s %.1f%s", name, dosage, unit);
            else
                strncpy(r.detail, name, sizeof(r.detail) - 1);
            strncpy(r.method, name, sizeof(r.method) - 1);  // up to 7 chars of med name

        } else if (strcmp(queries[qi].type, "Temp") == 0) {
            float temp = entry["temperature"] | 0.0f;
            if (temp > 0.0f) {
                snprintf(r.method, sizeof(r.method), "%.1f", temp);
                snprintf(r.detail, sizeof(r.detail), "%.1f", temp);
            }

        } else {
            // Sleep, Tummy — show duration
            const char* endStr = entry["end"] | startStr;
            struct tm tmS = {}, tmE = {};
            sscanf(startStr, "%d-%d-%dT%d:%d:%d",
                   &tmS.tm_year, &tmS.tm_mon, &tmS.tm_mday,
                   &tmS.tm_hour, &tmS.tm_min, &tmS.tm_sec);
            sscanf(endStr, "%d-%d-%dT%d:%d:%d",
                   &tmE.tm_year, &tmE.tm_mon, &tmE.tm_mday,
                   &tmE.tm_hour, &tmE.tm_min, &tmE.tm_sec);
            tmS.tm_year -= 1900; tmS.tm_mon -= 1;
            tmE.tm_year -= 1900; tmE.tm_mon -= 1;
            _formatDuration(mktime(&tmS), mktime(&tmE), r.detail, sizeof(r.detail));
            strncpy(r.method, r.detail, sizeof(r.method) - 1);  // same compact string
        }

        count++;
    }

    // Insertion sort by timestamp descending — most recent first
    for (int i = 1; i < count; i++) {
        BBRecentRecord key = all[i];
        int j = i - 1;
        while (j >= 0 && all[j].timestamp < key.timestamp) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = key;
    }

    int outCount = count < 3 ? count : 3;
    for (int i = 0; i < outCount; i++) records[i] = all[i];
    return outCount;
}

// ── Offline queue (NVS indexed keys — avoids JSON-in-JSON complexity) ─────────
// Keys: "count" (int), "m0","p0","b0", "m1","p1","b1", ...

void BabyBuddyAPI::enqueueOffline(const char* method, const char* endpoint,
                                   const char* body) {
    Preferences p;
    p.begin(NVS_QUEUE_NS, false);
    int n = p.getInt("count", 0);
    if (n >= OFFLINE_MAX) { p.end(); return; }

    char km[5], kp[5], kb[5];
    snprintf(km, sizeof(km), "m%d", n);
    snprintf(kp, sizeof(kp), "p%d", n);
    snprintf(kb, sizeof(kb), "b%d", n);
    p.putString(km, method);
    p.putString(kp, endpoint);
    p.putString(kb, body);
    p.putInt("count", n + 1);
    p.end();
}

int BabyBuddyAPI::offlineCount() {
    Preferences p;
    p.begin(NVS_QUEUE_NS, true);
    int n = p.getInt("count", 0);
    p.end();
    return n;
}

int BabyBuddyAPI::replayOffline() {
    Preferences p;
    p.begin(NVS_QUEUE_NS, false);
    int n = p.getInt("count", 0);

    // Load all events first
    struct Event { char m[8]; char path[80]; char body[256]; };
    int replayed = 0;
    int remaining = 0;
    Event* events = new Event[n];

    for (int i = 0; i < n; i++) {
        char km[5], kp[5], kb[5];
        snprintf(km,sizeof(km),"m%d",i);
        snprintf(kp,sizeof(kp),"p%d",i);
        snprintf(kb,sizeof(kb),"b%d",i);
        p.getString(km, events[i].m,    sizeof(events[i].m));
        p.getString(kp, events[i].path, sizeof(events[i].path));
        p.getString(kb, events[i].body, sizeof(events[i].body));
    }
    p.clear();

    // Replay each event; failed ones go back into NVS
    for (int i = 0; i < n; i++) {
        BBResult r;
        if (strcmp(events[i].m, "DELETE") == 0) r = _del(events[i].path);
        else                                     r = _post(events[i].path, events[i].body);

        if (r.ok) {
            replayed++;
        } else {
            // Re-enqueue failed event
            char km[5], kp[5], kb[5];
            snprintf(km,sizeof(km),"m%d",remaining);
            snprintf(kp,sizeof(kp),"p%d",remaining);
            snprintf(kb,sizeof(kb),"b%d",remaining);
            p.putString(km, events[i].m);
            p.putString(kp, events[i].path);
            p.putString(kb, events[i].body);
            remaining++;
        }
    }
    p.putInt("count", remaining);
    p.end();
    delete[] events;
    return replayed;
}
