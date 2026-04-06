// main.cpp - Entry point for TextureServer64 (64-bit texture decode server).
// Usage: TextureServer64.exe [--threads N] [--cache-mb N] [--visible]

#include "Server.h"
#include "ServerLog.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Global server pointer for signal/ctrl handler.
static TexServer::Server* g_server = nullptr;
static constexpr size_t kMiB = 1024ULL * 1024ULL;
static constexpr size_t kGiB = 1024ULL * 1024ULL * 1024ULL;

static auto DetectDefaultCacheBytes() -> size_t {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if ((GlobalMemoryStatusEx(&mem) == 0) || mem.ullTotalPhys == 0) {
        return 4ULL * kGiB;
    }

    uint64_t target = mem.ullTotalPhys * 35ULL / 100ULL;
    const uint64_t minBytes = 4ULL * kGiB;
    const uint64_t maxBytes = 32ULL * kGiB;

    target = std::max(target, minBytes);
    target = std::min(target, maxBytes);
    return static_cast<size_t>(target);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    if (g_server != nullptr) {
        TexServer::ServerLog("Caught console signal, shutting down...");
        g_server->Stop();
    }
    return TRUE;
}

static void PrintUsage(const char* exe) {
    fprintf(stderr, "Usage: %s [--threads N] [--cache-mb N] [--visible]\n", exe);
    fprintf(stderr, "  --threads N    Worker thread count (0 = auto, default)\n");
    fprintf(stderr, "  --cache-mb N   LRU cache size in MiB (default auto: 35%% RAM, min 4096, max 32768)\n");
    fprintf(stderr, "  --visible      Keep console window open for logging\n");
}

auto main(int argc, char* argv[]) -> int {
    TexServer::ServerConfig config;
    bool visible = false;
    bool cacheOverride = false;

    // Parse command line.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.thread_count = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--cache-mb") == 0 && i + 1 < argc) {
            auto const mb = static_cast<size_t>(atoi(argv[++i]));
            config.cache_max_bytes = mb * kMiB;
            cacheOverride = true;
        } else if (strcmp(argv[i], "--visible") == 0) {
            visible = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (!cacheOverride || config.cache_max_bytes == 0) {
        config.cache_max_bytes = DetectDefaultCacheBytes();
    }

    // If --visible, ensure we have a console window (even if launched with
    // CREATE_NO_WINDOW). If not visible, we still log to stdout/stderr for
    // redirection purposes.
    if (visible) {
        if (GetConsoleWindow() == nullptr) {
            AllocConsole();
            // Re-open standard handles to the new console.
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
        }
        SetConsoleTitleA("TextureServer64");
    }

    // Install console control handler for graceful shutdown.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    printf("=============================================================\n");
    TexServer::ServerLog("TextureServer64 starting...");
    TexServer::ServerLog("  PID:      %lu", static_cast<unsigned long>(GetCurrentProcessId()));
    TexServer::ServerLog("  threads:  %u %s", config.thread_count, config.thread_count == 0 ? "(auto)" : "");
    TexServer::ServerLog("  cache:    %.0f MiB", static_cast<double>(config.cache_max_bytes) / (1024.0 * 1024.0));
    TexServer::ServerLog("  cacheSrc: %s", cacheOverride ? "command line" : "auto (35% RAM)");
    TexServer::ServerLog("  visible:  %s", visible ? "yes" : "no");
    printf("=============================================================\n");
    fflush(stdout);

    TexServer::Server server(config);
    g_server = &server;

    if (!server.Start()) {
        TexServer::ServerLogError("Failed to start. Exiting.");
        if (visible) {
            TexServer::ServerLog("Press Enter to exit...");
            getchar();
        }
        return 1;
    }

    TexServer::ServerLog("Server started. Waiting for pipe connections on:");
    TexServer::ServerLog("  Pipe: %s", TexProto::PIPE_NAME);
    TexServer::ServerLog("  SHM:  %s", TexProto::SHM_NAME);

    // Block in the accept loop until Stop() is called.
    server.Run();

    g_server = nullptr;
    TexServer::ServerLog("Exited.");

    if (visible) {
        TexServer::ServerLog("Press Enter to close...");
        getchar();
    }
    return 0;
}
