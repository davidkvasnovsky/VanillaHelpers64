#!/usr/bin/env bash
# Run clang-tidy on macOS with MinGW cross-compilation headers.
# Usage:
#   ./run-tidy.sh              # all project .cpp files (server + DLL + tests)
#   ./run-tidy.sh server       # server sources only
#   ./run-tidy.sh dll          # DLL sources only
#   ./run-tidy.sh <files...>   # specific files
#   ./run-tidy.sh --fix [...]  # auto-fix all fixable warnings
set -euo pipefail

FIX_FLAG=""
if [[ "${1:-}" == "--fix" ]]; then
    FIX_FLAG="--fix"
    shift
fi

CT=/opt/homebrew/opt/llvm@20/bin/clang-tidy
CLANG_RES=$(/opt/homebrew/opt/llvm@20/bin/clang -print-resource-dir)

# MinGW toolchain paths
BASE64=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64
BASE32=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-i686

# GCC C++ stdlib version
GCCVER=15.2.0

run_tidy() {
    local build_dir="$1"
    local base="$2"
    local triple="$3"
    shift 3

    "$CT" -p "$build_dir" --quiet $FIX_FLAG \
        --extra-arg="--target=$triple" \
        --extra-arg="-isystem$base/$triple/include/c++/$GCCVER" \
        --extra-arg="-isystem$base/$triple/include/c++/$GCCVER/$triple" \
        --extra-arg="-isystem$base/$triple/include/c++/$GCCVER/backward" \
        --extra-arg="-isystem$CLANG_RES/include" \
        --extra-arg="-isystem$base/$triple/include" \
        --extra-arg="-fexceptions" \
        "$@" 2>&1 | grep -v '^\[' | grep -v 'warnings generated' | grep -v '^Suppressed'
}

# Server + test files (64-bit)
SERVER_FILES=(
    TextureServer64/server/src/Server.cpp
    TextureServer64/server/src/SharedMemory.cpp
    TextureServer64/server/src/ThreadPool.cpp
    TextureServer64/server/src/BlpDecoder.cpp
    TextureServer64/server/src/TgaDecoder.cpp
    TextureServer64/server/src/main.cpp
    TextureServer64/server/tests/test_protocol.cpp
    TextureServer64/server/tests/test_thread_pool.cpp
    TextureServer64/server/tests/test_shared_memory.cpp
    TextureServer64/server/tests/test_blp_decoder.cpp
    TextureServer64/server/tests/test_tga_decoder.cpp
)

# DLL + client files (32-bit)
DLL_FILES=(
    VanillaHelpers/src/Allocator.cpp
    VanillaHelpers/src/Blips.cpp
    VanillaHelpers/src/Common.cpp
    VanillaHelpers/src/DllMain.cpp
    VanillaHelpers/src/FileIO.cpp
    VanillaHelpers/src/Game.cpp
    VanillaHelpers/src/Morph.cpp
    VanillaHelpers/src/Offsets.cpp
    VanillaHelpers/src/TexBridge.cpp
    VanillaHelpers/src/Texture.cpp
)

# Generate compile_commands.json if missing
ensure_compile_db() {
    if [ ! -f build-tidy/compile_commands.json ]; then
        echo "Generating server compile_commands.json..."
        cmake -S TextureServer64 -B build-tidy \
            -DCMAKE_TOOLCHAIN_FILE="$(pwd)/mingw-x86_64-toolchain.cmake" \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
    fi
    if [ ! -f build-tidy-dll/compile_commands.json ]; then
        echo "Generating DLL compile_commands.json..."
        cmake -S VanillaHelpers -B build-tidy-dll \
            -DCMAKE_TOOLCHAIN_FILE="$(pwd)/mingw-i686-toolchain.cmake" \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
    fi
}

ensure_compile_db

case "${1:-all}" in
    server)
        run_tidy build-tidy "$BASE64" x86_64-w64-mingw32 "${SERVER_FILES[@]}"
        ;;
    dll)
        run_tidy build-tidy-dll "$BASE32" i686-w64-mingw32 "${DLL_FILES[@]}"
        ;;
    all)
        echo "=== Server (64-bit) ==="
        run_tidy build-tidy "$BASE64" x86_64-w64-mingw32 "${SERVER_FILES[@]}"
        echo ""
        echo "=== DLL (32-bit) ==="
        run_tidy build-tidy-dll "$BASE32" i686-w64-mingw32 "${DLL_FILES[@]}"
        ;;
    *)
        # Specific files — guess which build dir based on path
        for f in "$@"; do
            if [[ "$f" == VanillaHelpers/* ]]; then
                run_tidy build-tidy-dll "$BASE32" i686-w64-mingw32 "$f"
            else
                run_tidy build-tidy "$BASE64" x86_64-w64-mingw32 "$f"
            fi
        done
        ;;
esac
