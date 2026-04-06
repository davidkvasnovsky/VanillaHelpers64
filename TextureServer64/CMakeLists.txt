cmake_minimum_required(VERSION 3.25)
project(TextureServer64 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Server: must be built as 64-bit
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_subdirectory(server)
    message(STATUS "Building 64-bit TextureServer64.exe")
endif()

# Client: must be built as 32-bit (linked into VanillaHelpers.dll)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_subdirectory(client)
    message(STATUS "Building 32-bit TexClient static library")
endif()
