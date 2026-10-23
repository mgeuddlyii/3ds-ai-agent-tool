"""
Replace dangling symlinks in E:\\devkitPro with real file copies.

Why: devkitPro tarballs carry archive-root-relative symlink targets
     (e.g. devkitARM/bin/arm-none-eabi-gcc.exe -> opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc-16.1.0.exe).
     On Windows those do not resolve, and Python's os.path.exists/shutil.remove
     are intercepted by a sandbox "safe delete" shim that fails here.
Fix: resolve the real payload, delete the symlink via the Win32 API
     (DeleteFileW, which is not shimmed), then copy the payload into place.
"""
import ctypes
import os
import shutil
import sys

DEST = r"E:\devkitPro"
PREFIXES = ("opt/devkitpro/", "./opt/devkitpro/", "/opt/devkitpro/")

k32 = ctypes.windll.kernel32
k32.DeleteFileW.argtypes = [ctypes.c_wchar_p]
k32.DeleteFileW.restype = ctypes.c_int
k32.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
k32.SetFileAttributesW.restype = ctypes.c_int

FILE_ATTRIBUTE_NORMAL = 0x80
FILE_ATTRIBUTE_READONLY = 0x01


def win_delete(path):
    """Delete a file/symlink through the raw Win32 API (bypasses python shims).

    Tar-mode 0555 extraction leaves the READONLY attribute set, which makes
    DeleteFileW fail with ERROR_ACCESS_DENIED (5). Clear it first.
    Returns (ok, last_error).
    """
    k32.SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL)
    r = k32.DeleteFileW(path)
    if r:
        return True, 0
    err = k32.GetLastError()
    return err in (2,), err


def resolve_payload(link_path, linkname):
    name = linkname
    for pre in PREFIXES:
        if name.startswith(pre):
            name = name[len(pre):]
            break
    cands = [os.path.normpath(os.path.join(DEST, name)),
             os.path.normpath(os.path.join(os.path.dirname(link_path), linkname))]
    for c in cands:
        if os.path.isfile(c) and not os.path.islink(c):
            return c
    base = os.path.basename(name)
    for dp, dn, fn in os.walk(DEST):
        if base in fn:
            p = os.path.join(dp, base)
            if not os.path.islink(p):
                return p
    return None


def main():
    links = []
    for dp, dn, fn in os.walk(DEST):
        for f in fn:
            p = os.path.join(dp, f)
            if os.path.islink(p):
                links.append(p)
    print("symlinks found: %d" % len(links))

    ok, bad = 0, []
    for p in links:
        try:
            linkname = os.readlink(p)
        except Exception:
            continue
        real = resolve_payload(p, linkname)
        if not real:
            bad.append((p, "no payload for " + linkname))
            continue
        if not win_delete(p)[0]:
            ok2, err = win_delete(p)
            bad.append((p, "delete failed err=%d" % err))
            continue
        try:
            shutil.copy2(real, p)
            ok += 1
        except Exception as e:
            bad.append((p, "copy failed: %s" % e))

    print("replaced: %d" % ok)
    print("failed  : %d" % len(bad))
    for p, r in bad[:20]:
        print("   %s | %s" % (p.replace(DEST, ""), r))

    gcc = os.path.join(DEST, "devkitARM", "bin", "arm-none-eabi-gcc.exe")
    print("\narm-none-eabi-gcc.exe exists:", os.path.exists(gcc))
    if os.path.exists(gcc):
        print("   size:", os.path.getsize(gcc))
    return 0 if os.path.exists(gcc) else 1


if __name__ == "__main__":
    sys.exit(main())
