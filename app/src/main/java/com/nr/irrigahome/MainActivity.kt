/**
 * Arquivo: MainActivity.kt
 *
 * O que fazemos aqui:
 * - Ponto de entrada principal da aplicação IrrigaHome
 * - Gerencia o ciclo de vida da Activity e inicializa o Jetpack Compose
 * - Controla a navegação entre Splash, Auth e Home a partir do estado da sessão
 * - Mantém a composição reativa para auto-login e logout
 * - Gerencia o suporte a edge-to-edge e tema visual da aplicação
 * - Ativa e desativa o app bar personalizado da aplicação
 */

package com.nr.irrigahome

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.BorderStroke
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen
import com.nr.irrigahome.ui.theme.AppScreenBackgroundColor
import com.nr.irrigahome.ui.theme.IrrigaHomeTheme
import com.nr.irrigahome.presentation.auth.AuthViewModel
import com.nr.irrigahome.presentation.auth.LoginScreen
import com.nr.irrigahome.presentation.home.IrrigaViewModel
import com.nr.irrigahome.presentation.home.PrincipalScreen
import com.nr.irrigahome.presentation.history.HistoricoScreen
import com.nr.irrigahome.presentation.schedule.AgendamentoScreen
import com.nr.irrigahome.domain.model.DeviceValidationState
import com.nr.irrigahome.ui.components.BaseScreen
import com.nr.irrigahome.ui.components.Screen
import com.nr.irrigahome.util.SessionManager
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.delay

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            IrrigaHomeTheme { IrrigaHomeApp(onExitApp = ::finishAffinity) }
        }
    }
}

@Composable
private fun IrrigaHomeApp(onExitApp: () -> Unit) {
    val authViewModel: AuthViewModel = viewModel()
    val irrigationViewModel: IrrigaViewModel = viewModel()
    val uiState = authViewModel.uiState
    val deviceId = irrigationViewModel.deviceId.value

    // flag para sinalizar que o logout acabou de acontecer e devemos
    // exibir um feedback visual na tela de login
    val logoutFeedback = rememberSaveable { mutableStateOf(false) }
    val showSplash = rememberSaveable { mutableStateOf(true) }
    val lifecycleOwner = LocalLifecycleOwner.current

    val performFullLogout = {
        irrigationViewModel.resetSessionState()
        authViewModel.logout()
        SessionManager.resetSession()
        logoutFeedback.value = true
    }

    LaunchedEffect(irrigationViewModel.deviceValidationState.value) {
        when (irrigationViewModel.deviceValidationState.value) {
            DeviceValidationState.Linked -> authViewModel.grantSessionAccess()
            DeviceValidationState.Blocked -> authViewModel.blockSessionAccess("Usuário autenticado, mas sem irrigador vinculado.")
            else -> Unit
        }
    }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (!showSplash.value && (event == Lifecycle.Event.ON_START || event == Lifecycle.Event.ON_RESUME)) {
                if (SessionManager.consumeAutoLogoutPending()) {
                    performFullLogout()
                }
                authViewModel.refreshSessionState()
            }
        }

        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    when {
        showSplash.value -> SplashScreen(onTimeout = {
            showSplash.value = false
            authViewModel.refreshSessionState()
        })
        uiState.isSessionActive == true -> HomePagerContent(deviceId = deviceId, onLogout = {
            // logout efetivo: limpa a sessão e deixa a UI decidir a próxima tela pelo estado
            performFullLogout()
        })
        uiState.isSessionActive == false && irrigationViewModel.deviceValidationState.value == DeviceValidationState.Blocked -> DeviceBindingScreen(
            isLoading = irrigationViewModel.isBindingDevice.value,
            errorMessage = irrigationViewModel.deviceBindingError.value,
            onBind = { code -> irrigationViewModel.bindDeviceToCurrentUser(code) },
            onClearError = { irrigationViewModel.clearDeviceBindingError() },
            onLogout = {
                performFullLogout()
            }
        )
        else -> LoginScreen(
            onLoginSuccess = { irrigationViewModel.refreshLinkedDeviceState() },
            onExitApp = onExitApp,
            authViewModel = authViewModel,
            validationInProgress = irrigationViewModel.deviceValidationState.value == DeviceValidationState.Loading,
            showLogoutFeedback = logoutFeedback.value,
            onConsumeLogoutFeedback = { logoutFeedback.value = false }
        )
    }
}

@Composable
private fun DeviceBindingScreen(
    isLoading: Boolean,
    errorMessage: String?,
    onBind: (String) -> Unit,
    onClearError: () -> Unit,
    onLogout: () -> Unit
) {
    var deviceCode by rememberSaveable { mutableStateOf("") }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(AppScreenBackgroundColor)
            .safeDrawingPadding()
            .padding(horizontal = 16.dp, vertical = 12.dp),
        contentAlignment = Alignment.Center
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = AppCardContainerColor),
            border = BorderStroke(1.dp, AppCardBorderColor),
            elevation = CardDefaults.cardElevation(defaultElevation = 4.dp),
            shape = RoundedCornerShape(16.dp)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 18.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Text(
                    text = "Vincular irrigador",
                    style = MaterialTheme.typography.titleMedium,
                    color = AppCardTitleColor,
                    fontWeight = FontWeight.SemiBold
                )

                Text(
                    text = "Digite o código do irrigador exibido no painel local do ESP32.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color(0xFF3E4A59),
                    textAlign = TextAlign.Center
                )

                OutlinedTextField(
                    value = deviceCode,
                    onValueChange = {
                        deviceCode = it.trim().lowercase()
                        if (errorMessage != null) onClearError()
                    },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    label = { Text("Código do irrigador") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
                    enabled = !isLoading
                )

                if (!errorMessage.isNullOrBlank()) {
                    Text(
                        text = errorMessage,
                        style = MaterialTheme.typography.bodySmall,
                        color = Color(0xFFB42318),
                        textAlign = TextAlign.Center
                    )
                }

                Button(
                    onClick = { onBind(deviceCode) },
                    enabled = !isLoading,
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = AppPrimaryGreen),
                    shape = RoundedCornerShape(12.dp)
                ) {
                    if (isLoading) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(18.dp),
                            color = Color.White,
                            strokeWidth = 2.dp
                        )
                    } else {
                        Text("Vincular dispositivo", color = Color.White)
                    }
                }

                TextButton(onClick = onLogout, enabled = !isLoading) {
                    Text("Sair da conta")
                }
            }
        }
    }
}

@Composable
private fun HomePagerContent(deviceId: String?, onLogout: () -> Unit) {
    val screens = remember { Screen.entries }
    var currentScreen by rememberSaveable { mutableStateOf(Screen.PRINCIPAL) }
    var showLogoutConfirmation by rememberSaveable { mutableStateOf(false) }
    val pagerState = rememberPagerState(
        initialPage = screens.indexOf(currentScreen),
        pageCount = { screens.size }
    )

    LaunchedEffect(currentScreen) {
        val targetPage = screens.indexOf(currentScreen)
        if (targetPage >= 0 && pagerState.currentPage != targetPage) {
            pagerState.animateScrollToPage(targetPage)
        }
    }

    LaunchedEffect(pagerState.currentPage) {
        val pageScreen = screens.getOrNull(pagerState.currentPage) ?: Screen.PRINCIPAL
        if (pageScreen != currentScreen) {
            currentScreen = pageScreen
        }
    }

    BaseScreen(
        currentScreen = currentScreen,
        onScreenSelected = { selected ->
            currentScreen = selected
        },
        onLogout = {
            // Ao clicar em "Sair", abre diálogo de confirmação
            showLogoutConfirmation = true
        },
        deviceId = deviceId
    ) {
        HorizontalPager(state = pagerState) { page ->
            when (screens[page]) {
                Screen.PRINCIPAL -> PrincipalScreen()
                Screen.AGENDAMENTO -> AgendamentoScreen()
                Screen.HISTORICO -> HistoricoScreen()
            }
        }
    }

    // Diálogo de confirmação de logout
    if (showLogoutConfirmation) {
        StyledConfirmLogoutDialog(
            onDismiss = { showLogoutConfirmation = false },
            onConfirm = {
                showLogoutConfirmation = false
                onLogout()
            }
        )
    }
}

// SplashScreen
@Composable
fun SplashScreen(onTimeout: () -> Unit) {
    LaunchedEffect(Unit) {
        delay(3000)
        onTimeout()
    }
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(AppScreenBackgroundColor)
            .safeDrawingPadding()
    ) {
        // Logo marca e Slogan - Centralizados
        Column(
            modifier = Modifier
                .fillMaxSize(),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = "🌱",
                color = AppPrimaryGreen,
                fontSize = 64.sp
            )
            Text(
                //text = "Irriga Home",
                //color = AppCardTitleColor,
                //fontSize = 28.sp
                text = "Irriga Home",
                fontSize = 28.sp,
                fontWeight = FontWeight.Bold,
                color = AppCardTitleColor,
                maxLines = 1

            )
            Text(
                text = "Cuidando das suas plantas com inteligência",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 14.sp,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
        }
        // Footer
        Text(
            text = "Versão ${BuildConfig.VERSION_NAME} · CodeWave | 2026",
            fontSize = 9.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 12.dp),
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

// Diálogo customizado simples de confirmação de logout com identidade visual
@Composable
fun StyledConfirmLogoutDialog(
    onDismiss: () -> Unit,
    onConfirm: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = AppCardContainerColor,
        shape = RoundedCornerShape(12.dp),
        icon = {
            Box(modifier = Modifier.size(44.dp), contentAlignment = Alignment.Center) {
                Text(
                    text = "❓",
                    fontSize = 28.sp,
                    color = AppPrimaryGreen
                )
            }
        },
        title = {
            Text(
                text = "Confirmar saída",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = AppCardTitleColor
            )
        },
        text = {
            Text(
                text = "Tem certeza que deseja sair da aplicação?",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        },
        confirmButton = {
            Button(
                onClick = onConfirm,
                colors = ButtonDefaults.buttonColors(containerColor = AppPrimaryGreen)
            ) {
                Text(text = "Sair", color = Color.White)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(text = "Cancelar", color = AppPrimaryGreen)
            }
        }
    )
}
