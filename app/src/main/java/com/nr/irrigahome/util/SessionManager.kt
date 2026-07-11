package com.nr.irrigahome.util

object SessionManager {

    private var backgroundTimestamp: Long = 0L
    private var autoLogoutPending: Boolean = false

    private const val SESSION_TIMEOUT = 120_000L

    fun onAppBackgrounded() {
        backgroundTimestamp = System.currentTimeMillis()
    }

    fun onAppForegrounded() {
        backgroundTimestamp = 0L
    }

    fun markAutoLogoutPending() {
        autoLogoutPending = true
    }

    fun consumeAutoLogoutPending(): Boolean {
        if (!autoLogoutPending) return false
        autoLogoutPending = false
        return true
    }

    fun shouldLogout(): Boolean {
        if (backgroundTimestamp == 0L) return false
        val timeInBackground = System.currentTimeMillis() - backgroundTimestamp
        return timeInBackground > SESSION_TIMEOUT
    }

    fun resetSession() {
        backgroundTimestamp = 0L
        autoLogoutPending = false
    }
}
