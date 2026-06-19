// =============================================================================
// secrets.example.h — Modelo de credenciais para o IrrigaHome
// =============================================================================
// Copie este arquivo para secrets.h e preencha com seus valores reais.
// NÃO edite este arquivo com dados reais — ele é commitado no repositório.
// =============================================================================

#pragma once

// ---- Wi-Fi ------------------------------------------------------------------
#define WIFI_SSID "SUA_REDE_WIFI"
#define WIFI_PASS "SUA_SENHA_WIFI"

// ---- MQTT (HiveMQ Cloud) ----------------------------------------------------
#define MQTT_SERVER "SEU_BROKER.s1.eu.hivemq.cloud"
#define MQTT_PORT   8883
#define MQTT_USER   "SEU_USUARIO_MQTT"
#define MQTT_PASS   "SUA_SENHA_MQTT"

// ---- Firebase Cloud Functions -----------------------------------------------
#define FIREBASE_PROJECT_URL "https://us-central1-SEU_PROJETO.cloudfunctions.net"

// ---- Web UI — senha de autenticação local -----------------------------------
#define WEBSERVER_PASS "SUA_SENHA_WEB"
