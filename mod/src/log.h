// FC4VR - logging
#pragma once
#include <windows.h>

void LogInit(const wchar_t* path);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogFlush();

// Log only the first N times a call site is reached.
#define LOG_FIRST(n, ...) do { static volatile LONG _c = 0; if (InterlockedIncrement(&_c) <= (n)) Log(__VA_ARGS__); } while (0)
