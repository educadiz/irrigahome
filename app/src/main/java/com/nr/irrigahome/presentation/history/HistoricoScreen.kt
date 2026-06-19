/**
 * Arquivo: Historico.kt
 *
 * Responsabilidade:
 * - Tela de histórico de irrigações da aplicação com integração Firebase
 * - Exibe lista completa de irrigações em ordem cronológica (mais recentes primeiro)
 * - Implementa filtros funcionais por tipo de acionamento e período de datas
 * - Renderiza cards informativos com detalhes de cada evento de irrigação
 * - Integra com Firebase Firestore e endpoint HTTP para recuperar histórico persistente
 * - Implementa estados de carregamento, erro e lista vazia
 * - Fornece carregamento dinâmico: hoje, última semana, último mês, personalizado
 * - Interface responsiva adaptada para diferentes tamanhos de tela
 * - Sincronização com dados reais do dispositivo IoT
 *
 * Alterações:
 * - HistoricoEventCard: exibe bloco de telemetria (temperatura, umidade do solo,
 *   umidade do ar, nível de água) na área expandida quando os dados estiverem presentes.
 * - HistoricoEventCard: mostra badge "⚠️ Falha" no cabeçalho do card quando
 *   event.hasFailed == true, com exibição do stopReason quando disponível.
 */

package com.nr.irrigahome.presentation.history

import com.nr.irrigahome.domain.model.HistoryLoadingState
import com.nr.irrigahome.domain.model.IrrigationHistoryEvent
import com.nr.irrigahome.domain.model.TriggerType
import com.nr.irrigahome.presentation.home.IrrigaViewModel
import com.nr.irrigahome.ui.components.BaseScreen
import com.nr.irrigahome.ui.components.ScreenHeader
import com.nr.irrigahome.presentation.history.HistoricoViewModel
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen
import java.text.SimpleDateFormat
import java.util.Locale
import kotlinx.coroutines.delay

@Composable
fun HistoricoScreen() {
    val viewModel: HistoricoViewModel = viewModel()
    val irrigationViewModel: IrrigaViewModel = viewModel()
    val hasLinkedDevice = irrigationViewModel.hasLinkedDevice.value
    val loadingState = viewModel.loadingState.value
    val filteredEvents = viewModel.filteredEvents
    val selectedFilter = viewModel.selectedFilterType.value
    val actionStatus = viewModel.actionStatus.value

    var expandedEventId by remember { mutableStateOf<String?>(null) }
    var popupMessage by remember { mutableStateOf("") }
    var showPopup by remember { mutableStateOf(false) }
    var popupIsError by remember { mutableStateOf(false) }

    LaunchedEffect(actionStatus) {
        if (actionStatus.isBlank()) return@LaunchedEffect

        popupMessage = if (actionStatus.contains("removido", ignoreCase = true)) {
            "Registro removido com sucesso"
        } else {
            actionStatus
        }
        popupIsError = actionStatus.contains("erro", ignoreCase = true)
        showPopup = true

        try {
            delay(2000)
        } finally {
            showPopup = false
            viewModel.clearActionStatus()
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        if (!hasLinkedDevice) {
            NoLinkedDeviceCard(
                title = "Nenhum dispositivo vinculado",
                message = "O histórico de irrigações só aparece para usuários com IoT cadastrado e vinculado ao MAC."
            )
            return@Box
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(16.dp)
                .imePadding(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Cabeçalho padrão comum
            ScreenHeader(
                title = "Histórico de Irrigações",
                subTitle = "Clique no registro para visualizar detalhes"
            )

            Text(
                text = "Filtrar eventos",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                color = AppCardTitleColor,
                modifier = Modifier.fillMaxWidth()
            )

            // Filtros compactos em uma única linha
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
                border = BorderStroke(1.dp, AppCardBorderColor)
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    FilterChip(
                        modifier = Modifier.weight(1f),
                        selected = selectedFilter == null,
                        onClick = { viewModel.clearFilters() },
                        label = {
                            Text(
                                text = "Todos",
                                style = MaterialTheme.typography.bodySmall,
                                fontWeight = FontWeight.Bold,
                                textAlign = TextAlign.Center,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    )
                    FilterChip(
                        modifier = Modifier.weight(1f),
                        selected = selectedFilter == TriggerType.MANUAL,
                        onClick = { viewModel.filterByTriggerType(if (selectedFilter == TriggerType.MANUAL) null else TriggerType.MANUAL) },
                        label = {
                            Text(
                                text = "Manual",
                                style = MaterialTheme.typography.bodySmall,
                                fontWeight = FontWeight.Bold,
                                textAlign = TextAlign.Center,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    )
                    FilterChip(
                        modifier = Modifier.weight(1f),
                        selected = selectedFilter == TriggerType.AUTOMATIC,
                        onClick = { viewModel.filterByTriggerType(if (selectedFilter == TriggerType.AUTOMATIC) null else TriggerType.AUTOMATIC) },
                        label = {
                            Text(
                                text = "Auto",
                                style = MaterialTheme.typography.bodySmall,
                                fontWeight = FontWeight.Bold,
                                textAlign = TextAlign.Center,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    )
                    FilterChip(
                        modifier = Modifier.weight(1f),
                        selected = selectedFilter == TriggerType.SCHEDULE,
                        onClick = { viewModel.filterByTriggerType(if (selectedFilter == TriggerType.SCHEDULE) null else TriggerType.SCHEDULE) },
                        label = {
                            Text(
                                text = "Agenda",
                                style = MaterialTheme.typography.bodySmall,
                                fontWeight = FontWeight.Bold,
                                textAlign = TextAlign.Center,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    )
                }
            }

            // Conteúdo principal
            when (loadingState) {
                is HistoryLoadingState.Loading -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f),
                        contentAlignment = Alignment.Center
                    ) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            CircularProgressIndicator(color = AppPrimaryGreen)
                            Spacer(modifier = Modifier.height(16.dp))
                            Text(
                                text = "Carregando histórico...",
                                style = MaterialTheme.typography.bodyMedium,
                                color = AppCardTitleColor
                            )
                        }
                    }
                }

                is HistoryLoadingState.Empty -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f),
                        contentAlignment = Alignment.Center
                    ) {
                        Card(
                            modifier = Modifier.fillMaxWidth(0.85f),
                            colors = CardDefaults.cardColors(
                                containerColor = AppCardContainerColor
                            ),
                            border = BorderStroke(1.dp, AppCardBorderColor)
                        ) {
                            Column(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(24.dp),
                                horizontalAlignment = Alignment.CenterHorizontally
                            ) {
                                Text(
                                    text = "📭",
                                    style = MaterialTheme.typography.headlineMedium
                                )
                                Spacer(modifier = Modifier.height(12.dp))
                                Text(
                                    text = "Nenhum registro encontrado",
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.Bold,
                                    color = AppCardTitleColor
                                )
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = "Não há eventos de irrigação neste período",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = Color.Gray,
                                    textAlign = TextAlign.Center
                                )
                            }
                        }
                    }
                }

                is HistoryLoadingState.Error -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f),
                        contentAlignment = Alignment.Center
                    ) {
                        Card(
                            modifier = Modifier.fillMaxWidth(0.85f),
                            colors = CardDefaults.cardColors(
                                containerColor = Color(0xFFFFEBEE)
                            ),
                            border = BorderStroke(1.dp, Color(0xFFEF5350))
                        ) {
                            Column(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(24.dp),
                                horizontalAlignment = Alignment.CenterHorizontally
                            ) {
                                Text(
                                    text = "⚠️",
                                    style = MaterialTheme.typography.headlineMedium
                                )
                                Spacer(modifier = Modifier.height(12.dp))
                                Text(
                                    text = "Erro ao carregar histórico",
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.Bold,
                                    color = Color(0xFFC62828)
                                )
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = (loadingState as HistoryLoadingState.Error).message,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = Color.Gray,
                                    textAlign = TextAlign.Center
                                )
                                Spacer(modifier = Modifier.height(16.dp))
                                Button(
                                    onClick = { viewModel.loadHistory() },
                                    colors = ButtonDefaults.buttonColors(containerColor = AppPrimaryGreen)
                                ) {
                                    Text("Tentar Novamente")
                                }
                            }
                        }
                    }
                }

                is HistoryLoadingState.Success -> {
                    if (filteredEvents.isEmpty()) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .weight(1f),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = "Nenhum resultado com os filtros aplicados",
                                style = MaterialTheme.typography.bodyMedium,
                                color = Color.Gray,
                                textAlign = TextAlign.Center
                            )
                        }
                    } else {
                        LazyColumn(
                            modifier = Modifier
                                .weight(1f)
                                .fillMaxWidth(),
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            item {
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(horizontal = 8.dp, vertical = 4.dp),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Text(
                                        text = "Registros (${filteredEvents.size})",
                                        style = MaterialTheme.typography.titleMedium,
                                        color = AppCardTitleColor,
                                        fontWeight = FontWeight.SemiBold
                                    )
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        IconButton(onClick = { viewModel.loadHistory() }) {
                                            Icon(
                                                imageVector = Icons.Filled.Refresh,
                                                contentDescription = "Atualizar registros",
                                                tint = AppPrimaryGreen
                                            )
                                        }
                                    }
                                }
                            }

                            items(
                                items = filteredEvents,
                                // key estável garante que o Compose rastreie cada card
                                // pelo seu identificador único global, não pela posição.
                                // Isso evita que, após uma deleção + recarga da lista,
                                // o conteúdo de um card seja exibido em outro card.
                                key = { event -> event.eventId.ifBlank { event.id } }
                            ) { event ->
                                HistoricoEventCard(
                                    event = event,
                                    isExpanded = expandedEventId == event.eventId,
                                    onExpandClick = {
                                        expandedEventId =
                                            if (expandedEventId == event.eventId) null
                                            else event.eventId
                                    },
                                    onDeleteClick = {
                                        viewModel.removeHistoryEvent(event)
                                        if (expandedEventId == event.eventId) {
                                            expandedEventId = null
                                        }
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }

        AnimatedVisibility(
            visible = showPopup,
            enter = fadeIn(animationSpec = tween(durationMillis = 250)) +
                slideInVertically(
                    initialOffsetY = { -it / 2 },
                    animationSpec = tween(durationMillis = 300)
                ),
            exit = fadeOut(animationSpec = tween(durationMillis = 220)) +
                slideOutVertically(
                    targetOffsetY = { -it / 3 },
                    animationSpec = tween(durationMillis = 250)
                ),
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 16.dp)
        ) {
            Card(
                colors = CardDefaults.cardColors(
                    containerColor = if (popupIsError) Color(0xFFC62828) else AppPrimaryGreen
                ),
                elevation = CardDefaults.cardElevation(6.dp)
            ) {
                Text(
                    text = popupMessage,
                    color = Color.White,
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp)
                )
            }
        }
    }
}

@Composable
private fun NoLinkedDeviceCard(title: String, message: String) {
    Card(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
        border = BorderStroke(1.dp, AppCardBorderColor)
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(text = "📭", style = MaterialTheme.typography.headlineMedium)
            Spacer(modifier = Modifier.height(12.dp))
            Text(text = title, style = MaterialTheme.typography.titleMedium, color = AppCardTitleColor)
            Spacer(modifier = Modifier.height(8.dp))
            Text(text = message, style = MaterialTheme.typography.bodyMedium, color = AppCardTitleColor)
        }
    }
}

@Composable
fun HistoricoEventCard(
    event: IrrigationHistoryEvent,
    isExpanded: Boolean,
    onExpandClick: () -> Unit,
    onDeleteClick: () -> Unit
) {
    val dateFormat = SimpleDateFormat("dd/MM/yyyy HH:mm", Locale.getDefault())
    val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    // Borda vermelha sinaliza falha; borda padrão nos demais casos.
    val cardBorder = if (event.hasFailed) {
        BorderStroke(1.5.dp, Color(0xFFEF5350))
    } else {
        BorderStroke(1.dp, AppCardBorderColor)
    }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onExpandClick() },
        elevation = CardDefaults.cardElevation(4.dp),
        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
        border = cardBorder
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp)
        ) {
            // ── Cabeçalho do card ────────────────────────────────────────────
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Coluna esquerda: data e linha de duração/hora
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = dateFormat.format(event.startTime),
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Bold,
                        color = AppCardTitleColor
                    )
                    Row(
                        modifier = Modifier.padding(top = 4.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = "⏱️ ${formatarDuracao(event.durationSeconds)}",
                            style = MaterialTheme.typography.bodyMedium,
                            color = Color.Gray
                        )
                        Text(
                            text = "🕐 ${timeFormat.format(event.startTime)}",
                            style = MaterialTheme.typography.bodyMedium,
                            color = Color.Gray
                        )
                    }
                }

                // ⚠️ Badge de falha — alinhado com TriggerTypeBadge e ❌
                // Só aparece quando success == false
                if (event.hasFailed) {
                    FailureBadge()
                }

                // Badge do tipo de acionamento
                TriggerTypeBadge(triggerType = event.triggerType)

                // Botão de remoção
                Text(
                    //text = "❌",
                    text = "\uD83D\uDDD1\uFE0F",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier
                        .clickable(onClick = onDeleteClick)
                        .padding(6.dp)
                )
            }

            // ── Conteúdo expandido ───────────────────────────────────────────
            // AnimatedVisibility garante que a expansão/colapso seja animada
            // e que APENAS o card cujo isExpanded == true exiba os detalhes.
            // O estado de isExpanded é controlado exclusivamente pelo pai
            // (HistoricoScreen), via expandedEventId baseado em event.eventId.
            AnimatedVisibility(
                visible = isExpanded,
                enter = fadeIn(animationSpec = tween(200)) +
                    slideInVertically(
                        initialOffsetY = { -it / 4 },
                        animationSpec = tween(250)
                    ),
                exit = fadeOut(animationSpec = tween(180)) +
                    slideOutVertically(
                        targetOffsetY = { -it / 4 },
                        animationSpec = tween(200)
                    )
            ) {
                Column {
                Spacer(modifier = Modifier.height(12.dp))
                HorizontalDivider(color = Color.Gray.copy(alpha = 0.3f))
                Spacer(modifier = Modifier.height(12.dp))

                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {

                    val deviceDisplayName = formatMacAddressForDisplay(event.macAddress)
                        ?: event.deviceId

                    HistoricoDetailItem(
                        label = "📟 Dispositivo",
                        value = deviceDisplayName
                    )

                    // Horários e duração
                    HistoricoDetailItem(
                        label = "🟢 Início",
                        value = timeFormat.format(event.startTime)
                    )
                    HistoricoDetailItem(
                        label = "🔴 Término",
                        value = timeFormat.format(event.endTime)
                    )
                    HistoricoDetailItem(
                        label = "⏳ Duração",
                        value = if (event.durationMinutes > 0) {
                            "${event.durationMinutes}min ${event.durationSecondsRemainder}s"
                        } else {
                            "${event.durationSeconds}s"
                        }
                    )

                    // Tipo de acionamento
                    HistoricoDetailItem(
                        label = "🎛️ Acionamento",
                        value = event.triggerType.displayName
                    )

                    // ── Bloco de telemetria ──────────────────────────────────
                    // Exibe os dados de sensor registrados no momento do evento.
                    // Cada linha só é renderizada se o valor estiver disponível.
                    val hasTelemetry = event.temperature != null ||
                        event.soilHumidity != null ||
                        event.airHumidity != null ||
                        event.waterLevel != null ||
                        event.totalVolumeMl != null

                    if (hasTelemetry) {
                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider(color = Color.Gray.copy(alpha = 0.2f))
                        Spacer(modifier = Modifier.height(4.dp))

                        Text(
                            text = "Telemetria no evento",
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = Color.Gray
                        )

                        event.temperature?.let {
                            HistoricoDetailItem(
                                label = "🌡️ Temperatura",
                                value = "$it °C"
                            )
                        }
                        event.soilHumidity?.let {
                            HistoricoDetailItem(
                                label = "🌱 Umidade do solo",
                                value = "$it %"
                            )
                        }
                        event.airHumidity?.let {
                            HistoricoDetailItem(
                                label = "💧 Umidade do ar",
                                value = "$it %"
                            )
                        }
                        event.waterLevel?.let {
                            HistoricoDetailItem(
                                label = "🪣 Reservatório de água",
                                value = it
                            )
                        }
                        // Volume consumido: exibe em litros quando >= 1000 ml para
                        // facilitar a leitura (ex: 1.200 ml → "1,2 L"); abaixo
                        // disso mantém em ml para evitar casas decimais desnecessárias.
                        event.totalVolumeMl?.let { ml ->
                            val volumeDisplay = if (ml >= 1000) {
                                val litros = ml / 1000.0
                                String.format("%.1f L", litros).replace(".", ",")
                            } else {
                                "$ml ml"
                            }
                            HistoricoDetailItem(
                                label = "🚿 Volume consumido",
                                value = volumeDisplay
                            )
                        }
                    }
                    // ────────────────────────────────────────────────────────

                    // ── Bloco de ocorrência (falha) ──────────────────────────
                    // Só aparece quando success == false.
                    if (event.hasFailed) {
                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider(color = Color(0xFFEF5350).copy(alpha = 0.4f))
                        Spacer(modifier = Modifier.height(4.dp))

                        Text(
                            text = "⚠️ Ocorrência",
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = Color(0xFFEF5350)
                        )

                        val motivoDisplay = when (event.stopReason?.lowercase()) {
                            "no_water"   -> "Sem água no reservatório"
                            "timeout"    -> "Tempo limite atingido"
                            "manual_off" -> "Desligado manualmente"
                            else         -> event.stopReason ?: "Motivo não registrado"
                        }

                        HistoricoDetailItem(
                            label = "Motivo",
                            value = motivoDisplay
                        )
                    }
                    // ────────────────────────────────────────────────────────

                    // ID do evento (rodapé do card)
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = "ID: ${event.eventId}",
                        style = MaterialTheme.typography.labelSmall,
                        color = Color.Gray,
                        modifier = Modifier.padding(top = 4.dp)
                    )
                }
                } // fecha Column externo do AnimatedVisibility
            } // fecha AnimatedVisibility
        }
    }
}

private fun formatMacAddressForDisplay(rawMacAddress: String?): String? {
    val normalized = rawMacAddress
        ?.trim()
        ?.replace(":", "")
        ?.replace("-", "")
        ?.uppercase()
        ?.takeIf { it.length == 12 && it.all { char -> char.isDigit() || char in 'A'..'F' } }
        ?: return null

    return normalized.chunked(2).joinToString(":")
}

// ── Composables auxiliares ───────────────────────────────────────────────────

/**
 * Badge vermelho compacto exibido no cabeçalho do card quando hasFailed == true.
 */
@Composable
fun FailureBadge() {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = Color(0xFFEF5350)
    ) {
        Text(
            text = "⚠️ Falha",
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall,
            color = Color.White,
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
fun TriggerTypeBadge(triggerType: TriggerType) {
    val (backgroundColor, textColor, emoji) = when (triggerType) {
        TriggerType.MANUAL    -> Triple(Color(0xFF2196F3), Color.White, "🎛️")
        TriggerType.AUTOMATIC -> Triple(Color(0xFF4CAF50), Color.White, "🤖")
        TriggerType.SCHEDULE  -> Triple(Color(0xFFFF9800), Color.White, "📅")
        TriggerType.UNKNOWN   -> Triple(Color.Gray,        Color.White, "❓")
    }

    Surface(
        shape = RoundedCornerShape(12.dp),
        color = backgroundColor
    ) {
        Text(
            text = "$emoji ${triggerType.displayName}",
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall,
            color = textColor,
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
fun HistoricoDetailItem(
    label: String,
    value: String
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = Color.Gray,
            modifier = Modifier.weight(1f)
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            color = AppCardTitleColor,
            textAlign = TextAlign.End,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

fun formatarDuracao(segundos: Long): String {
    val minutos = segundos / 60
    val segundosRest = segundos % 60

    return when {
        minutos > 0 && segundosRest > 0 -> "${minutos}m ${segundosRest}s"
        minutos > 0 -> "${minutos}min"
        else -> "${segundos}s"
    }
}
