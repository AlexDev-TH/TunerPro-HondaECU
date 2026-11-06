@echo off
rem Builds the 32-bit plug-in (dist\HondaECU.dll) and the installer (dist\HondaECU-Setup.exe).
rem Needs Visual Studio with the C++ desktop tools. The installer uses the csc.exe in Windows.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. Install Visual Studio with the C++ desktop tools.
  exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1

if not exist obj mkdir obj
if not exist dist mkdir dist

set SOURCES=src\plugin.cpp src\ui.cpp src\kline_worker.cpp src\honda_ecu.cpp src\adapter_ftdi.cpp src\adapter_j2534.cpp
set LIBS=user32.lib gdi32.lib comdlg32.lib comctl32.lib advapi32.lib shell32.lib winmm.lib
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS %EXTRA_CFLAGS% /Fo:obj\ %SOURCES% /LD /Fe:dist\HondaECU.dll /link %LIBS%
if errorlevel 1 exit /b 1
del /q dist\HondaECU.exp dist\HondaECU.lib 2>nul

if exist installer\Setup.cs (
  "%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe" /nologo /codepage:65001 /target:winexe /platform:anycpu /optimize+ /win32manifest:installer\app.manifest /resource:dist\HondaECU.dll,HondaECU.dll /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /out:dist\HondaECU-Setup.exe installer\Setup.cs
  if errorlevel 1 exit /b 1
)
echo Built dist\HondaECU.dll
