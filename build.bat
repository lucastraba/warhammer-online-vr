@echo off
rem Builds WARVR.dll and WARVRInjector.exe (32-bit, Release) and runs the unit tests.
rem Needs Visual Studio 2022 or newer with "Desktop development with C++" (it includes CMake).
rem Result: build\Release\  (WARVR.dll, WARVRInjector.exe, openvr_api.dll) - copy those three into the WAR folder.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio was not found. Install Visual Studio Community with "Desktop development with C++".
  goto :fail
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio is installed but the C++ tools are missing. Add "Desktop development with C++" in the Visual Studio Installer.
  goto :fail
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || goto :fail

cmake -S . -B build -A Win32 || goto :fail
cmake --build build --config Release || goto :fail
ctest --test-dir build -C Release --output-on-failure || goto :fail

echo.
echo Done. Copy these into your Warhammer folder:
echo   %CD%\build\Release\WARVR.dll
echo   %CD%\build\Release\WARVRInjector.exe
echo   %CD%\build\Release\openvr_api.dll
pause
exit /b 0

:fail
echo.
echo BUILD FAILED (see the messages above).
pause
exit /b 1
