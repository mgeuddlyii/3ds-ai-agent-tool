"""Run the TLS 1.2 probe inside Azahar and collect its report.

Why this is trustworthy while the normal acceptance runs are not:
Azahar's http_c.cpp unconditionally disables certificate verification, so any
httpc-based HTTPS test in the emulator is a guaranteed false positive. This
probe instead links libcurl against 3ds-mbedtls *statically into the binary* —
the handshake is performed by mbedtls code running on the emulated ARM11, over
sockets the emulator maps to the host. What it reports is what the real stack
would do.

Usage:  python tools/run_tls12.py
"""

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

AZAHAR = r"E:\Gudyi\_tools\azahar\azahar-windows-msvc-2126.1.1\azahar.exe"
if not os.path.exists(AZAHAR):
    AZAHAR = r"E:\Azahar\azahar-windows-msvc-2126.1.1\azahar.exe"

ROM = os.path.join(ROOT, "tools", "tls12", "tls12.3dsx")

#: Mozilla CA bundle (from https://curl.se/ca/cacert.pem). curl on the 3DS
#: looks for its default CA file at sdmc:/config/ssl/cacert.pem; without it
#: CURLOPT_SSL_VERIFYPEER fails with "Error reading ca cert file".
CACERT = os.path.join(ROOT, "tools", "tls12", "cacert.pem")
SD_SUBDIR = os.path.join("3ds", "aiconsole")
RESULT_NAMES = [os.path.join(SD_SUBDIR, "tls12.txt"), "tls12.txt"]


def sdmc_roots(exe_dir):
    appdata = os.environ.get("APPDATA", "")
    localapp = os.environ.get("LOCALAPPDATA", "")
    cands = [
        os.path.join(exe_dir, "sdmc"),
        os.path.join(exe_dir, "user", "sdmc"),
        os.path.join(exe_dir, "user"),
        os.path.join(appdata, "azahar", "sdmc"),
        os.path.join(appdata, "Citra", "sdmc"),
        os.path.join(localapp, "azahar", "sdmc"),
    ]
    out, seen = [], set()
    for c in cands:
        c = os.path.normpath(c)
        if c and c not in seen:
            seen.add(c)
            out.append(c)
    return out


def writable_roots(roots):
    ok = []
    for r in roots:
        try:
            os.makedirs(r, exist_ok=True)
            p = os.path.join(r, ".probe")
            with open(p, "w") as fh:
                fh.write("x")
            os.remove(p)
            ok.append(r)
        except OSError:
            continue
    return ok


def read_stable(path, tries=40):
    """Wait until the file stops changing: fopen("w") truncates immediately,
    so a naive mtime check reads an empty file (lesson from the M-pass)."""
    prev, stable = None, 0
    for _ in range(tries):
        if os.path.exists(path):
            try:
                with open(path, "rb") as fh:
                    cur = fh.read()
            except OSError:
                cur = None
            if cur is not None and cur == prev and cur:
                stable += 1
                if stable >= 2:
                    return cur.decode("utf-8", "replace")
            else:
                stable = 0
            prev = cur
        time.sleep(1.0)
    return prev.decode("utf-8", "replace") if prev else None


def main():
    if not os.path.exists(AZAHAR):
        print("azahar not found:", AZAHAR)
        return 2
    if not os.path.exists(ROM):
        print("probe not built:", ROM)
        return 2

    # Kill stale instances first (they hold the SD card and spawn zombies).
    subprocess.run(["taskkill", "/F", "/IM", "azahar.exe"],
                   capture_output=True)
    time.sleep(1)

    exe_dir = os.path.dirname(AZAHAR)
    roots = writable_roots(sdmc_roots(exe_dir))
    if not roots:
        print("no writable sdmc root among:", sdmc_roots(exe_dir))
        return 2
    primary = roots[0]
    print("sdmc roots used:", roots[:3])

    for r in roots[:3]:
        dst_dir = os.path.join(r, "3ds")
        os.makedirs(dst_dir, exist_ok=True)
        shutil.copy2(ROM, os.path.join(dst_dir, "tls12.3dsx"))
        os.makedirs(os.path.join(r, SD_SUBDIR), exist_ok=True)

    # Drop the CA bundle where curl expects it, so the "verify" pass tests
    # the certificate chain rather than just "no CA file present".
    if os.path.exists(CACERT):
        for r in roots[:3]:
            d = os.path.join(r, "config", "ssl")
            os.makedirs(d, exist_ok=True)
            shutil.copy2(CACERT, os.path.join(d, "cacert.pem"))
        print("deployed cacert.pem (%d bytes)" % os.path.getsize(CACERT))
    else:
        print("! no cacert.pem, the verify pass will fail for the wrong reason")

    # Clear any previous report so we never read a stale one.
    for r in roots[:3]:
        for n in RESULT_NAMES:
            p = os.path.join(r, n)
            if os.path.exists(p):
                os.remove(p)

    print("\nlaunching azahar ...")
    proc = subprocess.Popen([AZAHAR, os.path.join(primary, "3ds", "tls12.3dsx")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    result = None
    deadline = time.time() + 180
    while time.time() < deadline:
        for r in roots[:3]:
            for n in RESULT_NAMES:
                p = os.path.join(r, n)
                if os.path.exists(p):
                    txt = read_stable(p, tries=25)
                    if txt and "=== done ===" in txt:
                        result = (p, txt)
                        break
            if result:
                break
        if result:
            break
        time.sleep(1.0)

    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
    subprocess.run(["taskkill", "/F", "/IM", "azahar.exe"], capture_output=True)

    if not result:
        print("\n!! probe never produced a report")
        return 1

    path, txt = result
    print("\n===== %s =====" % path)
    print(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
