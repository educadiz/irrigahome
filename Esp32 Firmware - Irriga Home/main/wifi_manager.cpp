// Responsabilidade: implementar conexao Wi-Fi e sincronizacao de horario via NTP.
// O que faz: conecta o ESP32 na rede, tenta sincronizar o relogio e mantem estado
// indicando se o horario esta pronto para uso em agendamentos.

#include "wifi_manager.h"
#include "config.h"
#include "firmware_logger.h"
#include <time.h>
#include <esp_wifi.h>

static const unsigned long WIFI_RECONNECT_BACKOFF_MAX_MS = 60000UL;
static const unsigned long CLOCK_SYNC_BACKOFF_MAX_MS = 60000UL;

// Attempt NTP synchronization using configTime(). Sets `clockReady` on success.
bool WiFiManagerCustom::syncClock() {
    configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER);

    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 10)) {
        char timeBuffer[32];
        strftime(timeBuffer, sizeof(timeBuffer), "%d/%m/%Y %H:%M:%S", &timeInfo);
        fwLogf("INFO", "NET", "Relogio sincronizado: %s", timeBuffer);
        clockReady = true;
        clockSyncPending = false;
        clockSyncBackoffMs = 5000UL;
        return true;
    }

    fwLogLine("WARN", "NET", "NTP nao sincronizado; agendamentos por hora podem falhar");
    clockReady = false;
    clockSyncPending = true;
    return false;
}

// Start non-blocking WiFi connection flow. Does not block setup() and initializes backoff timers.
void WiFiManagerCustom::connect() {
    String macAddress = WiFi.macAddress();
    String deviceId = getDeviceIdFromMac();
    fwLogSection("NET", "Conexao Wi-Fi");
    fwLogLine("INFO", "NET", "Conectando Wi-Fi...");
    fwLogf("INFO", "NET", "MAC raw: %s", macAddress.c_str());
    fwLogf("INFO", "NET", "deviceId normalizado: %s", deviceId.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    wasConnected = false;
    clockReady = false;
    clockSyncPending = true;
    lastReconnectAttempt = millis();
    lastClockSyncAttempt = 0;
    reconnectBackoffMs = 5000UL;
    clockSyncBackoffMs = 5000UL;
}

// Non-blocking WiFi maintenance loop: handles reconnection backoff and schedules NTP sync.
void WiFiManagerCustom::loop() {
    wl_status_t status = WiFi.status();
    unsigned long now = millis();

    if (status == WL_CONNECTED) {
        if (!wasConnected) {
            String localIp = WiFi.localIP().toString();
            String macAddress = WiFi.macAddress();
            String deviceId = getDeviceIdFromMac();
            fwLogSection("NET", "Wi-Fi reconectado");
            fwLogf("INFO", "NET", "IP: %s", localIp.c_str());
            fwLogf("INFO", "NET", "MAC raw: %s", macAddress.c_str());
            fwLogf("INFO", "NET", "deviceId normalizado: %s", deviceId.c_str());
            clockReady = false;
            clockSyncPending = true;
            lastClockSyncAttempt = 0;
        }
        wasConnected = true;

        if (clockSyncPending && (now - lastClockSyncAttempt) >= clockSyncBackoffMs) {
            lastClockSyncAttempt = now;
            if (syncClock()) {
                return;
            }

            if (clockSyncBackoffMs < CLOCK_SYNC_BACKOFF_MAX_MS) {
                clockSyncBackoffMs *= 2UL;
                if (clockSyncBackoffMs > CLOCK_SYNC_BACKOFF_MAX_MS) {
                    clockSyncBackoffMs = CLOCK_SYNC_BACKOFF_MAX_MS;
                }
            }
        }

        return;
    }

    if (wasConnected) {
        fwLogLine("WARN", "NET", "Wi-Fi desconectado");
        clockReady = false;
        clockSyncPending = false;
        wasConnected = false;
    }

    if ((now - lastReconnectAttempt) < reconnectBackoffMs) {
        return;
    }

    lastReconnectAttempt = now;
    fwLogLine("INFO", "NET", "Tentando reconectar Wi-Fi...");

    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    if (reconnectBackoffMs < WIFI_RECONNECT_BACKOFF_MAX_MS) {
        reconnectBackoffMs *= 2UL;
        if (reconnectBackoffMs > WIFI_RECONNECT_BACKOFF_MAX_MS) {
            reconnectBackoffMs = WIFI_RECONNECT_BACKOFF_MAX_MS;
        }
    }
}

bool WiFiManagerCustom::isClockReady() {
    return clockReady;
}