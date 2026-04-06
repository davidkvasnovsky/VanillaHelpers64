#pragma once
// ServerLog.h - Structured logging for TextureServer64.
// Adds timestamps and thread IDs to all server output.

#include <cstdarg>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace TexServer {

inline DWORD g_serverStartTick = GetTickCount();

inline void ServerLogV(FILE *out, const char *level_tag, const char *fmt, va_list args) {
    char body[512];
    vsnprintf(body, sizeof(body), fmt, args);
    DWORD elapsed = GetTickCount() - g_serverStartTick;
    fprintf(out, "[TextureServer] +%07lu T%04lu %s%s\n",
            elapsed, GetCurrentThreadId() % 10000, level_tag, body);
}

inline void ServerLog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ServerLogV(stdout, "INFO ", fmt, args);
    va_end(args);
}

inline void ServerLogError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ServerLogV(stderr, "ERROR ", fmt, args);
    va_end(args);
    fflush(stderr);
}

} // namespace TexServer
