@echo off
REM ---------------------------------------------------------------------------
REM  3DS AI Console - devkitPro environment
REM
REM  Usage from cmd.exe:
REM      call tools\env.cmd        set variables in the CURRENT session
REM      tools\env.cmd             spawn a new cmd.exe with them set
REM
REM  Only DEVKITPRO / DEVKITARM matter: the devkitPro Makefiles address the
REM  compiler as $(DEVKITARM)/bin/... so PATH is not strictly required. The
REM  two bin dirs are still appended for convenience (gcc, makerom, 3dsxtool).
REM ---------------------------------------------------------------------------

set "DEVKITPRO=E:\devkitPro"
set "DEVKITARM=E:\devkitPro\devkitARM"

set "PATH=%DEVKITPRO%\devkitARM\bin;%DEVKITPRO%\tools\bin;%PATH%"

echo DEVKITPRO = %DEVKITPRO%
echo DEVKITARM = %DEVKITARM%

if /i "%~1"=="shell" (
    echo starting a new cmd with the toolchain environment ...
    cmd /k
)
