package com.nr.irrigahome.presentation.home

import android.app.Application
import android.util.Log
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.nr.irrigahome.data.local.IrrigationSettingsRepository
import com.nr.irrigahome.data.remote.DeviceRepository
import com.nr.irrigahome.data.remote.IrrigationScheduleRepository
import com.nr.irrigahome.domain.model.ConflictCheckResult
import com.nr.irrigahome.domain.model.DeviceValidationState
import com.nr.irrigahome.domain.model.IrrigationMode
import com.nr.irrigahome.domain.model.IrrigationSchedule
import com.nr.irrigahome.util.MqttManager
import com.nr.irrigahome.util.ScheduleConflictDetector
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import javax.inject.Inject

@Suppress("SpellCheckingInspection")
@HiltViewModel
class IrrigaViewModel @Inject constructor(
    application: Application,
    private val mqttManager: MqttManager,
    private val settingsRepository: IrrigationSettingsRepository,
    private val scheduleRepository: IrrigationScheduleRepository,
    private val deviceRepository: DeviceRepository
) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "IrrigaViewModel"
        private const val DEFAULT_MANUAL_COOLDOWN_SECONDS = 120
        private const val DEFAULT_AUTOMATIC_COOLDOWN_SECONDS = 300
        private const val DEFAULT_WATERING_DURATION_SECONDS = 5
        private const val DEFAULT_AUTOMATIC_WATERING_DURATION_SECONDS = 10
        private const val DEFAULT_AUTOMATIC_SOIL_THRESHOLD = 30
        private const val DEFAULT_MAX_MANUAL_WATER_TEMP_C = 35
    }

    private val _isOnline = mutableStateOf(false)
    val isOnline: State<Boolean> = _isOnline

    private val _umidSolo = mutableIntStateOf(0)
    val umidSolo: State<Int> = _umidSolo

    private val _temperatura = mutableIntStateOf(0)
    val temperatura: State<Int> = _temperatura

    private val _umidAmbiente = mutableIntStateOf(0)
    val umidAmbiente: State<Int> = _umidAmbiente

    private val _nivelAgua = mutableStateOf("Vazio")
    val nivelAgua: State<String> = _nivelAgua

    private val _isWatering = mutableStateOf(false)
    val isWatering: State<Boolean> = _isWatering

    private val _wateringRemainingSeconds = mutableIntStateOf(0)
    val wateringRemainingSeconds: State<Int> = _wateringRemainingSeconds

    private val _cooldownRemainingSeconds = mutableIntStateOf(0)
    val cooldownRemainingSeconds: State<Int> = _cooldownRemainingSeconds

    private val _manualWaterDurationSeconds = mutableIntStateOf(DEFAULT_WATERING_DURATION_SECONDS)
    val manualWaterDurationSeconds: State<Int> = _manualWaterDurationSeconds

    private val _automaticWaterDurationSeconds = mutableIntStateOf(DEFAULT_AUTOMATIC_WATERING_DURATION_SECONDS)
    val automaticWaterDurationSeconds: State<Int> = _automaticWaterDurationSeconds

    private val _automaticSoilThreshold = mutableIntStateOf(DEFAULT_AUTOMATIC_SOIL_THRESHOLD)
    val automaticSoilThreshold: State<Int> = _automaticSoilThreshold

    private val _automaticCooldownSeconds = mutableIntStateOf(DEFAULT_AUTOMATIC_COOLDOWN_SECONDS)
    val automaticCooldownSeconds: State<Int> = _automaticCooldownSeconds

    private val _irrigationMode = mutableStateOf(IrrigationMode.MANUAL)
    val irrigationMode: State<IrrigationMode> = _irrigationMode

    private val _maxManualWaterTempC = mutableIntStateOf(DEFAULT_MAX_MANUAL_WATER_TEMP_C)
    val maxManualWaterTempC: State<Int> = _maxManualWaterTempC

    private val _saveStatus = mutableStateOf("")
    @Suppress("unused")
    val saveStatus: State<String> = _saveStatus

    private val _agendamentos = mutableStateListOf<IrrigationSchedule>()
    val agendamentos: List<IrrigationSchedule> = _agendamentos

    private val _isLoadingSchedules = mutableStateOf(false)
    val isLoadingSchedules: State<Boolean> = _isLoadingSchedules

    private val _deviceId = mutableStateOf("")
    val deviceId: State<String> = _deviceId

    private val _deviceDisplayName = mutableStateOf("")
    val deviceDisplayName: State<String> = _deviceDisplayName

    private val _deviceOwnerUid = mutableStateOf<String?>(null)
    val deviceOwnerUid: State<String?> = _deviceOwnerUid

    private val _deviceFirmwareVersion = mutableStateOf<String?>(null)
    val deviceFirmwareVersion: State<String?> = _deviceFirmwareVersion

    private val _hasLinkedDevice = mutableStateOf(false)
    val hasLinkedDevice: State<Boolean> = _hasLinkedDevice

    private val _deviceValidationState = mutableStateOf(DeviceValidationState.Idle)
    val deviceValidationState: State<DeviceValidationState> = _deviceValidationState

    private val _hasReceivedSoilTelemetry = mutableStateOf(false)

    private var wateringJob: Job? = null
    private var cooldownJob: Job? = null

    private val _pendingConflict = mutableStateOf<ConflictCheckResult.ConflictDetected?>(null)
    val pendingConflict: State<ConflictCheckResult.ConflictDetected?> = _pendingConflict

    private var schedulesLoaded = false
    private var pendingWaterRequestAfterLoad = false

    fun refreshLinkedDeviceState() {
        syncLinkedDeviceState()
    }

    fun resetSessionState() {
        wateringJob?.cancel()
        cooldownJob?.cancel()
        mqttManager.disconnect()
        deviceRepository.clearCache()

        updateOnMain {
            _isOnline.value = false
            _umidSolo.intValue = 0
            _temperatura.intValue = 0
            _umidAmbiente.intValue = 0
            _nivelAgua.value = "Vazio"
            _isWatering.value = false
            _wateringRemainingSeconds.intValue = 0
            _cooldownRemainingSeconds.intValue = 0
            _saveStatus.value = ""
            _agendamentos.clear()
            _deviceId.value = ""
            _deviceDisplayName.value = ""
            _deviceOwnerUid.value = null
            _deviceFirmwareVersion.value = null
            _hasLinkedDevice.value = false
            _deviceValidationState.value = DeviceValidationState.Idle
            _pendingConflict.value = null
        }

        schedulesLoaded = false
        pendingWaterRequestAfterLoad = false
        _hasReceivedSoilTelemetry.value = false
    }

    init {
        loadSavedSettings()

        mqttManager.onStatusReceived = { status ->
            updateOnMain {
                _isOnline.value = (status == "online")
                if (_isOnline.value) evaluateAutomaticIrrigation()
            }
        }

        mqttManager.onUmidSoloReceived = { valor ->
            updateOnMain {
                _umidSolo.intValue = valor
                _hasReceivedSoilTelemetry.value = true
                evaluateAutomaticIrrigation()
            }
        }

        mqttManager.onTempReceived = { valor ->
            updateOnMain {
                _temperatura.intValue = valor
                evaluateAutomaticIrrigation()
            }
        }

        mqttManager.onUmidAmbReceived = { valor ->
            updateOnMain { _umidAmbiente.intValue = valor }
        }

        mqttManager.onNivelAguaReceived = { estado ->
            updateOnMain {
                _nivelAgua.value = estado
                evaluateAutomaticIrrigation()
            }
        }

        mqttManager.onDeviceIdReceived = { id ->
            if (!_hasLinkedDevice.value) {
                mqttManager.disconnect()
            } else {
                val currentDeviceId = _deviceId.value
                updateOnMain { _deviceId.value = id }
                mqttManager.configureDeviceId(id)

                if (currentDeviceId.trim().lowercase() != id.trim().lowercase()) {
                    loadDeviceProfile(id)
                }
            }
        }

        syncLinkedDeviceState()
        carregarUltimosAgendamentos()
    }

    // ── Acionamento manual ────────────────────────────────────────────────────

    fun onWaterButtonClick() {
        if (_irrigationMode.value == IrrigationMode.AUTOMATIC) return
        if (!_isOnline.value || _nivelAgua.value != "Cheio") return
        if (isWaterBlockedByTemperature()) return
        if (isIrrigationBlocked()) return

        if (!schedulesLoaded) {
            Log.d(TAG, "Agendamentos não carregados ainda; aguardando reload para checar conflito.")
            pendingWaterRequestAfterLoad = true
            carregarUltimosAgendamentos()
            return
        }

        resolveConflictAndIrrigate()
    }

    fun onManualIrrigationConflictDismissed() {
        _pendingConflict.value = null
    }

    fun onManualIrrigationConflictConfirmed() {
        _pendingConflict.value = null

        if (_irrigationMode.value == IrrigationMode.AUTOMATIC) return
        if (!_isOnline.value || _nivelAgua.value != "Cheio") return
        if (isWaterBlockedByTemperature()) return
        if (isIrrigationBlocked()) return

        executeManualIrrigation()
    }

    // ── Configurações ─────────────────────────────────────────────────────────

    fun setIrrigationMode(mode: String) {
        val sanitizedMode = when (mode.uppercase()) {
            "AUTOMATICO", "AUTOMATIC" -> IrrigationMode.AUTOMATIC
            else -> IrrigationMode.MANUAL
        }
        _irrigationMode.value = sanitizedMode
        settingsRepository.saveIrrigationMode(sanitizedMode)
        publishCurrentMqttConfig()
        if (sanitizedMode == IrrigationMode.AUTOMATIC) evaluateAutomaticIrrigation()
    }

    fun setAutomaticSoilThreshold(threshold: Int) {
        if (threshold in listOf(20, 25, 30, 35, 40)) {
            _automaticSoilThreshold.intValue = threshold
            settingsRepository.saveAutomaticSoilThreshold(threshold)
            publishCurrentMqttConfig()
            evaluateAutomaticIrrigation()
        }
    }

    fun setAutomaticWaterDurationSeconds(seconds: Int) {
        if (seconds in listOf(5, 10, 15, 20)) {
            _automaticWaterDurationSeconds.intValue = seconds
            settingsRepository.saveAutomaticWaterDurationSeconds(seconds)
            publishCurrentMqttConfig()
        }
    }

    fun setAutomaticCooldownSeconds(seconds: Int) {
        if (seconds in listOf(180, 300, 600, 900)) {
            _automaticCooldownSeconds.intValue = seconds
            settingsRepository.saveAutomaticCooldownSeconds(seconds)
            publishCurrentMqttConfig()
        }
    }

    fun isAutomaticMode(): Boolean = _irrigationMode.value == IrrigationMode.AUTOMATIC

    fun isManualWaterBlockedByTemperature(): Boolean = isWaterBlockedByTemperature()

    fun setManualWaterDurationSeconds(seconds: Int) {
        if (seconds in listOf(5, 10, 15, 20)) {
            _manualWaterDurationSeconds.intValue = seconds
            settingsRepository.saveManualWaterDurationSeconds(seconds)
            publishCurrentMqttConfig()
        }
    }

    fun setMaxManualWaterTempC(tempC: Int) {
        if (tempC in listOf(25, 30, 35, 40)) {
            _maxManualWaterTempC.intValue = tempC
            settingsRepository.saveMaxManualWaterTempC(tempC)
        }
    }

    fun isWaterBlockedByTemperature(): Boolean =
        _isOnline.value && _temperatura.intValue > _maxManualWaterTempC.intValue

    override fun onCleared() {
        wateringJob?.cancel()
        cooldownJob?.cancel()
        _isWatering.value = false
        _wateringRemainingSeconds.intValue = 0
        mqttManager.onStatusReceived = null
        mqttManager.onUmidSoloReceived = null
        mqttManager.onTempReceived = null
        mqttManager.onUmidAmbReceived = null
        mqttManager.onNivelAguaReceived = null
        mqttManager.onDeviceIdReceived = null
        super.onCleared()
    }

    // ── Agendamentos ──────────────────────────────────────────────────────────

    @Suppress("unused")
    fun salvarAgendamento(diasSemana: List<Int>, hora: String, duracaoSegundos: Int) {
        scheduleRepository.saveSchedule(
            diasSemana = diasSemana,
            hora = hora,
            duracaoSegundos = duracaoSegundos,
            deviceId = _deviceId.value,
            onSuccess = { message ->
                _saveStatus.value = message
                carregarUltimosAgendamentos()
            },
            onError = { message ->
                _saveStatus.value = message
            }
        )
    }

    fun carregarUltimosAgendamentos() {
        _isLoadingSchedules.value = true
        scheduleRepository.loadLatestSchedules(
            onSuccess = { schedules ->
                updateOnMain {
                    _isLoadingSchedules.value = false
                    _agendamentos.clear()
                    _agendamentos.addAll(schedules)
                    schedulesLoaded = true

                    if (pendingWaterRequestAfterLoad) {
                        pendingWaterRequestAfterLoad = false
                        onSchedulesLoadedWithPendingRequest()
                    }
                }
            },
            onError = { error ->
                Log.e(TAG, "Falha ao carregar agendamentos", error)
                updateOnMain {
                    _isLoadingSchedules.value = false
                    _saveStatus.value = "Erro ao carregar agendamentos: ${error.message ?: "erro desconhecido"} ❌"

                    if (pendingWaterRequestAfterLoad) {
                        pendingWaterRequestAfterLoad = false
                        Log.w(TAG, "Falha ao carregar agendamentos para verificar conflito; irrigando sem verificação.")
                        executeManualIrrigation()
                    }
                }
            }
        )
    }

    fun removerAgendamento(scheduleId: String) {
        if (scheduleId.isBlank()) return
        scheduleRepository.removeSchedule(
            scheduleId = scheduleId,
            onSuccess = {
                _agendamentos.removeAll { it.id == scheduleId }
                _saveStatus.value = "Agendamento removido com sucesso ✅"
            },
            onError = { error ->
                Log.e(TAG, "Falha ao remover agendamento", error)
                _saveStatus.value = "Erro ao remover: ${error.message ?: "erro desconhecido"} ❌"
            }
        )
    }

    fun limparSaveStatus() {
        _saveStatus.value = ""
    }

    // ── Privado ───────────────────────────────────────────────────────────────

    private fun onSchedulesLoadedWithPendingRequest() {
        if (_irrigationMode.value == IrrigationMode.AUTOMATIC) return
        if (!_isOnline.value || _nivelAgua.value != "Cheio") return
        if (isWaterBlockedByTemperature()) return
        if (isIrrigationBlocked()) return
        resolveConflictAndIrrigate()
    }

    private fun resolveConflictAndIrrigate() {
        when (val result = ScheduleConflictDetector.check(activeSchedules = _agendamentos)) {
            is ConflictCheckResult.NoConflict -> executeManualIrrigation()
            is ConflictCheckResult.ConflictDetected -> {
                Log.d(TAG, "Conflito detectado: agendamento às ${result.scheduledTimeLabel} em ${result.minutesUntilStart}min")
                _pendingConflict.value = result
            }
        }
    }

    private fun executeManualIrrigation() {
        startWateringCycle(
            durationSeconds = getSelectedWateringDurationSeconds(),
            cooldownSeconds = getSelectedCooldownSeconds()
        )
    }

    private fun startWateringCycle(durationSeconds: Int, cooldownSeconds: Int) {
        wateringJob?.cancel()
        cooldownJob?.cancel()

        val safeDuration = durationSeconds.coerceAtLeast(1)
        val safeCooldown = cooldownSeconds.coerceAtLeast(1)
        val expiryTimestamp = System.currentTimeMillis() + (safeCooldown * 1000L)

        settingsRepository.saveCooldownExpiryTimestamp(expiryTimestamp)

        wateringJob = viewModelScope.launch(Dispatchers.Main.immediate) {
            _isWatering.value = true
            _wateringRemainingSeconds.intValue = safeDuration
            mqttManager.publishIrrigationCommand(safeDuration)

            try {
                while (_wateringRemainingSeconds.intValue > 0) {
                    delay(1000)
                    _wateringRemainingSeconds.intValue -= 1
                }
            } finally {
                _isWatering.value = false
                _wateringRemainingSeconds.intValue = 0
                startCooldownCountdown(safeCooldown)
            }
        }
    }

    private fun startCooldownCountdown(cooldownSeconds: Int) {
        cooldownJob?.cancel()
        _cooldownRemainingSeconds.intValue = cooldownSeconds

        cooldownJob = viewModelScope.launch(Dispatchers.Main.immediate) {
            while (_cooldownRemainingSeconds.intValue > 0) {
                delay(1000)
                _cooldownRemainingSeconds.intValue -= 1
            }
            settingsRepository.clearCooldownExpiryTimestamp()
            evaluateAutomaticIrrigation()
        }
    }

    private fun getSelectedWateringDurationSeconds(): Int =
        if (isAutomaticMode()) _automaticWaterDurationSeconds.intValue
        else _manualWaterDurationSeconds.intValue

    private fun getSelectedCooldownSeconds(): Int =
        if (isAutomaticMode()) _automaticCooldownSeconds.intValue
        else DEFAULT_MANUAL_COOLDOWN_SECONDS

    private fun publishCurrentMqttConfig() {
        mqttManager.publishSetConfig(
            mode = if (isAutomaticMode()) "auto" else "manual",
            threshold = _automaticSoilThreshold.intValue,
            durationSeconds = getSelectedWateringDurationSeconds(),
            cooldownSeconds = getSelectedCooldownSeconds()
        )
    }

    private fun isIrrigationBlocked(): Boolean =
        _isWatering.value || wateringJob?.isActive == true ||
        _cooldownRemainingSeconds.intValue > 0 || cooldownJob?.isActive == true

    private fun evaluateAutomaticIrrigation() {
        if (!isAutomaticMode()) return
        if (!_hasReceivedSoilTelemetry.value) return
        if (_umidSolo.intValue > _automaticSoilThreshold.intValue) return
        if (!_isOnline.value || _nivelAgua.value != "Cheio") return
        if (isWaterBlockedByTemperature()) return
        if (isIrrigationBlocked()) return

        startWateringCycle(
            durationSeconds = _automaticWaterDurationSeconds.intValue,
            cooldownSeconds = _automaticCooldownSeconds.intValue
        )
    }

    private fun updateOnMain(update: () -> Unit) {
        if (Dispatchers.Main.immediate.isDispatchNeeded(viewModelScope.coroutineContext)) {
            viewModelScope.launch(Dispatchers.Main.immediate) { update() }
        } else {
            update()
        }
    }

    private fun loadDeviceProfile(deviceId: String) {
        if (deviceId.isBlank()) {
            updateOnMain {
                _hasLinkedDevice.value = false
                _deviceValidationState.value = DeviceValidationState.Blocked
                _deviceDisplayName.value = ""
                _deviceOwnerUid.value = null
                _deviceFirmwareVersion.value = null
            }
            return
        }

        deviceRepository.fetchDeviceProfile(
            deviceId = deviceId,
            onSuccess = { profile ->
                updateOnMain {
                    _deviceId.value = deviceId
                    if (profile != null) {
                        _hasLinkedDevice.value = true
                        _deviceDisplayName.value = profile.displayName
                        _deviceOwnerUid.value = profile.ownerUid
                        _deviceFirmwareVersion.value = profile.firmwareVersion
                    } else {
                        _hasLinkedDevice.value = true
                        _deviceDisplayName.value = ""
                        _deviceOwnerUid.value = null
                        _deviceFirmwareVersion.value = null
                    }
                }
            },
            onError = { error ->
                Log.w(TAG, "Falha ao carregar metadados do device $deviceId", error)
                updateOnMain {
                    _hasLinkedDevice.value = true
                    _deviceId.value = deviceId
                    _deviceDisplayName.value = ""
                    _deviceOwnerUid.value = null
                    _deviceFirmwareVersion.value = null
                }
            }
        )
    }

    private fun syncLinkedDeviceState() {
        updateOnMain { _deviceValidationState.value = DeviceValidationState.Loading }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                val deviceId = selectPrimaryDeviceId(deviceIds)
                if (!deviceId.isNullOrBlank()) {
                    updateOnMain { _hasLinkedDevice.value = true }
                    updateOnMain { _deviceValidationState.value = DeviceValidationState.Linked }
                    mqttManager.configureDeviceId(deviceId)
                    updateOnMain { _deviceId.value = deviceId }
                    loadDeviceProfile(deviceId)
                    publishCurrentMqttConfig()

                    viewModelScope.launch(Dispatchers.IO) { mqttManager.connect() }
                } else {
                    updateOnMain {
                        _hasLinkedDevice.value = false
                        _deviceValidationState.value = DeviceValidationState.Blocked
                        _deviceId.value = ""
                        _deviceDisplayName.value = ""
                        _deviceOwnerUid.value = null
                        _deviceFirmwareVersion.value = null
                    }
                    mqttManager.disconnect()
                }
            },
            onError = { error ->
                Log.w(TAG, "Falha ao descobrir deviceId do usuário autenticado", error)
                updateOnMain {
                    _hasLinkedDevice.value = false
                    _deviceValidationState.value = DeviceValidationState.Blocked
                    _deviceId.value = ""
                    _deviceDisplayName.value = ""
                    _deviceOwnerUid.value = null
                    _deviceFirmwareVersion.value = null
                }
                mqttManager.disconnect()
            }
        )
    }

    private fun loadSavedSettings() {
        val settings = settingsRepository.loadSettings()
        _manualWaterDurationSeconds.intValue = settings.manualWaterDurationSeconds
        _maxManualWaterTempC.intValue = settings.maxManualWaterTempC
        _automaticWaterDurationSeconds.intValue = settings.automaticWaterDurationSeconds
        _automaticSoilThreshold.intValue = settings.automaticSoilThreshold
        _automaticCooldownSeconds.intValue = settings.automaticCooldownSeconds
        _irrigationMode.value = settings.irrigationMode
        restoreCooldownFromStorage(settings.cooldownExpiryTimestamp)
    }

    private fun selectPrimaryDeviceId(deviceIds: Set<String>): String? {
        return deviceIds.map { it.trim() }
            .firstOrNull { it.isNotBlank() && it != "_placeholder" }
            ?: deviceIds.firstOrNull { it.trim().isNotBlank() }
    }

    private fun restoreCooldownFromStorage(expiryTimestamp: Long) {
        if (expiryTimestamp == 0L) { _cooldownRemainingSeconds.intValue = 0; return }

        val remaining = expiryTimestamp - System.currentTimeMillis()
        if (remaining <= 0) {
            settingsRepository.clearCooldownExpiryTimestamp()
            _cooldownRemainingSeconds.intValue = 0
            return
        }

        _cooldownRemainingSeconds.intValue = ((remaining + 999) / 1000).toInt()

        cooldownJob?.cancel()
        cooldownJob = viewModelScope.launch(Dispatchers.Main.immediate) {
            while (_cooldownRemainingSeconds.intValue > 0) {
                delay(1000)
                _cooldownRemainingSeconds.intValue -= 1
            }
            settingsRepository.clearCooldownExpiryTimestamp()
            evaluateAutomaticIrrigation()
        }
    }
}
