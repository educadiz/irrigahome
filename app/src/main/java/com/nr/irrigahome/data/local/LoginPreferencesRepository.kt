package com.nr.irrigahome.data.local

import android.content.Context

data class RememberedLoginState(
    val rememberUser: Boolean = false,
    val email: String = ""
)

class LoginPreferencesRepository(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun load(): RememberedLoginState {
        val rememberUser = prefs.getBoolean(KEY_REMEMBER_USER, false)
        val email = prefs.getString(KEY_REMEMBERED_EMAIL, "").orEmpty()
        return RememberedLoginState(
            rememberUser = rememberUser,
            email = email
        )
    }

    fun save(email: String) {
        prefs.edit()
            .putBoolean(KEY_REMEMBER_USER, true)
            .putString(KEY_REMEMBERED_EMAIL, email.trim())
            .apply()
    }

    fun clear() {
        prefs.edit()
            .remove(KEY_REMEMBER_USER)
            .remove(KEY_REMEMBERED_EMAIL)
            .apply()
    }

    private companion object {
        private const val PREFS_NAME = "login_preferences"
        private const val KEY_REMEMBER_USER = "remember_user"
        private const val KEY_REMEMBERED_EMAIL = "remembered_email"
    }
}
