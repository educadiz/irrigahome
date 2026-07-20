package com.nr.irrigahome.util

import com.nr.irrigahome.BuildConfig
import org.eclipse.paho.client.mqttv3.*
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONObject
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class MqttManager @Inject constructor() {
    private val brokerUrl = BuildConfig.MQTT_BROKER_URL
    private val username = BuildConfig.MQTT_USERNAME
    private val mqttPassword = BuildConfig.MQTT_PASSWORD

    private val clientId = "IrrigaHome_${System.currentTimeMillis()}"
    private var client: MqttClient? = null

    @Volatile private var connectInProgress = false
    @Volatile private var lastStatusTime = System.currentTimeMillis()
    @Volatile private var monitoring = false
    @Volatile private var activeDeviceId: String? = null
    @Volatile private var subscribedDeviceId: String? = null

    var onStatusReceived: ((String) -> Unit)? = null
    var onUmidSoloReceived: ((Int) -> Unit)? = null
    var onTempReceived: ((Int) -> Unit)? = null
    var onUmidAmbReceived: ((Int) -> Unit)? = null
    var onNivelAguaReceived: ((String) -> Unit)? = null
    var onDeviceIdReceived: ((String) -> Unit)? = null

    fun configureDeviceId(deviceId: String) {
        val normalized = normalizeDeviceId(deviceId)
        if (normalized.isNullOrBlank()) return
        if (activeDeviceId == normalized) return

        val previousDeviceId = subscribedDeviceId
        activeDeviceId = normalized
        if (client?.isConnected == true) {
            if (!previousDeviceId.isNullOrBlank() && previousDeviceId != normalized) {
                unsubscribeTopics(previousDeviceId)
            }
            subscribeTopics()
        }
    }

    fun connect() {
        try {
            if (client?.isConnected == true || connectInProgress) return
            connectInProgress = true

            val mqttClient = client ?: MqttClient(brokerUrl, clientId, MemoryPersistence()).also { client = it }

            mqttClient.setCallback(object : MqttCallbackExtended {
                override fun connectComplete(reconnect: Boolean, serverURI: String?) {
                    println("✅ MQTT conectado${if (reconnect) " (reconectado)" else ""}")
                    lastStatusTime = System.currentTimeMillis()
                    onStatusReceived?.invoke("online")
                    subscribeTopics(force = true)
                }

                override fun connectionLost(cause: Throwable?) {
                    println("❌ Conexão perdida: ${cause?.message}")
                    subscribedDeviceId = null
                }

                override fun messageArrived(topic: String?, message: MqttMessage?) {
                    val msg = message?.toString().orEmpty()
                    println("📥 [$topic] $msg")
                    when (topic) {
                        telemetryTopic() -> handleTelemetryMessage(msg)
                        statusTopic() -> handleStatusMessage(msg)
                    }
                }

                override fun deliveryComplete(token: IMqttDeliveryToken?) {}
            })

            val options = MqttConnectOptions().apply {
                isAutomaticReconnect = true
                isCleanSession = true
                userName = username
                password = mqttPassword.toCharArray()
                connectionTimeout = 10
                keepAliveInterval = 60
            }

            println("🔌 Conectando ao HiveMQ Cloud...")
            mqttClient.connect(options)
            startMonitoring()
        } catch (e: Exception) {
            println("❌ Falha ao conectar: ${e.message}")
            e.printStackTrace()
        } finally {
            connectInProgress = false
        }
    }

    private fun startMonitoring() {
        if (monitoring) return
        monitoring = true

        Thread {
            println("🧠 Monitoramento de conexão iniciado")
            while (monitoring) {
                val now = System.currentTimeMillis()
                if (now - lastStatusTime > 30000) {
                    println("⚠️ Timeout detectado → OFFLINE")
                    onStatusReceived?.invoke("offline")
                    lastStatusTime = now
                }
                Thread.sleep(2000)
            }
        }.start()
    }

    private fun subscribeTopics(force: Boolean = false) {
        try {
            if (client?.isConnected != true) return
            val deviceId = activeDeviceId ?: return
            if (!force && subscribedDeviceId == deviceId) return

            println("📡 Inscrevendo nos tópicos...")
            val telemetry = telemetryTopic(deviceId) ?: return
            val status = statusTopic(deviceId) ?: return
            val commands = commandsTopic(deviceId) ?: return

            client?.subscribe(telemetry, 1)
            client?.subscribe(status, 1)
            client?.subscribe(commands, 1)
            subscribedDeviceId = deviceId
        } catch (e: Exception) {
            println("❌ Erro ao inscrever: ${e.message}")
        }
    }

    private fun unsubscribeTopics(deviceId: String) {
        if (client?.isConnected != true) return
        val topics = listOfNotNull(telemetryTopic(deviceId), statusTopic(deviceId), commandsTopic(deviceId))
        if (topics.isNotEmpty()) client?.unsubscribe(topics.toTypedArray())
    }

    fun disconnect() {
        try {
            monitoring = false
            connectInProgress = false
            subscribedDeviceId = null
            client?.let {
                if (it.isConnected) it.disconnect()
                it.close()
            }
        } catch (e: Exception) {
            e.printStackTrace()
        } finally {
            client = null
        }
    }

    fun publishBomba(ligar: Boolean) {
        sendIrrigationCommand(if (ligar) 10 else 0, trigger = "manual")
    }

    fun publishIrrigationCommand(durationSeconds: Int) {
        sendIrrigationCommand(durationSeconds, trigger = "manual")
    }

    fun publishIrrigationCommand(durationSeconds: Int, trigger: String) {
        sendIrrigationCommand(durationSeconds, trigger)
    }

    fun publishSetConfig(mode: String, threshold: Int, durationSeconds: Int, cooldownSeconds: Int) {
        sendAutoConfig(mode = mode, threshold = threshold, duration = durationSeconds, cooldown = cooldownSeconds)
    }

    fun sendIrrigationCommand(duration: Int, trigger: String = "manual") {
        try {
            val safeDuration = duration.coerceAtLeast(0)
            val normalizedTrigger = when (trigger.trim().lowercase()) {
                "automatic", "automatico", "auto" -> "automatic"
                "schedule", "scheduled", "agendado" -> "schedule"
                else -> "manual"
            }
            val payload = JSONObject()
                .put("action", "irrigate")
                .put("deviceId", activeDeviceId ?: "")
                .put("macAddress", activeDeviceId ?: "")
                .put("trigger", normalizedTrigger)
                .put("duration", safeDuration)

            publishCommandPayload(payload)
            println("📤 Enviado comando irrigate: $payload")
        } catch (e: Exception) {
            println("❌ Erro ao publicar: ${e.message}")
        }
    }

    fun sendAutoConfig(mode: String, threshold: Int, duration: Int, cooldown: Int) {
        try {
            val modePayload = normalizeMode(mode) ?: "manual"
            val payload = JSONObject()
                .put("action", "setConfig")
                .put("deviceId", activeDeviceId ?: "")
                .put("macAddress", activeDeviceId ?: "")
                .put("mode", modePayload)
                .put("threshold", threshold)
                .put("duration", duration.coerceAtLeast(0))
                .put("cooldown", cooldown.coerceAtLeast(0))

            publishCommandPayload(payload)
            println("📤 Enviado comando setConfig: $payload")
        } catch (e: Exception) {
            println("❌ Erro ao publicar configuração: ${e.message}")
        }
    }

    private fun publishCommandPayload(payload: JSONObject) {
        if (client?.isConnected != true) {
            connect()
        }

        val commands = commandsTopic() ?: "irrigahome/commands"
        val message = MqttMessage(payload.toString().toByteArray()).apply {
            qos = 1
            isRetained = false
        }
        client?.publish(commands, message)
    }

    private fun handleTelemetryMessage(payload: String) {
        try {
            val telemetry = JSONObject(payload)
            lastStatusTime = System.currentTimeMillis()
            onStatusReceived?.invoke("online")

            if (telemetry.has("soilMoisture")) onUmidSoloReceived?.invoke(telemetry.optInt("soilMoisture"))
            if (telemetry.has("temperature")) onTempReceived?.invoke(telemetry.optInt("temperature"))
            if (telemetry.has("humidity")) onUmidAmbReceived?.invoke(telemetry.optInt("humidity"))

            if (telemetry.has("waterLevel")) {
                val waterLevel = telemetry.optString("waterLevel")
                if (waterLevel == "Cheio" || waterLevel == "Vazio") onNivelAguaReceived?.invoke(waterLevel)
            }

            if (telemetry.has("mode")) normalizeMode(telemetry.optString("mode"))

            if (telemetry.has("deviceId")) {
                val deviceId = telemetry.optString("deviceId")
                if (deviceId.isNotBlank()) {
                    onDeviceIdReceived?.invoke(normalizeDeviceId(deviceId) ?: deviceId.trim())
                }
            }
        } catch (e: Exception) {
            println("❌ Telemetria JSON inválida: ${e.message}")
        }
    }

    private fun handleStatusMessage(payload: String) {
        val status = payload.trim().lowercase()
        if (status == "online" || status == "offline") {
            if (status == "online") lastStatusTime = System.currentTimeMillis()
            onStatusReceived?.invoke(status)
        }
    }

    private fun normalizeMode(mode: String?): String? {
        return when (mode?.trim()?.lowercase()) {
            "auto", "automatic", "automatico" -> "auto"
            "manual" -> "manual"
            else -> null
        }
    }

    private fun normalizeDeviceId(deviceId: String?): String? {
        return deviceId?.trim()?.lowercase()?.replace(":", "")?.replace("-", "")?.takeIf { it.isNotBlank() }
    }

    private fun telemetryTopic(deviceId: String? = activeDeviceId): String? =
        deviceId?.let { "irrigahome/$it/telemetry" }

    private fun statusTopic(deviceId: String? = activeDeviceId): String? =
        deviceId?.let { "irrigahome/$it/status" }

    private fun commandsTopic(deviceId: String? = activeDeviceId): String? =
        deviceId?.let { "irrigahome/$it/commands" }
}
