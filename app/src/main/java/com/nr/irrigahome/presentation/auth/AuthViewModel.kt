package com.nr.irrigahome.presentation.auth

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.nr.irrigahome.domain.usecase.AutoLoginUseCase
import com.nr.irrigahome.domain.usecase.LoginUseCase
import com.nr.irrigahome.domain.usecase.LogoutUseCase
import com.nr.irrigahome.domain.usecase.RegisterUseCase
import com.nr.irrigahome.domain.usecase.SendPasswordResetUseCase
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

data class AuthUiState(
    val isLoading: Boolean = false,
    val isRegisterMode: Boolean = false,
    val isAuthenticated: Boolean = false,
    val isSessionActive: Boolean? = null,
    val errorMessage: String? = null,
    val infoMessage: String? = null
)

@HiltViewModel
class AuthViewModel @Inject constructor(
    private val autoLoginUseCase: AutoLoginUseCase,
    private val loginUseCase: LoginUseCase,
    private val logoutUseCase: LogoutUseCase,
    private val registerUseCase: RegisterUseCase,
    private val sendPasswordResetUseCase: SendPasswordResetUseCase
) : ViewModel() {

    companion object {
        private const val MESSAGE_MISSING_LOGIN_FIELDS = "Informe e-mail e senha."
        private const val MESSAGE_MISSING_NAME = "Informe o nome."
        private const val MESSAGE_MISSING_REGISTRATION_FIELDS = "Preencha nome, e-mail e senha."
        private const val MESSAGE_PASSWORD_TOO_SHORT = "A senha deve ter ao menos 6 caracteres."
        private const val MESSAGE_PASSWORDS_MISMATCH = "As senhas não coincidem."
        private const val MESSAGE_RESET_EMAIL_REQUIRED = "Informe o e-mail para recuperar a senha."
        private const val MESSAGE_RESET_EMAIL_SENT = "Enviamos um e-mail para redefinir a senha."
        private const val MESSAGE_GENERIC_AUTH_FAILURE = "Falha na autenticação."
        private const val MESSAGE_INVALID_LOGIN_DATA = "Os dados informados estao incorretos ou inconsistentes. Entre com os dados novamente."
    }

    var uiState by mutableStateOf(AuthUiState())
        private set

    init {
        refreshSessionState()
    }

    fun setRegisterMode(enabled: Boolean) {
        updateState(isRegisterMode = enabled, errorMessage = null, infoMessage = null)
    }

    fun clearError() {
        updateState(errorMessage = null)
    }

    fun clearInfo() {
        updateState(infoMessage = null)
    }

    fun consumeAuthenticationSuccess() {
        updateState(isAuthenticated = false)
    }

    fun refreshSessionState() {
        val hasFirebaseSession = autoLoginUseCase()
        updateState(
            isAuthenticated = hasFirebaseSession,
            isSessionActive = if (hasFirebaseSession) uiState.isSessionActive else false,
            errorMessage = null,
            infoMessage = null
        )
    }

    fun grantSessionAccess() {
        updateState(isSessionActive = true, errorMessage = null, infoMessage = null)
    }

    fun blockSessionAccess(message: String) {
        updateState(isSessionActive = false, errorMessage = message, infoMessage = null)
    }

    fun logout() {
        logoutUseCase()
        updateState(
            isAuthenticated = false,
            isSessionActive = false,
            errorMessage = null,
            infoMessage = null
        )
    }

    fun login(email: String, password: String) {
        val cleanEmail = email.trim()
        if (cleanEmail.isBlank() || password.isBlank()) {
            setError(MESSAGE_MISSING_LOGIN_FIELDS); return
        }

        setLoading(true)
        clearTransientMessages()

        loginUseCase(
            email = cleanEmail,
            password = password,
            onSuccess = {
                setLoading(false)
                updateState(isAuthenticated = true, isSessionActive = false)
            },
            onFailure = { error ->
                setLoading(false)
                setError(mapAuthError(error))
            }
        )
    }

    fun register(name: String, email: String, password: String, confirmPassword: String) {
        val cleanName = name.trim()
        val cleanEmail = email.trim()

        when {
            cleanName.isBlank() -> { setError(MESSAGE_MISSING_NAME); return }
            cleanEmail.isBlank() || password.isBlank() || confirmPassword.isBlank() -> {
                setError(MESSAGE_MISSING_REGISTRATION_FIELDS); return
            }
            password.length < 6 -> { setError(MESSAGE_PASSWORD_TOO_SHORT); return }
            password != confirmPassword -> { setError(MESSAGE_PASSWORDS_MISMATCH); return }
        }

        setLoading(true)
        clearTransientMessages()

        registerUseCase(
            name = cleanName,
            email = cleanEmail,
            password = password,
            onSuccess = {
                setLoading(false)
                updateState(
                    isRegisterMode = false,
                    isAuthenticated = true,
                    isSessionActive = false,
                    infoMessage = "Cadastro realizado com sucesso."
                )
            },
            onFailure = { error ->
                setLoading(false)
                setError(mapAuthError(error))
            }
        )
    }

    fun sendPasswordReset(email: String) {
        val cleanEmail = email.trim()
        if (cleanEmail.isBlank()) { setError(MESSAGE_RESET_EMAIL_REQUIRED); return }

        setLoading(true)
        clearTransientMessages()
        sendPasswordResetUseCase(
            email = cleanEmail,
            onSuccess = { setLoading(false); setInfo() },
            onFailure = { error -> setLoading(false); setError(mapAuthError(error)) }
        )
    }

    private fun mapAuthError(error: Exception): String {
        val message = error.message?.lowercase().orEmpty()
        return when {
            "password is invalid" in message ||
                "wrong-password" in message ||
                "invalid login credentials" in message ||
                "supplied auth credential is incorrect" in message ||
                "invalid-credential" in message ||
                "no user record" in message ||
                "user-not-found" in message -> MESSAGE_INVALID_LOGIN_DATA
            "badly formatted" in message -> "E-mail ou senha invalidos."
            "already in use" in message || "email-already-in-use" in message -> "Este e-mail ja esta cadastrado."
            else -> error.message ?: MESSAGE_GENERIC_AUTH_FAILURE
        }
    }

    private fun setLoading(isLoading: Boolean) { updateState(isLoading = isLoading) }
    private fun setError(message: String) { updateState(errorMessage = message) }
    private fun setInfo() { updateState(infoMessage = MESSAGE_RESET_EMAIL_SENT) }
    private fun clearTransientMessages() { updateState(errorMessage = null, infoMessage = null) }

    private fun updateState(
        isLoading: Boolean = uiState.isLoading,
        isRegisterMode: Boolean = uiState.isRegisterMode,
        isAuthenticated: Boolean = uiState.isAuthenticated,
        isSessionActive: Boolean? = uiState.isSessionActive,
        errorMessage: String? = uiState.errorMessage,
        infoMessage: String? = uiState.infoMessage
    ) {
        uiState = uiState.copy(
            isLoading = isLoading,
            isRegisterMode = isRegisterMode,
            isAuthenticated = isAuthenticated,
            isSessionActive = isSessionActive,
            errorMessage = errorMessage,
            infoMessage = infoMessage
        )
    }
}
