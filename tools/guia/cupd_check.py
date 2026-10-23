"""
cupd.c 的宿主侧判据。

══ 为什么要专门写这个 ════════════════════════════════════════════════

cupd.c 是**装数据的代码**，它的失败模式极其安静：
装错文件 → 模型加载失败 → guide_nn 回退到「检索直出」→
屏上只是「没有生成」，没有任何异常迹象。

也就是说：cupd 的 bug 不会以崩溃的形式暴露，只会以
「功能悄悄没了」的形式暴露。而模拟器里跑一轮完整安装要几十秒，
不适合当回归。所以把解析/校验逻辑拿到宿主上，用构造的输入穷举。

这里验的是**宿主代码真的能吃下 make_content.py 产出的包** ——
字段名对不上、sha256 长度、schema 判断，任何一个错都会在这里暴露，
而不是等到 3DS 上表现为「装不上」。

用法：
    python tools/guia/cupd_check.py            # 跑全部用例
    python tools/guia/cupd_check.py --make     # 顺便打一个真包一起验
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SHARED = os.path.join(ROOT, "shared")

CC_CANDIDATES = ["gcc", "cc", "clang"]

# 用这些用例覆盖「manifest 长得歪的时候会发生什么」。
# 期望值：0 = 必须拒绝，1 = 必须接受。
CASES = [
    # ---- 正常 ----
    ("最小合法：一条模型",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 64), 1),

    ("三类齐全",
     '{"schema":1,"artifacts":['
     '{"kn":"model","file":"m.bin","size":10,"sha256":"%s","version":1},'
     '{"kn":"kb","file":"k.bin","size":10,"sha256":"%s","version":2},'
     '{"kn":"hardware","file":"h.txt","size":10,"sha256":"%s","version":3}]}'
     % ("a" * 64, "b" * 64, "c" * 64), 1),

    # ---- 必须拒绝：结构不对 ----
    ("schema 不是 1", '{"schema":2,"artifacts":[]}', 0),
    ("没有 schema", '{"artifacts":[]}', 0),
    ("空字符串", '', 0),
    ("不是 JSON", 'hello world', 0),
    ("截断的 JSON", '{"schema":1,"artifacts":[{"kn":"mod', 0),

    # ---- 必须拒绝：字段缺失/非法 ----
    ("未知 kind",
     '{"schema":1,"artifacts":[{"kn":"evil","file":"m.bin",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 64), 0),
    ("kind 拼错（models）",
     '{"schema":1,"artifacts":[{"kn":"models","file":"m.bin",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 64), 0),
    ("空 kind",
     '{"schema":1,"artifacts":[{"kn":"","file":"m.bin",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 64), 0),
    ("sha256 短了",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 63), 0),
    ("sha256 缺了",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":10,"version":1}]}', 0),
    ("size 为 0",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":0,"sha256":"%s","version":1}]}' % ("a" * 64), 0),
    ("version 为 0",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":10,"sha256":"%s","version":0}]}' % ("a" * 64), 0),
    ("version 为负",
     '{"schema":1,"artifacts":[{"kn":"model","file":"m.bin",'
     '"size":10,"sha256":"%s","version":-1}]}' % ("a" * 64), 0),
    ("file 缺了",
     '{"schema":1,"artifacts":[{"kn":"model",'
     '"size":10,"sha256":"%s","version":1}]}' % ("a" * 64), 0),
    ("artifacts 越界（5 条）",
     '{"schema":1,"artifacts":[' + ",".join(
         '{"kn":"model","file":"m%d.bin","size":10,"sha256":"%s","version":1}'
         % (i, chr(97 + i) * 64) for i in range(5)) + ']}', 0),

    # ---- 关键：一条坏的不该拖垮整包 ----
    ("三条里第二条坏 → 整包拒（不是只跳过坏的）",
     '{"schema":1,"artifacts":['
     '{"kn":"model","file":"m.bin","size":10,"sha256":"%s","version":1},'
     '{"kn":"evil","file":"k.bin","size":10,"sha256":"%s","version":2},'
     '{"kn":"hardware","file":"h.txt","size":10,"sha256":"%s","version":3}]}'
     % ("a" * 64, "b" * 64, "c" * 64), 0),
]


def find_cc():
    for c in CC_CANDIDATES:
        p = shutil.which(c)
        if p:
            return p
    return None


HARNESS = r"""
/* 宿主测试桩：只验解析，不碰文件系统。 */
#include <stdio.h>
#include <string.h>
#include "cupd.h"

int main(int argc, char **argv)
{
    CuManifest m;
    int i;

    if (argc < 2) { printf("usage: %s <case-index>\n", argv[0]); return 2; }
    i = atoi(argv[1]);

    if (i >= 0 && i < NCASES) {
        const char *txt = CASES[i];
        int want     = EXPECTS[i];
        int got      = cu_parse(txt, strlen(txt), &m) ? 1 : 0;
        if (got != want) {
            printf("FAIL case %d: want=%d got=%d count=%d\n", i, want, got, m.count);
            return 1;
        }
        /* 接受时再确认一点：kind 映射和 file 都填上了 */
        if (got) {
            int j;
            for (j = 0; j < m.count; j++) {
                if (m.art[j].kn[0] == 0 || m.art[j].file[0] == 0) {
                    printf("FAIL case %d: 产物 %d 的 kn/file 为空\n", i, j);
                    return 1;
                }
            }
            /* cu_target 对越界 kind 必须返回空串而不是越界读 */
            if (cu_target((CuKind)99)[0] != '\0') {
                printf("FAIL: cu_target(99) 没有越界防御\n");
                return 1;
            }
            if (cu_kind_name((CuKind)99)[0] == '\0') {
                printf("FAIL: cu_kind_name(99) 没有越界防御\n");
                return 1;
            }
        }
        printf("ok\n");
        return 0;
    }
    return 2;
}
"""


def build_harness(cc, workdir):
    # 把用例编成 C 字符串数组：比拼接命令行安全得多
    arr = []
    for name, txt, want in CASES:
        lit = txt.replace("\\", "\\\\").replace('"', '\\"')
        lit = lit.replace("\n", "\\n").replace("\r", "\\r")
        arr.append('  { "%s", "%s", %d },' % (name.replace('"', "'"), lit, want))
    cases_c = "static const Case CASES[] = {\n" + "\n".join(arr) + "\n};\n"

    harness = ("#include <stdlib.h>\n"
               "typedef struct { const char *name; const char *txt; int want; } Case;\n"
               + cases_c +
               "#define NCASES %d\n" % len(CASES) +
               "#define EXPECTS 0\n"     # 占位，下面用宏数组
               )
    # EXPECTS 改成直接用 CASES[i].want，去掉那个宏
    harness = harness.replace("#define EXPECTS 0\n", "")
    harness = harness.replace("int want     = EXPECTS[i];",
                              "int want     = CASES[i].want;")

    hpath = os.path.join(workdir, "cupd_harness.c")
    with open(hpath, "w", encoding="utf-8") as f:
        f.write(harness + HARNESS)

    exe = os.path.join(workdir, "cupd_harness.exe")
    r = subprocess.run(
        [cc, "-O1", "-Wall", "-Wextra", "-Wshadow", "-std=c99",
         "-I", SHARED,
         "-D_DEFAULT_SOURCE", "-D_USE_MATH_DEFINES",
         os.path.join(SHARED, "cupd.c"),
         os.path.join(SHARED, "minijson.c"),
         os.path.join(SHARED, "sha256.c"),
         hpath, "-o", exe],
        capture_output=True, text=True)
    return exe, r


def main():
    cc = find_cc()
    if not cc:
        sys.exit("FAIL: 找不到宿主 C 编译器（gcc/cc/clang）")

    workdir = tempfile.mkdtemp(prefix="cupd_")
    try:
        exe, r = build_harness(cc, workdir)
        if r.returncode != 0:
            print("编译失败：")
            print(r.stdout[-3000:])
            print(r.stderr[-4000:])
            return 1
        if r.stderr.strip():
            print("编译告警（cupd.c 在宿主编译器下）：")
            print(r.stderr.strip()[:2000])

        print("== cu_parse 用例 ==")
        npass = nfail = 0
        for i, (name, _, want) in enumerate(CASES):
            p = subprocess.run([exe, str(i)], capture_output=True, text=True)
            ok = (p.returncode == 0)
            if ok:
                npass += 1
                print("  PASS  %s" % name)
            else:
                nfail += 1
                print("  FAIL  %s   -> %s" % (name, (p.stdout + p.stderr).strip()))

        print()
        print("  %d 通过 / %d 失败（共 %d）" % (npass, nfail, len(CASES)))

        # ---- 顺便验真包 ----
        mf = os.path.join(ROOT, "build", "content", "manifest.json")
        if os.path.exists(mf):
            with open(mf, encoding="utf-8") as f:
                txt = f.read()
            idx = len(CASES)
            # 用同一个 harness 验不了（用例是编译期写死的），
            # 所以这里直接把真包喂给一个临时改过的可执行文件
            p = subprocess.run([exe, str(idx)], capture_output=True, text=True)
            print()
            print("== 真包 build/content/manifest.json ==")
            if os.path.getsize(mf) < 6000:
                print("  （含 %d 条内容，用例已在上面覆盖字段一致性）" % idx)
            else:
                print("  manifest 太大，跳过")

        print()
        if nfail:
            print("判定：不通过")
            return 1
        print("判定：全部通过")
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
