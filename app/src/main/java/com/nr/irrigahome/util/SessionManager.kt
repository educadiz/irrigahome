package com.nr.irrigahome.util

object SessionManager {

    private var backgroundTimestamp: Long = 0L

    private const val SESSION_TIMEOUT = 30_000L

    fun onAppBackgrounded() {
        backgroundTimestamp = System.currentTimeMillis()
    }

    fun shouldLogout(): Boolean {
        if (backgroundTimestamp == 0L) return false
        val timeInBackground = System.currentTimeMillis() - backgroundTimestamp
        return timeInBackground > SESSION_TIMEOUT
    }

    fun resetSession() {
        backgroundTimestamp = 0L
    }
}
