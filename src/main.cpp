// ============================================================================
//  Hive monitor firmware  -  ESP32  -  AdEMNEA / AWS data delivery
//  v3.2.0  (DHT22 x2 + MH-Z19 UART + DS3231 RTC + INA219 x2 + I2S mic + SD)
//
//  Logging: every message goes to Serial AND to a RAM ring buffer that the dashboard
//  shows live (logging.cpp, GET /logs), coloured by level (red error, green success).
//
//  Data path:
//    sensors -> POST <gateway>/ingest      (202 = queued, not "stored")
//    health  -> POST <gateway>/heartbeat   (202)  [mac, battery voltage/current, chip]
//    media   -> POST <ademnea>/api/iot/media/upload-url (200)
//            -> PUT  <signed S3 url>       (binary, 200/204)
//            -> POST <gateway>/media/confirm (202)
//
//  Reliability rules:
//   * Every reading goes to SD first and is deleted only after the gateway
//     answered 202. recorded_at is fixed at sample time and reused on retry.
//   * Missing sensor values are NEVER invented. A failed DHT/CO2 read is an
//     empty CSV field and that zone/message is simply not sent.
//   * Media queue (/media_q.txt) is persisted after EVERY step (S3 key after
//     the PUT, line removal after confirm), so a reset cannot cause a
//     duplicate upload. Recordings are written to /rec.tmp and renamed only
//     when complete, so partial files are never queued.
//   * Task watchdog, reset-reason reporting, heap guard, exponential backoff
//     with jitter, SD auto-remount, bounded queues (drop-oldest for media).
//   * HTTPS only. Credentials come from secrets.h (see bottom of this file).
//
//  Build flags (platformio.ini):
//     -DHIVE_DEBUG=0 -DCORE_DEBUG_LEVEL=0
//     -DHIVE_AP_SSID=\"Hive-A1\" -DHIVE_AP_PASS=\"long-random-pass\"   (connection.cpp)
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include <functional>
#include <driver/i2s.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_freertos_hooks.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif
#include <WebServer.h>
#include <Adafruit_INA219.h>
#include <DHTesp.h>
#include <RTClib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "dashboard.h"
#include "settings.h"
#include "connection.h"
#include "logging.h"   // declares extern gmtOffset_sec / daylightOffset_sec BEFORE they are defined below
#include "hive_util.h"
#include "secrets.h"

// ===== DEBUG SWITCH (OFF by default for production) ==========================
#ifndef HIVE_DEBUG
#define HIVE_DEBUG 0
#endif
#if HIVE_DEBUG
#define DBG(...) logPrintf(LV_DBG, __VA_ARGS__)
#define DBG_T0(v) const uint32_t v = millis()
#else
#define DBG(...) do { } while (0)
#define DBG_T0(v) do { } while (0)
#endif

#ifndef FW_VERSION
#if HIVE_DEBUG
#define FW_VERSION "3.2.0-dbg"
#else
#define FW_VERSION "3.2.0"
#endif
#endif

#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(24 * 1024);
#endif

Preferences prefs;

// ============================================================================
// CONFIGURATION
// ============================================================================
#ifndef GATEWAY_BASE_URL
#define GATEWAY_BASE_URL "https://nyw6l52jgf.execute-api.eu-north-1.amazonaws.com/prod"
#endif
#if defined(DEVICE_API_KEY)
#define HIVE_GATEWAY_KEY DEVICE_API_KEY
#else
#define HIVE_GATEWAY_KEY AWS_API_KEY_VALUE
#endif
#if defined(ADEMNEA_API_KEY)
#define HIVE_ADEMNEA_KEY ADEMNEA_API_KEY
#else
#define HIVE_ADEMNEA_KEY HIVE_GATEWAY_KEY
#endif

const char *ssid     = WIFI_SSID;
const char *password = WIFI_PASS;

static const char *AWS_API_BASE   = GATEWAY_BASE_URL;
static const char *ADEMNEA_BASE   = ADEMNEA_BASE_URL;
static const char *GATEWAY_KEY    = HIVE_GATEWAY_KEY;
static const char *ADEMNEA_KEY    = HIVE_ADEMNEA_KEY;
static const char *DEVICE_ID      = DEVICE_ID_VALUE;

#define HIVE_ZONE_1 "brood_section"   // DHT1
#define HIVE_ZONE_2 "honey_section"   // DHT2

String hiveNo = "1";

// ---- default schedule -------------------------------------------------------
#define DEFAULT_SENSOR_S   60
#define DEFAULT_UPLOAD_S   300
#define DEFAULT_RECORD_S   900

#define SENSOR_MIN_S 10
#define SENSOR_MAX_S 3600
#define RECORD_MIN_S 30
#define RECORD_MAX_S 86400
#define UPLOAD_MIN_S 30
#define UPLOAD_MAX_S 86400

// ---- delivery behaviour -----------------------------------------------------
#define HEARTBEAT_INTERVAL_MS  300000UL
#define UPLOAD_BUDGET_MS       60000UL
#define BACKLOG_RETRY_MS       15000UL
#define BACKOFF_MAX_MS         3600000UL
#define MAX_MEDIA_PER_WINDOW   2
#define MEDIA_MAX_ATTEMPTS     8
#define SD_AUDIO_DROP_PCT      90.0f   // audio is only dropped (oldest first) above this SD usage
#define FAILED_MEDIA_MAX       20
#define REJECTED_CSV_MAX_BYTES 200000UL
#define NTP_RESYNC_MS          43200000UL
#define NTP_FAIL_RETRY_MS      3600000UL
#define MIN_TLS_HEAP           45000
#define LOW_HEAP_RESTARTS      3
#define SD_RETRY_MS            60000UL
#define HEALTH_PRINT_MS        60000UL
#define HOUSEKEEP_MS           86400000UL

#define RTC_CORRECT_THRESHOLD_S 5
static const time_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01

// ---- power ------------------------------------------------------------------
#define BATT_CELLS         3
#define BATT_LOW_CUTOFF_V  10.2f   // pause audio + media uploads
#define BATT_LOW_RESUME_V  11.1f
#define BATT_HARD_CUTOFF_V 9.9f    // deep sleep (3.3 V/cell) until recovered
#define BATT_HARD_SLEEP_S  3600

// ---- misc -------------------------------------------------------------------
#define WDT_TIMEOUT_S  120
#define AWAKE_GRACE_MS 30000UL
#define CO2_WARMUP_MS  180000UL
#define GATEWAY_CONNECT_TIMEOUT_MS 8000
#define GATEWAY_IO_TIMEOUT_MS      15000
#define GATEWAY_RETRIES_PER_WINDOW 3
#define SD_USAGE_REFRESH_MS        21600000UL
#define MIC_SETTLE_SAMPLES         2400
#define MIC_PEAK_SAMPLES           1600

// ============================================================================
// TLS TRUST ANCHORS
// ============================================================================
static const char AWS_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)PEM";

static const char ADEMNEA_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

// ============================================================================
// PINS / HARDWARE
// ============================================================================
#define I2C_SDA      21
#define I2C_SCL      22
#define DHT_PIN_1    4
#define DHT_PIN_2    15
#define LED_PIN      32          // also the mic power/indicator line
#define CO2_RX_PIN   16
#define CO2_TX_PIN   17

#define SD_CS_PIN    5
#define SD_MOSI_PIN  23
#define SD_MISO_PIN  19
#define SD_SCK_PIN   18

#define I2S_BCK_PIN   27
#define I2S_WS_PIN    26
#define I2S_DATA_PIN  25

#define SAMPLE_RATE      16000
#define RECORD_TIME_SEC  10

const char *ntpServer = "pool.ntp.org";
const long  gmtOffset_sec      = 3 * 3600;  // EAT (UTC+3), no DST in Uganda
const int   daylightOffset_sec = 0;
#define LOCAL_OFFSET_SEC ((long)(gmtOffset_sec + daylightOffset_sec))

// ============================================================================
// STATE / GLOBAL OBJECTS
// ============================================================================
DHTesp          dht1, dht2;
Adafruit_INA219 batterySensor(0x40);
Adafruit_INA219 solarSensor(0x41);
RTC_DS3231      rtc;
SPIClass        spiSD(VSPI);
WebServer       server(80);

unsigned long sensorInterval = DEFAULT_SENSOR_S * 1000UL;   // ms
unsigned long recordInterval = DEFAULT_RECORD_S;            // SECONDS
unsigned long uploadInterval = DEFAULT_UPLOAD_S * 1000UL;   // ms
bool          fullDutyCycle  = false;

static unsigned long nextSensorAt = 0, nextRecordAt = 0, nextUploadAt = 0;
static unsigned long lastHeartbeatTime = 0, lastNtpSyncTime = 0, ntpNextAttempt = 0;
static unsigned long lastSdRetryTime = 0, lastHealthPrint = 0, lastHousekeeping = 0;
static unsigned long awakeSince = 0, co2ReadyAt = 0;
static bool          heartbeatEverSent = false;
static bool          everSynced = false;
static uint8_t       hbLevel = 2;   // heartbeat payload: 2 = mac + voltage/current/chip, 1 = mac only, 0 = documented fields only
static bool          wokeFromSleep = false;

// dashboard snapshot (display only; the CSV gets raw values)
static float    dispT1 = NAN, dispH1 = NAN, dispT2 = NAN, dispH2 = NAN;
static int      dispCo2 = -1;
static float    batteryVoltage = 0, battCurrentA = 0, solarPowerW = 0;
static bool     battSampled = false;   // battCurrentA holds a real reading
static long     audioPeak = 0;
static bool     solarPresent = false, mic_ok = false, sd_ok = false, rtc_ok = false;

static float lastBatteryVoltage = 0.0f;

static uint8_t uploadFailStreak = 0;
RTC_DATA_ATTR static bool    rtcBattHardStop = false;

static bool     uploadBacklog = false;
static uint8_t  lowHeapStreak = 0;
static bool     battLow       = false;

static uint32_t rebootCount = 0;
static uint32_t sensorReadOk = 0, sensorReadTotal = 0;
static const char *bootResetCode = nullptr;
static char deviceMac[18] = "00:00:00:00:00:00";

void normalLoop();
void dutyCycleLoop();

static inline bool reached(unsigned long t) { return (long)(millis() - t) >= 0; }

// ---- CPU meter (idle-hook based, core running loop()) -----------------------
static volatile uint32_t idleHookCount = 0;
static uint32_t idleBaselinePerMs = 0;
static uint64_t cpuWinStartUs = 0;
static uint32_t cpuWinStartCount = 0;
static bool idleHook() { idleHookCount++; return false; }

static void cpuMeterBegin()
{
    esp_register_freertos_idle_hook_for_cpu(idleHook, xPortGetCoreID());
    const uint32_t c0 = idleHookCount;
    delay(200);
    idleBaselinePerMs = (uint32_t)(idleHookCount - c0) / 200;
    cpuWinStartUs = esp_timer_get_time();
    cpuWinStartCount = idleHookCount;
}

static float cpuUsagePct()
{
    if (idleBaselinePerMs < 1) return 0;
    const uint64_t wallUs = esp_timer_get_time() - cpuWinStartUs;
    if (wallUs < 1000000ULL) return 0;
    const float expected = (float)idleBaselinePerMs * ((float)wallUs / 1000.0f);
    const float idle = (float)(uint32_t)(idleHookCount - cpuWinStartCount);
    float u = 100.0f * (1.0f - idle / expected);
    if (u < 0) u = 0;
    if (u > 100) u = 100;
    return u;
}

// ============================================================================
// ERROR FLAGS -> heartbeat "error_codes"
// ============================================================================
enum ErrFlag : uint32_t
{
    ERR_DHT1 = 1u << 0, ERR_DHT2 = 1u << 1, ERR_BATT = 1u << 2, ERR_SD = 1u << 3,
    ERR_MIC = 1u << 4, ERR_CLOCK = 1u << 5, ERR_AWS_UPLOAD = 1u << 6, ERR_MEDIA = 1u << 7,
    ERR_AUTH = 1u << 8, ERR_HEAP_LOW = 1u << 9, ERR_BATT_LOW = 1u << 10, ERR_MEDIA_FULL = 1u << 11,
    ERR_RESET = 1u << 12, ERR_CO2 = 1u << 13, ERR_RTC = 1u << 14,
};
static uint32_t errFlags = 0;

static const struct { uint32_t f; const char *name; } ERR_MAP[] = {
    {ERR_DHT1, "DHT1_READ"},       {ERR_DHT2, "DHT2_READ"},   {ERR_BATT, "BATT_SENSOR"},
    {ERR_SD, "SD_FAIL"},           {ERR_MIC, "MIC_FAIL"},     {ERR_CLOCK, "CLOCK_UNSYNCED"},
    {ERR_AWS_UPLOAD, "AWS_UPLOAD"},{ERR_MEDIA, "MEDIA_UPLOAD"},{ERR_AUTH, "AUTH_REJECTED"},
    {ERR_HEAP_LOW, "HEAP_LOW"},    {ERR_BATT_LOW, "BATT_LOW"},{ERR_MEDIA_FULL, "SD_NEARLY_FULL"},
    {ERR_CO2, "CO2_READ"},         {ERR_RTC, "RTC_FAIL"},
};

static void setErr(uint32_t f, bool on)
{
#if HIVE_DEBUG
    if (((errFlags & f) != 0) != on)
        for (auto &m : ERR_MAP) if (m.f == f) { DBG("ERRFLAG %s -> %s", m.name, on ? "SET" : "cleared"); break; }
#endif
    if (on) errFlags |= f; else errFlags &= ~f;
}

static void buildErrorCodes(char *out, size_t n)
{
    out[0] = 0;
    size_t len = 0;
    auto add = [&](const char *name) {
        int w = snprintf(out + len, n - len, "%s\"%s\"", len ? "," : "", name);
        if (w > 0 && (size_t)w < n - len) len += w;
    };
    for (auto &m : ERR_MAP) if (errFlags & m.f) add(m.name);
    if ((errFlags & ERR_RESET) && bootResetCode) add(bootResetCode);
}

// Rate-limited logger for problems: the same message is logged at most once per 10 min.
// Shown red (LV_ERR) or amber (LV_WARN) on the dashboard.
static void noteError(const String &msg, uint8_t level = LV_ERR)
{
    static const unsigned long QUIET_MS = 600000UL;
    static struct { uint32_t h; unsigned long at; bool used; } seen[8];
    static uint8_t next = 0;

    uint32_t h = 2166136261u;
    for (size_t i = 0; i < msg.length(); i++) { h ^= (uint8_t)msg[i]; h *= 16777619u; }

    const unsigned long now = millis();
    for (auto &e : seen)
        if (e.used && e.h == h)
        {
            if (now - e.at < QUIET_MS) return;
            e.at = now; logMsg(level, msg.c_str()); return;
        }
    seen[next] = {h, now, true};
    next = (next + 1) % 8;
    logMsg(level, msg.c_str());
}

// ============================================================================
// WATCHDOG / HEAP / POWER / IDENTITY
// ============================================================================
static void wdtBegin()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic = true;
    esp_task_wdt_deinit();
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);
}
static inline void wdtFeed() { esp_task_wdt_reset(); }
static bool heapOkForTls() { return ESP.getMaxAllocHeap() >= MIN_TLS_HEAP; }
static void idleDelay(uint32_t ms) { delay(ms); }

// Factory-programmed station MAC (the one printed on most ESP32 labels).
static void readDeviceMac()
{
    uint8_t m[6] = {0};
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(deviceMac, sizeof(deviceMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void classifyReset(esp_reset_reason_t rr)
{
    switch (rr)
    {
    case ESP_RST_PANIC:    bootResetCode = "RESET_PANIC";    break;
    case ESP_RST_INT_WDT:  bootResetCode = "RESET_INT_WDT";  break;
    case ESP_RST_TASK_WDT: bootResetCode = "RESET_TASK_WDT"; break;
    case ESP_RST_WDT:      bootResetCode = "RESET_WDT";      break;
    case ESP_RST_BROWNOUT: bootResetCode = "RESET_BROWNOUT"; break;
    default:               bootResetCode = nullptr;          break;
    }
    if (bootResetCode) setErr(ERR_RESET, true);
}

static void updateBatteryGuard()
{
    if (lastBatteryVoltage > 0)
    {
        if (!battLow && lastBatteryVoltage < BATT_LOW_CUTOFF_V)
        {
            battLow = true;
            noteError("Battery low: audio and media uploads paused", LV_WARN);
        }
        else if (battLow && lastBatteryVoltage > BATT_LOW_RESUME_V)
        {
            battLow = false;
        }
    }
    setErr(ERR_BATT_LOW, battLow);
}

static void powerOffMicrophone();

static void goToSleep(uint32_t sec)
{
    logPrintf(LV_WARN, "Deep sleep for %lu s", (unsigned long)sec);
    Serial.flush();
    powerOffMicrophone();
    if (sd_ok) SD.end();
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
    esp_deep_sleep_start();
}

// ============================================================================
// DEVICE IDENTITY / WIFI
// ============================================================================
static void logDeviceIdentity()
{
    logMsg(LV_INFO, "---- DEVICE IDENTITY ----");
    logPrintf(LV_INFO, "  device_id : %s", DEVICE_ID);
    logPrintf(LV_INFO, "  hive_no   : %s", hiveNo.c_str());
    logPrintf(LV_INFO, "  STA MAC   : %s", deviceMac);
    logPrintf(LV_INFO, "  AP  MAC   : %s", WiFi.softAPmacAddress().c_str());
    logPrintf(LV_INFO, "  chip      : %s rev %d", ESP.getChipModel(), (int)ESP.getChipRevision());
    logPrintf(LV_INFO, "  firmware  : %s", FW_VERSION);
    logMsg(LV_INFO, "-------------------------");
}

// Connect STA unless it is already up.
static bool staEnsure()
{
    if (WiFi.status() == WL_CONNECTED) return true;
    return connectSTA();   // connection.cpp, bounded
}

// ============================================================================
// TIME  (DS3231 is the local clock; NTP corrects it)
// ============================================================================
static bool clockSynced() { return time(nullptr) > MIN_VALID_EPOCH; }

static bool getUtcIso(char *out /*>=21*/)
{
    if (!clockSynced()) return false;
    hu::epochToIso((int64_t)time(nullptr), out);
    return true;
}

static void rtcWriteFromSystem()
{
    if (!rtc_ok) return;
    const time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return;
    struct tm tmv;
    gmtime_r(&now, &tmv);
    rtc.adjust(DateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec));
    DBG("RTC: written from system clock");
}

// RTC holds UTC. Loaded into the system clock at boot so the CSV has real timestamps without WiFi.
static bool rtcLoadSystem()
{
    if (!rtc_ok) return false;
    if (rtc.lostPower()) { DBG("RTC: lost-power flag set; ignoring stored time"); return false; }
    DateTime now = rtc.now();
    int64_t epoch = 0;
    if (!hu::civilToEpoch(now.year(), now.month(), now.day(),
                          now.hour(), now.minute(), now.second(), epoch)) return false;
    if (epoch < MIN_VALID_EPOCH) return false;
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    return clockSynced();
}

static bool syncClockFromHttpDate(int minDeltaS = 0);
static void flushPendingReadings();

// Runs inside an upload window (WiFi up). The first sync happens at the first window even when
// the RTC is valid; a failed attempt is retried (1 h if a clock exists, next window otherwise)
// and is never counted as a success.
static void syncClockIfNeeded()
{
    if (!reached(ntpNextAttempt)) return;

    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    setupTime();   // connection.cpp, bounded
    bool ok = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
    if (!ok)
    {
        DBG("CLOCK: NTP gave no answer, trying the HTTPS Date header");
        ok = syncClockFromHttpDate(clockSynced() ? RTC_CORRECT_THRESHOLD_S : 0);
    }

    if (ok && clockSynced())
    {
        const int64_t nowEpoch = (int64_t)time(nullptr);
        if (rtc_ok)
        {
            DateTime rt = rtc.now();
            int64_t rtcEpoch = 0;
            hu::civilToEpoch(rt.year(), rt.month(), rt.day(), rt.hour(), rt.minute(), rt.second(), rtcEpoch);
            if (llabs((long long)(rtcEpoch - nowEpoch)) > RTC_CORRECT_THRESHOLD_S)
            {
                rtcWriteFromSystem();
                logPrintf(LV_INFO, "RTC corrected from NTP (delta %lld s)", (long long)(nowEpoch - rtcEpoch));
            }
        }
        everSynced = true;
        lastNtpSyncTime = millis();
        ntpNextAttempt = millis() + NTP_RESYNC_MS;
        flushPendingReadings();
    }
    else
    {
        ntpNextAttempt = millis() + (clockSynced() ? NTP_FAIL_RETRY_MS : 0);
        if (!clockSynced()) noteError("Clock: NTP and HTTPS fallback failed");
    }
    setErr(ERR_CLOCK, !clockSynced());
}

static bool parseHttpDate(const char *d, int64_t &epoch)
{
    char mon[4] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(d, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return false;
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(MONTHS, mon);
    if (!p || (p - MONTHS) % 3 != 0) return false;
    return hu::civilToEpoch(year, (int)((p - MONTHS) / 3) + 1, day, hh, mm, ss, epoch);
}

static bool syncClockFromHttpDate(int minDeltaS)
{
    if (!heapOkForTls()) return false;
    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    http.setConnectTimeout(GATEWAY_CONNECT_TIMEOUT_MS);
    http.setTimeout(8000);
    static const char *keys[] = {"Date"};
    http.collectHeaders(keys, 1);

    bool ok = false;
    if (http.begin(tls, String(AWS_API_BASE) + "/"))
    {
        const int code = http.GET();
        const String date = http.header("Date");
        int64_t epoch = 0;
        if (code > 0 && date.length() > 0 && parseHttpDate(date.c_str(), epoch) && epoch > MIN_VALID_EPOCH)
        {
            // With a valid clock, only step it when the server disagrees by more than minDeltaS
            // (the Date header has 1 s resolution; do not jitter a good RTC).
            const bool had = clockSynced();
            if (!had || llabs((long long)(epoch - (int64_t)time(nullptr))) > minDeltaS)
            {
                struct timeval tv;
                tv.tv_sec = (time_t)epoch; tv.tv_usec = 0;
                settimeofday(&tv, nullptr);
                if (rtc_ok) rtcWriteFromSystem();
            }
            ok = clockSynced();
        }
        http.end();
    }
    return ok;
}

// ============================================================================
// DASHBOARD SETTINGS
// ============================================================================
static unsigned long clampUL(long v, long lo, long hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (unsigned long)v;
}

static void putULongIfChanged(const char *k, unsigned long v)
{
    if (prefs.getULong(k, 0xFFFFFFFFUL) != v) prefs.putULong(k, v);   // protect NVS from wear
}

static void loadSettings()
{
    sensorInterval = clampUL(prefs.getULong("sensor", DEFAULT_SENSOR_S * 1000UL) / 1000, SENSOR_MIN_S, SENSOR_MAX_S) * 1000UL;
    recordInterval = clampUL(prefs.getULong("record", DEFAULT_RECORD_S), RECORD_MIN_S, RECORD_MAX_S);
    uploadInterval = clampUL(prefs.getULong("upload", DEFAULT_UPLOAD_S * 1000UL) / 1000, UPLOAD_MIN_S, UPLOAD_MAX_S) * 1000UL;
    fullDutyCycle  = prefs.getBool("fullDuty", false);
}

void handleRoot()         { server.send_P(200, "text/html", INDEX_HTML); }
void handleSettingsPage() { server.send_P(200, "text/html", SETTINGS_HTML); }

void handleSetDuty()
{
    if (server.hasArg("sensor"))
        sensorInterval = clampUL(server.arg("sensor").toInt(), SENSOR_MIN_S, SENSOR_MAX_S) * 1000UL;
    if (server.hasArg("record"))
        recordInterval = clampUL(server.arg("record").toInt(), RECORD_MIN_S, RECORD_MAX_S);
    if (server.hasArg("upload"))
        uploadInterval = clampUL(server.arg("upload").toInt(), UPLOAD_MIN_S, UPLOAD_MAX_S) * 1000UL;
    if (server.hasArg("fullDuty"))
        fullDutyCycle = server.arg("fullDuty").toInt() == 1;

    putULongIfChanged("sensor", sensorInterval);
    putULongIfChanged("record", recordInterval);
    putULongIfChanged("upload", uploadInterval);
    if (prefs.getBool("fullDuty", false) != fullDutyCycle) prefs.putBool("fullDuty", fullDutyCycle);

    nextSensorAt = millis() + sensorInterval;
    nextRecordAt = millis() + recordInterval * 1000UL;
    nextUploadAt = millis() + uploadInterval;
    logPrintf(LV_OK, "Settings updated from UI: sensor %lus  record %lus  upload %lus  mode %s",
              sensorInterval / 1000UL, recordInterval, uploadInterval / 1000UL,
              fullDutyCycle ? "duty-cycle" : "normal");
    server.send(200, "text/plain", "Settings saved");
}

void handleSettings()
{
    String json;
    json.reserve(160);
    json = "{\"sensor\":" + String(sensorInterval / 1000);
    json += ",\"record\":" + String(recordInterval);
    json += ",\"upload\":" + String(uploadInterval / 1000);
    json += ",\"fullDuty\":";
    json += fullDutyCycle ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

static String jnum(float v, unsigned int dp) { return isnan(v) ? String("null") : String(v, dp); }

void handleData()
{
    String j;
    j.reserve(420);
    j = "{\"temp\":" + jnum(dispT1, 1) + ",\"humidity\":" + jnum(dispH1, 1);
    j += ",\"temp2\":" + jnum(dispT2, 1) + ",\"humidity2\":" + jnum(dispH2, 1);
    j += ",\"co2\":" + (dispCo2 >= 0 ? String(dispCo2) : String("null"));
    j += ",\"battV\":" + String(batteryVoltage, 2);
    j += ",\"battA\":" + String(battCurrentA, 3);
    j += ",\"solW\":" + String(solarPowerW, 2);
    j += ",\"audio\":" + String(audioPeak);
    j += ",\"co2_ok\":";   j += (errFlags & ERR_CO2) ? "false" : "true";
    j += ",\"batt_ok\":";  j += (errFlags & ERR_BATT) ? "false" : "true";
    j += ",\"solar_ok\":"; j += solarPresent ? "true" : "false";
    j += ",\"mic_ok\":";   j += (errFlags & ERR_MIC) ? "false" : "true";   // mic is powered off between uses
    j += ",\"sd_ok\":";    j += sd_ok ? "true" : "false";
    j += ",\"cloud_ok\":"; j += (!(errFlags & (ERR_AWS_UPLOAD | ERR_AUTH)) && uploadFailStreak == 0) ? "true" : "false";
    j += ",\"mac\":\"" + String(deviceMac) + "\",\"fw\":\"" FW_VERSION "\"}";
    server.send(200, "application/json", j);
}

// ============================================================================
// SD CARD
// ============================================================================
static String csvPath() { return "/" + hiveNo + ".csv"; }
static const char *CSV_TMP      = "/upload.tmp";
static const char *REC_TMP      = "/rec.tmp";
static const char *REJECTED_CSV = "/rejected.csv";
static const char *MEDIA_Q      = "/media_q.txt";
static const char *MEDIA_Q_TMP  = "/media_q.tmp";
static const char *FAILED_DIR   = "/failed";

static void sdRemoveIfExists(const char *p) { if (SD.exists(p)) SD.remove(p); }

static void sdFault(const char *why)
{
    DBG("SD FAULT: %s", why);
    setErr(ERR_SD, true);
    sd_ok = false;
    noteError(String("SD fault: ") + why);
}

static void recoverTmp(const char *path, const char *tmp)
{
    const bool hasMain = SD.exists(path), hasTmp = SD.exists(tmp);
    if (!hasMain && hasTmp)     SD.rename(tmp, path);
    else if (hasMain && hasTmp) SD.remove(tmp);
}

static void ensureCSVExists()
{
    if (!sd_ok) return;
    String p = csvPath();
    if (SD.exists(p)) return;
    File f = SD.open(p, FILE_WRITE);
    if (!f) { sdFault("cannot create csv"); return; }
    f.close();
}

static int readLine(File &f, char *buf, size_t n)
{
    size_t len = 0;
    bool any = false, over = false;
    while (f.available())
    {
        int c = f.read();
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 < n) buf[len++] = (char)c;
        else over = true;
    }
    buf[len] = 0;
    if (!any) return -1;
    return over ? 0 : (int)len;
}

static void quarantineRow(const char *line)
{
    File f = SD.open(REJECTED_CSV, FILE_APPEND);
    if (!f) return;
    if (f.size() < REJECTED_CSV_MAX_BYTES) f.println(line);
    f.close();
}

// ---- readings --------------------------------------------------------------
struct Sample
{
    float t1 = NAN, h1 = NAN, t2 = NAN, h2 = NAN;
    int   co2 = -1;   // -1 = missing
    bool any() const { return !isnan(t1) || !isnan(h1) || !isnan(t2) || !isnan(h2) || co2 >= 0; }
};

// RAM buffer, only used when the clock is not set (RTC dead/lost power AND no NTP yet).
struct PendingReading { uint32_t atMs; Sample s; };
#define PENDING_MAX 360
static PendingReading pending[PENDING_MAX];
static uint16_t       pendingCount   = 0;
static uint32_t       pendingDropped = 0;

enum LogResult { LOG_FAILED = 0, LOG_SAVED = 1, LOG_BUFFERED = 2 };

// Row: local timestamp,t1,h1,t2,h2,co2   (an EMPTY field means "not measured")
static int formatCsvRow(char *row, size_t cap, int64_t epoch, const Sample &s)
{
    char stamp[24], a[4][12], c[8];
    hu::epochToLocalStamp(epoch, LOCAL_OFFSET_SEC, stamp);
    const float v[4] = {s.t1, s.h1, s.t2, s.h2};
    for (int i = 0; i < 4; i++)
    {
        if (isnan(v[i])) a[i][0] = 0; else snprintf(a[i], sizeof(a[i]), "%.2f", v[i]);
    }
    if (s.co2 < 0) c[0] = 0; else snprintf(c, sizeof(c), "%d", s.co2);
    return snprintf(row, cap, "%s,%s,%s,%s,%s,%s\n", stamp, a[0], a[1], a[2], a[3], c);
}

static bool optFloat(const char *s, float lo, float hi, float &out)
{
    if (!*s) { out = NAN; return true; }
    char *e;
    const float v = strtof(s, &e);
    if (e == s || isnan(v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

// Parses a CSV row (empty fields allowed). Fills the UTC ISO timestamp used as the dedupe key.
static bool parseCsvRow(char *line, long tzOffsetSec, char *isoOut, Sample &s)
{
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\r' || line[L - 1] == '\n' || line[L - 1] == ' ' || line[L - 1] == '\t')) line[--L] = 0;
    if (L < 19) return false;

    char *f[6];
    int n = 0;
    char *q = line;
    while (n < 6)
    {
        f[n++] = q;
        char *c = strchr(q, ',');
        if (!c) break;
        *c = 0;
        q = c + 1;
    }
    if (n < 6) return false;

    int y, mo, d, hh, mi, ss;
    if (sscanf(f[0], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &hh, &mi, &ss) != 6) return false;
    if (y < 2024) return false;
    int64_t epoch = 0;
    if (!hu::civilToEpoch(y, mo, d, hh, mi, ss, epoch)) return false;
    hu::epochToIso(epoch - tzOffsetSec, isoOut);

    if (!optFloat(f[1], -40, 85, s.t1)) return false;
    if (!optFloat(f[2], 0, 100, s.h1)) return false;
    if (!optFloat(f[3], -40, 85, s.t2)) return false;
    if (!optFloat(f[4], 0, 100, s.h2)) return false;
    if (!*f[5]) s.co2 = -1;
    else
    {
        char *e;
        long c = strtol(f[5], &e, 10);
        if (e == f[5] || c < 0 || c > 40000) return false;
        s.co2 = (int)c;
    }
    return s.any();
}

static void pendingAdd(const Sample &s)
{
    if (pendingCount >= PENDING_MAX)
    {
        memmove(&pending[0], &pending[1], (PENDING_MAX - 1) * sizeof(PendingReading));
        pendingCount = PENDING_MAX - 1;
        pendingDropped++;
    }
    pending[pendingCount].atMs = millis();
    pending[pendingCount].s = s;
    pendingCount++;
}

static void flushPendingReadings()
{
    if (pendingCount == 0 || !sd_ok || !clockSynced()) return;

    ensureCSVExists();
    File file = SD.open(csvPath(), FILE_APPEND);
    if (!file) { sdFault("cannot open csv (backfill)"); return; }

    const int64_t  now   = (int64_t)time(nullptr);
    const uint32_t nowMs = millis();
    uint16_t written = 0;
    for (uint16_t i = 0; i < pendingCount; i++)
    {
        const int64_t epoch = now - (int64_t)((uint32_t)(nowMs - pending[i].atMs) / 1000UL);
        char row[128];
        const int n = formatCsvRow(row, sizeof(row), epoch, pending[i].s);
        if (n <= 0 || file.write((const uint8_t *)row, (size_t)n) != (size_t)n) break;
        written++;
        if ((i & 31) == 31) wdtFeed();
    }
    file.flush();
    file.close();

    if (written > 0) logPrintf(LV_OK, "Clock set: %u buffered readings written", (unsigned)written);
    if (written == pendingCount) pendingCount = 0;
    else
    {
        memmove(&pending[0], &pending[written], (size_t)(pendingCount - written) * sizeof(PendingReading));
        pendingCount = (uint16_t)(pendingCount - written);
        sdFault("short write to csv (backfill)");
    }
}

static LogResult logToCSV(const Sample &s)
{
    if (!sd_ok) return LOG_FAILED;

    if (!clockSynced())
    {
        setErr(ERR_CLOCK, true);
        pendingAdd(s);
        noteError("Clock not set: readings buffered in RAM", LV_WARN);
        return LOG_BUFFERED;
    }

    flushPendingReadings();
    ensureCSVExists();
    File file = SD.open(csvPath(), FILE_APPEND);
    if (!file) { sdFault("cannot open csv"); return LOG_FAILED; }

    char row[128];
    const int n = formatCsvRow(row, sizeof(row), (int64_t)time(nullptr), s);
    const size_t w = (n > 0) ? file.write((const uint8_t *)row, (size_t)n) : 0;
    file.flush();
    file.close();

    if (n <= 0 || w != (size_t)n) { sdFault("short write to csv"); return LOG_FAILED; }
    setErr(ERR_SD, false);
    return LOG_SAVED;
}

static float sdUsageNow()
{
    wdtFeed();
    const uint64_t total = SD.totalBytes();
    const float p = total ? 100.0f * (float)SD.usedBytes() / (float)total : 0;
    wdtFeed();
    return p;
}

static float sdUsagePercent()
{
    static float cached = 0;
    static unsigned long last = 0;
    static bool have = false;
    if (!sd_ok) return cached;
    if (!have || millis() - last > SD_USAGE_REFRESH_MS)
    {
        wdtFeed();
        const uint64_t total = SD.totalBytes();
        cached = total ? 100.0f * (float)SD.usedBytes() / (float)total : 0;
        wdtFeed();
        last = millis(); have = true;
    }
    return cached;
}

void setupSDCard()
{
    logMsg(LV_INFO, "[SD CARD]");
    SD.end();
    spiSD.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    if (!SD.begin(SD_CS_PIN, spiSD, 4000000))
    {
        noteError("SD card initialization failed");
        setErr(ERR_SD, true);
        sd_ok = false;
        return;
    }
    sd_ok = true;
    setErr(ERR_SD, false);
    logPrintf(LV_OK, "READY, %llu MB", (unsigned long long)(SD.cardSize() / (1024 * 1024)));

    recoverTmp(csvPath().c_str(), CSV_TMP);
    recoverTmp(MEDIA_Q, MEDIA_Q_TMP);
    sdRemoveIfExists(REC_TMP);      // an unfinished recording is never queued
    ensureCSVExists();
}

// ============================================================================
// HTTP helpers / /ingest
// ============================================================================
enum PostResult { POST_OK, POST_REJECTED, POST_RETRY, POST_AUTH };

static void httpDefaults(HTTPClient &http, uint16_t ioTimeoutMs = GATEWAY_IO_TIMEOUT_MS)
{
    http.setConnectTimeout(GATEWAY_CONNECT_TIMEOUT_MS);
    http.setTimeout(ioTimeoutMs);
}

static uint8_t gatewayRetries = 0;

static bool transientHttpError(int code)
{
    return code < 0 && code != -6 && code != -7 && code != -8 && code != -9;
}

static PostResult gatewayPost(HTTPClient &http, WiFiClientSecure &tls, const char *path,
                              const char *body, int &codeOut)
{
    wdtFeed();
    const String url = String(AWS_API_BASE) + path;
    if (!url.startsWith("https://")) { noteError("Gateway URL is not HTTPS: refusing to send"); codeOut = -1; return POST_RETRY; }
    const size_t len = strlen(body);

    for (;;)
    {
        DBG_T0(t0);
        http.begin(tls, url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Api-Key", GATEWAY_KEY);
        codeOut = http.POST((uint8_t *)body, len);
#if HIVE_DEBUG
        String respBody;
        if (codeOut > 0 && codeOut != 200 && codeOut != 202) respBody = http.getString();
#endif
        http.end();
        DBG("POST %s -> %d in %lu ms (heap=%u maxBlock=%u)", path, codeOut,
            (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#if HIVE_DEBUG
        if (codeOut != 200 && codeOut != 202)
        {
            DBG("  request body : %s", body);
            if (respBody.length()) DBG("  response body: %.300s", respBody.c_str());
        }
#endif
        wdtFeed();

        if (!transientHttpError(codeOut) || gatewayRetries == 0) break;
        gatewayRetries--;
        tls.stop();
        server.handleClient();
        delay(500);
    }

    server.handleClient();
    wdtFeed();

    if (codeOut == 202 || codeOut == 200) return POST_OK;
    if (codeOut == 400 || codeOut == 422) return POST_REJECTED;
    if (codeOut == 401 || codeOut == 403) return POST_AUTH;
    return POST_RETRY;
}

static void buildEnvelope(char *out, size_t n, const char *sensorType, const char *atIso, const char *readingJson)
{
    snprintf(out, n,
             "{\"device_id\":\"%s\",\"sensor_type\":\"%s\",\"recorded_at\":\"%s\",\"reading\":%s}",
             DEVICE_ID, sensorType, atIso, readingJson);
}

// {"brood_section":a,"honey_section":b}, omitting NaN zones. Returns false if both are missing.
static bool zoneJson(char *o, size_t n, float a, float b)
{
    size_t l = snprintf(o, n, "{");
    bool first = true;
    if (!isnan(a)) { l += snprintf(o + l, n - l, "\"" HIVE_ZONE_1 "\":%.2f", a); first = false; }
    if (!isnan(b)) { l += snprintf(o + l, n - l, "%s\"" HIVE_ZONE_2 "\":%.2f", first ? "" : ",", b); first = false; }
    snprintf(o + l, n - l, "}");
    return !first;
}

// One CSV row -> up to 3 messages (temperature, humidity, co2). Only measured values are sent.
// Idempotent by recorded_at, so a retry of the same row is discarded server-side.
static PostResult sendRow(HTTPClient &http, WiFiClientSecure &tls, const char *atIso, const Sample &s)
{
    struct Item { const char *type; const char *json; };
    Item items[3];
    int ni = 0;
    char jTemp[128], jHum[128], jCo2[48], body[400];

    if (zoneJson(jTemp, sizeof(jTemp), s.t1, s.t2)) items[ni++] = Item{"temperature", jTemp};
    if (zoneJson(jHum, sizeof(jHum), s.h1, s.h2))   items[ni++] = Item{"humidity", jHum};
    if (s.co2 >= 0)
    {
        snprintf(jCo2, sizeof(jCo2), "{\"co2_level\":%.1f}", (float)s.co2);
        items[ni++] = Item{"co2", jCo2};
    }

    bool anyRejected = false;
    for (int i = 0; i < ni; i++)
    {
        buildEnvelope(body, sizeof(body), items[i].type, atIso, items[i].json);
        int code;
        const PostResult r = gatewayPost(http, tls, "/ingest", body, code);
        if (r == POST_OK) continue;
        if (r == POST_REJECTED)
        {
            logPrintf(LV_WARN, "AWS rejected %s (HTTP %d)", items[i].type, code);
            anyRejected = true;
            continue;
        }
        noteError("AWS ingest failed: HTTP " + String(code));
        return r;   // network/auth problem: stop, keep the row
    }
    return anyRejected ? POST_REJECTED : POST_OK;
}

struct CsvStats { int sent = 0, rejected = 0, skipped = 0; bool failed = false; bool more = false; };

static CsvStats uploadCsvToAws(unsigned long budgetEndMs)
{
    CsvStats st;
    String path = csvPath();
    if (!sd_ok) return st;
    if (WiFi.status() != WL_CONNECTED) { st.failed = true; return st; }
    if (!SD.exists(path)) return st;

    File csv = SD.open(path, FILE_READ);
    if (!csv) { sdFault("cannot read csv"); st.failed = true; return st; }

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    http.setReuse(true);
    httpDefaults(http);

    char raw[200], work[200], at[24];
    char stopLine[200] = "";
    bool haveStop = false;

    logMsg(LV_INFO, "===== SENDING CSV TO AWS =====");

    int r;
    while ((r = readLine(csv, raw, sizeof(raw))) >= 0)
    {
        if (r == 0) continue;

        strcpy(work, raw);
        Sample s;
        if (!parseCsvRow(work, 0, at, s))
        {
            quarantineRow(raw); st.skipped++; continue;
        }

        if ((long)(millis() - budgetEndMs) >= 0)
        {
            st.more = true; strcpy(stopLine, raw); haveStop = true; break;
        }

        const PostResult pr = sendRow(http, tls, at, s);
        if (pr == POST_OK) st.sent++;
        else if (pr == POST_REJECTED) { quarantineRow(raw); st.rejected++; }
        else
        {
            st.failed = true;
            strcpy(stopLine, raw); haveStop = true;
            setErr(ERR_AUTH, pr == POST_AUTH);
            break;
        }
    }

    if (!st.failed) setErr(ERR_AUTH, false);
    setErr(ERR_AWS_UPLOAD, st.failed);

    if (haveStop)
    {
        sdRemoveIfExists(CSV_TMP);
        File tmp = SD.open(CSV_TMP, FILE_WRITE);
        if (tmp)
        {
            tmp.println(stopLine);
            uint8_t buf[512];
            while (csv.available())
            {
                const size_t n = csv.read(buf, sizeof(buf));
                if (n == 0) break;
                tmp.write(buf, n);
                wdtFeed();
            }
            tmp.close();
            csv.close();
            SD.remove(path);
            if (!SD.rename(CSV_TMP, path)) sdFault("csv rename failed");
        }
        else { csv.close(); sdFault("cannot write csv tmp"); }
    }
    else
    {
        csv.close();
        SD.remove(path);
        ensureCSVExists();
    }

    logPrintf(st.failed ? LV_ERR : (st.rejected || st.skipped) ? LV_WARN : LV_OK,
              "AWS: %d sent, %d quarantined, %d skipped%s", st.sent, st.rejected, st.skipped,
              st.failed ? " (will retry)" : st.more ? " (more pending)" : "");
    return st;
}

// ============================================================================
// /heartbeat   (includes the device MAC address)
// ============================================================================
static float batteryPercent(float packV)
{
    static const struct { float v, pct; } curve[] = {
        {3.00f, 0},  {3.30f, 3},  {3.50f, 6},  {3.60f, 12}, {3.70f, 25},
        {3.80f, 42}, {3.90f, 60}, {4.00f, 76}, {4.10f, 90}, {4.20f, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    const float v = packV / BATT_CELLS;
    if (v <= curve[0].v)     return 0;
    if (v >= curve[n - 1].v) return 100;
    for (int i = 1; i < n; i++)
        if (v <= curve[i].v)
            return curve[i - 1].pct + (v - curve[i - 1].v) * (curve[i].pct - curve[i - 1].pct) /
                                          (curve[i].v - curve[i - 1].v);
    return 0;
}

// Extra health fields: battery voltage (V), battery current (A) and the chip model.
// A value that was not measured is left out, never invented.
static void buildHeartbeatExtras(char *out, size_t n, float battV)
{
    size_t len = 0;
    out[0] = 0;
    const bool battOk = !(errFlags & ERR_BATT);
    if (battOk && battV > 0)
        len += snprintf(out + len, n - len, ",\"battery_voltage\":%.2f", battV);
    if (battOk && battSampled)
        len += snprintf(out + len, n - len, ",\"battery_current\":%.3f", battCurrentA);
    snprintf(out + len, n - len, ",\"chip\":\"%s rev %d\"", ESP.getChipModel(), (int)ESP.getChipRevision());
}

static bool sendHeartbeat(float battV, int wifiRssi)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    const uint64_t nowUs = esp_timer_get_time();
    const float readRate = sensorReadTotal ? (float)sensorReadOk / (float)sensorReadTotal : 1.0f;

    char codes[256];
    buildErrorCodes(codes, sizeof(codes));

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    httpDefaults(http);

    PostResult r = POST_RETRY;
    int code = 0;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        // If the backend rejects an extra field, step down: full -> mac only -> documented payload.
        char macField[48] = "";
        if (hbLevel >= 1) snprintf(macField, sizeof(macField), ",\"mac_address\":\"%s\"", deviceMac);

        char extras[160] = "";
        if (hbLevel >= 2) buildHeartbeatExtras(extras, sizeof(extras), battV);

        char body[900];
        snprintf(body, sizeof(body),
                 "{\"battery_level\":%.1f,\"signal_strength\":%d,\"uptime\":%lu,"
                 "\"firmware_version\":\"%s\",\"cpu_usage\":%.1f,\"storage_usage\":%.1f,"
                 "\"reboot_count\":%lu,\"sensor_read_success_rate\":%.2f,\"error_codes\":[%s]%s%s}",
                 batteryPercent(battV), wifiRssi, (unsigned long)(nowUs / 1000000ULL),
                 FW_VERSION, cpuUsagePct(), sdUsagePercent(),
                 (unsigned long)rebootCount, readRate, codes, macField, extras);

        DBG("HB body: %s", body);
        r = gatewayPost(http, tls, "/heartbeat", body, code);
        if (r == POST_REJECTED && hbLevel > 0)
        {
            hbLevel--;
            noteError(hbLevel == 1 ? "Heartbeat rejected with voltage/current/chip: resending without them"
                                   : "Heartbeat rejected with mac_address: resending without it", LV_WARN);
            tls.stop();
            continue;
        }
        break;
    }

    if (r == POST_OK)
    {
        logPrintf(LV_OK, "Heartbeat: 202 queued (%.2f V, %.3f A)", battV, battCurrentA);
        sensorReadOk = sensorReadTotal = 0;
        cpuWinStartUs = nowUs;
        cpuWinStartCount = idleHookCount;
        setErr(ERR_AUTH, false);
        setErr(ERR_RESET, false);
        return true;
    }
    if (r == POST_AUTH) setErr(ERR_AUTH, true);
    noteError("Heartbeat failed: HTTP " + String(code));
    return false;
}

// ============================================================================
// MEDIA QUEUE   line format: path|capturedAt|s3Key|attempts
// ============================================================================
struct MediaItem { String path, capturedAt, s3Key; uint8_t attempts = 0; };
enum MediaResult { MEDIA_DONE, MEDIA_GIVE_UP, MEDIA_RETRY_LATER, MEDIA_RETRY_ITEM };

static bool ademneaIsLocalhost()
{
    return strstr(ADEMNEA_BASE, "127.0.0.1") || strstr(ADEMNEA_BASE, "localhost");
}

static bool mediaInfoForPath(const String &p, const char *&type, const char *&mime)
{
    String l = p; l.toLowerCase();
    if (l.endsWith(".wav"))                        { type = "audio"; mime = "audio/wav";  return true; }
    if (l.endsWith(".jpg") || l.endsWith(".jpeg")) { type = "photo"; mime = "image/jpeg"; return true; }
    if (l.endsWith(".mp4"))                        { type = "video"; mime = "video/mp4";  return true; }
    return false;
}

static bool parseMediaLine(const String &line, MediaItem &it)
{
    const int a = line.indexOf('|');
    const int b = line.indexOf('|', a + 1);
    const int c = line.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) return false;
    it.path       = line.substring(0, a);
    it.capturedAt = line.substring(a + 1, b);
    it.s3Key      = line.substring(b + 1, c);
    it.attempts   = (uint8_t)line.substring(c + 1).toInt();
    return it.path.length() && it.capturedAt.length();
}

static String formatMediaLine(const MediaItem &it)
{
    return it.path + "|" + it.capturedAt + "|" + it.s3Key + "|" + String(it.attempts);
}

// Reads up to maxLines valid lines from the FRONT of the queue (the queue itself stays on the SD).
static void queueHead(std::vector<String> &out, size_t maxLines)
{
    out.clear();
    if (!sd_ok || !SD.exists(MEDIA_Q)) return;
    File q = SD.open(MEDIA_Q, FILE_READ);
    if (!q) return;
    char buf[400];
    int r;
    while (out.size() < maxLines && (r = readLine(q, buf, sizeof(buf))) >= 0)
        if (r > 0) out.push_back(String(buf));
    q.close();
}

// Atomically replaces the first `replaceCount` valid lines of the queue with `newHead`; every
// later line is streamed through unchanged. tmp+rename means a reset leaves the old or the new
// queue, never a broken one (recoverTmp() at boot finishes an interrupted rename).
static bool queueReplaceHead(size_t replaceCount, const std::vector<String> &newHead)
{
    if (!sd_ok) return false;
    sdRemoveIfExists(MEDIA_Q_TMP);
    File out = SD.open(MEDIA_Q_TMP, FILE_WRITE);
    if (!out) { sdFault("cannot write media queue tmp"); return false; }
    size_t written = 0;
    for (auto &l : newHead) { out.println(l); written++; }

    if (SD.exists(MEDIA_Q))
    {
        File in = SD.open(MEDIA_Q, FILE_READ);
        if (in)
        {
            char buf[400];
            int r;
            size_t skipped = 0, n = 0;
            while ((r = readLine(in, buf, sizeof(buf))) >= 0)
            {
                if (r == 0) continue;
                if (skipped < replaceCount) { skipped++; continue; }
                out.println(buf);
                written++;
                if ((++n & 63) == 0) wdtFeed();
            }
            in.close();
        }
    }
    out.close();
    sdRemoveIfExists(MEDIA_Q);
    if (written == 0) { sdRemoveIfExists(MEDIA_Q_TMP); return true; }
    if (!SD.rename(MEDIA_Q_TMP, MEDIA_Q)) { sdFault("cannot rename media queue"); return false; }
    return true;
}

static uint32_t fnv1a(const String &s)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static bool enqueueMedia(const String &path, const String &capturedAt)
{
    File f = SD.open(MEDIA_Q, FILE_APPEND);
    if (!f) { sdFault("cannot open media queue"); return false; }
    f.println(path + "|" + capturedAt + "||0");
    f.close();
    return true;
}

static void moveToFailed(const String &path)
{
    if (!SD.exists(path)) return;
    SD.mkdir(FAILED_DIR);
    const int slash = path.lastIndexOf('/');
    String name = slash >= 0 ? path.substring(slash + 1) : path;
    if (!SD.rename(path, String(FAILED_DIR) + "/" + name)) SD.remove(path);
}

// Free-space policy: when the SD card is nearly full the OLDEST waiting recording is deleted.
static bool dropOldestMedia()
{
    std::vector<String> head;
    queueHead(head, 1);
    if (head.empty()) return false;
    MediaItem it;
    if (parseMediaLine(head[0], it)) sdRemoveIfExists(it.path.c_str());
    queueReplaceHead(1, std::vector<String>());
    noteError("SD nearly full: oldest waiting recording deleted", LV_WARN);
    return true;
}

static bool beginHttps(HTTPClient &http, WiFiClientSecure &tls, const String &url)
{
    if (!url.startsWith("https://")) { noteError("Refusing non-HTTPS media URL"); return false; }
    return http.begin(tls, url);
}

class WdtFileStream : public Stream
{
  public:
    explicit WdtFileStream(File &file) : f(file) {}
    using Stream::readBytes;
    int    available() override { return f.available(); }
    int    read() override { return f.read(); }
    int    peek() override { return f.peek(); }
    size_t write(uint8_t) override { return 0; }
    size_t readBytes(char *b, size_t n) override { wdtFeed(); return f.readBytes(b, n); }
    void   flush() override {}
  private:
    File &f;
};

// saveProgress() persists the queue line (called right after the S3 PUT succeeds).
static MediaResult uploadMediaItem(MediaItem &it, const std::function<void()> &saveProgress)
{
    const char *mtype, *mime;
    if (!mediaInfoForPath(it.path, mtype, mime)) { noteError("Media: unknown type " + it.path); return MEDIA_GIVE_UP; }
    if (!SD.exists(it.path))                     { noteError("Media: file missing " + it.path); return MEDIA_GIVE_UP; }

    File probe = SD.open(it.path, FILE_READ);
    const uint32_t fileSize = probe ? (uint32_t)probe.size() : 0;
    if (probe) probe.close();
    if (fileSize < 1024) { noteError("Media: file too small " + it.path); return MEDIA_GIVE_UP; }

    auto itemFailed = [&]() -> MediaResult {
        it.attempts++;
        return it.attempts >= MEDIA_MAX_ATTEMPTS ? MEDIA_GIVE_UP : MEDIA_RETRY_ITEM;
    };

    if (it.s3Key.length() == 0)
    {
        static char uploadUrl[2304];
        char s3Key[200];
        int code;
        String resp;
        {
            WiFiClientSecure tls;
            tls.setCACert(ADEMNEA_ROOT_CA);
            HTTPClient http;
            httpDefaults(http);
            wdtFeed();
            if (!beginHttps(http, tls, String(ADEMNEA_BASE) + "/api/iot/media/upload-url")) return MEDIA_RETRY_LATER;
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-Api-Key", ADEMNEA_KEY);

            char body[96];
            snprintf(body, sizeof(body), "{\"media_type\":\"%s\",\"content_type\":\"%s\"}", mtype, mime);
            code = http.POST((uint8_t *)body, strlen(body));
            if (code == 200 || code == 201)
            {
                if (http.getSize() > 6144) code = -100;
                else resp = http.getString();
            }
            http.end();
            DBG("MEDIA step1: HTTP %d", code);
            wdtFeed();
        }

        if (code == 401 || code == 403) { setErr(ERR_AUTH, true); noteError("Media step1: key rejected"); return MEDIA_RETRY_LATER; }
        if (code == 400 || code == 422) { noteError("Media step1 rejected: HTTP " + String(code)); return itemFailed(); }
        if (code != 200 && code != 201) { noteError("Media step1 failed: HTTP " + String(code)); return MEDIA_RETRY_LATER; }

        if (!hu::jsonGetString(resp.c_str(), "upload_url", uploadUrl, sizeof(uploadUrl)) ||
            !hu::jsonGetString(resp.c_str(), "s3_object_key", s3Key, sizeof(s3Key)))
        {
            noteError("Media step1: bad response");
            return itemFailed();
        }
        logPrintf(LV_OK, "Media step1 OK. Key: %s", s3Key);

        {
            WiFiClientSecure tls;
            if (String(uploadUrl).indexOf("amazonaws.com") >= 0) tls.setCACert(AWS_ROOT_CA);
            else                                                  tls.setCACert(ADEMNEA_ROOT_CA);
            HTTPClient http;
            httpDefaults(http, 60000);
            if (!beginHttps(http, tls, String(uploadUrl))) return itemFailed();
            http.addHeader("Content-Type", mime);

            File f = SD.open(it.path, FILE_READ);
            if (!f) { sdFault("cannot reopen media"); return MEDIA_RETRY_LATER; }
            wdtFeed();
            WdtFileStream ws(f);
            DBG_T0(putT0);
            code = http.sendRequest("PUT", &ws, f.size());
            DBG("MEDIA step2: HTTP %d in %lu ms", code, (unsigned long)(millis() - putT0));
            f.close();
            http.end();
            wdtFeed();
        }

        if (code != 204 && code != 200)
        {
            noteError("Media step2 (PUT) failed: HTTP " + String(code));
            setErr(ERR_MEDIA, true);
            return (code >= 400 && code < 500) ? itemFailed() : MEDIA_RETRY_LATER;
        }
        it.s3Key = s3Key;
        saveProgress();   // crash-safe: the file is on S3, never upload it again
        logMsg(LV_OK, "Media step2 OK");
    }

    char body[384];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"media_type\":\"%s\",\"s3_object_key\":\"%s\","
             "\"captured_at\":\"%s\",\"file_size_bytes\":%lu}",
             DEVICE_ID, mtype, it.s3Key.c_str(), it.capturedAt.c_str(), (unsigned long)fileSize);

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    httpDefaults(http);
    int code;
    const PostResult r = gatewayPost(http, tls, "/media/confirm", body, code);

    if (r == POST_OK)   { logMsg(LV_OK, "Media step3 OK"); return MEDIA_DONE; }
    if (r == POST_AUTH) { setErr(ERR_AUTH, true); return MEDIA_RETRY_LATER; }
    if (r == POST_REJECTED)
    {
        noteError("Media confirm rejected: HTTP " + String(code));
        it.s3Key = "";
        return itemFailed();
    }
    noteError("Media step3 failed: HTTP " + String(code));
    return MEDIA_RETRY_LATER;
}

struct MediaStatus { bool retryLater = false; bool more = false; };

static MediaStatus processMediaQueue(int maxItems, unsigned long budgetEndMs)
{
    MediaStatus ms;
    if (!sd_ok || battLow || WiFi.status() != WL_CONNECTED || !SD.exists(MEDIA_Q)) return ms;

    if (ademneaIsLocalhost())
    {
        noteError("Media: ADEMNEA_BASE_URL is localhost");
        setErr(ERR_MEDIA, true);
        return ms;
    }

    // Only the lines this window can handle (+1 to learn whether more are waiting) are held in RAM.
    std::vector<String> head;
    queueHead(head, (size_t)maxItems + 1);
    if (head.empty()) return ms;
    size_t frontCount = head.size();   // how many lines at the front of the file `head` stands for
    auto save = [&]() { queueReplaceHead(frontCount, head); frontCount = head.size(); };

    int attempted = 0;
    size_t i = 0;
    bool anyProblem = false;

    while (i < head.size() && attempted < maxItems)
    {
        if ((long)(millis() - budgetEndMs) >= 0) break;

        MediaItem it;
        if (!parseMediaLine(head[i], it)) { head.erase(head.begin() + i); save(); continue; }

        attempted++;
        const MediaResult res = uploadMediaItem(it, [&]() { head[i] = formatMediaLine(it); save(); });

        if (res == MEDIA_DONE)
        {
            sdRemoveIfExists(it.path.c_str());
            head.erase(head.begin() + i);
            save();
        }
        else if (res == MEDIA_GIVE_UP)
        {
            anyProblem = true;
            moveToFailed(it.path);
            head.erase(head.begin() + i);
            save();
        }
        else
        {
            anyProblem = true;
            head[i] = formatMediaLine(it);
            save();
            if (res == MEDIA_RETRY_LATER) { ms.retryLater = true; break; }
            i++;
        }
        server.handleClient();
        wdtFeed();
    }

    if (i < head.size()) ms.more = true;   // lines left in this batch (budget/limit reached)
    if (!anyProblem) setErr(ERR_MEDIA, false);
    return ms;
}

static void mediaHousekeeping()
{
    if (!sd_ok) return;

    if (SD.exists(FAILED_DIR))
    {
        std::vector<String> names;
        File d = SD.open(FAILED_DIR);
        if (d)
        {
            for (File f = d.openNextFile(); f; f = d.openNextFile())
            {
                String n = f.name();
                const int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s + 1);
                if (!f.isDirectory()) names.push_back(n);
                f.close();
            }
            d.close();
        }
        if ((int)names.size() > FAILED_MEDIA_MAX)
        {
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i + FAILED_MEDIA_MAX < names.size(); i++)
                SD.remove(String(FAILED_DIR) + "/" + names[i]);
        }
    }

    std::vector<uint32_t> queued;   // 4 bytes per queued file: safe even with thousands waiting
    if (SD.exists(MEDIA_Q))
    {
        File q = SD.open(MEDIA_Q, FILE_READ);
        if (q)
        {
            char buf[400];
            int r;
            while ((r = readLine(q, buf, sizeof(buf))) >= 0)
            {
                MediaItem it;
                if (r > 0 && parseMediaLine(String(buf), it)) queued.push_back(fnv1a(it.path));
            }
            q.close();
        }
    }
    std::sort(queued.begin(), queued.end());

    File root = SD.open("/");
    if (!root) return;
    std::vector<String> orphans;
    const String prefix = hiveNo + "_";
    for (File f = root.openNextFile(); f; f = root.openNextFile())
    {
        String n = f.name();
        const int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s + 1);
        const bool isWav = n.endsWith(".wav") && n.startsWith(prefix);
        const size_t sz = f.size();
        const bool dir = f.isDirectory();
        f.close();
        if (dir || !isWav) continue;
        const String path = "/" + n;
        if (std::binary_search(queued.begin(), queued.end(), fnv1a(path))) continue;
        if (sz < 1024) { SD.remove(path); continue; }
        orphans.push_back(path);
    }
    root.close();

    for (auto &path : orphans)
    {
        const int u = path.indexOf('_');
        const String st = path.substring(u + 1, path.length() - 4);
        if (st.length() != 16 || st[8] != 'T' || st[15] != 'Z') continue;
        const String iso = st.substring(0, 4) + "-" + st.substring(4, 6) + "-" + st.substring(6, 8) + "T" +
                           st.substring(9, 11) + ":" + st.substring(11, 13) + ":" + st.substring(13, 15) + "Z";
        enqueueMedia(path, iso);
        logPrintf(LV_INFO, "Adopted orphan recording: %s", path.c_str());
    }
}

// ============================================================================
// SENSORS
// ============================================================================
// DHT22 read. On failure the value is left MISSING (NaN): nothing is substituted.
static bool readOneDHT(DHTesp &dht, float &t, float &h, uint32_t errFlag)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        const TempAndHumidity th = dht.getTempAndHumidity();
        const bool bad = isnan(th.temperature) || th.temperature < -40 || th.temperature > 85 ||
                         isnan(th.humidity) || th.humidity < 0 || th.humidity > 100;
        if (!bad)
        {
            t = th.temperature; h = th.humidity;
            setErr(errFlag, false);
            return true;
        }
        if (attempt == 0)
        {
            const unsigned long until = millis() + dht.getMinimumSamplingPeriod() + 100;
            while (millis() < until) { wdtFeed(); delay(50); }
        }
    }
    t = NAN; h = NAN;
    setErr(errFlag, true);
    return false;
}

// ---- MH-Z19 UART -----------------------------------------------------------
static void mhz19DisableABC()
{
    // Auto-baseline calibration assumes regular exposure to 400 ppm air; inside a hive it
    // would drift the baseline down. Calibrate manually instead.
    const uint8_t cmd[9] = {0xFF, 0x01, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86};
    Serial2.write(cmd, sizeof(cmd));
    Serial2.flush();
}

static int readMHZ19()
{
    const uint8_t cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
    uint8_t resp[9] = {0};

    while (Serial2.available()) Serial2.read();
    Serial2.write(cmd, sizeof(cmd));
    Serial2.flush();

    const unsigned long deadline = millis() + 500UL;
    int got = 0;
    while (got < 9 && millis() < deadline)
    {
        if (Serial2.available()) { resp[got++] = (uint8_t)Serial2.read(); continue; }
        delay(2);
    }
    if (got != 9 || resp[0] != 0xFF || resp[1] != 0x86) return -1;
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) sum += resp[i];
    sum = 0xFF - sum + 1;
    if (sum != resp[8]) return -1;
    return (resp[2] << 8) | resp[3];
}

// Returns -1 (missing) during warm-up or on failure; never a made-up value.
static int readCO2()
{
    if (!reached(co2ReadyAt)) return -1;   // not an error, sensor is still stabilising
    const int v = readMHZ19();
    if (v < 200 || v > 10000) { setErr(ERR_CO2, true); return -1; }
    setErr(ERR_CO2, false);
    return v;
}

static float readBatteryVoltage()
{
    float voltage = batterySensor.getBusVoltage_V();
    if (isnan(voltage) || voltage <= 0) { setErr(ERR_BATT, true); return lastBatteryVoltage; }
    setErr(ERR_BATT, false);
    if (lastBatteryVoltage > 0) voltage = 0.7f * voltage + 0.3f * lastBatteryVoltage;   // light smoothing (guard only)
    lastBatteryVoltage = voltage;
    return voltage;
}

// ============================================================================
// MICROPHONE  (16-bit LEFT)
// ============================================================================
static void setupMicrophone()
{
    if (mic_ok) { i2s_driver_uninstall(I2S_NUM_0); mic_ok = false; }

    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_DATA_PIN,
    };
    esp_err_t ie = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (ie != ESP_OK) { noteError("Microphone driver install failed"); setErr(ERR_MIC, true); return; }
    ie = i2s_set_pin(I2S_NUM_0, &pins);
    if (ie != ESP_OK) { i2s_driver_uninstall(I2S_NUM_0); noteError("Microphone pin config failed"); setErr(ERR_MIC, true); return; }
    gpio_set_pull_mode((gpio_num_t)I2S_DATA_PIN, GPIO_PULLUP_ONLY);
    i2s_zero_dma_buffer(I2S_NUM_0);
    mic_ok = true;
    setErr(ERR_MIC, false);
}

static void powerOnMicrophone()  { digitalWrite(LED_PIN, HIGH); delay(50); setupMicrophone(); }
static void powerOffMicrophone() { if (mic_ok) i2s_driver_uninstall(I2S_NUM_0); mic_ok = false; digitalWrite(LED_PIN, LOW); }

static void micDiscard(int samples)
{
    if (!mic_ok) return;
    int16_t tmp[64];
    size_t got = 0;
    const unsigned long deadline = millis() + 1000UL;
    while (samples > 0 && millis() < deadline)
    {
        if (i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) break;
        samples -= (int)(got / sizeof(int16_t));
    }
}

// Peak amplitude after DC removal (dashboard only).
static long readMicrophonePeak(int numSamples = MIC_PEAK_SAMPLES)
{
    if (!mic_ok) return 0;
    if (numSamples > MIC_PEAK_SAMPLES) numSamples = MIC_PEAK_SAMPLES;

    static int16_t buf[MIC_PEAK_SAMPLES];
    int16_t chunk[64];
    size_t bytesRead = 0;
    int n = 0;

    micDiscard(MIC_SETTLE_SAMPLES);

    const unsigned long deadline = millis() + 1000UL;
    while (n < numSamples && millis() < deadline)
    {
        const esp_err_t err = i2s_read(I2S_NUM_0, chunk, sizeof(chunk), &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) break;
        const int cnt = (int)(bytesRead / sizeof(int16_t));
        for (int i = 0; i < cnt && n < numSamples; i++) buf[n++] = chunk[i];
    }
    if (n == 0) return 0;

    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i];
    const long mean = (long)(sum / n);

    long peak = 0;
    for (int i = 0; i < n; i++)
    {
        const long level = labs((long)buf[i] - mean);
        if (level > peak) peak = level;
    }
    return peak;
}

// ============================================================================
// AUDIO RECORDING  (16-bit mono PCM WAV, 44-byte header)
// ============================================================================
static void writeWavHeader(File &file, int sampleRate, int totalSamples)
{
    const uint32_t dataSize   = (uint32_t)totalSamples * 2;
    const uint32_t chunkSize  = 36 + dataSize;
    const uint32_t byteRate   = (uint32_t)sampleRate * 2;
    const uint32_t fmtSize    = 16;
    const uint16_t audioFmt   = 1, channels = 1, blockAlign = 2, bits = 16;
    const uint32_t sr         = (uint32_t)sampleRate;

    file.write((const uint8_t *)"RIFF", 4);
    file.write((const uint8_t *)&chunkSize, 4);
    file.write((const uint8_t *)"WAVE", 4);
    file.write((const uint8_t *)"fmt ", 4);
    file.write((const uint8_t *)&fmtSize, 4);
    file.write((const uint8_t *)&audioFmt, 2);
    file.write((const uint8_t *)&channels, 2);
    file.write((const uint8_t *)&sr, 4);
    file.write((const uint8_t *)&byteRate, 4);
    file.write((const uint8_t *)&blockAlign, 2);
    file.write((const uint8_t *)&bits, 2);
    file.write((const uint8_t *)"data", 4);
    file.write((const uint8_t *)&dataSize, 4);
}

static bool recordAndEnqueue()
{
    if (!sd_ok || !clockSynced()) return false;

    // Audio is never dropped while the card has room. Above SD_AUDIO_DROP_PCT the OLDEST waiting
    // recordings are deleted until there is space again; if none are left, this recording is skipped
    // (the sensor CSV must keep its free space).
    {
        int guard = 0;
        while (sdUsageNow() > SD_AUDIO_DROP_PCT && guard++ < 20)
            if (!dropOldestMedia()) break;
        if (sdUsageNow() > SD_AUDIO_DROP_PCT)
        {
            setErr(ERR_MEDIA_FULL, true);
            noteError("SD card nearly full and no audio left to delete: recording skipped");
            return false;
        }
        setErr(ERR_MEDIA_FULL, false);
    }

    const int64_t epoch = (int64_t)time(nullptr);
    char iso[24];
    hu::epochToIso(epoch, iso);
    int y, mo, d, h, mi, s;
    hu::splitEpoch(epoch, y, mo, d, h, mi, s);
    char name[48];
    snprintf(name, sizeof(name), "/%s_%04d%02d%02dT%02d%02d%02dZ.wav", hiveNo.c_str(), y, mo, d, h, mi, s);
    logPrintf(LV_INFO, "RECORDING AUDIO -> %s", name);

    powerOnMicrophone();
    if (!mic_ok) { powerOffMicrophone(); return false; }

    sdRemoveIfExists(REC_TMP);
    File f = SD.open(REC_TMP, FILE_WRITE);
    if (!f) { powerOffMicrophone(); sdFault("cannot create wav"); return false; }

    const int total = SAMPLE_RATE * RECORD_TIME_SEC;
    writeWavHeader(f, SAMPLE_RATE, total);
    micDiscard(MIC_SETTLE_SAMPLES);

    static int16_t chunk[512];
    int written = 0;
    bool writeFail = false;
    DBG_T0(recT0);
    const unsigned long deadline = millis() + (RECORD_TIME_SEC * 1000UL) + 3000UL;
    while (written < total && millis() < deadline)
    {
        const size_t want = (size_t)min((int)(sizeof(chunk) / 2), total - written) * 2;
        size_t br = 0;
        if (i2s_read(I2S_NUM_0, chunk, want, &br, pdMS_TO_TICKS(200)) != ESP_OK || br == 0) break;
        if (f.write((const uint8_t *)chunk, br) != br) { writeFail = true; break; }
        written += (int)(br / 2);
        wdtFeed();
    }
    DBG("REC: %d/%d samples in %lu ms", written, total, (unsigned long)(millis() - recT0));

    if (!writeFail && written >= SAMPLE_RATE && written != total)
    {
        f.seek(0);                                  // short recording: fix the header lengths
        writeWavHeader(f, SAMPLE_RATE, written);
    }
    f.close();
    powerOffMicrophone();

    if (writeFail) { sdRemoveIfExists(REC_TMP); sdFault("short write to wav"); return false; }
    if (written < SAMPLE_RATE) { sdRemoveIfExists(REC_TMP); setErr(ERR_MIC, true); noteError("Recording too short: discarded", LV_WARN); return false; }
    setErr(ERR_MIC, false);

    if (!SD.rename(REC_TMP, name)) { sdFault("cannot rename wav"); return false; }
    return enqueueMedia(String(name), String(iso));
}

// ============================================================================
// SAMPLE + LOG
// ============================================================================
static void enterBatteryHardStop()
{
    rtcBattHardStop = true;
    noteError("Battery critical: deep sleep until recovered");
    goToSleep(BATT_HARD_SLEEP_S);
}

static void sampleAndLog()
{
    wdtFeed();
    Sample s;

    sensorReadTotal += 3;
    if (readOneDHT(dht1, s.t1, s.h1, ERR_DHT1)) sensorReadOk++;
    if (readOneDHT(dht2, s.t2, s.h2, ERR_DHT2)) sensorReadOk++;
    s.co2 = readCO2();
    if (s.co2 >= 0 || !reached(co2ReadyAt)) sensorReadOk++;   // warm-up is not a failure

    batteryVoltage = readBatteryVoltage();
    updateBatteryGuard();
    battCurrentA = batterySensor.getCurrent_mA() / 1000.0f;
    battSampled = !(errFlags & ERR_BATT);
    if (solarPresent) solarPowerW = solarSensor.getPower_mW() / 1000.0f;

    powerOnMicrophone();
    audioPeak = readMicrophonePeak();
    powerOffMicrophone();

    // dashboard shows the most recent good value (display only; never written to CSV)
    if (!isnan(s.t1)) { dispT1 = s.t1; dispH1 = s.h1; }
    if (!isnan(s.t2)) { dispT2 = s.t2; dispH2 = s.h2; }
    if (s.co2 >= 0) dispCo2 = s.co2;

    setErr(ERR_CLOCK, !clockSynced());

    if (!s.any()) noteError("No valid sensor values this cycle: nothing logged", LV_WARN);
    else
    {
        const LogResult lr = logToCSV(s);
        logPrintf(lr == LOG_SAVED ? LV_OK : lr == LOG_BUFFERED ? LV_WARN : LV_ERR,
                      "Sample %s: T1=%s H1=%s T2=%s H2=%s CO2=%s Batt=%.2fV peak=%ld",
                      lr == LOG_SAVED ? "saved" : lr == LOG_BUFFERED ? "buffered" : "NOT saved",
                      jnum(s.t1, 1).c_str(), jnum(s.h1, 1).c_str(), jnum(s.t2, 1).c_str(), jnum(s.h2, 1).c_str(),
                      s.co2 >= 0 ? String(s.co2).c_str() : "--", batteryVoltage, audioPeak);
    }

    if (lastBatteryVoltage > 0 && lastBatteryVoltage < BATT_HARD_CUTOFF_V) enterBatteryHardStop();
}

// ============================================================================
// UPLOAD WINDOW
// ============================================================================
static void uploadWindow()
{
    logMsg(LV_INFO, "===== UPLOAD WINDOW =====");
    wdtFeed();
    const unsigned long budgetEnd = millis() + UPLOAD_BUDGET_MS;
    bool failed = false, more = false;

    if (!heapOkForTls())
    {
        lowHeapStreak++;
        setErr(ERR_HEAP_LOW, true);
        noteError("Heap too low for TLS (maxBlock " + String((unsigned)ESP.getMaxAllocHeap()) + ")");
        if (lowHeapStreak >= LOW_HEAP_RESTARTS) { logMsg(LV_ERR, "Restarting: heap fragmentation"); delay(100); ESP.restart(); }
        failed = true;
    }
    else
    {
        lowHeapStreak = 0;
        setErr(ERR_HEAP_LOW, false);

        if (!staEnsure()) { failed = true; noteError("WiFi: router not reachable"); }
        else
        {
            gatewayRetries = GATEWAY_RETRIES_PER_WINDOW;
            syncClockIfNeeded();
            wdtFeed();

            const CsvStats st = uploadCsvToAws(budgetEnd);
            failed = st.failed;
            more   = st.more;

            if (!failed && (!heartbeatEverSent || millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS))
            {
                if (sendHeartbeat(lastBatteryVoltage, WiFi.RSSI()))
                {
                    heartbeatEverSent = true;
                    lastHeartbeatTime = millis();
                }
            }

            if (!failed)
            {
                const MediaStatus ms = processMediaQueue(MAX_MEDIA_PER_WINDOW, budgetEnd);
                if (ms.retryLater) failed = true;
                if (ms.more) more = true;
            }
            disconnectSTA();   // radio-on time is the main power cost: release the router after every window
        }
    }

    if (failed) { if (uploadFailStreak < 250) uploadFailStreak++; }
    else uploadFailStreak = 0;
    uploadBacklog = failed || more;

    unsigned long delayMs;
    if (failed)
    {
        const uint8_t sh = uploadFailStreak > 6 ? 6 : uploadFailStreak;
        uint64_t d = (uint64_t)uploadInterval << sh;
        if (d > BACKOFF_MAX_MS) d = BACKOFF_MAX_MS;
        delayMs = (unsigned long)d;
        delayMs += (unsigned long)random(0, (long)(delayMs / 10) + 1);   // jitter: fleet must not retry in lockstep
    }
    else if (more) delayMs = BACKLOG_RETRY_MS;
    else           delayMs = uploadInterval;
    nextUploadAt = millis() + delayMs;

    logPrintf(failed ? LV_WARN : LV_OK, "===== WINDOW DONE (%s), next in %lu s =====", failed ? "retry" : "ok", delayMs / 1000UL);
}

// ============================================================================
// SCHEDULER
// ============================================================================
void normalLoop()
{
    if (!sd_ok && reached(lastSdRetryTime + SD_RETRY_MS))
    {
        lastSdRetryTime = millis();
        setupSDCard();
    }

    if (reached(nextSensorAt))
    {
        nextSensorAt = millis() + sensorInterval;
        sampleAndLog();
    }

    if (reached(nextRecordAt))
    {
        nextRecordAt = millis() + recordInterval * 1000UL;
        if (!battLow) recordAndEnqueue();
    }

    if (reached(nextUploadAt)) uploadWindow();

    if (millis() - lastHousekeeping > HOUSEKEEP_MS) { lastHousekeeping = millis(); mediaHousekeeping(); }

#if HIVE_DEBUG
    if (millis() - lastHealthPrint > HEALTH_PRINT_MS)
    {
        lastHealthPrint = millis();
        DBG("HEALTH: up=%lus heap=%u maxBlock=%u stackFree=%uB errFlags=0x%04lX batt=%.2fV failStreak=%u backlog=%d apClients=%d sensor=%lus record=%lus upload=%lus",
            (unsigned long)(millis() / 1000UL), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned long)errFlags, lastBatteryVoltage,
            (unsigned)uploadFailStreak, (int)uploadBacklog, (int)WiFi.softAPgetStationNum(),
            sensorInterval / 1000UL, recordInterval, uploadInterval / 1000UL);
    }
#endif
    idleDelay(20);
}

// ---- full duty cycle: same scheduler, light sleep between jobs (RAM and clock are retained) ----
static uint32_t remainingMs(unsigned long t) { return reached(t) ? 0 : (uint32_t)(t - millis()); }

void dutyCycleLoop()
{
    if (!sd_ok && reached(lastSdRetryTime + SD_RETRY_MS)) { lastSdRetryTime = millis(); setupSDCard(); }
    if (WiFi.softAPgetStationNum() > 0) awakeSince = millis();

    if (reached(nextSensorAt)) { nextSensorAt = millis() + sensorInterval; sampleAndLog(); }
    if (reached(nextRecordAt)) { nextRecordAt = millis() + recordInterval * 1000UL; if (!battLow) recordAndEnqueue(); }
    if (reached(nextUploadAt)) uploadWindow();
    if (millis() - lastHousekeeping > HOUSEKEEP_MS) { lastHousekeeping = millis(); mediaHousekeeping(); }

    // Sleep only when nobody is using the dashboard and the grace period is over.
    if (WiFi.softAPgetStationNum() == 0 && millis() - awakeSince > AWAKE_GRACE_MS)
    {
        uint32_t sleepMs = remainingMs(nextSensorAt);
        const uint32_t r = remainingMs(nextRecordAt), u = remainingMs(nextUploadAt);
        if (r < sleepMs) sleepMs = r;
        if (u < sleepMs) sleepMs = u;
        if (sleepMs > 1000)
        {
            logPrintf(LV_INFO, "Light sleep for %lu ms", (unsigned long)sleepMs);
            Serial.flush();
            powerOffMicrophone();
            if (WiFi.status() == WL_CONNECTED) disconnectSTA();
            wdtFeed();
            esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
            esp_light_sleep_start();
            wdtFeed();
            awakeSince = millis();   // dashboard grace window after every wake
            return;
        }
    }
    idleDelay(10);
}

// ============================================================================
// SETUP / LOOP
// ============================================================================
void setup()
{
    Serial.begin(115200);
    delay(200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    const esp_reset_reason_t rr = esp_reset_reason();
    wokeFromSleep = (rr == ESP_RST_DEEPSLEEP);
    classifyReset(rr);
    readDeviceMac();
    Serial.println();
    logPrintf(LV_INFO, "[BOOT] Hive firmware %s  device=%s  MAC=%s  reset=%d", FW_VERSION, DEVICE_ID, deviceMac, (int)rr);

    wdtBegin();
    prefs.begin("duty", false);   // same namespace as the previous firmware: settings and boot counter survive
    loadSettings();
    logPrintf(LV_INFO, "Schedule: sensor %lus  record %lus  upload %lus  mode %s",
                  sensorInterval / 1000UL, recordInterval, uploadInterval / 1000UL,
                  fullDutyCycle ? "duty-cycle" : "normal");
    rebootCount = prefs.getULong("boots", 0);
    if (!wokeFromSleep) { rebootCount++; prefs.putULong("boots", rebootCount); }   // sleep wake-ups are not reboots

    cpuMeterBegin();
    setenv("TZ", "EAT-3", 1);
    tzset();

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setTimeOut(100);

    rtc_ok = rtc.begin();
    setErr(ERR_RTC, !rtc_ok);
    if (!rtc_ok) noteError("RTC (DS3231) not found");
    else if (!rtcLoadSystem()) noteError("RTC time invalid: waiting for NTP", LV_WARN);
    setErr(ERR_CLOCK, !clockSynced());

    if (!batterySensor.begin()) { setErr(ERR_BATT, true); noteError("Battery INA219 not found"); }
    solarPresent = solarSensor.begin();

    dht1.setup(DHT_PIN_1, DHTesp::DHT22);
    dht2.setup(DHT_PIN_2, DHTesp::DHT22);

    Serial2.begin(9600, SERIAL_8N1, CO2_RX_PIN, CO2_TX_PIN);
    delay(50);
    mhz19DisableABC();
    co2ReadyAt = wokeFromSleep ? millis() : millis() + CO2_WARMUP_MS;   // sensor stays powered in deep sleep

    // battery: refuse to run (and drain further) while the pack is critical
    readBatteryVoltage(); delay(60); readBatteryVoltage();
    if (lastBatteryVoltage > 0)
    {
        if (rtcBattHardStop && lastBatteryVoltage < BATT_LOW_RESUME_V) goToSleep(BATT_HARD_SLEEP_S);
        if (lastBatteryVoltage >= BATT_LOW_RESUME_V) rtcBattHardStop = false;
        if (lastBatteryVoltage < BATT_HARD_CUTOFF_V) enterBatteryHardStop();
    }
    batteryVoltage = lastBatteryVoltage;
    updateBatteryGuard();

    setupSDCard();

    if (!String(AWS_API_BASE).startsWith("https://") || !String(ADEMNEA_BASE).startsWith("https://"))
        noteError("Config: gateway/AdEMNEA URL must be https://");

    startAP();   // connection.cpp
    server.on("/", HTTP_GET, handleRoot);
    server.on("/data", HTTP_GET, handleData);
    server.on("/logs", HTTP_GET, handleLogs);
    server.on("/settingsPage", HTTP_GET, handleSettingsPage);
    server.on("/settings", HTTP_GET, handleSettings);
    server.on("/setDuty", HTTP_GET, handleSetDuty);
    server.begin();

    logDeviceIdentity();

    // Boot window: sync the clock (even with a valid RTC), then release the router again.
    ntpNextAttempt = millis();
    if (staEnsure())
    {
        gatewayRetries = GATEWAY_RETRIES_PER_WINDOW;
        syncClockIfNeeded();
        disconnectSTA();
    }
    wdtFeed();

    awakeSince = millis();
    nextSensorAt = millis();
    nextRecordAt = millis() + 5000UL;
    // No valid clock yet: upload window first, to get NTP time before sampling.
    nextUploadAt = clockSynced() ? millis() + uploadInterval : millis();
    lastHousekeeping = millis();

    mediaHousekeeping();
    logMsg(LV_OK, "Setup complete");
}

void loop()
{
    wdtFeed();
    server.handleClient();
    if (fullDutyCycle) dutyCycleLoop();
    else               normalLoop();
}
