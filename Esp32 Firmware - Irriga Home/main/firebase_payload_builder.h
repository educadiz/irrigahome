// firebase_payload_builder.h
// CORRIGIDO - Envia trigger como STRING, valida timestamps

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "secrets.h"
#include "irrigation_event_manager.h"

class FirebasePayloadBuilder {
public:
    static String buildIrrigationEventPayload(const IrrigationEvent& event) {
        String deviceId = getDeviceIdFromMac();
        String ownerUid = getOwnerUid();
        
        // ===== CORREÇÃO: Validar timestamps =====
        // Se startAt for 0 ou < 1000000000 (ano 2001), usar time atual
        time_t startTime = event.startAt;
        time_t endTime = event.endAt;
        time_t now = time(nullptr);
        
        if (startTime < 1000000000L) {
            startTime = now;
            Serial.println("[FIREBASE] startAt inválido, usando time atual");
        }
        if (endTime < 1000000000L) {
            endTime = now;
            Serial.println("[FIREBASE] endAt inválido, usando time atual");
        }
        
        char startIso[32];
        char endIso[32];
        IrrigationEventManager::formatIso8601(startTime, startIso, sizeof(startIso));
        IrrigationEventManager::formatIso8601(endTime, endIso, sizeof(endIso));
        
        JsonDocument doc;
        
        // Campos obrigatórios
        doc["deviceId"] = deviceId;
        doc["macAddress"] = deviceId;
        doc["historyCollection"] = "events";
        doc["ownerUid"] = ownerUid;
        
        // ===== CORREÇÃO: trigger como STRING =====
        doc["eventId"] = event.eventId;
        doc["startAt"] = startIso;
        doc["endAt"] = endIso;
        doc["durationSec"] = event.durationSec;
        doc["trigger"] = getTriggerString(event.trigger);  // ← Agora é STRING
        doc["stopReason"] = getStopReasonString(event.stopReason);
        
        // Campos de volume
        doc["totalVolumeLiters"] = event.totalVolumeLiters;
        doc["totalVolumeMl"] = event.totalVolumeMl;
        doc["accountingVolumeMl"] = event.accountingVolumeMl;
        doc["avgFlowRateLpm"] = event.avgFlowRateLpm;
        doc["nominalFlowRateMlPerMin"] = event.nominalFlowRateMlPerMin;
        doc["totalPulses"] = event.totalPulses;
        
        // Status
        doc["flowDetected"] = event.flowDetected;
        doc["flowStatus"] = event.flowStatus;
        doc["success"] = (event.stopReason == STOP_REASON_COMPLETED);
        
        // Timestamps
        doc["createdAt"] = startIso;
        doc["updatedAt"] = endIso;
        
        String payload;
        serializeJson(doc, payload);
        return payload;
    }
    
    static String buildFullPayload(const IrrigationEvent& event, 
                                   int soilHumidity, 
                                   int airHumidity, 
                                   float temperature, 
                                   bool waterLevel) {
        String deviceId = getDeviceIdFromMac();
        String ownerUid = getOwnerUid();
        
        // Validar timestamps
        time_t startTime = event.startAt;
        time_t endTime = event.endAt;
        time_t now = time(nullptr);
        
        if (startTime < 1000000000L) {
            startTime = now;
        }
        if (endTime < 1000000000L) {
            endTime = now;
        }
        
        char startIso[32];
        char endIso[32];
        IrrigationEventManager::formatIso8601(startTime, startIso, sizeof(startIso));
        IrrigationEventManager::formatIso8601(endTime, endIso, sizeof(endIso));
        
        JsonDocument doc;
        
        // Campos obrigatórios
        doc["deviceId"] = deviceId;
        doc["macAddress"] = deviceId;
        doc["historyCollection"] = "events";
        doc["ownerUid"] = ownerUid;
        
        // Evento
        doc["eventId"] = event.eventId;
        doc["startAt"] = startIso;
        doc["endAt"] = endIso;
        doc["durationSec"] = event.durationSec;
        doc["trigger"] = getTriggerString(event.trigger);
        doc["stopReason"] = getStopReasonString(event.stopReason);
        
        // Sensores (usar valores recebidos ou 0 como fallback)
        doc["soilHumidity"] = (soilHumidity > 0) ? soilHumidity : 0;
        doc["airHumidity"] = (airHumidity > 0) ? airHumidity : 0;
        doc["temperature"] = (temperature > 0) ? temperature : 0.0;
        doc["waterLevel"] = waterLevel ? "Cheio" : "Vazio";
        
        // Volume
        doc["totalVolumeLiters"] = event.totalVolumeLiters;
        doc["totalVolumeMl"] = event.totalVolumeMl;
        doc["accountingVolumeMl"] = event.accountingVolumeMl;
        doc["avgFlowRateLpm"] = event.avgFlowRateLpm;
        doc["nominalFlowRateMlPerMin"] = event.nominalFlowRateMlPerMin;
        doc["totalPulses"] = event.totalPulses;
        doc["flowDetected"] = event.flowDetected;
        doc["flowStatus"] = event.flowStatus;
        doc["success"] = (event.stopReason == STOP_REASON_COMPLETED);
        
        doc["createdAt"] = startIso;
        doc["updatedAt"] = endIso;
        
        String payload;
        serializeJson(doc, payload);
        return payload;
    }
    
private:
    static String getDeviceIdFromMac() {
        String deviceId = WiFi.macAddress();
        deviceId.replace(":", "");
        deviceId.replace("-", "");
        deviceId.toLowerCase();
        return deviceId;
    }
    
    // ===== CORREÇÃO: Converter trigger para STRING =====
    static String getTriggerString(IrrigationTriggerType trigger) {
        switch (trigger) {
            case TRIGGER_MANUAL: return "manual";
            case TRIGGER_AUTOMATIC: return "automatic";
            case TRIGGER_SCHEDULE: return "schedule";
            default: return "manual";  // Fallback para manual
        }
    }
    
    static String getStopReasonString(IrrigationStopReason reason) {
        switch (reason) {
            case STOP_REASON_COMPLETED: return "completed";
            case STOP_REASON_MANUAL: return "manual";
            case STOP_REASON_NO_WATER: return "no_water";
            case STOP_REASON_NO_FLOW: return "no_flow";
            default: return "completed";
        }
    }
};