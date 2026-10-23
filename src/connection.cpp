#include "connection.h"
#include "logging.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>

// ======================================================
// VARIABLES (defined in main.cpp)
// ======================================================
extern WebServer server;

extern const char *ssid;
extern const char *password;

extern const char *ntpServer;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

// Local dashboard access point. Override from build_flags, e.g.
//   -DHIVE_AP_SSID=\"Hive-A1\" -DHIVE_AP_PASS=\"a-long-random-pass\"
// (WPA2 needs >= 8 characters.) CHANGE THE DEFAULT PASSWORD before deployment.
#ifndef HIVE_AP_SSID
#define HIVE_AP_SSID "BeehiveMonitor_2"
#endif
#ifndef HIVE_AP_PASS
#define HIVE_AP_PASS "12345678"
#endif

#define STA_CONNECT_TIMEOUT_MS 15000UL
// No valid clock yet. NTP answers within ~2-5 s when it works at all; when it does not,
// waiting longer only keeps the radio on (boot took 37 s). main.cpp falls back to the
// HTTPS Date header (~4 s), which has worked every time so far.
#define NTP_FIRST_SYNC_TIMEOUT_MS 8000UL
#define NTP_RESYNC_TIMEOUT_MS 5000UL      // clock already valid: don't hold the radio on

static const time_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01

static inline void wdtPet() { esp_task_wdt_reset(); } // harmless if this task is not subscribed

// ======================================================
// STARTING AP
// ======================================================
void startAP()
{
    // Never write WiFi settings to flash: this firmware connects on every upload window
    // and would otherwise wear the NVS partition.
    WiFi.persistent(false);
    // AP+STA from the start and NEVER switched again: tearing the station interface down and
    // up on every upload window fragments the heap (largest free block fell from 110 kB to
    // 40-60 kB) and kicks dashboard clients. The idle station costs nothing.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);   // we connect ourselves, only inside upload windows

    WiFi.softAP(HIVE_AP_SSID, HIVE_AP_PASS);

    logPrintf(LV_INFO, "Dashboard IP: %s", WiFi.softAPIP().toString().c_str());
}

// ======================================================
// CONNECTING TO STA
// ======================================================
static char failReason[32] = "";
const char *staLastFailReason() { return failReason; }

bool connectSTA()
{
    logPrintf(LV_INFO, "Connecting to \"%s\"...", ssid);

    WiFi.persistent(false);
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);   // full power to join; main.cpp lowers it on a strong link
    failReason[0] = 0;
    WiFi.begin(ssid, password);

    const unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        server.handleClient();
        wdtPet();
        yield();

        if (millis() - start > STA_CONNECT_TIMEOUT_MS)
        {
            Serial.println();
            switch (WiFi.status())
            {
            case WL_NO_SSID_AVAIL: strlcpy(failReason, "network not found", sizeof(failReason)); break;
            case WL_CONNECT_FAILED: strlcpy(failReason, "wrong password or rejected", sizeof(failReason)); break;
            default:               strlcpy(failReason, "no response (timeout)", sizeof(failReason)); break;
            }
            logPrintf(LV_WARN, "STA connection failed: %s", failReason);
            // Stop connecting, but leave the station interface initialised (see startAP()).
            WiFi.disconnect(false);
            return false;
        }

        delay(250);
        Serial.print(".");   // progress dots: Serial only, not worth a dashboard line each
    }

    Serial.println();
    logPrintf(LV_OK, "STA connected, IP: %s", WiFi.localIP().toString().c_str());

    return true;
}

// ======================================================
// DISCONNECTING FROM STA
// ======================================================
void disconnectSTA()
{
    // Drops the association only. The station interface and the SoftAP stay up, so dashboard
    // clients are not kicked and the heap is not churned on every upload window.
    WiFi.disconnect(false);

    logMsg(LV_INFO, "STA disconnected");
}

// ======================================================
// TIME SETUP
// ======================================================
static volatile bool ntpCompleted = false;
static void onNtpSync(struct timeval *) { ntpCompleted = true; }

// Bounded. It used to loop forever when NTP was unreachable (router online but no
// internet), which ended in a watchdog reset loop. The caller checks the clock
// afterwards and simply retries in the next upload window.
void setupTime()
{
    logMsg(LV_INFO, "[TIME]");

    ntpCompleted = false;
    sntp_set_time_sync_notification_cb(onNtpSync);
    // Three servers: one blocked or unresolvable name no longer means "no time".
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.google.com", "time.cloudflare.com");

    const bool hadClock = time(nullptr) > MIN_VALID_EPOCH;
    const unsigned long limit = hadClock ? NTP_RESYNC_TIMEOUT_MS : NTP_FIRST_SYNC_TIMEOUT_MS;
    const unsigned long start = millis();

    while (!ntpCompleted && millis() - start < limit)
    {
        server.handleClient();
        wdtPet();
        yield();
        delay(100);
    }

    if (ntpCompleted)                         logMsg(LV_OK,   "Time synchronized successfully");
    else if (time(nullptr) > MIN_VALID_EPOCH) logMsg(LV_WARN, "NTP did not answer; keeping the current clock");
    else                                      logMsg(LV_ERR,  "NTP failed; clock not set");
}

// ======================================================
// GET TIMESTAMP
// ======================================================
// Uses time() instead of getLocalTime(): getLocalTime() blocks for up to 5 s per
// call while the clock is unset, which stalled the main loop and every error log.
String getTimestamp()
{
    const time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return "NO_TIME";

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d_%02d%02d%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    return String(buffer);
}
