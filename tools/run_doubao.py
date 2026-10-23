"""Drive the Doubao probe inside Azahar, in two phases.

The 3DS side does all the network work; this script only shuttles files.

  start     deploy, launch Azahar, wait until the 3DS has fetched a login QR,
            decode it to a PNG on the PC side, then return. Azahar keeps
            running so the 3DS can keep polling for the scan.
  collect   wait for the login to land, then print cookie / history / chat.

Two phases exist because a human has to scan the QR with the Douyin app, and
that human is not necessarily sitting at this prompt while it happens.

Usage:
    python tools/run_doubao.py start
    python tools/run_doubao.py collect
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qr_png import png_to_matrix, upscale  # noqa: E402

AZAHAR = r"E:\Gudyi\_tools\azahar\azahar-windows-msvc-2126.1.1\azahar.exe"
if not os.path.exists(AZAHAR):
    AZAHAR = r"E:\Azahar\azahar-windows-msvc-2126.1.1\azahar.exe"

ROM = os.path.join(ROOT, "tools", "doubao", "doubao.3dsx")

#: On this PC Kaspersky re-signs outbound HTTPS, so mbedtls inside the
#: emulator sees a Kaspersky root instead of the real one and verification
#: fails with CURLE_PEER_FAILED_VERIFICATION (-60). This bundle is the
#: Mozilla set plus that one local root. It exists only so the emulator can
#: validate at all; a real 3DS on a normal network needs the plain bundle.
CACERT = os.path.join(ROOT, "tools", "tls12", "cacert_local.pem")
if not os.path.exists(CACERT):
    CACERT = os.path.join(ROOT, "tools", "tls12", "cacert.pem")

SD_SUBDIR = os.path.join("3ds", "aiconsole", "doubao")
OUT_DIR = os.path.join(HERE, "out")
QR_PNG = os.path.join(OUT_DIR, "doubao_qr.png")
QR_BIG = os.path.join(OUT_DIR, "doubao_qr_big.png")
QR_TXT = os.path.join(OUT_DIR, "doubao_qr_matrix.txt")
PID_FILE = os.path.join(OUT_DIR, "azahar_pid.txt")


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


def find_file(name, roots):
    for r in roots:
        p = os.path.join(r, SD_SUBDIR, name)
        if os.path.exists(p):
            return p
    return None


def read_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return None


def clear_outputs(roots):
    for r in roots:
        d = os.path.join(r, SD_SUBDIR)
        if os.path.isdir(d):
            for n in ("status.txt", "qr_b64.txt", "qr_token.txt",
                      "cookie.txt", "history.txt", "chat.txt", "report.txt"):
                p = os.path.join(d, n)
                if os.path.exists(p):
                    os.remove(p)
        for n in ("doubao_qr.png", "doubao_qr_big.png", "doubao_qr_matrix.txt"):
            p = os.path.join(OUT_DIR, n)
            if os.path.exists(p):
                os.remove(p)


def deploy(roots):
    for r in roots:
        d = os.path.join(r, "3ds")
        os.makedirs(d, exist_ok=True)
        shutil.copy2(ROM, os.path.join(d, "doubao.3dsx"))
        os.makedirs(os.path.join(r, SD_SUBDIR), exist_ok=True)
        if os.path.exists(CACERT):
            cd = os.path.join(r, "config", "ssl")
            os.makedirs(cd, exist_ok=True)
            shutil.copy2(CACERT, os.path.join(cd, "cacert.pem"))


def seed_session(roots):
    """Hand the 3DS the session the PC already negotiated.

    A QR lives 60 seconds, so re-scanning just to test chat/history is
    pointless. Dropping cookie.txt on the SD card lets the probe skip
    straight to the interesting part.
    """
    src = os.path.join(HERE, "doubao_session.json")
    if not os.path.exists(src):
        return None
    sess = json.load(open(src, encoding="utf-8"))
    cookie = sess.get("cookie_header") or "; ".join(
        "%s=%s" % (k, v) for k, v in sess["cookies"].items())
    written = []
    for r in roots:
        d = os.path.join(r, SD_SUBDIR)
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "cookie.txt")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(cookie)
        written.append(p)
    print("seeded session cookie (%d chars) -> %s" % (len(cookie), written[0]))
    return cookie


def launch(primary):
    flags = 0
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(
        [AZAHAR, os.path.join(primary, "3ds", "doubao.3dsx")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=flags)
    with open(PID_FILE, "w") as fh:
        fh.write(str(proc.pid))
    return proc


def phase_start():
    if not os.path.exists(AZAHAR):
        print("azahar not found:", AZAHAR)
        return 2
    if not os.path.exists(ROM):
        print("probe not built:", ROM)
        return 2

    subprocess.run(["taskkill", "/F", "/IM", "azahar.exe"], capture_output=True)
    time.sleep(1)

    exe_dir = os.path.dirname(AZAHAR)
    roots = writable_roots(sdmc_roots(exe_dir))[:3]
    if not roots:
        print("no writable sdmc root")
        return 2
    print("sdmc roots:", roots)

    os.makedirs(OUT_DIR, exist_ok=True)
    clear_outputs(roots)
    deploy(roots)
    print("deployed doubao.3dsx + cacert.pem")
    if "--no-session" not in sys.argv:
        seed_session(roots)

    print("launching azahar (detached) ...")
    launch(roots[0])

    deadline = time.time() + 120
    qr = None
    while time.time() < deadline:
        p = find_file("qr_b64.txt", roots)
        st = find_file("status.txt", roots)
        if p:
            b64 = read_text(p) or ""
            b64 = "".join(b64.split())
            if len(b64) > 200:
                os.makedirs(OUT_DIR, exist_ok=True)
                raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
                with open(QR_PNG, "wb") as fh:
                    fh.write(raw)

                # Usually the server already returns an upscaled image
                # (~512 px), which scans fine as-is. Only when one pixel
                # equals one module (n <= 64) is it too small to scan, and
                # then - and only then - do we resample and enlarge.
                n, mat = png_to_matrix(raw)
                qr = QR_PNG
                if n and n <= 64:
                    upscale(mat, QR_BIG, scale=12, quiet=4)
                    with open(QR_TXT, "w") as fh:
                        for row in mat:
                            fh.write("".join("#" if v else "." for v in row) + "\n")
                    print("  matrix: %dx%d modules" % (n, n))
                    qr = QR_BIG
                break
        if st:
            s = (read_text(st) or "").strip()
            if s.startswith(("scan_", "bootstrap", "soc_")) and "waiting" not in s:
                print("  status:", s)
        time.sleep(1.0)

    if not qr:
        rep = find_file("report.txt", roots)
        print("!! no QR produced")
        if rep:
            print(read_text(rep))
        return 1

    print("\nQR ready -> %s (%d bytes)" % (qr, os.path.getsize(qr)))
    print("scan it with the DOUYIN app (not the Doubao app).")
    print("the 3DS keeps polling; when done run:")
    print("  python tools/run_doubao.py collect")
    return 0


def phase_collect():
    exe_dir = os.path.dirname(AZAHAR)
    roots = writable_roots(sdmc_roots(exe_dir))[:3]

    deadline = time.time() + 300
    last = None
    while time.time() < deadline:
        st = find_file("status.txt", roots)
        if st:
            s = (read_text(st) or "").strip()
            if s != last:
                print("  status:", s)
                last = s
            if s in ("done", "logged_in") or s.startswith(
                    ("scan_expired", "scan_canceled", "scan_failed",
                     "scan_timeout", "soc_fail")):
                break
        time.sleep(1.0)

    print()
    for name in ("status.txt", "cookie.txt", "history.txt", "chat.txt"):
        p = find_file(name, roots)
        if not p:
            print("--- %s: (missing) ---" % name)
            continue
        txt = read_text(p) or ""
        print("--- %s (%d bytes) ---" % (name, len(txt)))
        print(txt[:1500])
        print()

    rep = find_file("report.txt", roots)
    if rep:
        print("===== report.txt =====")
        print(read_text(rep))

    subprocess.run(["taskkill", "/F", "/IM", "azahar.exe"], capture_output=True)
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("start", "collect"):
        print(__doc__)
        return 2
    return phase_start() if sys.argv[1] == "start" else phase_collect()


if __name__ == "__main__":
    sys.exit(main())
