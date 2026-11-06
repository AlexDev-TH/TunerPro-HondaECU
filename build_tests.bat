@echo off
rem Builds and runs the test suite against simulated drivers. No hardware is touched.
rem Usage: build_tests.bat [path to the HondaECU source folder]
rem With a folder, tests\golden.inc is generated again from that Python source first (needs Python 3).
rem Without one, the tests use the tests\golden.inc already in this repository.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1

if not exist obj\tests mkdir obj\tests
if not exist tests\bin mkdir tests\bin

if not "%~1"=="" (
  python -I tools\gen_golden.py "%~1" tests\golden.inc
  if errorlevel 1 exit /b 1
)

set FLAGS=/nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fo:obj\tests\
cl %FLAGS% tests\fake_ftd2xx.cpp /LD /Fe:tests\bin\ftd2xx.dll /link user32.lib /DEF:tests\fake_ftd2xx.def
if errorlevel 1 exit /b 1
cl %FLAGS% tests\fake_j2534.cpp /LD /Fe:tests\bin\fake_j2534.dll /link user32.lib /DEF:tests\fake_j2534.def
if errorlevel 1 exit /b 1
cl %FLAGS% tests\test_core.cpp src\kline_worker.cpp src\honda_ecu.cpp src\adapter_ftdi.cpp src\adapter_j2534.cpp /Fe:tests\bin\test_core.exe /link user32.lib advapi32.lib winmm.lib
if errorlevel 1 exit /b 1

if "%HONDAECU_BUILD_ONLY%"=="1" exit /b 0
tests\bin\test_core.exe
