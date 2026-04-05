cmake_minimum_required(VERSION 3.20)

# ── Server executable (64-bit) ──────────────────────────────────────────

add_executable(TextureServer64
    src/main.cpp
    src/Server.cpp
    src/SharedMemory.cpp
    src/ThreadPool.cpp
    src/BlpDecoder.cpp
    src/TgaDecoder.cpp
)

target_include_directories(TextureServer64 PRIVATE
    src
    ../shared
)

target_compile_definitions(TextureServer64 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Static runtime to avoid CRT DLL dependency.
if(MSVC)
    set_property(TARGET TextureServer64 PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    target_compile_options(TextureServer64 PRIVATE -O2 -fno-exceptions -fno-rtti)
    target_link_options(TextureServer64 PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

# ── Tests ────────────────────────────────────────────────────────────────

enable_testing()

# test_protocol: header-only, no extra sources needed
add_executable(test_protocol tests/test_protocol.cpp)
target_include_directories(test_protocol PRIVATE ../shared)
set_property(TARGET test_protocol PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME protocol COMMAND test_protocol)

# test_thread_pool: needs ThreadPool.cpp
add_executable(test_thread_pool tests/test_thread_pool.cpp src/ThreadPool.cpp)
target_include_directories(test_thread_pool PRIVATE src ../shared)
set_property(TARGET test_thread_pool PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME thread_pool COMMAND test_thread_pool)

# test_shared_memory: needs SharedMemory.cpp
add_executable(test_shared_memory tests/test_shared_memory.cpp src/SharedMemory.cpp)
target_include_directories(test_shared_memory PRIVATE src ../shared)
set_property(TARGET test_shared_memory PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME shared_memory COMMAND test_shared_memory)

# test_blp_decoder: needs BlpDecoder.cpp
add_executable(test_blp_decoder tests/test_blp_decoder.cpp src/BlpDecoder.cpp)
target_include_directories(test_blp_decoder PRIVATE src ../shared)
set_property(TARGET test_blp_decoder PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME blp_decoder COMMAND test_blp_decoder)

# test_tga_decoder: needs TgaDecoder.cpp + BlpDecoder.cpp (for DecodedTexture)
add_executable(test_tga_decoder tests/test_tga_decoder.cpp src/TgaDecoder.cpp src/BlpDecoder.cpp)
target_include_directories(test_tga_decoder PRIVATE src ../shared)
set_property(TARGET test_tga_decoder PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME tga_decoder COMMAND test_tga_decoder)
