#ifndef CONNECTION_H
#define CONNECTION_H

#include <WiFi.h>

void startAP();
bool connectSTA();
void disconnectSTA();
void setupTime();
String getTimestamp();

#endif
