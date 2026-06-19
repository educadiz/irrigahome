/**
 * Arquivo: ManualIrrigationConflictDialog.kt
 *
 * Responsabilidade:
 * - Renderizar o diálogo de alerta de conflito de irrigação manual
 * - Exibir: horário programado, tempo restante em linguagem natural, aviso de redundância
 * - Oferecer duas ações: aguardar (prioridade visual verde) e irrigar mesmo assim (secundária)
 * - Stateless: recebe dados via parâmetros, emite eventos via lambdas
 *
 * Sem alterações em relação à v1.
 */

package com.nr.irrigahome.ui.components

import com.nr.irrigahome.domain.model.ConflictCheckResult
import com.nr.irrigahome.domain.model.IrrigationSchedule

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen

@Composable
fun ManualIrrigationConflictDialog(
    conflict: ConflictCheckResult.ConflictDetected,
    onCancel: () -> Unit,
    onProceed: () -> Unit
) {
    val timeLabel = conflict.scheduledTimeLabel
    val minutesUntil = conflict.minutesUntilStart

    val timeRemainingText = when {
        minutesUntil == 0L  -> "em menos de 1 minuto"
        minutesUntil == 1L  -> "em 1 minuto"
        minutesUntil < 60L  -> "em $minutesUntil minutos"
        minutesUntil == 60L -> "em 1 hora"
        else                -> "em ${minutesUntil / 60}h ${minutesUntil % 60}min"
    }

    Dialog(
        onDismissRequest = onCancel,
        properties = DialogProperties(
            dismissOnBackPress    = true,
            dismissOnClickOutside = false,
            usePlatformDefaultWidth = true
        )
    ) {
        Surface(
            modifier        = Modifier.fillMaxWidth(),
            color           = AppCardContainerColor,
            shape           = MaterialTheme.shapes.extraLarge,
            shadowElevation = 10.dp,
            border          = BorderStroke(1.dp, AppCardBorderColor)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 22.dp, vertical = 20.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(14.dp)
            ) {
                // Ícone
                Surface(color = Color(0xFFFFF3E0), shape = CircleShape) {
                    Text(
                        text     = "⚠️",
                        fontSize = 28.sp,
                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 8.dp)
                    )
                }

                // Título
                Text(
                    text       = "Agendamento próximo",
                    fontWeight = FontWeight.Bold,
                    fontSize   = 17.sp,
                    color      = AppCardTitleColor,
                    textAlign  = TextAlign.Center,
                    maxLines   = 1
                )

                HorizontalDivider(color = AppCardBorderColor, thickness = 0.8.dp)

                // Informações do conflito
                Column(
                    modifier            = Modifier.fillMaxWidth(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    ConflictInfoRow(label = "Próxima irrigação agendada", value = "às $timeLabel h")
                    ConflictInfoRow(label = "Tempo restante", value = timeRemainingText)
                }

                HorizontalDivider(color = AppCardBorderColor, thickness = 0.8.dp)

                // Aviso
                Text(
                    text = "Irrigar agora pode ser redundante. " +
                           "O solo já será irrigado automaticamente $timeRemainingText, " +
                           "o que pode resultar em excesso de água.",
                    fontSize   = 13.sp,
                    color      = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign  = TextAlign.Center,
                    lineHeight = 19.sp
                )

                // Ações
                Column(
                    modifier            = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Button(
                        onClick  = onCancel,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 44.dp),
                        colors   = ButtonDefaults.buttonColors(containerColor = AppPrimaryGreen)
                    ) {
                        Text(
                            text       = "Aguardar irrigação agendada",
                            fontWeight = FontWeight.SemiBold,
                            fontSize   = 14.sp
                        )
                    }

                    OutlinedButton(
                        onClick  = onProceed,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 44.dp),
                        border   = BorderStroke(1.dp, AppCardBorderColor)
                    ) {
                        Text(
                            text     = "Irrigar agora mesmo assim",
                            fontSize = 13.sp,
                            color    = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ConflictInfoRow(label: String, value: String) {
    Row(
        modifier              = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment     = Alignment.CenterVertically
    ) {
        Text(
            text     = label,
            fontSize = 13.sp,
            color    = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f)
        )
        Text(
            text       = value,
            fontSize   = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color      = AppCardTitleColor,
            textAlign  = TextAlign.End,
            modifier   = Modifier.padding(start = 8.dp)
        )
    }
}
