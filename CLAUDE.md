# VanillaHelpers64

Helper library for WoW 1.12 (TurtleWoW). Two components: a 32-bit DLL injected into WoW and a 64-bit texture streaming server.

## Build

First-time clone requires submodules: `git submodule update --init --recursive`

### MSVC (CI — Windows, `.github/workflows/build.yml`)

```bash
# VanillaHelpers.dll (32-bit)
cmake -S VanillaHelpers -B build -A Win32
cmake --build build --config Release
# Output: build/Release/VanillaHelpers.dll

# TextureServer64.exe (64-bit)
cmake -S TextureServer64 -B build-server -A x64
cmake --build build-server --config Release
# Output: build-server/server/Release/TextureServer64.exe
```

### MinGW (local macOS cross-compile)

```bash
# VanillaHelpers.dll (32-bit)
cmake -S VanillaHelpers -B build -DCMAKE_TOOLCHAIN_FILE=mingw-i686-toolchain.cmake
cmake --build build
# Output: build/VanillaHelpers.dll

# TextureServer64.exe (64-bit)
cmake -S TextureServer64 -B build-server -DCMAKE_TOOLCHAIN_FILE=mingw-x86_64-toolchain.cmake
cmake --build build-server
# Output: build-server/server/TextureServer64.exe
```

Toolchains expect MinGW at `/opt/homebrew/opt/mingw-w64`.

### Version tag

Pass `-DVANILLAHELPERS_TAG=vMAJOR.MINOR.PATCH` to cmake. Encodes as `MAJOR*10000 + MINOR*100 + PATCH`. Default dev value: `999999`.

### Clean rebuild

No cmake clean target. Delete build directories: `rm -rf build build-server build-tidy build-tidy-dll`

### Debug builds

```bash
# MSVC
cmake --build build --config Debug

# MinGW (no debug variant defined — add -DCMAKE_BUILD_TYPE=Debug manually)
cmake -S VanillaHelpers -B build-debug -DCMAKE_TOOLCHAIN_FILE=mingw-i686-toolchain.cmake -DCMAKE_BUILD_TYPE=Debug
```

## Tests

Server-side only (Windows APIs — cannot run from macOS cross-compile). Built automatically with the server cmake:

```bash
cd build-server && ctest --output-on-failure
```

5 test executables: `test_protocol`, `test_thread_pool`, `test_shared_memory`, `test_blp_decoder`, `test_tga_decoder`. Minimal custom framework (macro assertions, no external test lib).

CI (`.github/workflows/build.yml`) builds both components on `windows-latest` but does not run tests or publish releases. Releases are manual.

## Architecture

```
WoW.exe ← VanillaHelpers.dll (32-bit, MinHook)
               │
               │ named pipe: \\.\pipe\VH_TextureServer
               │ shared memory: VH_TexServer_SharedMem + 4 data windows
               ▼
          TextureServer64.exe (64-bit)
               ├── BlpDecoder (BLP1/BLP2: JPEG, Palette, DXT, Uncompressed)
               ├── TgaDecoder
               ├── LruCache (35% RAM, 4–32 GiB)
               ├── ThreadPool (priority queue)
               └── SharedMemory (4 windows × 16 slots × 4 MiB)
```

### Key source files

| File | Role |
|------|------|
| `VanillaHelpers/src/DllMain.cpp` | Entry point, hook installation, initialization sequence |
| `VanillaHelpers/src/TexBridge.cpp` | Client-side texture bridge (pipe + shared memory + D3D9 swap) |
| `VanillaHelpers/src/Texture.cpp` | Texture size patches, hi-res character skins |
| `VanillaHelpers/src/Allocator.cpp` | Memory allocator hooks (16→32 MiB regions) |
| `VanillaHelpers/src/Game.cpp` | Function pointer bindings (`Game::Init`), WoW struct constructors, minimap drawing |
| `VanillaHelpers/src/Offsets.h` | 149 WoW 1.12 addresses (X-macro, overridable via `offsets.ini`) |
| `VanillaHelpers/src/Offsets.cpp` | Offset definitions with compiled-in defaults + INI file loader |
| `VanillaHelpers/src/Common.h` | `HOOK_FUNCTION` / `UNHOOK_FUNCTION` macros |
| `TextureServer64/shared/Protocol.h` | IPC wire format (packed structs, enums, constants) |
| `TextureServer64/server/src/Server.cpp` | Server accept loop, request dispatch |
| `TextureServer64/server/src/LruCache.h` | Thread-safe LRU cache (shared_ptr, single mutex) |
| `TextureServer64/server/src/SharedMemory.cpp` | Windowed shared memory mapping |
| `TextureServer64/server/src/WinHandle.h` | RAII wrappers for Win32 HANDLE and MapViewOfFile |
| `TextureServer64/server/src/ServerLog.h` | Structured server logging with timestamps and thread IDs |
| `TextureServer64/client/src/TexClient.cpp` | 32-bit shared memory client (linked into DLL) |
| `TextureServer64/client/src/WorkingSet.cpp` | GPU texture LRU budget (512 MiB default) |

### IPC protocol

- **Request** (8 bytes packed): cmd, priority, path_len, data_size — followed by path + raw file bytes
- **Response** (19 bytes packed): status, slot_id, width, height, data_size, pixel_format, mip_levels
- **Slot states**: EMPTY→READING→DECODING→READY→UPLOADED, then server-side reclaim to READING. Uploaded slots are reusable immediately; stale Ready slots are reclaimed after a 5s timeout via `ready_tick`. (`volatile uint32_t`, lock-free polling)
- **Path hash**: FNV-1a with slash normalization (`/`→`\`) and case folding

### Initialization sequence (DllMain.cpp)

DLL_PROCESS_ATTACH: `Offsets::LoadFromFile` → `Game::Init` → `MH_Initialize` → `Allocator::Initialize` → `Texture::Initialize` → hook 4 game functions → `Allocator::InstallHooks` → `Texture::InstallHooks`.

Deferred in `InitializeGlobal_h` (after game init is safe): `Texture::InstallCharacterSkin` → `TexBridge::Initialize` → `TexBridge::EnsureServerRunning` → `TexBridge::InstallHooks`.

## Code conventions

- **Namespaces**: `PascalCase` (`TexBridge`, `TexClient`, `TexServer`)
- **Functions**: `PascalCase` throughout (public and internal)
- **Hook naming**: `FunctionName_h` for hook, `FunctionName_o` for original pointer
- **Constants**: `SCREAMING_SNAKE_CASE`
- **Wire structs**: `#pragma pack(push, 1)` with `static_assert` on sizes
- **C++23**, no exceptions or RTTI in release builds, static linking
- **Headers**: `#pragma once`, LGPL boilerplate, system includes → Windows → local

## Gotchas

- **No unhook on detach**: `MH_Uninitialize` is intentionally skipped in `DLL_PROCESS_DETACH` — it interferes with MinGW CRT cleanup under Wine.
- **Loader lock**: `CreateProcess` and thread creation are deferred to `InitializeGlobal_h`, never called from `DllMain` directly.
- **Wine log flushing**: `TexBridge.log` flushed every ~60 frame ticks, not on every write. Immediate flush causes hitching under Wine.
- **Texture lifecycle**: WoW recycles `HTEXTURE`/`CGxTex` objects. A destroy hook clears helper tracking and restores the managed binding before releasing D3DPOOL_DEFAULT textures.
- **Shared memory windowing**: 4 separate file mappings (not one large mapping) to avoid issues on older systems. Window = slot / 16, offset = (slot % 16) × SLOT_TOTAL.
- **D3D9 format constants**: Hardcoded (e.g., `D3DFMT_A8R8G8B8 = 21`, `D3DFMT_DXT1 = 827611204`). Same values under native D3D9, DXVK, and D3DMetal.
- **Offsets.h**: All addresses are WoW 1.12 specific. Version mismatch = crash. No runtime validation.
- **Offset overrides**: Place `offsets.ini` next to the DLL with `KEY = 0xHEX` lines to override compiled-in defaults. See `offsets.ini.example`. Missing file is not an error.
- **Init order**: `Offsets::LoadFromFile` and `Game::Init` must run before `MH_Initialize` — function pointers are nullptr until `Game::Init` binds them.
- **Cross-process volatile**: `SlotHeader::state` and `ShmHeader::sequence` intentionally use `volatile` (not `std::atomic`) because `std::atomic` layout is implementation-defined across 32/64-bit compilers. Correctness relies on x86 TSO + `Interlocked*` at access sites.
- **RAII handles server-only**: `WinHandle.h` is used only in TextureServer64. The DLL avoids RAII destructors for module-level handles because `DLL_PROCESS_DETACH` cleanup order is fragile.
- **Log format**: `TexBridge.log` uses `[TexBridge] +ELAPSED TTID LEVEL message`. Server stdout uses `[TextureServer] +ELAPSED TTID INFO message`. Elapsed is ms since log start; TTID is thread ID mod 10000.
- **Back-pressure**: Max 16 in-flight decodes, 128 MiB queued raw bytes. Requests dropped if exceeded.
- **`heigth`**: Intentional — matches the WoW internal struct field name in `Game.h`.

## File toggles

Place these files next to the DLL or in the WoW game folder:

- `offsets.ini` — override compiled-in WoW 1.12 addresses (see `offsets.ini.example`)
- `TexBridgeVisible.txt` — keep TextureServer64 console window visible
- `TexBridgeFullLog.txt` — enable verbose TexBridge.log output
- `VanillaHelpers/ResizeCharacterSkin.txt` (inside MPQ) — contains `2` or `4` for skin scale multiplier

## Common workflows

### Adding a new hook

1. Add address to `OFFSET_LIST` in `Offsets.h` (e.g., `X(FUN_MY_FUNCTION, 0x12345)`)
2. Declare function type in `Game.h` (e.g., `typedef bool (__fastcall *MyFunction_t)(...)`)
3. Add static original pointer and hook function in the relevant `.cpp`:
   ```cpp
   static Game::MyFunction_t MyFunction_o = nullptr;
   static bool __fastcall MyFunction_h(...) {
       // pre-hook logic
       auto result = MyFunction_o(...);
       // post-hook logic
       return result;
   }
   ```
4. Call `HOOK_FUNCTION(Offsets::FUN_MY_FUNCTION, MyFunction_h, MyFunction_o)` in `DllMain.cpp` DLL_PROCESS_ATTACH

### Adding a new Lua function

1. Write the script function in the relevant namespace `.cpp`:
   ```cpp
   static int Script_MyFunc(void *L) {
       // use Game::FrameScript_GetString / Game::FrameScript_PushNumber etc.
       return 0; // number of return values
   }
   ```
2. Register it in that namespace's `RegisterLuaFunctions()`:
   ```cpp
   Game::FrameScript_RegisterFunction("MyFunc", &Script_MyFunc);
   ```
3. If new namespace, add `YourNamespace::RegisterLuaFunctions()` call to `LoadScriptFunctions_h()` in `DllMain.cpp`

## Formatting & static analysis

Configured via `.clang-format`, `.clang-tidy`, and `.clang-format-ignore` at repo root.
LLVM 20 tools are at `/opt/homebrew/opt/llvm@20/bin/` (not on PATH by default).

```bash
# Format all project source files
/opt/homebrew/opt/llvm@20/bin/clang-format -i $(find VanillaHelpers/src TextureServer64/server/src \
  TextureServer64/client/src TextureServer64/shared \
  -name '*.cpp' -o -name '*.h' | grep -v stb_image)

# Check without modifying (CI)
clang-format --dry-run --Werror <files>

# Run clang-tidy (macOS — uses run-tidy.sh wrapper for MinGW cross-compile headers)
./run-tidy.sh              # report warnings (server + DLL + tests)
./run-tidy.sh --fix        # auto-fix safe checks
./run-tidy.sh server       # server only
./run-tidy.sh dll          # DLL only
```

Vendored code (`external/`, `stb_image.h`) is excluded from both tools.
Checks disabled from `--fix` (broken auto-fixes): `misc-include-cleaner`, `modernize-use-std-print`, `modernize-use-integer-sign-comparison`, `bugprone-reserved-identifier`, `modernize-loop-convert`.

## Attribution

Never add Claude as co-author or mention AI assistance in commits, PRs, issues, release notes, or anywhere else.

## Dependencies

- **MinHook** v1.3.3 (git submodule at `VanillaHelpers/external/minhook`)
- **stb_image.h** (vendored in `TextureServer64/server/src/` for JPEG decoding in BLP1)
- No other external dependencies. Standard library only.
