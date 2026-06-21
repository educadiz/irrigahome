/**
 * Arquivo: LoginScreen.kt
 * 
 * Responsabilidade:
 * - Composable que renderiza a tela de login e registro da aplicação
 * - Gerencia os campos de entrada: e-mail, senha, nome (para registro)
 * - Exibe alternância entre modo login e modo de registro
 * - Renderiza popups de carregamento durante autenticação
 * - Renderiza popups de erro com mensagens customizadas
 * - Implementa validação visual dos campos de entrada
 * - Gerencia visibilidade de senha e campos obrigatórios
 * - Estiliza a tela com identidade visual do app (cores, tipografia, card)
 * - Integra callbacks com AuthViewModel para autenticação
 * - Fornece opções de "Esqueci minha senha" e "Primeiro acesso"
 * - Renderiza botão "Sair" para encerrar a aplicação
 */

package com.nr.irrigahome.presentation.auth

import com.nr.irrigahome.data.local.LoginPreferencesRepository
import com.nr.irrigahome.ui.components.AuthLoadingPopup

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nr.irrigahome.ui.theme.AppCardBorderColor
import com.nr.irrigahome.ui.theme.AppCardContainerColor
import com.nr.irrigahome.ui.theme.AppCardTitleColor
import com.nr.irrigahome.ui.theme.AppPrimaryGreen
import com.nr.irrigahome.ui.theme.AppScreenBackgroundColor

@Composable
fun LoginScreen(
    onLoginSuccess: () -> Unit,
    onExitApp: () -> Unit,
    authViewModel: AuthViewModel,
    validationInProgress: Boolean = false,
    showLogoutFeedback: Boolean = false,
    onConsumeLogoutFeedback: () -> Unit = {}
) {
    val uiState = authViewModel.uiState
    val context = LocalContext.current
    val loginPreferences = remember(context) { LoginPreferencesRepository(context) }

    var fullName by rememberSaveable { mutableStateOf("") }
    var email by rememberSaveable { mutableStateOf("") }
    var password by rememberSaveable { mutableStateOf("") }
    var confirmPassword by rememberSaveable { mutableStateOf("") }
    var rememberUser by rememberSaveable { mutableStateOf(false) }
    var showNoInternetDialog by rememberSaveable { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        val savedState = loginPreferences.load()
        rememberUser = savedState.rememberUser
        if (savedState.rememberUser && email.isBlank()) {
            email = savedState.email
        }
    }

    LaunchedEffect(uiState.isAuthenticated) {
        if (uiState.isAuthenticated) {
            if (rememberUser) {
                loginPreferences.save(email)
            } else {
                loginPreferences.clear()
            }
            onLoginSuccess()
            authViewModel.consumeAuthenticationSuccess()
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(AppScreenBackgroundColor)
            .padding(horizontal = 12.dp, vertical = 10.dp),
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
                    .padding(horizontal = 14.dp, vertical = 16.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                AuthBrandHeader()

                Spacer(modifier = Modifier.height(4.dp))

                AuthCredentialFields(
                    isRegisterMode = uiState.isRegisterMode,
                    fullName = fullName,
                    onFullNameChange = { fullName = it },
                    email = email,
                    onEmailChange = { email = it },
                    password = password,
                    onPasswordChange = { password = it },
                    confirmPassword = confirmPassword,
                    onConfirmPasswordChange = { confirmPassword = it }
                )

                if (!uiState.isRegisterMode) {
                    AuthRememberUserToggle(
                        checked = rememberUser,
                        onCheckedChange = { checked ->
                            rememberUser = checked
                            if (!checked) {
                                loginPreferences.clear()
                            }
                        }
                    )
                }

                Spacer(modifier = Modifier.height(6.dp))

                AuthPrimaryButton(
                    isLoading = uiState.isLoading,
                    isRegisterMode = uiState.isRegisterMode,
                    onClick = {
                        val cm = context.getSystemService(ConnectivityManager::class.java)
                        val hasInternet = cm?.activeNetwork
                            ?.let { cm.getNetworkCapabilities(it) }
                            ?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true

                        if (!hasInternet) {
                            showNoInternetDialog = true
                            return@AuthPrimaryButton
                        }

                        if (uiState.isRegisterMode) {
                            authViewModel.register(
                                name = fullName,
                                email = email,
                                password = password,
                                confirmPassword = confirmPassword
                            )
                        } else {
                            authViewModel.login(email = email, password = password)
                        }
                    }
                )

                AuthSecondaryButton(onClick = onExitApp)

                AuthAuxiliaryActions(
                    isRegisterMode = uiState.isRegisterMode,
                    onPasswordReset = { authViewModel.sendPasswordReset(email) },
                    onToggleMode = { authViewModel.setRegisterMode(!uiState.isRegisterMode) }
                )
            }
        }
        // footer
        Text(
            //text = "Criado e desenvolvido por CodeWave | 2026",
            //fontSize = 10.sp,
            //color = MaterialTheme.colorScheme.onSurfaceVariant,
            //modifier = Modifier
            //    .align(Alignment.BottomCenter)
            //    .padding(bottom = 12.dp),
            //textAlign = TextAlign.Center,
            //maxLines = 1

            text = "Criado e desenvolvido por CodeWave | 2026",
            fontSize = 9.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 50.dp),
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }

    AuthLoadingPopup(visible = uiState.isLoading || validationInProgress)

    if (showNoInternetDialog) {
        StyledAuthDialog(
            onDismiss = { showNoInternetDialog = false },
            icon = "📵",
            title = "Sem conexão",
            message = "Não há conexão com a internet disponível. Conecte-se à Web para acessar o aplicativo.",
            onConfirm = { showNoInternetDialog = false }
        )
    }

    if (!validationInProgress) {
        uiState.errorMessage?.let { message ->
            StyledAuthDialog(
                onDismiss = { authViewModel.clearError() },
                icon = "⚠\uFE0F",
                title = "Atenção",
                message = message,
                onConfirm = { authViewModel.clearError() }
            )
        }

        uiState.infoMessage?.let { infoMessage ->
            StyledAuthDialog(
                onDismiss = { authViewModel.clearInfo() },
                icon = "🌱",
                title = "Aviso",
                message = infoMessage,
                onConfirm = { authViewModel.clearInfo() }
            )
        }
    }

    // Feedback visual após logout (vindo da ação na tela principal)
    if (showLogoutFeedback) {
        StyledAuthDialog(
            onDismiss = onConsumeLogoutFeedback,
            icon = "✅",
            title = "Sessão encerrada",
            message = "Você saiu com sucesso.",
            onConfirm = onConsumeLogoutFeedback
        )
    }
}

@Composable
private fun AuthBrandHeader() {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = "🌱",
            color = AppPrimaryGreen,
            fontSize = 48.sp
        )

        Text(
            text = "Irriga Home",
            fontSize = 32.sp,
            fontWeight = FontWeight.Bold,
            color = AppCardTitleColor,
            maxLines = 1
        )

        Text(
            text = "Entre para acessar o monitoramento das suas plantas",
            style = MaterialTheme.typography.bodySmall,
            textAlign = TextAlign.Center,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun AuthCredentialFields(
    isRegisterMode: Boolean,
    fullName: String,
    onFullNameChange: (String) -> Unit,
    email: String,
    onEmailChange: (String) -> Unit,
    password: String,
    onPasswordChange: (String) -> Unit,
    confirmPassword: String,
    onConfirmPasswordChange: (String) -> Unit
) {
    var passwordVisible by rememberSaveable { mutableStateOf(false) }
    var confirmPasswordVisible by rememberSaveable { mutableStateOf(false) }

    if (isRegisterMode) {
        AuthTextField(
            value = fullName,
            onValueChange = onFullNameChange,
            label = "Nome completo",
            placeholder = "Seu nome"
        )
    }

    AuthTextField(
        value = email,
        onValueChange = onEmailChange,
        label = "E-mail",
        placeholder = "usuario@email.com"
    )

    AuthTextField(
        value = password,
        onValueChange = onPasswordChange,
        label = "Senha",
        placeholder = "senha",
        visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
        trailingIcon = {
            IconButton(onClick = { passwordVisible = !passwordVisible }) {
                Icon(
                    imageVector = if (passwordVisible) Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                    contentDescription = if (passwordVisible) "Ocultar senha" else "Mostrar senha"
                )
            }
        }
    )

    if (isRegisterMode) {
        AuthTextField(
            value = confirmPassword,
            onValueChange = onConfirmPasswordChange,
            label = "Confirmar senha",
            placeholder = "Repita a senha",
            visualTransformation = if (confirmPasswordVisible) VisualTransformation.None else PasswordVisualTransformation(),
            trailingIcon = {
                IconButton(onClick = { confirmPasswordVisible = !confirmPasswordVisible }) {
                    Icon(
                        imageVector = if (confirmPasswordVisible) Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                        contentDescription = if (confirmPasswordVisible) "Ocultar confirmação de senha" else "Mostrar confirmação de senha"
                    )
                }
            }
        )
    }
}

@Composable
private fun AuthTextField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    placeholder: String,
    visualTransformation: VisualTransformation = VisualTransformation.None,
    trailingIcon: @Composable (() -> Unit)? = null
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = Modifier.fillMaxWidth(),
        singleLine = true,
        label = { Text(label) },
        placeholder = { Text(placeholder) },
        visualTransformation = visualTransformation,
        trailingIcon = trailingIcon,
        textStyle = MaterialTheme.typography.bodyMedium,
        colors = OutlinedTextFieldDefaults.colors(
            focusedBorderColor = AppPrimaryGreen,
            unfocusedBorderColor = AppCardBorderColor,
            focusedLabelColor = AppCardTitleColor,
            cursorColor = AppPrimaryGreen
        )
    )
}

@Composable
private fun AuthPrimaryButton(
    isLoading: Boolean,
    isRegisterMode: Boolean,
    onClick: () -> Unit
) {
    Button(
        onClick = onClick,
        modifier = Modifier
            .fillMaxWidth()
            .height(42.dp),
        enabled = !isLoading
    ) {
        if (isLoading) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.onPrimary
            )
        } else {
            Text(
                text = if (isRegisterMode) "Criar conta" else "Entrar",
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1
            )
        }
    }
}

@Composable
private fun AuthSecondaryButton(onClick: () -> Unit) {
    OutlinedButton(
        onClick = onClick,
        modifier = Modifier
            .fillMaxWidth()
            .height(42.dp),
        border = BorderStroke(1.dp, AppCardBorderColor)
    ) {
        Text(
            text = "Sair",
            style = MaterialTheme.typography.labelLarge,
            color = AppCardTitleColor,
            maxLines = 1
        )
    }
}

@Composable
private fun AuthAuxiliaryActions(
    isRegisterMode: Boolean,
    onPasswordReset: () -> Unit,
    onToggleMode: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically
    ) {
        TextButton(onClick = onPasswordReset) {
            Text("Esqueci minha senha")
        }

        Spacer(modifier = Modifier.width(6.dp))

        TextButton(onClick = onToggleMode) {
            Text(if (isRegisterMode) "Já tenho conta" else "Primeiro acesso")
        }
    }
}

@Composable
private fun AuthRememberUserToggle(
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 2.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Checkbox(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = CheckboxDefaults.colors(
                checkedColor = AppPrimaryGreen,
                uncheckedColor = AppCardBorderColor,
                checkmarkColor = AppScreenBackgroundColor
            )
        )

        Text(
            text = "Lembrar usuário",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
private fun StyledAuthDialog(
    onDismiss: () -> Unit,
    icon: String,
    title: String,
    message: String,
    onConfirm: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = AppCardContainerColor,
        shape = RoundedCornerShape(16.dp),
        tonalElevation = 2.dp,
        icon = {
            Text(
                text = icon,
                fontSize = 22.sp,
                color = AppPrimaryGreen,
                modifier = Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center
            )
        },
        title = {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = AppCardTitleColor,
                modifier = Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center
            )
        },
        text = {
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 6.dp),
                textAlign = TextAlign.Center
            )
        },
        confirmButton = {
            Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                Button(
                    onClick = onConfirm,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = AppPrimaryGreen,
                        contentColor = MaterialTheme.colorScheme.onPrimary
                    ),
                    shape = RoundedCornerShape(10.dp)
                ) {
                    Text(text = "Ok", style = MaterialTheme.typography.labelLarge)
                }
            }
        }
    )
}


