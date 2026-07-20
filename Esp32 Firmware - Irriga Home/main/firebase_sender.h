// firebase_sender.h
// Responsabilidade: enviar eventos de irrigação para o Firebase Firestore
// Gerencia a comunicação HTTP com as Cloud Functions
// USO: Chamar sendPendingEvents() periodicamente no loop()

#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "secrets.h"
#include "irrigation_event_manager.h"
#include "firebase_payload_builder.h"
#include "firmware_logger.h"

class FirebaseSender {
public:
    // Envia um único evento para o Firebase via POST com JSON
    static bool sendIrrigationEvent(const IrrigationEvent& event) {
        if (!WiFi.isConnected()) {
            fwLogLine("WARN", "FIRE", "WiFi desconectado, não é possível enviar evento");
            return false;
        }

        String payload = FirebasePayloadBuilder::buildIrrigationEventPayload(event);
        
        HTTPClient http;
        WiFiClientSecure httpsClient;
        httpsClient.setInsecure();
        httpsClient.setTimeout(15000);

        http.begin(httpsClient, FIREBASE_SAVE_EVENT_URL);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(15000);
        
        fwLogf("INFO", "FIRE", "Enviando evento para Firebase: %s", event.eventId);
        Serial.println("📤 Payload:");
        Serial.println(payload);
        
        int httpResponseCode = http.POST(payload);
        String response = http.getString();
        
        http.end();
        
        if (httpResponseCode == 200 || httpResponseCode == 201) {
            fwLogf("INFO", "FIRE", "Evento salvo com sucesso no Firebase: %d", httpResponseCode);
            Serial.println("✅ Resposta: " + response);
            return true;
        } else {
            fwLogf("ERROR", "FIRE", "Erro ao salvar evento: %d - %s", httpResponseCode, response.c_str());
            return false;
        }
    }
    
    // Envia evento com retry
    static bool sendIrrigationEventWithRetry(const IrrigationEvent& event, int maxRetries = 3) {
        for (int attempt = 1; attempt <= maxRetries; attempt++) {
            fwLogf("INFO", "FIRE", "Tentativa %d de %d para enviar evento %s", attempt, maxRetries, event.eventId);
            
            if (sendIrrigationEvent(event)) {
                return true;
            }
            
            if (attempt < maxRetries) {
                delay(2000 * attempt);
            }
        }
        
        fwLogf("ERROR", "FIRE", "Falha ao enviar evento %s após %d tentativas", event.eventId, maxRetries);
        return false;
    }
    
    // Processa todos os eventos pendentes na fila
    static void sendPendingEvents() {
        IrrigationEvent event;
        int pendingCount = irrigationEventManager.getPendingEventCount();
        
        if (pendingCount == 0) {
            return;
        }
        
        fwLogf("INFO", "FIRE", "Processando %d eventos pendentes", pendingCount);
        
        while (irrigationEventManager.getNextPendingEvent(&event)) {
            if (sendIrrigationEventWithRetry(event)) {
                irrigationEventManager.removeFromHistory(event.eventId);
                irrigationEventManager.markEventSent(event.eventId);
                fwLogf("INFO", "FIRE", "Evento enviado e removido da fila: %s", event.eventId);
            } else {
                fwLogf("WARN", "FIRE", "Evento mantido na fila para próxima tentativa: %s", event.eventId);
                break;
            }
        }
    }
};