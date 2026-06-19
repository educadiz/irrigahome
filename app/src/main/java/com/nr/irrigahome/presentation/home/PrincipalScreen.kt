/**
 * Arquivo: Principal.kt
 * 
 * Responsabilidade:
 * - Tela principal da aplicação após login bem-sucedido
 * - Exibe painéis de telemetria (umidade do solo, temperatura, umidade do ar, nível de água)
 * - Renderiza card de status da planta com imagem dinâmica baseada em umidade
 * - Renderiza card de nível de água do reservatório
 * - Implementa botão de irrigação manual/automática com estados dinâmicos
 * - Monitora estado de conexão com dispositivo IoT e internet
 * - Exibe popups de carregamento enquanto aguarda dados do dispositivo IoT
 * - Exibe popup de aviso quando não há conexão com internet
 * - Implementa menu de ajustes (SettingsDialog) para configuração de:
 *   - Modo de irrigação (Manual/Automático)
 *   - Duração de irrigação
 *   - Threshold de umidade automática
 *   - Cooldown entre irrigações
 *   - Temperatura máxima de segurança
 * - Renderiza componentes responsivos adaptados para diferentes tamanhos de tela
 * - Integra com IrrigaViewModel para gerenciar estado e lógica da tela
 */

package com.nr.irrigahome.presentation.home

import com.nr.irrigahome.domain.model.ConflictCheckResult
import com.nr.irrigahome.domain.model.IrrigationMode
import com.nr.irrigahome.ui.components.LoadingPopup
import com.nr.irrigahome.ui.components.ManualIrrigationConflictDialog
import com.nr.irrigahome.ui.components.ScreenHeader

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.DeviceThermostat
import androidx.compose.material.icons.filled.Opacity
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.viewmodel.compose.viewModel
import com.nr.irrigahome.ui.components.DeviceConnectingPopup
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.R
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardDividerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen

@Composable
// Aqui vai a tela principal e seus ajustes:
//
fun PrincipalScreen() {
    val viewModel: IrrigaViewModel = viewModel()
    val isInternetAvailable = rememberInternetAvailable()

    // Aqui monitoro o que vem do irragaviewmodel.kt
    val isWatering = viewModel.isWatering.value
    val isOnline = viewModel.isOnline.value
    val umidSolo = viewModel.umidSolo.value
    val localTemp = viewModel.temperatura.value
    val umidAmbiente = viewModel.umidAmbiente.value
    val waterLevel = viewModel.nivelAgua.value
    val cooldownRemainingSeconds = viewModel.cooldownRemainingSeconds.value
    val wateringRemainingSeconds = viewModel.wateringRemainingSeconds.value
    val manualWaterDurationSeconds = viewModel.manualWaterDurationSeconds.value
    val automaticWaterDurationSeconds = viewModel.automaticWaterDurationSeconds.value
    val automaticSoilThreshold = viewModel.automaticSoilThreshold.value
    val automaticCooldownSeconds = viewModel.automaticCooldownSeconds.value
    val irrigationMode = viewModel.irrigationMode.value
    val maxManualWaterTempC = viewModel.maxManualWaterTempC.value
    val manualWaterBlockedByTemp = viewModel.isManualWaterBlockedByTemperature()
    val deviceId = viewModel.deviceId.value
    val deviceDisplayName = viewModel.deviceDisplayName.value
    val hasLinkedDevice = viewModel.hasLinkedDevice.value
    val pendingConflict = viewModel.pendingConflict.value
    val isWaitingForIotTelemetry = hasLinkedDevice && isOnline && umidSolo == 0 && localTemp == 0

    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        val compactWidth = maxWidth < 380.dp
        val compactHeight = maxHeight < 720.dp
        val horizontalPadding = if (compactWidth) 8.dp else 12.dp
        val verticalPadding = if (compactHeight) 6.dp else 8.dp
        val sectionSpacing = if (compactHeight) 6.dp else 8.dp

        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = horizontalPadding, vertical = verticalPadding)
        ) {
            Column(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(sectionSpacing)
            ) {
                CompactScreenHeader(
                    title = "Irrigação e Monitoramento",
                    subTitle = "Monitore a saúde de suas plantas"
                )

                Column(
                    modifier = Modifier
                        .weight(1f, fill = true),
                    verticalArrangement = Arrangement.spacedBy(sectionSpacing)
                ) {
                    if (hasLinkedDevice) {
                        PlantStatusCard(
                            umidSolo = umidSolo,
                            irrigadorOnline = isOnline
                        )
                        MoistureCard(
                            umidSolo = umidSolo,
                            localTemp = localTemp,
                            umidAmbiente = umidAmbiente,
                            irrigadorOnline = isOnline
                        )
                        WaterLevelCard(
                            level = waterLevel,
                            irrigadorOnline = isOnline
                        )
                    } else {
                        NoLinkedDeviceCard(horizontalPadding = horizontalPadding)
                    }
                }
                // Botão de acionar o IOT para irrigar
                if (hasLinkedDevice) {
                    ToolsMenuButton(
                        irrigationMode = irrigationMode,
                        manualWaterDurationSeconds = manualWaterDurationSeconds,
                        automaticWaterDurationSeconds = automaticWaterDurationSeconds,
                        automaticSoilThreshold = automaticSoilThreshold,
                        automaticCooldownSeconds = automaticCooldownSeconds,
                        maxManualWaterTempC = maxManualWaterTempC,
                        deviceId = deviceId,
                        deviceDisplayName = deviceDisplayName,
                        horizontalPadding = horizontalPadding,
                        onModeSelected = { viewModel.setIrrigationMode(it.name) },
                        onManualDurationSelected = { viewModel.setManualWaterDurationSeconds(it) },
                        onAutomaticDurationSelected = { viewModel.setAutomaticWaterDurationSeconds(it) },
                        onAutomaticThresholdSelected = { viewModel.setAutomaticSoilThreshold(it) },
                        onAutomaticCooldownSelected = { viewModel.setAutomaticCooldownSeconds(it) },
                        onMaxTempSelected = { viewModel.setMaxManualWaterTempC(it) }
                    )

                    WaterButton(
                        irrigationMode = irrigationMode,
                        isWatering = isWatering,
                        wateringRemainingSeconds = wateringRemainingSeconds,
                        manualWaterDurationSeconds = manualWaterDurationSeconds,
                        automaticWaterDurationSeconds = automaticWaterDurationSeconds,
                        isOnline = isOnline,
                        level = waterLevel,
                        cooldownRemainingSeconds = cooldownRemainingSeconds,
                        isTempBlocked = manualWaterBlockedByTemp,
                        maxManualWaterTempC = maxManualWaterTempC,
                        currentTempC = localTemp,
                        onClick = { viewModel.onWaterButtonClick() }
                    )
                }
            }

            InternetOfflinePopup(visible = !isInternetAvailable)
            DeviceConnectingPopup(
                visible = isInternetAvailable && isWaitingForIotTelemetry
            )
            pendingConflict?.let { conflict ->
                ManualIrrigationConflictDialog(
                    conflict = conflict,
                    onCancel = { viewModel.onManualIrrigationConflictDismissed() },
                    onProceed = { viewModel.onManualIrrigationConflictConfirmed() }
                )
            }
        }
    }
}

@Composable
private fun ToolsMenuButton(
    irrigationMode: IrrigationMode,
    manualWaterDurationSeconds: Int,
    automaticWaterDurationSeconds: Int,
    automaticSoilThreshold: Int,
    automaticCooldownSeconds: Int,
    maxManualWaterTempC: Int,
    deviceId: String,
    deviceDisplayName: String,
    horizontalPadding: Dp = 12.dp,
    onModeSelected: (IrrigationMode) -> Unit,
    onManualDurationSelected: (Int) -> Unit,
    onAutomaticDurationSelected: (Int) -> Unit,
    onAutomaticThresholdSelected: (Int) -> Unit,
    onAutomaticCooldownSelected: (Int) -> Unit,
    onMaxTempSelected: (Int) -> Unit
) {
    var showDialog by remember { mutableStateOf(false) }

    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Irrigador — lado esquerdo, recuado para alinhar com a engrenagem à direita
        Text(
            text = buildAnnotatedString {
                withStyle(SpanStyle(fontWeight = FontWeight.Bold, color = AppCardTitleColor)) {
                    append("Irrigador: ")
                }
                withStyle(SpanStyle(fontWeight = FontWeight.Normal, color = Color.Black)) {
                    append(
                        when {
                            deviceDisplayName.isNotBlank() -> deviceDisplayName
                            deviceId.isNotBlank() -> deviceId
                            else -> "—"
                        }
                    )
                }
            },
            style = MaterialTheme.typography.bodyMedium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .weight(1f)
                .padding(start = horizontalPadding)
        )

        // Ajustes — lado direito
        // Icon clicável sem padding interno do IconButton, com paddingEnd simétrico
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .clickable { showDialog = true }
                .padding(end = horizontalPadding, top = 8.dp, bottom = 8.dp)
        ) {
            Text(
                text = "Ajustes",
                fontWeight = FontWeight.Bold,
                style = MaterialTheme.typography.bodyMedium,
                color = AppCardTitleColor,
                maxLines = 1
            )
            Icon(
                imageVector = Icons.Filled.Settings,
                contentDescription = "Ajustes",
                tint = AppCardTitleColor,
                modifier = Modifier.padding(start = 4.dp)
            )
        }
    }

    if (showDialog) {
        SettingsDialog(
            irrigationMode = irrigationMode,
            manualWaterDurationSeconds = manualWaterDurationSeconds,
            automaticWaterDurationSeconds = automaticWaterDurationSeconds,
            automaticSoilThreshold = automaticSoilThreshold,
            automaticCooldownSeconds = automaticCooldownSeconds,
            maxManualWaterTempC = maxManualWaterTempC,
            onModeSelected = onModeSelected,
            onManualDurationSelected = onManualDurationSelected,
            onAutomaticDurationSelected = onAutomaticDurationSelected,
            onAutomaticThresholdSelected = onAutomaticThresholdSelected,
            onAutomaticCooldownSelected = onAutomaticCooldownSelected,
            onMaxTempSelected = onMaxTempSelected,
            onDismiss = { showDialog = false }
        )
    }
}

@Composable
private fun SettingsDialog(
    irrigationMode: IrrigationMode,
    manualWaterDurationSeconds: Int,
    automaticWaterDurationSeconds: Int,
    automaticSoilThreshold: Int,
    automaticCooldownSeconds: Int,
    maxManualWaterTempC: Int,
    onModeSelected: (IrrigationMode) -> Unit,
    onManualDurationSelected: (Int) -> Unit,
    onAutomaticDurationSelected: (Int) -> Unit,
    onAutomaticThresholdSelected: (Int) -> Unit,
    onAutomaticCooldownSelected: (Int) -> Unit,
    onMaxTempSelected: (Int) -> Unit,
    onDismiss: () -> Unit
) {
    var localManualDuration by remember { mutableIntStateOf(manualWaterDurationSeconds) }
    var localAutomaticDuration by remember { mutableIntStateOf(automaticWaterDurationSeconds) }
    var localAutomaticThreshold by remember { mutableIntStateOf(automaticSoilThreshold) }
    var localAutomaticCooldown by remember { mutableIntStateOf(automaticCooldownSeconds) }
    var localMaxTemp by remember { mutableIntStateOf(maxManualWaterTempC) }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(
            usePlatformDefaultWidth = true,
            dismissOnBackPress = true,
            dismissOnClickOutside = false
        )
    ) {
        Surface(
            modifier = Modifier
                .fillMaxWidth(0.94f)
                .padding(12.dp),
            color = AppCardContainerColor,
            shape = RoundedCornerShape(24.dp),
            shadowElevation = 10.dp,
            border = BorderStroke(1.dp, AppCardBorderColor)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Surface(
                        color = AppPrimaryGreen.copy(alpha = 0.12f),
                        shape = CircleShape
                    ) {
                        Text(
                            text = "⚙",
                            fontSize = 20.sp,
                            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp)
                        )
                    }
                    Text(
                        text = "Ajustes",
                        style = MaterialTheme.typography.headlineSmall,
                        fontWeight = FontWeight.Bold,
                        color = AppCardTitleColor
                    )
                    Text(
                        text = "Personalize o comportamento do seu sistema",
                        style = MaterialTheme.typography.bodySmall,
                        color = AppCardTitleColor.copy(alpha = 0.7f),
                        textAlign = TextAlign.Center
                    )
                }

                HorizontalDivider(color = AppCardDividerColor, thickness = 0.8.dp)

                SettingsSectionCard(
                    icon = "🌿",
                    title = "Modo de irrigação",
                    subtitle = "Defina como o sistema deve operar"
                ) {
                    ModeChoiceSelector(
                        currentMode = irrigationMode,
                        onModeSelected = onModeSelected
                    )
                }

                AnimatedVisibility(
                    visible = irrigationMode == IrrigationMode.MANUAL,
                    enter = expandVertically(),
                    exit = shrinkVertically()
                ) {
                    SettingsSectionCard(
                        icon = "⏱",
                        title = "Configuração manual",
                        subtitle = "Tempo acionado ao tocar no botão de irrigar"
                    ) {
                        SettingOptionSelector(
                            label = "Duração de irrigação",
                            value = localManualDuration,
                            options = listOf(5, 10, 15, 20),
                            unit = "s",
                            onChange = { newValue ->
                                localManualDuration = newValue
                                onManualDurationSelected(newValue)
                            }
                        )
                    }
                }

                AnimatedVisibility(
                    visible = irrigationMode == IrrigationMode.AUTOMATIC,
                    enter = expandVertically(),
                    exit = shrinkVertically()
                ) {
                    SettingsSectionCard(
                        icon = "💧",
                        title = "Configuração automática",
                        subtitle = "Regras para irrigação com base em telemetria"
                    ) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            SettingOptionSelector(
                                label = "Duração de irrigação",
                                value = localAutomaticDuration,
                                options = listOf(5, 10, 15, 20),
                                unit = "s",
                                onChange = { newValue ->
                                    localAutomaticDuration = newValue
                                    onAutomaticDurationSelected(newValue)
                                }
                            )

                            SettingOptionSelector(
                                label = "Umidade mínima",
                                value = localAutomaticThreshold,
                                options = listOf(20, 25, 30, 35, 40),
                                unit = "%",
                                onChange = { newValue ->
                                    localAutomaticThreshold = newValue
                                    onAutomaticThresholdSelected(newValue)
                                },
                                indicatorColor = { optionValue ->
                                    when {
                                        optionValue <= 25 -> Color(0xFFE53935)
                                        optionValue <= 30 -> Color(0xFFF9A825)
                                        else -> Color(0xFF43A047)
                                    }
                                }
                            )

                            SettingOptionSelector(
                                label = "Intervalo entre irrigações",
                                value = localAutomaticCooldown / 60,
                                options = listOf(3, 5, 10, 15),
                                unit = "min",
                                onChange = { newMinutes ->
                                    localAutomaticCooldown = newMinutes * 60
                                    onAutomaticCooldownSelected(newMinutes * 60)
                                }
                            )
                        }
                    }
                }

                SettingsSectionCard(
                    icon = "🌡",
                    title = "Segurança térmica",
                    subtitle = "Evita irrigações em temperaturas elevadas"
                ) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SettingOptionSelector(
                            label = "Temperatura máxima",
                            value = localMaxTemp,
                            options = listOf(25, 30, 35, 40),
                            unit = "°C",
                            onChange = { newValue ->
                                localMaxTemp = newValue
                                onMaxTempSelected(newValue)
                            },
                            indicatorColor = { optionValue ->
                                when {
                                    optionValue <= 28 -> Color(0xFF43A047)
                                    optionValue <= 35 -> Color(0xFFF9A825)
                                    else -> Color(0xFFE53935)
                                }
                            }
                        )

                        Text(
                            text = "Acima deste limite, a irrigação manual é bloqueada.",
                            style = MaterialTheme.typography.bodySmall,
                            color = AppCardTitleColor.copy(alpha = 0.7f)
                        )
                    }
                }

                HorizontalDivider(color = AppCardDividerColor, thickness = 0.8.dp)

                Button(
                    onClick = onDismiss,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(42.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = AppPrimaryGreen,
                        contentColor = Color.White
                    ),
                    shape = RoundedCornerShape(12.dp),
                    elevation = ButtonDefaults.buttonElevation(defaultElevation = 2.dp)
                ) {
                    Text(
                        text = "Ok",
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 14.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsSectionCard(
    icon: String,
    title: String,
    subtitle: String,
    content: @Composable () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, AppCardBorderColor),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(text = icon, fontSize = 17.sp)
                Column {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.SemiBold,
                        color = AppCardTitleColor
                    )
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = AppCardTitleColor.copy(alpha = 0.7f)
                    )
                }
            }

            content()
        }
    }
}

@Composable
private fun ModeChoiceSelector(
    currentMode: IrrigationMode,
    onModeSelected: (IrrigationMode) -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        listOf(IrrigationMode.MANUAL, IrrigationMode.AUTOMATIC).forEach { modeOption ->
            val selected = currentMode == modeOption
            Button(
                onClick = { onModeSelected(modeOption) },
                modifier = Modifier
                    .weight(1f)
                    .height(34.dp),
                shape = RoundedCornerShape(10.dp),
                border = BorderStroke(
                    1.dp,
                    if (selected) AppPrimaryGreen else AppCardBorderColor
                ),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (selected) AppPrimaryGreen else Color.White,
                    contentColor = if (selected) Color.White else AppCardTitleColor
                ),
                contentPadding = PaddingValues(horizontal = 6.dp, vertical = 0.dp),
                elevation = ButtonDefaults.buttonElevation(defaultElevation = if (selected) 2.dp else 0.dp)
            ) {
                Text(
                    text = if (modeOption == IrrigationMode.MANUAL) "Manual" else "Automático",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1
                )
            }
        }
    }
}

@Composable
private fun SettingOptionSelector(
    label: String,
    value: Int,
    options: List<Int>,
    unit: String,
    onChange: (Int) -> Unit,
    indicatorColor: ((Int) -> Color)? = null
) {
    val valueColor = indicatorColor?.invoke(value) ?: AppPrimaryGreen

    Column(
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelSmall,
                color = AppCardTitleColor.copy(alpha = 0.7f)
            )
            Surface(
                modifier = Modifier.size(56.dp, 24.dp),
                color = valueColor,
                shape = RoundedCornerShape(7.dp)
            ) {
                Box(
                    contentAlignment = Alignment.Center,
                    modifier = Modifier.fillMaxSize()
                ) {
                    Text(
                        text = "$value$unit",
                        style = MaterialTheme.typography.labelSmall,
                        color = Color.White,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 10.sp
                    )
                }
            }
        }

        val chunkSize = if (options.size <= 4) options.size else 3
        options.chunked(chunkSize).forEach { optionRow ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                optionRow.forEach { optionValue ->
                    val selected = value == optionValue
                    val optionTone = indicatorColor?.invoke(optionValue) ?: AppPrimaryGreen

                    Button(
                        onClick = { onChange(optionValue) },
                        modifier = Modifier
                            .weight(1f)
                            .height(32.dp),
                        shape = RoundedCornerShape(9.dp),
                        border = BorderStroke(
                            1.dp,
                            if (selected) optionTone else AppCardBorderColor
                        ),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (selected) optionTone else Color.White,
                            contentColor = if (selected) Color.White else AppCardTitleColor
                        ),
                        contentPadding = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
                        elevation = ButtonDefaults.buttonElevation(defaultElevation = if (selected) 2.dp else 0.dp)
                    ) {
                        Text(
                            text = "$optionValue$unit",
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1
                        )
                    }
                }

                repeat(chunkSize - optionRow.size) {
                    Box(modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
// Aqui eu monitoro a conexão com a internet
private fun rememberInternetAvailable(): Boolean {
    val context = LocalContext.current.applicationContext
    val connectivityManager = remember(context) {
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
    }

    fun isConnected(): Boolean {
        val manager = connectivityManager ?: return false
        val network = manager.activeNetwork ?: return false
        val capabilities = manager.getNetworkCapabilities(network) ?: return false
        return capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
            capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
    }

    var isAvailable by remember(connectivityManager) { mutableStateOf(isConnected()) }

    DisposableEffect(connectivityManager) {
        val manager = connectivityManager
        if (manager == null) {
            onDispose { }
        } else {
            val callback = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    isAvailable = isConnected()
                }

                override fun onLost(network: Network) {
                    isAvailable = isConnected()
                }

                override fun onCapabilitiesChanged(network: Network, networkCapabilities: NetworkCapabilities) {
                    isAvailable = isConnected()
                }
            }

            val request = NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build()

            manager.registerNetworkCallback(request, callback)

            onDispose {
                runCatching { manager.unregisterNetworkCallback(callback) }
            }
        }
    }

    return isAvailable
}

@Composable
// Aqui eu trato quando não tem conexão com internet
// vou gerar um pop-up
private fun InternetOfflinePopup(visible: Boolean) {
    if (!visible) return

    Dialog(
        onDismissRequest = { },
        properties = DialogProperties(
            dismissOnBackPress = false,
            dismissOnClickOutside = false,
            usePlatformDefaultWidth = true
        )
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
            elevation = CardDefaults.cardElevation(10.dp),
            border = BorderStroke(1.dp, AppCardBorderColor),
            shape = MaterialTheme.shapes.extraLarge
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 18.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Icon(
                    imageVector = Icons.Filled.WifiOff,
                    contentDescription = "Sem conexão com a internet",
                    tint = Color(0xFFD84315),
                    modifier = Modifier.size(40.dp)
                )

                Text(
                    text = "Sem conexão com a internet",
                    fontWeight = FontWeight.Bold,
                    fontSize = 16.sp,
                    color = AppCardTitleColor,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )

                Text(
                    text = "O app permanecerá aguardando até a conexão de rede ser restabelecida.",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
// Aqui vai o card da planta alternando a figura
// eo texto embaixo dela
fun PlantStatusCard(
    umidSolo: Int,
    irrigadorOnline: Boolean
) {
    val telemetryUnavailable = !irrigadorOnline

    val imagem = when {
        telemetryUnavailable -> R.drawable.plantamoderada
        umidSolo <= 20 -> R.drawable.plantaseca
        umidSolo <= 40 -> R.drawable.plantamoderada
        else -> R.drawable.plantaok
    }

    val status = when {
        telemetryUnavailable -> "--"
        umidSolo <= 20 -> "Sua planta precisa de água \uD83D\uDCA7"
        umidSolo <= 40 -> "Próximo da hora de rega"
        else -> "Planta saudável \uD83C\uDF3F"
    }
    // Aqui muda a cor do texto do status da planta de acordo
    // com a umidade do solo
    // aqui uso a variavel corTexto.
    val corTexto = when {
        telemetryUnavailable -> Color.Red
        umidSolo <= 20 -> Color.Red
        umidSolo <= 40 -> Color(0xFF000000)
        else -> Color(0xFF000000)
    }

    BoxWithConstraints {
        val compactWidth = maxWidth < 370.dp
        val minCardHeight = if (compactWidth) 156.dp else 186.dp
        val imageMax = if (compactWidth) 108.dp else 136.dp

        Card(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = minCardHeight),
            elevation = CardDefaults.cardElevation(4.dp),
            colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
            border = BorderStroke(1.dp, AppCardBorderColor)
        ) {
            BoxWithConstraints(
                modifier = Modifier.fillMaxWidth(),
                contentAlignment = Alignment.Center
            ) {
                val imageSize = (maxWidth * 0.45f).coerceAtMost(imageMax)

                Column(
                    modifier = Modifier.padding(10.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Crossfade(targetState = imagem, label = "PlantImageTransition") { imagemAtual ->
                        Image(
                            painter = painterResource(id = imagemAtual),
                            contentDescription = status,
                            contentScale = ContentScale.Fit,
                            modifier = Modifier.size(imageSize)
                        )
                    }

                    Text(
                        text = status,
                        fontSize = if (compactWidth) 14.sp else 16.sp,
                        color = corTexto,
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

@Composable
// Aqui vai o card onde coloco as telemetrias vindas
// do dispositivo IOT
fun MoistureCard(
    irrigadorOnline: Boolean,
    umidSolo: Int,
    localTemp: Int,
    umidAmbiente: Int
) {
    val telemetryUnavailable = !irrigadorOnline

    // Aqui eu mudo a cor do texto da umidade do solo
    val corUmidadeSolo = when {
        telemetryUnavailable -> Color.Red
        umidSolo <= 20 -> Color.Red
        umidSolo in 21..40 -> Color(0xFFF9A825)
        else -> Color(0xFF4CAF50)
    }
    // Aqui eu mudo a cor do texto da temperatura e tambem se
    // irrigador fica ONLINE ou OFFLINE
    val corTemperatura = if (telemetryUnavailable) Color.Red else if (localTemp > 32) Color.Red else Color(0xFF4CAF50)
    val corUmidadeAr = if (telemetryUnavailable) Color.Red else MaterialTheme.colorScheme.onSurface
    val corIrrigador = if (irrigadorOnline) Color(0xFF4CAF50) else Color.Red
    val textoIrrigador = if (irrigadorOnline) "Online ✅" else "Offline ❌"
    val umidSoloTexto = if (telemetryUnavailable) "--" else "$umidSolo%"
    val umidAmbienteTexto = if (telemetryUnavailable) "--" else "$umidAmbiente%"
    val temperaturaTexto = if (telemetryUnavailable) "--" else "$localTemp°C"

    BoxWithConstraints {
        val compactWidth = maxWidth < 400.dp
        val labelFont = if (compactWidth) 14.sp else 15.sp
        val valueFont = if (compactWidth) 16.sp else 17.sp
        val telemetryIconSize = if (compactWidth) 18.dp else 20.dp

        Card(
            modifier = Modifier.fillMaxWidth(),
            elevation = CardDefaults.cardElevation(4.dp),
            colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
            border = BorderStroke(1.dp, AppCardBorderColor)
        ) {
            Column(
                modifier = Modifier.padding(if (compactWidth) 12.dp else 14.dp),
                verticalArrangement = Arrangement.spacedBy(if (compactWidth) 7.dp else 9.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Row(
                        modifier = Modifier.weight(1f),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.PowerSettingsNew,
                            contentDescription = "Status do irrigador",
                            tint = Color(0xFF2E7D32),
                            modifier = Modifier.size(telemetryIconSize)
                        )
                        Text(
                            "Monitoramento",
                            fontWeight = FontWeight.Bold,
                            fontSize = if (compactWidth) 14.sp else 15.sp,
                            color = AppCardTitleColor,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }

                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        modifier = Modifier
                            .background(
                                color = corIrrigador.copy(alpha = 0.12f),
                                shape = RoundedCornerShape(50)
                            )
                            .padding(horizontal = if (compactWidth) 8.dp else 10.dp, vertical = 5.dp)
                    ) {
                        Box(
                            modifier = Modifier
                                .size(if (compactWidth) 8.dp else 10.dp)
                                .background(corIrrigador, CircleShape)
                        )
                        Text(
                            textoIrrigador,
                            fontSize = if (compactWidth) 13.sp else 14.sp,
                            fontWeight = FontWeight.Bold,
                            color = corIrrigador,
                            maxLines = 1
                        )
                    }
                }

                HorizontalDivider(color = AppCardDividerColor)

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Row(
                        modifier = Modifier.weight(1f),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.Opacity,
                            contentDescription = "Umidade do solo",
                            tint = Color(0xFF2E7D32),
                            modifier = Modifier.size(telemetryIconSize)
                        )
                        Text(
                            "Umidade do solo:",
                            fontWeight = FontWeight.Bold,
                            fontSize = labelFont,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                    Text(umidSoloTexto, fontSize = valueFont, color = corUmidadeSolo, maxLines = 1)
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Row(
                        modifier = Modifier.weight(1f),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.Cloud,
                            contentDescription = "Umidade do ar",
                            tint = Color(0xFF2E7D32),
                            modifier = Modifier.size(telemetryIconSize)
                        )
                        Text(
                            "Umidade do ar:",
                            fontWeight = FontWeight.Bold,
                            fontSize = labelFont,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                    Text(umidAmbienteTexto, fontSize = valueFont, color = corUmidadeAr, maxLines = 1)
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Row(
                        modifier = Modifier.weight(1f),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.DeviceThermostat,
                            contentDescription = "Temperatura",
                            tint = Color(0xFF2E7D32),
                            modifier = Modifier.size(telemetryIconSize)
                        )
                        Text(
                            "Temperatura:",
                            fontWeight = FontWeight.Bold,
                            fontSize = labelFont,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                    Text(temperaturaTexto, fontSize = valueFont, color = corTemperatura, maxLines = 1)
                }
            }
        }
    }
}

@Composable
// Aqui vai o card do reservatório de água
// onde mudo a imagem da garrafa e também tópico cheio / vazio do
// dispositivo IOT
fun WaterLevelCard(
    level: String,
    irrigadorOnline: Boolean
) {
    val telemetryUnavailable = !irrigadorOnline
    val isFull = irrigadorOnline && level == "Cheio"
    val isEmpty = irrigadorOnline && level == "Vazio"
    val statusColor = if (telemetryUnavailable) Color.Red else if (isFull) Color(0xFF4CAF50) else Color.Red
    val levelText = if (telemetryUnavailable) "--" else level

    // Animação de pulso elegante: só ativa quando o reservatório está Vazio e online.
    // Alpha oscila de 1f → 0.25f → 1f em 1200ms com easing linear, criando um
    // piscar suave sem agressividade. Quando não está vazio, alpha fixo em 1f.
    val infiniteTransition = rememberInfiniteTransition(label = "waterLevelPulse")
    val pulseAlpha by if (isEmpty) {
        infiniteTransition.animateFloat(
            initialValue = 1f,
            targetValue = 0.25f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 1000, easing = LinearEasing),
                repeatMode = RepeatMode.Reverse
            ),
            label = "pulseAlpha"
        )
    } else {
        infiniteTransition.animateFloat(
            initialValue = 1f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 1000),
                repeatMode = RepeatMode.Restart
            ),
            label = "pulseAlphaStatic"
        )
    }

    val imageRes = if (isFull) {
        R.drawable.garrafacheia
    } else {
        R.drawable.garrafavazia
    }

    BoxWithConstraints {
        val compactWidth = maxWidth < 400.dp
        val compactHeight = maxHeight < 120.dp

        // Borda pulsa junto com o texto quando vazio — reforça o alerta visualmente.
        val cardBorder = if (isEmpty) {
            BorderStroke(1.5.dp, Color.Red.copy(alpha = pulseAlpha))
        } else {
            BorderStroke(1.dp, AppCardBorderColor)
        }

        Card(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = if (compactWidth || compactHeight) 90.dp else 100.dp),
            elevation = CardDefaults.cardElevation(4.dp),
            colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
            border = cardBorder
        ) {
            Row(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(if (compactWidth) 6.dp else 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(if (compactWidth) 8.dp else 12.dp)
            ) {
                BoxWithConstraints(
                    modifier = Modifier.weight(if (compactWidth) 0.48f else 0.42f),
                    contentAlignment = Alignment.Center
                ) {
                    val imageSize = (maxWidth * 1.035f).coerceAtMost(if (compactWidth) 94.dp else 108.dp)

                    Image(
                        painter = painterResource(id = imageRes),
                        contentDescription = "Reservatório",
                        contentScale = ContentScale.Fit,
                        modifier = Modifier
                            .size(imageSize)
                            .graphicsLayer { alpha = if (isEmpty) pulseAlpha else 1f }
                    )
                }

                Column(
                    modifier = Modifier.weight(if (compactWidth) 0.52f else 0.58f),
                    horizontalAlignment = Alignment.End,
                    verticalArrangement = Arrangement.spacedBy(if (compactWidth) 2.dp else 4.dp)
                ) {
                    Text(
                        "Reservatório de água:",
                        fontSize = if (compactWidth) 14.sp else 15.sp,
                        fontWeight = FontWeight.Bold,
                        color = AppCardTitleColor,
                        textAlign = TextAlign.End,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )

                    HorizontalDivider(
                        color = AppCardDividerColor,
                        modifier = Modifier.padding(vertical = 2.dp)
                    )

                    Text(
                        levelText,
                        fontSize = if (compactWidth) 16.sp else 17.sp,
                        // Aplica o pulso de alpha no texto de status quando vazio.
                        color = statusColor.copy(alpha = if (isEmpty) pulseAlpha else 1f),
                        fontWeight = if (isEmpty) FontWeight.Bold else FontWeight.Normal,
                        textAlign = TextAlign.End,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

@Composable
// Aqui eu trato o botão de irrigação manual
// quando não tiver agua la no reservatório o botão
// fica inibido
fun WaterButton(
    irrigationMode: IrrigationMode,
    isWatering: Boolean,
    wateringRemainingSeconds: Int,
    manualWaterDurationSeconds: Int,
    automaticWaterDurationSeconds: Int,
    isOnline: Boolean,
    level: String,
    cooldownRemainingSeconds: Int,
    isTempBlocked: Boolean,
    maxManualWaterTempC: Int,
    currentTempC: Int,
    onClick: () -> Unit
) {
    val isAutomaticMode = irrigationMode == IrrigationMode.AUTOMATIC
    val cooldownActive = cooldownRemainingSeconds > 0
    val modePhrase = if (irrigationMode == IrrigationMode.AUTOMATIC) "automática" else "manual"
    val scheduledDurationSeconds = if (irrigationMode == IrrigationMode.AUTOMATIC) {
        automaticWaterDurationSeconds
    } else {
        manualWaterDurationSeconds
    }

    Button(
        onClick = {
            // Guard local: rejeita o toque se qualquer condição de bloqueio for verdadeira,
            // independentemente do estado de `enabled`. Isso cobre a janela de um frame
            // em que o Compose ainda não recompôs o botão após uma mudança de modo,
            // e toques buffereados pelo sistema de gestos do Android poderiam vazar.
            val canFire = !isAutomaticMode
                && !isWatering
                && !cooldownActive
                && !isTempBlocked
                && isOnline
                && level == "Cheio"
            if (canFire) onClick()
        },
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 42.dp),
        enabled = !isAutomaticMode && !isWatering && !cooldownActive && !isTempBlocked && isOnline && level == "Cheio",
        elevation = ButtonDefaults.elevatedButtonElevation(
            defaultElevation = 6.dp,
            pressedElevation = 8.dp
        )
    ) {
        Text(
            text = when {
                    isAutomaticMode -> "Irrigação em modo automático"
                    isWatering -> "Acionamento manual • irrigando... ${wateringRemainingSeconds}s"
                    cooldownActive -> "Irrigação bloqueada por ${cooldownRemainingSeconds}s"
                    isTempBlocked -> "Segurança térmica ativada: $currentTempC°C > ${maxManualWaterTempC}°C"
                    !isOnline -> "Aguardando conexão com irrigador..."
                    level != "Cheio" -> "⚠️ Sem água no reservatório"
                    else -> "Irrigação $modePhrase por ${scheduledDurationSeconds} seg."
            },
            style = MaterialTheme.typography.labelLarge,
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
// Aqui trato o cabeçalho interno
fun CompactScreenHeader(title: String, subTitle: String) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        elevation = CardDefaults.cardElevation(3.dp),
        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
        border = BorderStroke(1.dp, AppCardBorderColor)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = AppCardTitleColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = subTitle,
                style = MaterialTheme.typography.bodySmall,
                textAlign = TextAlign.Center,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
private fun NoLinkedDeviceCard(horizontalPadding: Dp) {
    androidx.compose.material3.Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = androidx.compose.material3.CardDefaults.cardColors(containerColor = Color.White)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = horizontalPadding, vertical = 18.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Text(
                text = "Nenhum dispositivo vinculado",
                style = MaterialTheme.typography.titleMedium,
                color = AppCardTitleColor,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = "Este usuário ainda não possui um irrigador cadastrado. Conecte o MAC ao usuário para liberar telemetria e comandos.",
                style = MaterialTheme.typography.bodyMedium,
                color = Color.DarkGray
            )
        }
    }
}