"""List / install extra devkitPro 3DS portlibs (curl, mbedtls, ...) on Windows.

Why this exists:
  tools/dkp_get.py only pulls the base `3ds-dev` group. The 3DS's own httpc
  tops out at TLS 1.1, so anything needing TLS 1.2 must be reached with
  libcurl linked against mbedtls. These packages are NOT in 3ds-dev.

Usage:
  python tools/dkp_pkgs.py list                # show every 3ds-* package
  python tools/dkp_pkgs.py install 3ds-curl 3ds-mbedtls
"""

import gzip
import io
import os
import sys
import tarfile
import urllib.request

try:
    import dkp_get as G
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dkp_get as G


def main():
    args = sys.argv[1:]
    if not args:
        args = ["list"]

    print("[db] loading %s ..." % G.REPOS[0], flush=True)
    allp = G.load_db(G.REPOS[0])
    print("     %d packages" % len(allp), flush=True)

    threeds = sorted(n for n in allp if n.startswith("3ds-"))

    if args[0] == "list":
        for n in threeds:
            p = allp[n]
            print("  %-26s %-12s %7.2f MB  %s" % (
                n, p["version"], p["size"] / 1048576.0, p["desc"][:44]))
        return 0

    if args[0] != "install":
        print("unknown command", args[0])
        return 2

    wanted = []
    for a in args[1:]:
        if a not in allp:
            print("  !! not in db:", a)
            continue
        wanted.append(a)
    if not wanted:
        print("nothing to install")
        return 2

    # pull deps too
    full = G.resolve(allp, wanted) if hasattr(G, "resolve") else wanted
    total = sum(allp[n]["size"] for n in full)
    print("  resolved %d packages (%.1f MB)" % (len(full), total / 1048576.0))
    for n in sorted(full):
        print("      -", n, allp[n]["version"])

    dest = G.DEST
    os.makedirs(dest, exist_ok=True)
    for i, n in enumerate(sorted(full), 1):
        p = allp[n]
        try:
            data = G.fetch(G.pkg_url(p["repo"], p["filename"]))
            cnt = G.extract_pkg(data, dest)
            print("  [%d/%d] %-24s %6.2fMB %4d files" % (
                i, len(full), n, len(data) / 1048576.0, cnt))
        except Exception as e:
            print("  [%d/%d] FAILED %s: %s" % (i, len(full), n, str(e)[:90]))

    G.materialise_links(dest)
    print("done ->", dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
