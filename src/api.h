#pragma once
#include <Arduino.h>
#include <time.h>

struct BBChild {
    int  id;
    char name[32];
};

struct BBTimer {
    int    id;        // -1 = none
    time_t start;
    char   name[32];
};

struct BBResult {
    bool   ok;
    int    httpCode;
    char   error[64];
};

struct BBRecentRecord {
    char   type[12];      // "Feed","Diap","Sleep","Tummy","Pump","Meds","Temp"
    int    iconType;      // 0=bottle,1=diaper,2=moon,3=hourglass,4=droplet,5=pill,6=thermometer
    char   timeStr[8];    // "HH:MM"
    time_t timestamp;     // epoch of the record start (0 if unavailable)
    char   method[8];     // feed: "L","R","B","F","P"; temp: "38.2"; empty otherwise
    char   detail[32];
};

class BabyBuddyAPI {
public:
    void begin(const char* baseUrl, const char* token);

    // Returns cached child ID; fetches via API if not cached
    int getChildId();

    // Fetches and caches the child's first name (call after getChildId)
    void fetchChildName(int childId);
    const char* getChildName() const { return _childName; }

    // Fetches all children from API (returns count, up to maxCount)
    int fetchChildren(BBChild* out, int maxCount);

    // Persists the chosen child to NVS and updates internal name cache
    void setActiveChild(int id, const char* name);

    // Timer management
    BBTimer getActiveTimer(const char* name);
    BBResult startTimer(const char* name, int childId, BBTimer& out);
    BBResult stopTimer(int timerId);

    // Activity logging
    BBResult logFeeding(int child, time_t start, time_t end, const char* method);
    BBResult logDiaper(int child, time_t when, bool wet, bool solid);
    BBResult logSleep(int child, time_t start, time_t end);
    BBResult logTummyTime(int child, time_t start, time_t end);
    BBResult logPumping(int child, time_t start, time_t end, float amountMl);
    BBResult logMedication(int child, time_t when, const char* name,
                           float amount, const char* unit);

    // Fetch the 3 most recent records for a child across all activity types; returns count (0-3)
    int getRecentRecords(BBRecentRecord records[3], int childId);

    // Offline queue stored in NVS
    void enqueueOffline(const char* method, const char* endpoint, const char* body);
    int  offlineCount();
    int  replayOffline();   // returns count successfully replayed

private:
    char _baseUrl[128];
    char _token[64];
    char _authHeader[80];  // "Token <token>"
    char _childName[32];

    // HTTP helpers
    BBResult _post(const char* path, const char* body);
    BBResult _del(const char* path);
    String   _get(const char* path, int& httpCode);

    // Timestamp: "YYYY-MM-DDTHH:MM:SS"
    void _isoTime(time_t t, char* buf, size_t len);

    // Format elapsed seconds as "Xh Ym" or "Zm" for display
    void _formatDuration(time_t start, time_t end, char* buf, size_t len);
};

extern BabyBuddyAPI api;
