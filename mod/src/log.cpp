// DDDAVR - logging (thread-safe, flushed on every line so a crash never loses the tail)
#include "log.h"
#include <stdio.h>
#include <stdarg.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static ULONGLONG g_t0;

void LogInit(const wchar_t* path) {
    InitializeCriticalSection(&g_cs);
    g_t0 = GetTickCount64();
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    ULONGLONG t = GetTickCount64() - g_t0;
    int n = snprintf(buf, sizeof(buf), "[%7llu.%03llu][%5lu] ", t / 1000, t % 1000, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += (m < (int)sizeof(buf) - n - 2) ? m : (int)sizeof(buf) - n - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    DWORD w;
    WriteFile(g_file, buf, (DWORD)n, &w, nullptr);
    LeaveCriticalSection(&g_cs);
}

void LogFlush() {
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
}
