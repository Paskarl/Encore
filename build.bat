@echo off
rem Builds build\version.dll with the MSVC x64 toolchain (VS 2022 Build Tools).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W3 /LD src\encore.cpp src\trigger_server.cpp src\companion_launcher.cpp src\version_proxy.cpp /Fobuild\ /Febuild\version.dll /link /DEF:src\version.def user32.lib ws2_32.lib || exit /b 1
echo Built build\version.dll
