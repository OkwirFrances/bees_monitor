#ifndef CONNECTION_H
#define CONNECTION_H

#include <WiFi.h>

void startAP();
bool connectSTA();
void disconnectSTA();
const char *staLastFailReason();   // short human text for the last failed connect ("network not found", ...)
void setupTime();
String getTimestamp();

#endif
