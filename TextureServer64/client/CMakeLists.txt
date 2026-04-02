cmake_minimum_required(VERSION 3.20)

# ── TexClient static library (32-bit) ───────────────────────────────────
# Linked into VanillaHelpers.dll to provide pipe client + working set.

add_library(TexClient STATIC
    src/TexClient.cpp
    src/WorkingSet.cpp
)

target_include_directories(TexClient PUBLIC
    src
    ../shared
)

target_compile_definitions(TexClient PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Static runtime (/MT) to match VanillaHelpers.
set_property(TARGET TexClient PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
