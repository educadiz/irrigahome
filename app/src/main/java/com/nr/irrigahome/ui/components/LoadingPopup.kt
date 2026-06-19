/**
 * Arquivo: LoadingPopup.kt
 *
 * Responsabilidade:
 * - Componente Composable que renderiza diálogo de carregamento bloqueante
 * - Exibe indicador circular de progresso durante operações assíncronas
 * - Permite customização de título e mensagem de carregamento
 * - Suporta exibição de ícone/emoji personalizado
 * - Impede interação com elementos de fundo durante o carregamento
 * - Mantém consistência visual com tema da aplicação
 * - Utilizado para feedback visual durante:
 *   - Autenticação (login/registro)
 *   - Conexão com dispositivo IoT
 *   - Operações de sincronização com Firestore
 * - Não permite fechamento por clique fora ou botão de voltar
 * - Renderização responsiva adaptada para diferentes tamanhos de tela
 */

package com.nr.irrigahome.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen

@Composable
fun LoadingPopup(
    visible: Boolean,
    title: String,
    message: String,
    icon: String? = null
) {
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
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 190.dp)
                    .padding(horizontal = 20.dp, vertical = 18.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterVertically)
            ) {
                icon?.let {
                    Text(
                        text = it,
                        fontSize = 30.sp,
                        modifier = Modifier.fillMaxWidth(),
                        textAlign = TextAlign.Center,
                        maxLines = 1
                    )
                }

                CircularProgressIndicator(
                    modifier = Modifier.size(36.dp),
                    color = AppPrimaryGreen,
                    strokeWidth = 3.dp
                )

                Text(
                    text = title,
                    fontWeight = FontWeight.Bold,
                    fontSize = 16.sp,
                    color = AppCardTitleColor,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )

                Text(
                    text = message,
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
fun AuthLoadingPopup(visible: Boolean) {
    LoadingPopup(
        visible = visible,
        icon = "🌱",
        title = "Autenticando",
        message = "Validando seus dados de acesso..."
    )
}

@Composable
fun DeviceConnectingPopup(visible: Boolean) {
    LoadingPopup(
        visible = visible,
        icon = "📡",
        title = "Conectando ao dispositivo irrigador",
        message = "Aguarde enquanto recebemos os dados de telemetria..."
    )
}
