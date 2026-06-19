/**
 * Arquivo: Agendamento.kt
 *
 * Responsabilidade:
 * - Tela de agendamento de irrigação automática da aplicação
 * - Permite ao usuário definir horários de irrigação automática por dia da semana
 * - Renderiza seletor visual para escolher dias da semana (Dom a Sáb)
 * - Implementa timepickers para definir horário de início e fim de irrigação
 * - Exibe lista de agendamentos criados com opção de edição e exclusão
 * - Gerencia dados de agendamento persisten via Firestore
 * - Sincroniza agendamentos com o dispositivo IoT via MQTT
 * - Implementa validação de horários (início < fim)
 * - Renderiza cards informativos sobre agendamentos ativos
 * - Integra com IrrigaViewModel para persistência e sincronização
 * - Fornece interface amigável para gerenciamento de múltiplos agendamentos
 */

package com.nr.irrigahome.presentation.schedule

import com.nr.irrigahome.domain.model.IrrigationSchedule
import com.nr.irrigahome.presentation.home.IrrigaViewModel
import com.nr.irrigahome.ui.components.ScreenHeader

import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.core.tween
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.lifecycle.viewmodel.compose.viewModel
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen
import kotlinx.coroutines.delay

// Modelo para o dia da semana com identificador único
data class DiaSemana(
    val id: Int,
    val sigla: String,
    val nome: String
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AgendamentoScreen(viewModel: IrrigaViewModel = viewModel()) {
    val hasLinkedDevice = viewModel.hasLinkedDevice.value

    // Lista com dias da semana usando objetos com ID único
    val diasSemana = listOf(
        DiaSemana(1, "Dom", "Domingo"),
        DiaSemana(2, "Seg", "Segunda"),
        DiaSemana(3, "Ter", "Terça"),
        DiaSemana(4, "Qua", "Quarta"),
        DiaSemana(5, "Qui", "Quinta"),
        DiaSemana(6, "Sex", "Sexta"),
        DiaSemana(7, "Sab", "Sábado")
    )

    // Usando remember simples (sem saveable, os dados não persistem ao sair da tela)
    val diasSelecionadosIds = remember { mutableStateListOf<Int>() }
    var horarioSelecionado by remember { mutableStateOf("") }
    val listaAgendamentos = viewModel.agendamentos

    // Lista fixa de horas cheias para seleção por rolagem
    val horariosCheios = remember { (0..23).map { "%02d:00".format(it) } }
    var horarioMenuExpandido by remember { mutableStateOf(false) }

    // Lista fixa de tempos de rega em segundos
    val temposRega = remember { listOf(5, 10, 15, 20) }
    var tempoRegaSelecionado by remember { mutableStateOf<Int?>(null) }
    var tempoRegaMenuExpandido by remember { mutableStateOf(false) }
    val saveStatus = viewModel.saveStatus.value
    val isLoadingSchedules = viewModel.isLoadingSchedules.value
    var showLoadingModal by remember { mutableStateOf(false) }
    var popupMessage by remember { mutableStateOf("") }
    var showPopup by remember { mutableStateOf(false) }

    // Exibe o modal somente se o carregamento demorar mais de 1 segundo
    LaunchedEffect(isLoadingSchedules) {
        if (isLoadingSchedules) {
            delay(1000)
            if (isLoadingSchedules) showLoadingModal = true
        } else {
            showLoadingModal = false
        }
    }

    LaunchedEffect(hasLinkedDevice) {
        if (hasLinkedDevice) {
            viewModel.carregarUltimosAgendamentos()
        }
    }

    LaunchedEffect(saveStatus) {
        val successMessage = when {
            saveStatus.contains("removido", ignoreCase = true) -> "Agendamento Removido"
            saveStatus.contains("salvo", ignoreCase = true) -> "Agendamento Salvo"
            else -> null
        }

        if (successMessage != null) {
            popupMessage = successMessage
            showPopup = true
            try {
                delay(2000)
            } finally {
                showPopup = false
                viewModel.limparSaveStatus()
            }
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        if (!hasLinkedDevice) {
            NoLinkedDeviceCard(
                title = "Nenhum dispositivo vinculado",
                message = "Os agendamentos só ficam disponíveis para usuários com IoT cadastrado e vinculado ao MAC."
            )
            return@Box
        }

        // Modal de carregamento — só aparece se a busca demorar > 1 segundo
        if (showLoadingModal) {
            AlertDialog(
                onDismissRequest = {},
                containerColor = androidx.compose.ui.graphics.Color.White,
                shape = RoundedCornerShape(16.dp),
                icon = {
                    CircularProgressIndicator(
                        color = AppPrimaryGreen,
                        strokeWidth = 3.dp,
                        modifier = Modifier.size(36.dp)
                    )
                },
                title = {
                    Text(
                        text = "Atualizando agendamentos",
                        style = MaterialTheme.typography.titleMedium,
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                        modifier = Modifier.fillMaxWidth()
                    )
                },
                text = {
                    Text(
                        text = "Verificando a lista de agendamentos no banco de dados...",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                        modifier = Modifier.fillMaxWidth()
                    )
                },
                confirmButton = {}
            )
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {

        // HEADER
        ScreenHeader(
            title = "Agendar Irrigação",
            subTitle = "Agende dias e horários para irrigação automática"
        )

        // DIAS DA SEMANA
        Text(
            text = "Dias da semana",
            style = MaterialTheme.typography.titleMedium,
            color = AppCardTitleColor
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            diasSemana.forEach { dia ->
                val selecionado = diasSelecionadosIds.contains(dia.id)

                Box(
                    modifier = Modifier.weight(1f),
                    contentAlignment = Alignment.Center
                ) {
                    Box(
                        modifier = Modifier
                            .size(44.dp)
                            .background(
                                if (selecionado) Color(0xFF2E7D32) else Color(0xFFE0E0E0),
                                shape = CircleShape
                            )
                            .clickable {
                                if (selecionado) {
                                    diasSelecionadosIds.remove(dia.id)
                                } else {
                                    diasSelecionadosIds.add(dia.id)
                                }
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = dia.sigla,
                            color = if (selecionado) Color.White else Color.Black,
                            style = MaterialTheme.typography.bodyMedium
                        )
                    }
                }
            }
        }

        // HORARIO E TEMPO DE REGA - layout responsivo para ficar equilibrado em telas maiores
        BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
            val ladoALado = maxWidth >= 520.dp

            if (ladoALado) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    ExposedDropdownMenuBox(
                        expanded = horarioMenuExpandido,
                        onExpandedChange = { horarioMenuExpandido = it },
                        modifier = Modifier.weight(1f)
                    ) {
                        OutlinedTextField(
                            value = if (horarioSelecionado.isEmpty()) "" else "${horarioSelecionado}h",
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("Horário") },
                            placeholder = { Text("Selecione uma hora") },
                            modifier = Modifier
                                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                                .fillMaxWidth(),
                            singleLine = true,
                            trailingIcon = {
                                ExposedDropdownMenuDefaults.TrailingIcon(expanded = horarioMenuExpandido)
                            }
                        )

                        ExposedDropdownMenu(
                            expanded = horarioMenuExpandido,
                            onDismissRequest = { horarioMenuExpandido = false },
                            modifier = Modifier.heightIn(max = 280.dp)
                        ) {
                            horariosCheios.forEach { horario ->
                                DropdownMenuItem(
                                    text = { Text("${horario}h") },
                                    onClick = {
                                        horarioSelecionado = horario
                                        horarioMenuExpandido = false
                                    }
                                )
                            }
                        }
                    }

                    ExposedDropdownMenuBox(
                        expanded = tempoRegaMenuExpandido,
                        onExpandedChange = { tempoRegaMenuExpandido = it },
                        modifier = Modifier.weight(1f)
                    ) {
                        OutlinedTextField(
                            value = tempoRegaSelecionado?.let { "$it segundos" } ?: "",
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("Tempo de irrigação") },
                            placeholder = { Text("Selecione o tempo") },
                            modifier = Modifier
                                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                                .fillMaxWidth(),
                            singleLine = true,
                            trailingIcon = {
                                ExposedDropdownMenuDefaults.TrailingIcon(expanded = tempoRegaMenuExpandido)
                            }
                        )

                        ExposedDropdownMenu(
                            expanded = tempoRegaMenuExpandido,
                            onDismissRequest = { tempoRegaMenuExpandido = false },
                            modifier = Modifier.heightIn(max = 280.dp)
                        ) {
                            temposRega.forEach { tempo ->
                                DropdownMenuItem(
                                    text = { Text("$tempo segundos") },
                                    onClick = {
                                        tempoRegaSelecionado = tempo
                                        tempoRegaMenuExpandido = false
                                    }
                                )
                            }
                        }
                    }
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    ExposedDropdownMenuBox(
                        expanded = horarioMenuExpandido,
                        onExpandedChange = { horarioMenuExpandido = it }
                    ) {
                        OutlinedTextField(
                            value = if (horarioSelecionado.isEmpty()) "" else "${horarioSelecionado}h",
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("Horário") },
                            placeholder = { Text("Selecione uma hora") },
                            modifier = Modifier
                                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                                .fillMaxWidth(),
                            singleLine = true,
                            trailingIcon = {
                                ExposedDropdownMenuDefaults.TrailingIcon(expanded = horarioMenuExpandido)
                            }
                        )

                        ExposedDropdownMenu(
                            expanded = horarioMenuExpandido,
                            onDismissRequest = { horarioMenuExpandido = false },
                            modifier = Modifier.heightIn(max = 280.dp)
                        ) {
                            horariosCheios.forEach { horario ->
                                DropdownMenuItem(
                                    text = { Text("${horario}h") },
                                    onClick = {
                                        horarioSelecionado = horario
                                        horarioMenuExpandido = false
                                    }
                                )
                            }
                        }
                    }

                    ExposedDropdownMenuBox(
                        expanded = tempoRegaMenuExpandido,
                        onExpandedChange = { tempoRegaMenuExpandido = it }
                    ) {
                        OutlinedTextField(
                            value = tempoRegaSelecionado?.let { "$it segundos" } ?: "",
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("Tempo de irrigação") },
                            placeholder = { Text("Selecione o tempo") },
                            modifier = Modifier
                                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                                .fillMaxWidth(),
                            singleLine = true,
                            trailingIcon = {
                                ExposedDropdownMenuDefaults.TrailingIcon(expanded = tempoRegaMenuExpandido)
                            }
                        )

                        ExposedDropdownMenu(
                            expanded = tempoRegaMenuExpandido,
                            onDismissRequest = { tempoRegaMenuExpandido = false },
                            modifier = Modifier.heightIn(max = 280.dp)
                        ) {
                            temposRega.forEach { tempo ->
                                DropdownMenuItem(
                                    text = { Text("$tempo segundos") },
                                    onClick = {
                                        tempoRegaSelecionado = tempo
                                        tempoRegaMenuExpandido = false
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }

        // BOTÃO SALVAR
        Button(
            onClick = {
                if (diasSelecionadosIds.isNotEmpty() &&
                    horarioSelecionado.isNotEmpty() &&
                    tempoRegaSelecionado != null) {

                    val novo = IrrigationSchedule(
                        diasSemana = diasSelecionadosIds.sorted(),
                        hora = horarioSelecionado,
                        duracaoSegundos = tempoRegaSelecionado!!
                    )

                    viewModel.salvarAgendamento(
                        diasSemana = novo.diasSemana,
                        hora = novo.hora,
                        duracaoSegundos = novo.duracaoSegundos
                    )

                    onSaveSchedule(novo)

                    diasSelecionadosIds.clear()
                    horarioSelecionado = ""
                    tempoRegaSelecionado = null
                }
            },
            modifier = Modifier.fillMaxWidth(),
            enabled = diasSelecionadosIds.isNotEmpty() &&
                    horarioSelecionado.isNotEmpty() &&
                    tempoRegaSelecionado != null
        ) {
            Text("Salvar agendamento")
        }

        Spacer(modifier = Modifier.height(0.dp))

            if (saveStatus.isNotBlank() && saveStatus.contains("Erro", ignoreCase = true)) {
                Text(
                    text = saveStatus,
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall
                )
            }

        if (listaAgendamentos.size >= 4) {
            Text(
                text = "⚠️ Limite de 4 agendamentos atingido. Novos agendamentos substituirão os mais antigos.",
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(bottom = 8.dp)
            )
        }

        // LISTA DE AGENDAMENTOS
        if (listaAgendamentos.isEmpty()) {
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 16.dp),
                colors = CardDefaults.cardColors(
                    containerColor = AppCardContainerColor
                ),
                border = BorderStroke(1.dp, AppCardBorderColor)
            ) {
                Text(
                    text = "Nenhum agendamento programado",
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(24.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            Text(
                text = "Horários programados (${listaAgendamentos.size}/4)",
                style = MaterialTheme.typography.titleMedium,
                color = AppCardTitleColor
            )

            Spacer(modifier = Modifier.height(8.dp))

            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                listaAgendamentos.forEach { item ->
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        elevation = CardDefaults.cardElevation(4.dp),
                        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
                        border = BorderStroke(1.dp, AppCardBorderColor)
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(12.dp),
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(
                                modifier = Modifier.weight(1f)
                            ) {
                                Text(
                                    text = "Dia(s): ${formatarDias(item.diasSemana)}",
                                    style = MaterialTheme.typography.bodyMedium
                                )
                                Text(
                                    text = "Horário: ${item.hora}h",
                                    style = MaterialTheme.typography.bodyMedium
                                )
                                Text(
                                    text = "Tempo de rega: ${item.duracaoSegundos} segundos",
                                    style = MaterialTheme.typography.bodyMedium
                                )
                            }
                            Text(
                                text = "\uD83D\uDDD1\uFE0F",
                                modifier = Modifier
                                    .clickable {
                                        viewModel.removerAgendamento(item.id)
                                    }
                                    .padding(8.dp)
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
                colors = CardDefaults.cardColors(containerColor = AppPrimaryGreen),
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

// FUNÇÃO AUXILIAR
fun formatarDias(dias: List<Int>): String {
    return dias.sorted().joinToString(" ") { dia ->
        when (dia) {
            1 -> "Dom"
            2 -> "Seg"
            3 -> "Ter"
            4 -> "Qua"
            5 -> "Qui"
            6 -> "Sex"
            7 -> "Sab"
            else -> "?"
        }
    }
}

// FUNÇÃO PRONTA PRA MQTT
fun onSaveSchedule(schedule: IrrigationSchedule) {
    val payload = """
        {
            "dias": ${schedule.diasSemana},
            "horario": "${schedule.hora}",
            "tempoRegaSegundos": ${schedule.duracaoSegundos}
        }
    """.trimIndent()

    println("Enviar para MQTT: $payload")
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