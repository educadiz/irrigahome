// Responsabilidade: centralizar parametros e constantes do firmware.
// O que faz: define credenciais, topicos MQTT, pinos, parametros de display/relogio
// e valores padrao usados pelos demais modulos.

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>

#pragma once

// Credenciais e dados sensíveis estão em secrets.h (não commitado no repositório).
// Copie secrets.example.h para secrets.h e preencha com seus valores.
#include "secrets.h"

// Nome amigavel local do dispositivo.
#define DEVICE_NAME "Jardim_01"

static inline String getDeviceIdFromMac() {
	String deviceId = WiFi.macAddress();
	deviceId.replace(":", "");
	deviceId.replace("-", "");
	deviceId.toLowerCase();
	return deviceId;
}

static inline String getMqttTelemetryTopic() {
	return String("irrigahome/") + getDeviceIdFromMac() + "/telemetry";
}

static inline String getMqttStatusTopic() {
	return String("irrigahome/") + getDeviceIdFromMac() + "/status";
}

static inline String getMqttCommandsTopic() {
	return String("irrigahome/") + getDeviceIdFromMac() + "/commands";
}

// Funções para gerenciamento do Owner UID
static inline String getOwnerUid() {
    if (LittleFS.begin()) {
        if (LittleFS.exists(OWNER_UID_STORAGE)) {
            File f = LittleFS.open(OWNER_UID_STORAGE, "r");
            if (f) {
                String uid = f.readString();
                uid.trim();
                f.close();
                if (uid.length() > 0) {
                    return uid;
                }
            }
        }
    }
    return String(DEFAULT_OWNER_UID);
}

static inline void setOwnerUid(const String& uid) {
    if (LittleFS.begin()) {
        File f = LittleFS.open(OWNER_UID_STORAGE, "w");
        if (f) {
            f.print(uid);
            f.close();
        }
    }
}

// PINOS
#define DHTPIN 4
#define RESET 27
#define DHTTYPE DHT22
#define SOIL_PIN 32
#define WATER_LEVEL_PIN 5
#define PUMP_PIN 18
#define PUMP_LED 2  
#define PUMP_NOMINAL_FLOW_ML_PER_MIN 600.0f
#define PUMP_NOMINAL_FLOW_LPM (PUMP_NOMINAL_FLOW_ML_PER_MIN / 1000.0f)

// DISPLAY
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DISPLAY_SDA_PIN 21
#define DISPLAY_SCL_PIN 22
#define DISPLAY_DC_PIN 16
#define DISPLAY_CS_PIN 17
#define DISPLAY_RST_PIN 19

// RELOGIO (NTP)
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SECONDS -10800
#define DAYLIGHT_OFFSET_SECONDS 0

// SOLO
#define SOLO_SECO 4095
#define SOLO_UMIDO 0
#define SOIL_ADC_SAMPLES 6

// ACIONAMENTO DA BOMBA
#define PUMP_ON HIGH
#define PUMP_OFF LOW
#define PUMP_LED_ON HIGH
#define PUMP_LED_OFF LOW

// AUTO MODO
#define THRESHOLD_UMIDADE_PADRAO 40
#define DURACAO_IRRIGACAO_PADRAO 10
#define DURACAO_IRRIGACAO_MAXIMA 1800
#define COOLDOWN_IRRIGACAO_PADRAO 60

// Protecao: irrigacao por agenda ou regra automatica so se solo < limiar (com persistencia)
#define SOLO_UMIDO_BLOQUEIO_PCT 75
#define SOLO_UMIDO_BLOQUEIO_PERSISTENCIA_MS 60000UL