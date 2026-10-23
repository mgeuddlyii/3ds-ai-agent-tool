#!/bin/sh
# ---------------------------------------------------------------------------
#  Runs `make` inside the project's app/ directory with the devkitPro
#  environment set up. Invoked by build.cmd, but usable on its own:
#
#      E:/devkitPro/msys2/usr/bin/bash.exe tools/make.sh cia
# ---------------------------------------------------------------------------

export DEVKITPRO=E:/devkitPro
export DEVKITARM=E:/devkitPro/devkitARM
export PATH="/e/devkitPro/devkitARM/bin:/e/devkitPro/tools/bin:$PATH"

# AIC_APPDIR is set by build.cmd; fall back to a path relative to this script.
if [ -n "$AIC_APPDIR" ]; then
    APP="$(cygpath -u "$AIC_APPDIR")"
else
    APP="$(cd "$(dirname "$0")/../app" && pwd)"
fi

cd "$APP" || { echo "cannot enter $APP"; exit 9; }
exec make "$@"
