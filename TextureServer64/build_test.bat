@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >/dev/null 2>&1
cd /d C:\Scripts\turtlewow\TextureServer64
cl /std:c++17 /EHsc /W4 /Ishared server\tests\test_protocol.cpp /Fe:test_protocol.exe > build_output.txt 2>&1
if %errorlevel% neq 0 (
    echo BUILD FAILED >> build_output.txt
    exit /b %errorlevel%
)
test_protocol.exe >> build_output.txt 2>&1
