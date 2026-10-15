@echo off
rem Builds build\version.dll with the MSVC x64 toolchain (VS 2022 Build Tools, or the newest
rem Visual Studio with C++ tools that vswhere finds, as on GitHub's Windows runners).
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W3 /LD src\pick.cpp src\trigger_server.cpp src\companion_launcher.cpp src\version_proxy.cpp /Fobuild\ /Febuild\version.dll /link /DEF:src\version.def user32.lib ws2_32.lib || exit /b 1
echo Built build\version.dll
