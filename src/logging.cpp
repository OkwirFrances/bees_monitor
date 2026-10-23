#include <Arduino.h>
#include <time.h>
#include <stdarg.h>
#include <esp_system.h>
#include "logging.h"

// ======================================================
// LOG BUFFERS (RAM ring buffers shown on the dashboard)
// ======================================================
#define LOG_RING      60    // every line, newest overwrite oldest (the browser keeps its own history)
#define PROBLEM_RING  16    // warnings + errors only, so they are not pushed out by chatty info lines
#define LOG_TEXT_LEN  128   // longer lines are truncated on the dashboard (Serial still gets the full line)

struct LogEntry
{
    uint32_t seq;
    uint8_t  level;
    char     ts[9];
    char     text[LOG_TEXT_LEN];
};

static LogEntry  logRing[LOG_RING];
static LogEntry  problemRing[PROBLEM_RING];
static uint32_t  logSeq = 0;         // sequence number of the newest line (first line is 1)
static uint32_t  problemTotal = 0;   // how many warnings/errors were ever stored
static uint32_t  bootId = 0;         // lets the browser notice that the device rebooted
static LogSink   sink = nullptr;
static bool      inSink = false;

static const time_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01

// time() instead of getLocalTime(): the latter blocks up to 5 s per call while the
// clock is unset, and log lines are most frequent exactly then.
static void makeStamp(char *ts /* >= 9 bytes */)
{
    const time_t now = time(nullptr);
    if (now >= MIN_VALID_EPOCH)
    {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        snprintf(ts, 9, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    else
    {
        strcpy(ts, "--:--:--");
    }
}

void logSetSink(LogSink fn) { sink = fn; }

void logMsg(uint8_t level, const char *msg)
{
    if (!msg) return;
    if (bootId == 0) bootId = esp_random() | 1u;

    char ts[9];
    makeStamp(ts);

    // ---- Serial ----
    switch (level)
    {
    case LV_ERR:  Serial.printf("[%s] [ERROR] %s\n", ts, msg); break;
    case LV_WARN: Serial.printf("[%s] [WARN] %s\n", ts, msg);  break;
    case LV_DBG:  Serial.printf("[DBG %lu] %s\n", (unsigned long)millis(), msg); break;
    default:      Serial.println(msg); break;
    }

    // ---- RAM copy for the dashboard (leading/trailing newlines removed) ----
    const char *m = msg;
    while (*m == '\n' || *m == '\r') m++;
    if (!*m) return;

    const uint32_t seq = ++logSeq;
    LogEntry &e = logRing[(seq - 1) % LOG_RING];
    e.seq = seq;
    e.level = level;
    memcpy(e.ts, ts, sizeof(e.ts));
    strncpy(e.text, m, LOG_TEXT_LEN - 1);
    e.text[LOG_TEXT_LEN - 1] = 0;
    size_t n = strlen(e.text);
    while (n && (e.text[n - 1] == '\n' || e.text[n - 1] == '\r')) e.text[--n] = 0;

    if (level == LV_WARN || level == LV_ERR)
        problemRing[problemTotal++ % PROBLEM_RING] = e;

    // ---- persistent sink (SD card). Guarded: the sink itself may log (e.g. an SD fault). ----
    if (sink && !inSink)
    {
        inSink = true;
        sink(level, e.text);
        inSink = false;
    }
}

void logPrintf(uint8_t level, const char *fmt, ...)
{
    char buf[512];   // loop() has a large stack; this keeps long debug lines intact on Serial
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logMsg(level, buf);
}

void addError(String msg)
{
    logMsg(LV_ERR, msg.c_str());
}

// Escapes s into dst as the body of a JSON string. Returns the number of bytes written.
static size_t escapeInto(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 3 < n; s++)
    {
        const char c = *s;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if ((unsigned char)c < 0x20) { dst[o++] = ' '; }
        else dst[o++] = c;
    }
    if (n) dst[o] = 0;
    return o;
}

// =====================================================
// API ENDPOINT FOR THE DASHBOARD LOG PANEL
// {"boot":123,"last":57,"logs":[{"s":55,"t":"10:30:37","l":1,"m":"..."},...]}
// l: 0 info, 1 ok, 2 warn, 3 error, 4 debug.
// Streamed from a static buffer in chunks: no heap is allocated per request (a dashboard that
// polls every few seconds must not fragment the heap that the TLS handshake needs).
// =====================================================
static char outBuf[1500];

void handleLogs()
{
    const bool problems = server.hasArg("problems");
    const uint32_t since = (uint32_t)server.arg("since").toInt();

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");

    size_t o = snprintf(outBuf, sizeof(outBuf), "{\"boot\":%lu,\"last\":%lu,\"logs\":[",
                        (unsigned long)bootId, (unsigned long)logSeq);
    bool first = true;

    auto flush = [&]() { if (o) { server.sendContent(outBuf, o); o = 0; } };
    auto emit = [&](const LogEntry &e)
    {
        if (o + 340 > sizeof(outBuf)) flush();          // one entry is at most ~310 bytes
        if (!first) outBuf[o++] = ',';
        first = false;
        o += snprintf(outBuf + o, sizeof(outBuf) - o, "{\"s\":%lu,\"t\":\"%s\",\"l\":%u,\"m\":\"",
                      (unsigned long)e.seq, e.ts, (unsigned)e.level);
        o += escapeInto(outBuf + o, sizeof(outBuf) - o, e.text);
        outBuf[o++] = '"';
        outBuf[o++] = '}';
    };

    if (problems)
    {
        const uint32_t n = problemTotal < PROBLEM_RING ? problemTotal : PROBLEM_RING;
        const uint32_t start = problemTotal - n;
        for (uint32_t i = 0; i < n; i++) emit(problemRing[(start + i) % PROBLEM_RING]);
    }
    else if (logSeq > 0)
    {
        const uint32_t oldest = logSeq > LOG_RING ? logSeq - LOG_RING + 1 : 1;
        uint32_t from = since + 1;
        if (from < oldest) from = oldest;
        for (uint32_t s = from; s <= logSeq; s++) emit(logRing[(s - 1) % LOG_RING]);
    }

    if (o + 4 > sizeof(outBuf)) flush();
    outBuf[o++] = ']';
    outBuf[o++] = '}';
    flush();
    server.sendContent("");   // ends the chunked reply
}
