#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>
#include <WebServer.h>

extern WebServer server;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

// Log levels. The dashboard colours them: INFO white, OK green, WARN amber, ERR red, DBG grey.
enum LogLevel : uint8_t { LV_INFO = 0, LV_OK = 1, LV_WARN = 2, LV_ERR = 3, LV_DBG = 4 };

// Every call prints to Serial AND stores the line in RAM for the web dashboard (/logs).
// Do not put a trailing "\n" in the message: the newline is added for you.
void logMsg(uint8_t level, const char *msg);
void logPrintf(uint8_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Optional persistent sink (main.cpp writes warnings/errors to the SD card so they survive a
// reboot). Called after the RAM copy is made; re-entrancy is blocked inside logging.cpp.
typedef void (*LogSink)(uint8_t level, const char *msg);
void logSetSink(LogSink fn);

// Kept for compatibility: same as logMsg(LV_ERR, ...)
void addError(String msg);

// GET /logs?since=<seq>        every line newer than <seq> (incremental polling)
// GET /logs?problems=1         last warnings + errors (survives longer than the main buffer)
void handleLogs();

#endif
