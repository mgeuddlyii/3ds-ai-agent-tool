@echo off
REM ---------------------------------------------------------------------------
REM  3DS AI Console - one-command build entry point
REM
REM      build.cmd             .elf + .smdh + .3dsx          (via build.py)
REM      build.cmd cia         ... and package AIConsole.cia  (via build.py)
REM      build.cmd make        same, but through the Makefile + MSYS2 make
REM      build.cmd make cia    Makefile + .cia
REM      build.cmd clean       delete objects and artefacts
REM      build.cmd doctor      run the toolchain self-check
REM ---------------------------------------------------------------------------
setlocal

set "DEVKITPRO=E:\devkitPro"
set "DEVKITARM=E:\devkitPro\devkitARM"
set "PATH=%DEVKITPRO%\devkitARM\bin;%DEVKITPRO%\tools\bin;%PATH%"

set "ROOT=%~dp0"
set "ROOTFWD=%ROOT:\=/%"
set "AIC_APPDIR=%ROOT%app"
set "PY=C:\Users\Kevin\.workbuddy\binaries\python\versions\3.13.12\python.exe"
set "MSYSBASH=%DEVKITPRO%\msys2\usr\bin\bash.exe"

if /i "%~1"=="make"    goto :make
if /i "%~1"=="doctor"  goto :doctor
if /i "%~1"=="env"     goto :env

"%PY%" "%ROOT%tools\build.py" %*
endlocal
exit /b %errorlevel%

:make
if not exist "%MSYSBASH%" (
    echo MSYS2 bash not found at %MSYSBASH%
    exit /b 1
)
"%MSYSBASH%" -lc "%ROOTFWD%tools/make.sh %2 %3"
endlocal
exit /b %errorlevel%

:doctor
"%PY%" "%ROOT%tools\doctor.py"
endlocal
exit /b %errorlevel%

:env
call "%ROOT%tools\env.cmd" shell
endlocal
exit /b 0
