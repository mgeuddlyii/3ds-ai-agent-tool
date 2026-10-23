"""
devkitPro 3DS toolchain fetcher (headless)
------------------------------------------
Pulls 3DS dev packages straight from pkg.devkitpro.org and unpacks them into
DEVKITPRO, bypassing the GUI installer (which stalls on the MSYS2 stage here).

Verified facts baked in:
  * libs db  : https://pkg.devkitpro.org/packages/dkp-libs.db
  * host db  : https://pkg.devkitpro.org/packages/windows/x86_64/dkp-windows.db
  * libs pkg : https://pkg.devkitpro.org/packages/<filename>
  * host pkg : https://pkg.devkitpro.org/packages/windows/x86_64/<filename>
    (note: the "dkp-windows" db segment is NOT part of the package path)
  * request UA must be "dkp-apt" or the server answers 403
  * packages are .pkg.tar.zst; zstd frames have no content size, so a streaming
    reader is mandatory (plain decompress() raises).
"""

import gzip
import io
import os
import shutil
import socket
import ssl
import sys
import tarfile
import time
import urllib.request
import urllib.error

try:
    import zstandard as zstd
except Exception:
    sys.exit("FATAL: need the 'zstandard' module (pip install zstandard)")

PKG_BASE = "https://pkg.devkitpro.org/packages"
UA = "dkp-apt"
DEST = r"E:\devkitPro"

REPOS = ["dkp-libs", "windows/x86_64/dkp-windows"]
HOST_REPO = "windows/x86_64/dkp-windows"
HOST_PKG_SUBDIR = "windows/x86_64"

GROUPS = ["3ds-dev"]

socket.setdefaulttimeout(120)
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE


def pkg_url(repo, filename):
    if repo == HOST_REPO:
        return "%s/%s/%s" % (PKG_BASE, HOST_PKG_SUBDIR, filename)
    return "%s/%s" % (PKG_BASE, filename)


def fetch(url, retries=3):
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "*/*"})
            with urllib.request.urlopen(req, timeout=180, context=CTX) as r:
                return r.read()
        except Exception as e:
            last = e
            if attempt < retries - 1:
                time.sleep(2 * (attempt + 1))
    raise last


def load_db(repo):
    raw = gzip.decompress(fetch("%s/%s.db" % (PKG_BASE, repo)))
    tf = tarfile.open(fileobj=io.BytesIO(raw), mode="r:")
    pkgs = {}
    for m in tf.getmembers():
        if not m.name.endswith("/desc"):
            continue
        txt = tf.extractfile(m).read().decode("utf-8", "replace")
        info, field, buf = {}, None, []
        for line in txt.splitlines():
            if line.startswith("%") and line.endswith("%"):
                if field:
                    info[field] = buf
                field, buf = line.strip("%"), []
            elif field is not None and line.strip():
                buf.append(line.strip())
        if field:
            info[field] = buf
        name = (info.get("NAME") or [""])[0]
        if name:
            pkgs[name] = {
                "name": name,
                "version": (info.get("VERSION") or [""])[0],
                "filename": (info.get("FILENAME") or [""])[0],
                "groups": info.get("GROUPS") or [],
                "depends": info.get("DEPENDS") or [],
                "desc": (info.get("DESC") or [""])[0],
                "size": int((info.get("CSIZE") or ["0"])[0] or 0),
                "repo": repo,
            }
    return pkgs


def resolve(pkgs, seeds):
    wanted, queue = set(), list(seeds)
    while queue:
        n = queue.pop()
        if n in wanted or n not in pkgs:
            continue
        wanted.add(n)
        for d in pkgs[n]["depends"]:
            if d not in wanted:
                queue.append(d)
    return wanted


def open_tar(data):
    if data[:4] == b"\x28\xb5\x2f\xfd":
        reader = zstd.ZstdDecompressor().stream_reader(io.BytesIO(data))
        return tarfile.open(fileobj=io.BytesIO(reader.read()), mode="r:")
    return tarfile.open(fileobj=io.BytesIO(data), mode="r:*")


# devkitPro tarballs use archive-root-relative, forward-slashed link targets
# (e.g. "opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc-16.1.0.exe"). Those never
# resolve on Windows, so we record the links and materialise them as real file
# copies once every package has been extracted.
PENDING_LINKS = []


def extract_pkg(data, dest):
    tf = open_tar(data)
    n = 0
    for m in tf.getmembers():
        base = os.path.basename(m.name)
        if base in (".PKGINFO", ".MTREE", ".BUILDINFO", ".INSTALL") or ".CHANGELOG" in m.name:
            continue
        rel = m.name
        for prefix in ("opt/devkitpro/", "./opt/devkitpro/"):
            if rel.startswith(prefix):
                rel = rel[len(prefix):]
                break
        target = os.path.join(dest, rel)
        if m.isdir():
            os.makedirs(target, exist_ok=True)
        elif m.issym() or m.islnk():
            # Defer: record and materialise as a real copy after extraction.
            PENDING_LINKS.append((target, m.linkname))
        else:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(tf.extractfile(m).read())
            try:
                os.chmod(target, m.mode)
            except Exception:
                pass
        n += 1
    return n


CRITICAL = ["devkitarm-gcc", "devkitarm-binutils", "devkitarm-newlib",
            "devkitarm-crtls", "devkitarm-rules", "libctru", "citro3d", "3dstools"]

LINK_PREFIXES = ("opt/devkitpro/", "./opt/devkitpro/", "/opt/devkitpro/")


def resolve_payload(link_path, linkname, dest):
    """Find the real file a recorded link should become a copy of."""
    name = linkname
    for pre in LINK_PREFIXES:
        if name.startswith(pre):
            name = name[len(pre):]
            break
    for c in [os.path.normpath(os.path.join(dest, name)),
              os.path.normpath(os.path.join(os.path.dirname(link_path), linkname))]:
        if os.path.isfile(c) and not os.path.islink(c):
            return c
    base = os.path.basename(name)
    for dp, dn, fn in os.walk(dest):
        if base in fn:
            p = os.path.join(dp, base)
            if not os.path.islink(p):
                return p
    return None


def materialise_links(dest):
    """Turn every deferred link into a real copy of its payload."""
    ok, bad = 0, []
    for link_path, linkname in PENDING_LINKS:
        real = resolve_payload(link_path, linkname, dest)
        if not real:
            bad.append((link_path, "payload missing: " + linkname))
            continue
        try:
            os.makedirs(os.path.dirname(link_path), exist_ok=True)
            shutil.copy2(real, link_path)
            ok += 1
        except Exception as e:
            bad.append((link_path, str(e)[:70]))
    return ok, bad


def main():
    print("=" * 64)
    print("devkitPro 3DS toolchain fetcher")
    print("=" * 64, flush=True)

    print("\n[1/5] package databases")
    allp = {}
    for repo in REPOS:
        try:
            db = load_db(repo)
            print("      %-28s %d packages" % (repo, len(db)))
            allp.update(db)
        except Exception as e:
            print("      %-28s FAILED (%s)" % (repo, e))
    if not allp:
        return 1

    print("\n[2/5] resolving deps for groups: %s" % ", ".join(GROUPS))
    seeds = []
    for g in GROUPS:
        m = [n for n, p in allp.items() if g in p["groups"]]
        print("      %-13s -> %d packages" % (g, len(m)))
        seeds += m
    wanted = resolve(allp, seeds)
    total = sum(allp[n]["size"] for n in wanted)
    print("      resolved %d packages, %.1f MB" % (len(wanted), total / 1048576))
    for c in CRITICAL:
        print("        %s %s" % ("OK  " if c in wanted else "MISS", c))

    missing = [c for c in CRITICAL if c not in wanted]
    if missing:
        print("      !! missing critical: %s" % missing)
        return 1

    print("\n[3/5] downloading + extracting -> %s" % DEST)
    os.makedirs(DEST, exist_ok=True)
    done, failed = 0, []
    for i, name in enumerate(sorted(wanted), 1):
        p = allp[name]
        try:
            data = fetch(pkg_url(p["repo"], p["filename"]))
            n = extract_pkg(data, DEST)
            done += 1
            print("      [%2d/%2d] %-24s %-9s %6.2fMB %4d files  %s" % (
                i, len(wanted), name, p["version"][:9], len(data) / 1048576, n, p["desc"][:26]))
        except Exception as e:
            failed.append((name, str(e)[:90]))
            print("      [%2d/%2d] FAILED %s: %s" % (i, len(wanted), name, str(e)[:80]))

    print("\n[4/5] summary")
    print("      extracted %d / %d" % (done, len(wanted)))
    for n, e in failed:
        print("        - %s: %s" % (n, e))

    print("\n[4b/5] materialising %d deferred links as real copies" % len(PENDING_LINKS))
    lok, lbad = materialise_links(DEST)
    print("      copied %d, problems %d" % (lok, len(lbad)))
    for p, r in lbad[:15]:
        print("        - %s: %s" % (os.path.relpath(p, DEST), r))

    print("\n[5/5] sanity check")
    gcc = os.path.join(DEST, "devkitARM", "bin", "arm-none-eabi-gcc.exe")
    rules = os.path.join(DEST, "devkitARM", "3ds_rules")
    hdr = os.path.join(DEST, "libctru", "include", "3ds.h")
    for label, p in [("arm-none-eabi-gcc", gcc), ("3ds_rules", rules), ("libctru/3ds.h", hdr)]:
        print("      %-20s %s" % (label, "OK" if os.path.exists(p) else "MISSING  " + p))

    if failed:
        return 2
    return 0 if os.path.exists(gcc) else 3


if __name__ == "__main__":
    sys.exit(main())
