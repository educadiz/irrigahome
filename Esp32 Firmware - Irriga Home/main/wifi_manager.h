// Responsabilidade: declarar a interface de conectividade Wi-Fi e sincronismo de relogio.
// O que faz: expor metodos para conectar na rede e informar se o NTP foi sincronizado.

#pragma once
#include <WiFi.h>

class WiFiManagerCustom {
public:
    void connect();
    void loop();
    bool isClockReady();

private:
    bool syncClock();
    bool clockReady = false;
    bool wasConnected = false;
    bool clockSyncPending = false;
    unsigned long lastReconnectAttempt = 0;
    unsigned long lastClockSyncAttempt = 0;
    unsigned long reconnectBackoffMs = 5000UL;
    unsigned long clockSyncBackoffMs = 5000UL;
};