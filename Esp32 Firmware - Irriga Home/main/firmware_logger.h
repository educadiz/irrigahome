#pragma once

#include <Arduino.h>

inline void fwLogPrefix(const char* level, const char* tag) {
    Serial.print('[');
    Serial.print(level);
    Serial.print("][");
    Serial.print(tag);
    Serial.print("][");
    Serial.print(millis());
    Serial.print("ms] ");
}

inline void fwLogLine(const char* level, const char* tag, const char* message) {
    fwLogPrefix(level, tag);
    Serial.println(message);
}

template <typename... Args>
inline void fwLogf(const char* level, const char* tag, const char* format, Args... args) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), format, args...);
    fwLogLine(level, tag, buffer);
}

inline void fwLogSection(const char* tag, const char* title) {
    Serial.println();
    Serial.print("========== [");
    Serial.print(tag);
    Serial.print("] ");
    Serial.print(title);
    Serial.println(" ==========");
}