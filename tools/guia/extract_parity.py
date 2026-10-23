"""
extract_parity.py — 验 C 端 extract.c 与 PC 侧 local_chat.py 的**行为一致**

══ 为什么必须有这个 ════════════════════════════════════════════════

我已经在 C 端犯过两个只有"实跑"才暴露的错：
  ① `c == '。'` 在 C 里是 multi-character constant（0x3002），
     截成 char 只剩 0x82 → **比较恒为假**，切不出句子。
     gcc 只给 warning，不报错。
  ② 往输出里写 `'。'` → 同样截成 0x82 → **多一个孤立续字节（乱码）**。

两次都是 `-Wall -Wextra` **报了 warning 但没当错误**。
→ 判据不能是"编译通过"，必须是"**输出与参照一致**"。

做法：把 extract.c 编成 PC 可用的 .so（用宿主 gcc，逻辑完全一样），
与 local_chat.extract() 逐条对比输出。

用法：python tools/guia/extract_parity.py
"""
import ctypes
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as GS          # noqa: E402
import local_chat as LC            # noqa: E402
import test_kb as TK                # noqa: E402

SRC = os.path.join(HERE, "..", "..", "shared", "nnet", "extract.c")
SO = os.path.join(HERE, "_extract_test.so")

# C 的 Gkb 结构布局（与 guide_kb.h 一致，字段顺序不能差）
class CStr(ctypes.Structure):
    _fields_ = [("p", ctypes.c_char_p)]


def build_so():
    """用宿主 gcc 编 .so。逻辑与 3DS 上一致，只是换平台。"""
    # ★ -DEXTRACT_TRACE：让 extract.c 里的 ET(...) 生效，把崩点打到 stderr。
    #   段错误杀进程、Python 捕不到异常，只能靠「崩前最后一行 stderr」定位。
    #   extract.c 里 ET 宏在未定义时是空的，所以 3DS 正式构建零开销。
    cmd = ["gcc", "-shared", "-fPIC", "-O1", "-g", "-Wall", "-Wextra",
           "-DEXTRACT_TRACE",
           "-I", os.path.join(HERE, "..", "..", "shared", "nnet"),
           SRC, "-o", SO]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("编译失败：")
        print(r.stderr[:2000])
        sys.exit(1)
    if "warning" in r.stderr.lower():
        print("⚠️ 编译有 warning（C 端最常见的坑是 multi-character "
              "constant，而它只是 warning）：")
        for l in r.stderr.split("\n"):
            if "warning" in l.lower() or "multichar" in l.lower():
                print("   " + l[:120])
    lib = ctypes.CDLL(SO)
    lib.gkb_extract.restype = ctypes.c_int
    lib.gkb_extract.argtypes = [
        ctypes.c_void_p,                    # const Gkb *
        ctypes.c_void_p,                    # const GkbHit *
        ctypes.c_int,                       # n_hit
        ctypes.c_char_p,                    # query
        ctypes.c_char_p,                    # out（当作可写缓冲）
        ctypes.c_int,                       # out_cap
    ]
    return lib


def main():
    if not os.path.exists(SRC):
        sys.exit("找不到 %s" % SRC)
    lib = build_so()
    print("已构建 %s\n" % os.path.basename(SO))

    # ---- 造一个 C 端能读的 Gkb 视图 ----
    # 直接用 C 端自己的 loader（test_kb.load 返回的就是同一份布局的 dict，
    # 但 C 函数要的是指针）。所以这里改用 ctypes 描述 Gkb。
    #
    # ⚠️ 更稳的做法是让 C 侧自己解包内嵌 blob 并交出指针 ——
    #    但那要把 zlib 链进来，代价大。
    #    这里用「ctypes 从 GKB1 头直接填字段」，与 guide_kb.c 的
    #    绑定顺序逐条对齐。判据：n / n_game / V 三项先对一遍。
    import pickle
    from train_bpe import BPE
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    kb = TK.load()
    R = GS.Retriever(kb, bpe, GS.GAME_ORDER)

    # 复刻 C 的 Gkb 布局：见 guide_kb.h 的 struct Gkb
    #   const char *text; int32 text_len; const int32 *pass_off; ...
    # test_kb.load() 给的是 dict，键名一一对应，直接取地址即可。
    text_arr = np.frombuffer(kb["text"], np.uint8)
    text_p = text_arr.ctypes.data_as(ctypes.c_void_p)

    class C_Kb(ctypes.Structure):
        _fields_ = [
            ("text", ctypes.c_void_p),
            ("text_len", ctypes.c_int32),
            ("pass_off", ctypes.c_void_p),
            ("pass_game", ctypes.c_void_p),
            ("post_off", ctypes.c_void_p),
            ("post_ids", ctypes.c_void_p),
            ("post_tf", ctypes.c_void_p),
            ("n_post", ctypes.c_int32),
            ("n_passage", ctypes.c_int32),
            # ★ 顺序必须与 guide_kb.h 的 Gkb 逐字段对齐。
            #   C 是「vocab 在前、n_game 在后」，这里曾经写反了。
            #   两者都是 int32，所以结构体大小不变、**不报任何错**，
            #   但 C 侧读到的 kb->vocab 是 0、kb->n_game 是 4096。
            #   而下面那句「对齐自检」打印的是 Python 自己字段的值，
            #   所以它照样打印 V=4096 —— 又一个「永远绿的假守卫」。
            ("vocab", ctypes.c_int32),
            ("n_game", ctypes.c_int32),
            ("avg_len", ctypes.c_float),
            ("blob", ctypes.c_void_p),
            ("pass_tlen", ctypes.c_void_p),
            ("avg_tlen", ctypes.c_float),
        ]

    n = kb["n"]
    ckb = C_Kb()
    ckb.text = text_p
    ckb.text_len = len(text_arr)

    # ★★ 必须把这些数组**存进活着的变量**。
    #   原写法 `ckb.x = np.ascontiguousarray(...).ctypes.data_as(c_void_p)`
    #   里，ascontiguousarray 的返回值是**临时对象**：test_kb.load() 用
    #   struct.unpack_from 解出来的是**元组**，所以每次都真的新建数组；
    #   而 .ctypes.data_as() 只保存**地址**、不持有引用 —— 语句一结束
    #   引用计数归零、数组被回收，ckb 里留下的是**悬空指针**。
    #   症状是 C 里一读就崩或读到垃圾，而且**不报错**。
    #   （当初能编过是因为 extract.c 只用 kb->text，而 text_arr 恰好是
    #     一直被持有的变量 —— 属于「侥幸没崩」，不是「没问题」。）
    a_pass_off = np.ascontiguousarray(kb["pass_off"], np.int32)
    # ⚠️ 2026-10-05：pass_game 由 int8 扩到 int16（语料超 127 款后 int8 回绕
    #    成负数）。这里若还传 np.int8，会把 gid>=128 截成负数再喂给 C，
    #    C 侧按 int16 读就是垃圾值 —— 对齐脚本会变"永远绿"（假通过）。
    a_pass_game = np.ascontiguousarray(kb["pass_game"], np.int16)
    a_post_off = np.ascontiguousarray(kb["post_off"], np.int32)
    a_post_ids = np.ascontiguousarray(kb["post_ids"], np.int32)
    a_post_tf = np.ascontiguousarray(kb["post_tf"], np.int8)

    ckb.pass_off = a_pass_off.ctypes.data_as(ctypes.c_void_p)
    ckb.pass_game = a_pass_game.ctypes.data_as(ctypes.c_void_p)
    ckb.post_off = a_post_off.ctypes.data_as(ctypes.c_void_p)
    ckb.post_ids = a_post_ids.ctypes.data_as(ctypes.c_void_p)
    ckb.post_tf = a_post_tf.ctypes.data_as(ctypes.c_void_p)
    ckb.n_passage = n
    ckb.vocab = kb["V"]
    # ★ n_post 之前**从没赋值**（一直是 0）。倒排表长度，
    #   凡是用它做边界的地方都会当成"倒排是空的"。
    ckb.n_post = int(kb.get("n_post", 0))
    ckb.avg_len = R.avg_len
    # 段落字节长度：kb["pass_tlen"] 存的是 **token 数**，C 端要字节
    tlen_bytes = [kb["pass_off"][i + 1] - kb["pass_off"][i] for i in range(n)]
    ckb.pass_tlen = np.ascontiguousarray(tlen_bytes, np.int32).ctypes.data_as(ctypes.c_void_p)

    print("对齐自检：n=%d  text_len=%d  V=%d"
          % (ckb.n_passage, ckb.text_len, ckb.vocab))
    if ckb.n_passage != n:
        sys.exit("段落数对不上")

    class C_Hit(ctypes.Structure):
        _fields_ = [("id", ctypes.c_int32), ("score", ctypes.c_float),
                    ("tok_off", ctypes.c_int32), ("tok_len", ctypes.c_int32),
                    ("game", ctypes.c_int32)]

    CASES = [
        ("火焰纹章觉醒 转职怎么转", "fe_awakening"),
        ("沙之杖怎么用", None),
        ("时之笛 钩索箭哪里拿", "zelda_oot3d"),
        ("大师之剑在哪里拿", None),
        ("路易吉洋馆2 吸尘器怎么用", "luigi2"),
        ("马力欧卡丁车7 漂移技巧", "mk7"),
        ("梅祖拉的假面 三天循环怎么过", "zelda_mm3d"),
        ("怪物猎人4G 弱点是什么", "mh4g"),
    ]

    print()
    print("%-22s %-6s %s" % ("问句", "一致", "C 端输出"))
    print("-" * 74)
    same = 0
    for q, gname in CASES:
        gid, rest = LC.split_game(q)
        if gname:
            gid = GS.GAME_ORDER.index(gname)
        hits = R.search(rest, gid, k=4) if gid >= 0 else R.search(rest, -1, k=4)
        if not hits:
            print("%-22s 检索 0 命中" % q)
            continue

        # 造 C 的 GkbHit 数组（注意 tok_len 要是**字节**）
        carr = (C_Hit * len(hits))()
        for i, (pid, sc) in enumerate(hits):
            carr[i].id = pid
            carr[i].score = sc
            carr[i].tok_off = kb["pass_off"][pid]
            carr[i].tok_len = tlen_bytes[pid]
            carr[i].game = kb["pass_game"][pid]

        # ---- 诊断：喂给 C 之前先把每条 hit 的字节范围打出来 ----
        # 段错误会直接杀掉进程，捕不到异常，所以「崩在哪一条」只能靠
        # **崩前最后一行打印**判断 —— 必须 flush=True，否则还留在
        # 缓冲区里的行会随进程一起消失，就什么线索都留不下了。
        info = [(int(kb["pass_off"][p]), int(tlen_bytes[p])) for p, _ in hits]
        print("  [诊断] %-22s hits=%d text_len=%d (off,len)=%s"
              % (q, len(hits), ckb.text_len, info), flush=True)
        bad = [(i, o, l) for i, (o, l) in enumerate(info)
               if o < 0 or l < 0 or (o + l) > ckb.text_len]
        if bad:
            print("  ❌ 越界，不喂给 C: %s" % bad, flush=True)
            continue

        buf = ctypes.create_string_buffer(2048)
        r = lib.gkb_extract(ctypes.byref(ckb),
                            ctypes.addressof(carr), len(hits),
                            rest.encode("utf-8"), buf, 2048)
        c_out = buf.value.decode("utf-8", "replace")
        print("  [诊断] 返回 r=%d 字节数=%d" % (r, len(buf.value)), flush=True)

        py_out, err, ncand, cov = LC.extract(q)

        ok = (c_out == (err if err else (py_out or "")))
        same += ok
        print("%-22s %-6s %s" % (q, "是" if ok else "★否", c_out[:44]))
        if not ok:
            print("    PC : %s" % (err if err else (py_out or ""))[:60])
            print("    C  : %s" % c_out[:60])

    print()
    print("一致 %d/%d" % (same, len(CASES)))
    return 0 if same == len(CASES) else 1


if __name__ == "__main__":
    sys.exit(main())
