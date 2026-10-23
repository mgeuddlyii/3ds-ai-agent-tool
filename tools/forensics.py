import os, re, subprocess, time

BASE = r"E:/Gudyi/WORK5/3ds-ai"
AZ   = r"E:/Azahar/azahar-windows-msvc-2126.1.1"
LOG  = AZ + "/user/log/azahar_log.txt"
LOGO = LOG + ".old.txt"
OUT  = BASE + "/logs"
os.makedirs(OUT, exist_ok=True)
BAR = "-" * 60

def mt(p):
    try:
        s = os.stat(p)
        return "%s %dB" % (time.strftime("%m-%d %H:%M:%S", time.localtime(s.st_mtime)), s.st_size)
    except OSError:
        return "MISSING"

print("### azahar processes ###")
tl = r"C:/Windows/System32/tasklist.exe"
try:
    r = subprocess.run([tl, "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=30)
    rows = [l for l in (r.stdout or "").splitlines() if "azahar" in l.lower()]
    print("\n".join(rows[:6]) if rows else "none running")
except Exception as e:
    print("tasklist failed:", e)

print("### artifacts ###")
for p in [BASE + "/app/AIConsole.3dsx", BASE + "/app/AIConsole.cia", BASE + "/app/AIConsole.elf"]:
    print(" %-16s %s" % (os.path.basename(p), mt(p)))

print("### sdmc contents (main root: user/sdmc) ###")
sd = AZ + "/user/sdmc/3ds/aiconsole"
if os.path.isdir(sd):
    for dp, ds, fs in os.walk(sd):
        if dp[len(sd):].count(os.sep) >= 2:
            ds[:] = []
            continue
        for f in fs:
            p = os.path.join(dp, f)
            print(" %-44s %s" % (p[len(sd)+1:], mt(p)))
else:
    print(" (dir missing)", sd)

MARK = ("undefined instruction", "data abort", "prefetch abort", "unmapped",
        "registers:", "stack dump", "guest thread crashed", "bad opcode")

def scan(path, tag):
    print(BAR)
    t = mt(path)
    if t == "MISSING":
        print("### %s: MISSING" % tag)
        return
    print("### %s (%s)" % (tag, t))
    hits, sdmc = [], []
    with open(path, "r", errors="replace") as f:
        for i, ln in enumerate(f):
            low = ln.lower()
            if any(m in low for m in MARK):
                hits.append((i, ln.rstrip()))
            if "sdmc" in low and len(sdmc) < 12 and any(k in low for k in ("mount", "dir", "path", "sdmc:", "sdmc/")):
                sdmc.append(ln.rstrip()[:170])
    print(" crash-mark lines:", len(hits))
    if hits:
        for i, ln in hits[:26]:
            print("  *", ln[:200])
        a, b = max(0, hits[0][0]-40), hits[-1][0]+60
        with open(path, "r", errors="replace") as f, open(OUT + "/" + tag + "_crashblock.txt", "w", errors="replace") as g:
            for i, ln in enumerate(f):
                if i < a: continue
                if i > b: break
                g.write(ln)
        print("  block %d..%d -> logs/%s_crashblock.txt" % (a, b, tag))
    else:
        open(OUT + "/" + tag + "_crashblock.txt", "w").write("(no crash marks)\n")
        print("  (no crash marks)")
    if sdmc:
        print(" sdmc-ish lines:")
        for s in sdmc[:8]:
            print("  -", s)

scan(LOG, "new")
scan(LOGO, "old")

print(BAR)
print("### qt-config search ###")
found = []
for root in [AZ + "/user", r"C:/Users/Kevin/AppData/Roaming/Azahar", r"C:/Users/Kevin/AppData/Local/Azahar"]:
    if not os.path.isdir(root):
        continue
    for dp, ds, fs in os.walk(root):
        if dp[len(root):].count(os.sep) >= 3:
            ds[:] = []
            continue
        for f in fs:
            if "qt-config" in f.lower():
                found.append(os.path.join(dp, f))
if not found:
    print(" none found")
for p in found:
    print(" FOUND:", p)
    try:
        with open(p, "r", errors="replace") as f:
            for line in f:
                ll = line.lower()
                if "sdmc" in ll or "data%20storage" in ll or "data storage" in ll:
                    print("   >", line.strip()[:160])
    except OSError:
        pass

print(BAR)
print("### elf symbols ###")
obj = r"E:/devkitPro/devkitARM/bin/arm-none-eabi-objdump.exe"
elf = BASE + "/app/AIConsole.elf"
if os.path.exists(obj) and os.path.exists(elf):
    r = subprocess.run([obj, "-t", elf], capture_output=True, text=True, timeout=60)
    with open(BASE + "/tools/_elfsym.txt", "w") as g:
        g.write(r.stdout)
    names = re.findall(r"^[0-9a-fA-F]{8,}\s+\S+\s+F\s+\.text\s+[0-9a-fA-F]+\s+(\S+)", r.stdout, re.M)
    print(" total F symbols:", len(names))
    print(" stack-ish:", [n for n in names if "stack" in n.lower()][:10])
else:
    print(" objdump or elf missing:", obj, os.path.exists(obj), elf, os.path.exists(elf))
print("done")
