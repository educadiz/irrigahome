/**
 * Arquivo: BaseScreen.kt
 *
 * Responsabilidade:
 * - Define o layout base e estrutura comum para todas as telas principais
 * - Implementa navegação por abas inferiores entre: Principal, Agendamento, Histórico
 * - Gerencia transições de telas com animações suaves
 * - Renderiza app bar superior com título dinâmico
 * - Implementa bottom navigation bar com ícones e labels para cada seção
 * - Fornece estrutura para conteúdo variável entre as telas
 * - Gerencia scaffold com tema visual consistente
 * - Coordena estado de navegação entre diferentes screens
 * - Aplica padding seguro em relação ao sistema (edge-to-edge)
 * - Mantém identidade visual uniforme em toda a aplicação
 */

// Esta é a tela base que conterá o layout para as
// outras telas

package com.nr.irrigahome.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Logout
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.sp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen
import com.nr.irrigahome.ui.theme.AppScreenBackgroundColor

// 🔹 Enum para controlar as telas
enum class Screen {
    PRINCIPAL,      // Tela inicial Principal
    AGENDAMENTO,    // Tela de Agendamento
    HISTORICO       // Tela de Histórico
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BaseScreen(
    currentScreen: Screen,
    onScreenSelected: (Screen) -> Unit,
    onLogout: () -> Unit = {},
    deviceId: String? = null,
    content: @Composable () -> Unit
) {

    val greenColor = AppPrimaryGreen

    Scaffold(
        // Montagem da barra superior
        containerColor = AppScreenBackgroundColor,
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            text = "Irriga 🌱 Home",
                            style = MaterialTheme.typography.titleLarge,
                            color = Color.White,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            fontWeight = FontWeight.Bold
                        )

                        formatMacAddressForHeader(deviceId)?.let { formattedMac ->
                            Text(
                                text = "MAC Address: $formattedMac",
                                style = MaterialTheme.typography.labelSmall,
                                color = Color.White.copy(alpha = 0.72f),
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                fontWeight = FontWeight.Normal,
                                textAlign = TextAlign.Start
                            )
                        }
                    }
                },
                actions = {
                    // botão de logout no appbar - à direita
                    IconButton(onClick = onLogout) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.Logout,
                            contentDescription = "Sair",
                            tint = Color.White,
                            modifier = Modifier.size(24.dp)
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = greenColor,
                    titleContentColor = Color.White,
                    actionIconContentColor = Color.White
                )
            )
        },

        // Montagem da barra inferior e controle de navegação
        bottomBar = {
            NavigationBar(
                containerColor = greenColor
            ) {

                NavigationBarItem(
                    selected = currentScreen == Screen.PRINCIPAL,
                    onClick = { onScreenSelected(Screen.PRINCIPAL) },
                    icon = {
                        Text("🚰", fontSize = 28.sp)
                    },
                    label = {
                        Text("Irrigação", maxLines = 1)
                    },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = Color.White,
                        selectedTextColor = Color.White,
                        unselectedIconColor = Color.White,
                        unselectedTextColor = Color.White,
                        indicatorColor = Color(0x66FFFFFF)
                    )
                )

                NavigationBarItem(
                    selected = currentScreen == Screen.AGENDAMENTO,
                    onClick = { onScreenSelected(Screen.AGENDAMENTO) },
                    icon = {
                        Text("📅", fontSize = 28.sp)
                    },
                    label = {
                        Text("Agendamento", maxLines = 1)
                    },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = Color.White,
                        selectedTextColor = Color.White,
                        unselectedIconColor = Color.White,
                        unselectedTextColor = Color.White,
                        indicatorColor = Color(0x66FFFFFF)
                    )
                )

                NavigationBarItem(
                    selected = currentScreen == Screen.HISTORICO,
                    onClick = { onScreenSelected(Screen.HISTORICO) },
                    icon = {
                        Text("📊", fontSize = 28.sp)
                    },
                    label = {
                        Text("Histórico", maxLines = 1)
                    },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = Color.White,
                        selectedTextColor = Color.White,
                        unselectedIconColor = Color.White,
                        unselectedTextColor = Color.White,
                        indicatorColor = Color(0x66FFFFFF)
                    )
                )
            }
        }

    ) { paddingValues ->
        // Área de conteúdo principal, onde as telas específicas
        // serão renderizadas
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(AppScreenBackgroundColor)
                .padding(paddingValues)
        ) {
            content()
        }
    }
}

private fun formatMacAddressForHeader(rawMacAddress: String?): String? {
    val normalized = rawMacAddress
        ?.trim()
        ?.replace(":", "")
        ?.replace("-", "")
        ?.lowercase()
        ?.takeIf { it.length == 12 && it.all { char -> char.isDigit() || char in 'a'..'f' } }
        ?: return null

    return normalized.chunked(2).joinToString(":")
}

// Base para o cabeçalho das telas, com título e subtítulo
@Composable
fun ScreenHeader(title: String, subTitle: String) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 8.dp),
        elevation = CardDefaults.cardElevation(3.dp),
        colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
        border = BorderStroke(1.dp, AppCardBorderColor)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {

            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = AppCardTitleColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )

            Spacer(modifier = Modifier.height(2.dp))

            Text(
                text = subTitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}