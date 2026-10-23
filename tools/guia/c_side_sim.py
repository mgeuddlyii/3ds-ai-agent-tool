#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
c_side_sim.py — 在 PC 上**逐行复刻 C 侧那条链路**，用来定位「检索 0 命中」

═══ 为什么需要它 ═══════════════════════════════════════════════════
2026-10-04：C 侧抽取式 X2 趟 1/8，8 条用例里 3 条报「检索 0 命中」。
现象是 gid 正确、rest 正确（`[  转职怎么转]`）、索引里该词有 16 个 posting，
而 PC 侧同样输入能命中 3 段 → 差异必在 gkb_search 内部，**但没人查出来**。

本文件把 C 侧四层原样搬过来，用**同一份真实索引**跑：
    nnet_text_encode()   <- shared/nnet/nnet_text.c
    gkb_query_tokens()   <- shared/nnet/guide_kb.c
    gkb_search()         <- shared/nnet/guide_kb.c
    gkb_split_game()     <- shared/nnet/guide_kb.c

★ 关键设计：词表**从卡上那份 app/data/guide_vocab.txt 读**，
  索引**从 tools/guia/ckpt/guide_kb.bin 读** ——
  两份都是产物里真实用的那一份，不是重建的。
  所以「如果 C 侧能跑出 0 命中，这里也会 0 命中」。

用法：
    python c_side_sim.py                # 用卡上词表（复现 bug）
    python c_side_sim.py --vocab ckpt   # 用 ckpt 词表（对照组）
"""
from __future__ import print_function

import math
import os
import pickle
import re
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
CARD_VOCAB = os.path.join(PROJ, "app", "data", "guide_vocab.txt")
KB_BIN = os.path.join(HERE, "ckpt", "guide_kb.bin")

# ---- BM25 参数：必须与 guide_kb.c 的 #define 一致 ----
K1 = 2.2
BB = 0.75
COV_ALPHA = 0.5
OVERVIEW_F = 0.35
GKB_HIT = 4
GKB_MAX_QUERY = 24

# ---- 停用词：逐字抄自 guide_kb.c（2026-10-04 已对齐 EX 29 / SUB 19）----
STOPSUB = ("怎么", "如何", "哪里", "在哪", "什么", "为什么", "为何", "多少",
           "要点", "用处", "教程", "方法", "介绍", "意思", "能否", "应该",
           "什么用", "有啥", "咋")
STOPEX = ("的", "了", "吗", "呢", "吧", "啊", "和", "与", "在", "是", "我",
          "会", "要", "个", "这", "那", "上", "下", "中", "之", "其", "有",
          "以及", "还有", "可以", "需要", "没有", "有没有", "哪")

# ---- 游戏别名：★ 运行时从 shared/nnet/guide_kb.c 的 g_alias[] 抠 ----
# 这里**曾经是手抄的 31 行字面量**，抄的是 gen_game_tables.py 的
# 「19 款时代」输出。语料扩到 66 款后那张表没人重跑，抄件也就整片过期 ——
# 复刻脚本拿过期表跑，验的是一个**不存在的 C 侧行为**，绿得没意义。
# 铁律 6b：复刻脚本的常量必须同源，抠不到就 FAIL，绝不给兜底值。
KB_C = os.path.join(PROJ, "shared", "nnet", "guide_kb.c")
CORPUS_DIR = os.path.join(PROJ, "tools", "guia", "corpus")


def parse_c_tables():
    """从 guide_kb.c 抠出 g_alias[] 与 g_game_names[]。

    返回 (alias_map, names)：
        alias_map = {alias_str: gid}
        names     = [游戏正式名]
    抠不到任何一块 -> 抛异常（= 判据红），不静默退化成空表。
    """
    src = open(KB_C, encoding="utf-8").read()

    m = re.search(r"static\s+const\s+GkbAlias\s+g_alias\[\]\s*=\s*\{(.*?)\};",
                  src, re.S)
    if not m:
        raise RuntimeError("%s 里找不到 g_alias[] —— 抠不到就不许跑" % KB_C)
    alias_map = {}
    for a, g in re.findall(r"\{\s*\"([^\"]*)\"\s*,\s*(-?\d+)\s*\}", m.group(1)):
        alias_map[a] = int(g)

    m2 = re.search(r"static\s+const\s+char\s+\*g_game_names\[\]\s*=\s*\{(.*?)\};",
                   src, re.S)
    if not m2:
        raise RuntimeError("%s 里找不到 g_game_names[] —— 抠不到就不许跑" % KB_C)
    names = re.findall(r"\"([^\"]*)\"", m2.group(1))
    if not names:
        raise RuntimeError("g_game_names[] 是空的 —— 抠错了还是表真空？")
    return alias_map, names


def corpus_files():
    """语料里的 guide_*.txt，按 build_kb.py 相同的排序规则（sorted）。"""
    return sorted(f for f in os.listdir(CORPUS_DIR)
                  if f.startswith("guide_") and f.endswith(".txt"))


def corpus_gid(stem, files):
    """语料文件基名 -> 真实 game_id。

    ★ build_kb.py 就是按 `sorted(glob("guide_*.txt"))` 的下标分配 game_id
    （见 build_kb.py 的 `game_id = 0 / game_id += 1`），所以这张表是
    **KB 的真相来源**，比任何手抄坐标都可靠。

    ★★ 但 game_id 是**位置**，位置只在「目录清单与 KB 一致」时才可比。
    实测踩到：索引 n_game=66 而目录已经 69 款（另一个会话正在往里加
    语料，文件名从 guide_mh3g.txt 改成 guide_mhxx.txt）。这时候算出来的
    「mk7 = 27」和 KB 里的「mk7 = 26」是两个不同的游戏，
    拿它当期望值等于拿新尺子量旧尺子 —— 判据会比出一场假红。
    → 所以先由调用方比对 `len(files) == n_game`，不等就直接判失败下场，
      不允许 corpus_gid 在错位状态下被用到。
    """
    want = "guide_%s.txt" % stem
    if want not in files:
        raise RuntimeError("语料里没有 %s（当前 %d 款）" % (want, len(files)))
    return files.index(want)

# 与 exttest.c 的 CASES[] 一致，但**第三项不再写 gid 数字**：
#   原来：("时之笛 钩索箭哪里拿", "钩索箭", 30)   # 30 是手抄的旧坐标
#   现在：("时之笛 钩索箭哪里拿", "钩索箭", "zelda_oot3d")   # 语料基名
# 真实 game_id 由 corpus_gid() 从 `sorted(guide_*.txt)` 下标现算。
# 为什么这么改：game_id 是**位置不是内容**（build_kb.py 按文件名排序分配），
# KB 一重建就整片位移，硬编码坐标必然过期。上一版这三条就是这么红的。
CASES = [
    ("火焰纹章觉醒 转职怎么转", "转职", "fe_awakening"),
    ("时之笛 钩索箭哪里拿", "钩索箭", "zelda_oot3d"),
    ("怪物猎人4G 弱点是什么", "弱点", "mh4g"),
    ("路易吉洋馆2 吸尘器怎么用", "吸尘器", "luigi2"),
    ("马力欧卡丁车7 漂移技巧", "漂移", "mk7"),
    ("大师之剑在哪里拿", "大师之剑", None),     # 无游戏限定
    ("沙之杖怎么用", "沙之杖", None),
    ("梅祖拉的假面 三天循环怎么过", "循环", "zelda_mm3d"),
]

# 用例关键词 -> 「这个词在索引里真实落在哪些 gid」。
# 检索循环前会全表扫一遍填进去（见 main() 的 kw_gids 预扫描）。
# ★ 必须显式建，不能靠 defaultdict：预设空集合是为了让
#   「这个词一次都没出现」与「出现了但落在别的 gid」是两条能分开的病因。
CASES_KW = {t[1]: set() for t in CASES}


# ================= 卡上词表加载（= gkb_vocab_load_mem）====================
class CVocab(object):
    def __init__(self, inv, merges):
        self.inv = inv            # id -> bytes
        self.merges = merges      # [(a,b)] 按优先级
        self.ranks = {}
        for i, p in enumerate(merges):
            self.ranks.setdefault(p, i)
        self.V = len(inv)

    def piece(self, tid):
        return self.inv[tid]


def load_card_vocab():
    lines = open(CARD_VOCAB, encoding="ascii").read().split("\n")
    assert lines[0] == "GVB1", "卡上词表 magic 不对：%r" % lines[0]
    hdr = lines[1].split()
    V, M = int(hdr[0]), int(hdr[1])
    inv, p = [], 2
    for _ in range(V):
        parts = lines[p].split()
        n = int(parts[0])
        raw = bytes.fromhex(parts[1])
        assert len(raw) == n, "第 %d 行长度对不上" % p
        inv.append(raw)
        p += 1
    merges = []
    for _ in range(M):
        a, b = lines[p].split()
        merges.append((int(a), int(b)))
        p += 1
    return CVocab(inv, merges)


def load_ckpt_vocab():
    d = pickle.load(open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb"))
    inv = d["inv"]
    # bpe.pkl 里的 inv 是 **{id: bytes}**（BPE 类内部用的字典），
    # 而 export_vocab.py 写出去的是按 id 排序的**列表**。
    # 两种形态都要归一成 list，否则顺序会错 —— 而顺序错就是本文件要查的病。
    if isinstance(inv, dict):
        inv = [inv[i] for i in range(len(inv))]
    return CVocab(list(inv), d["merges"])


# ================= nnet_text.c 复刻 =================
def norm_char_collect(s):
    """UTF-8 归一化：全角空格 -> ' '，全角 ASCII -> 半角（其余原样）。
    返回归一化后的 str（C 端是按字节，这里按码点等价处理）。"""
    out = []
    for ch in s:
        o = ord(ch)
        if o == 0x3000:
            out.append(" ")
        elif 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        else:
            out.append(ch)
    return "".join(out)


def bpe_word(v, wb):
    """对应 bpe_word()：按 merge rank 贪心合并。"""
    if len(wb) == 0:
        return []
    if len(wb) == 1:
        return [v.inv.index(wb) if wb in v.inv else None]
    ids = [wb[i:i + 1] for i in range(len(wb))]
    # 用 merges 表当 rank 来源：先建 bytes->id
    b2i = {}
    for i, b in enumerate(v.inv):
        b2i.setdefault(b, i)
    idv = [b2i[p] for p in ids]
    while len(idv) > 1:
        best, bpos = None, -1
        for i in range(len(idv) - 1):
            r = v.ranks.get((idv[i], idv[i + 1]))
            if r is not None and (best is None or r < best):
                best, bpos = r, i
        if best is None:
            break
        merged = v.inv[idv[bpos]] + v.inv[idv[bpos + 1]]
        if merged not in b2i:
            # C 端找不到就 break（merged < 0 -> break）
            break
        idv[bpos:bpos + 2] = [b2i[merged]]
    return idv


def text_encode(v, text, max_out=256):
    """对应 nnet_text_encode()：按空格切块 -> 逐块 bpe_word。"""
    t = norm_char_collect(text)
    out = []
    for w in t.split(" "):
        if not w:
            continue
        out.extend(bpe_word(v, w.encode("utf-8")))
        if len(out) >= max_out:
            break
    return out[:max_out]


# ================= guide_kb.c 复刻 =================
def gkb_query_tokens(kb, v, query):
    raw = text_encode(v, query, GKB_MAX_QUERY * 2)
    if not raw:
        return []
    toks = []
    for tid in raw:
        if tid is None or tid < 0 or tid >= kb["V"] - 3:
            continue
        if kb["post_off"][tid + 1] <= kb["post_off"][tid]:
            continue
        piece = v.piece(tid)
        try:
            pstr = piece.decode("utf-8")
        except UnicodeDecodeError:
            pstr = None
        if pstr is None:
            # 非合法 UTF-8 的碎片：按字节判停用词（C 端 memcmp 语义）
            pb = piece
            if any(pb == s.encode("utf-8") for s in STOPEX):
                continue
            if any(s.encode("utf-8") in pb for s in STOPSUB):
                continue
            toks.append(tid)
            continue
        if pstr in STOPEX:
            continue
        if any(s in pstr for s in STOPSUB):
            continue
        toks.append(tid)
    return toks


def gkb_search(kb, v, query, game_filter, max_out=GKB_HIT):
    toks = gkb_query_tokens(kb, v, query)
    if not toks:
        return [], toks
    N = float(kb["n"])
    score = defaultdict(float)
    hitc = defaultdict(int)
    for tid in toks:
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        df = float(e - s)
        idf = math.log(1.0 + (N - df + 0.5) / (df + 0.5))
        w = idf * idf
        for j in range(s, e):
            pid = kb["post_ids"][j]
            if game_filter >= 0 and kb["pass_game"][pid] != game_filter:
                continue
            tf = kb["post_tf"][j]
            L = float(kb["pass_tlen"][pid])
            num = tf * K1
            den = tf + K1 * (1.0 - BB + BB * L / max(1.0, kb["avg_len"]))
            score[pid] += w * num / den
            hitc[pid] += 1
    nq = float(len(toks))
    ranked = []
    for pid, sc in score.items():
        sc *= (hitc[pid] / nq) ** COV_ALPHA
        if kb["text_start"](pid).startswith("游戏概述"):
            sc *= OVERVIEW_F
        ranked.append((pid, sc))
    ranked.sort(key=lambda kv: -kv[1])
    return ranked[:max_out], toks


def gkb_split_game(q, alias_map):
    """对应 gkb_split_game()：最长匹配优先，认出则剔除别名。

    alias_map 从 guide_kb.c 的 g_alias[] 抠来（gkb_split_game 读的就是它）。
    返回 (gid, rest)；gid=-1 时 rest 就是原问句。
    """
    best = None
    for a, gid in alias_map.items():
        pos = q.find(a)
        if pos < 0:
            continue
        key = (len(a.encode("utf-8")), gid, pos)
        if best is None or key > best[0]:
            best = (key, gid, pos, a)
    if best is None:
        return -1, q
    _, gid, pos, a = best
    return gid, q[:pos] + q[pos + len(a):]


# ================= extract.c 复刻 =================
# ⚠️ 下面三个函数是 shared/nnet/extract.c 的**逐行复刻**。
# 复刻的是「逻辑」，不是「字节表示」—— Python 的 str 是 Unicode，
# 而 C 那边全程按字节走。所以下面刻意用 bytes 操作，
# 凡涉及字节下标的地方都注明了对应的 C 写法。

EXT_SENT_MAX = 512
EXT_CAND_MAX = 48
EXT_PICK = 3
ENT_BYTES = 16

# extract.c 的 is_title_like() 里的元信息前缀
TITLE_PREFIX = ("主要", "基本", "常见", "进阶", "隐藏", "前期", "中期", "后期",
                "道具", "玩法", "系统", "要点", "技巧", "总结", "简介", "一览",
                "说明", "注意", "警告", "提示")


def u8_len(c):
    """对应 extract.c 的 u8_len()：按首字节判断长度。c 是 int（0..255）。"""
    if c < 0x80:
        return 1
    if (c & 0xE0) == 0xC0:
        return 2
    if (c & 0xF0) == 0xE0:
        return 3
    return 4


def utf8_ok(b):
    """exttest.c 的 utf8_ok()：抓「孤立续字节」和「序列被截断」。"""
    i, n = 0, len(b)
    while i < n:
        c = b[i]
        if c < 0x80:
            i += 1
            continue
        if (c & 0xE0) == 0xC0:
            need = 1
        elif (c & 0xF0) == 0xE0:
            need = 2
        else:
            return False, i            # 首位就是续字节
        for k in range(1, need + 1):
            if i + k >= n:
                return False, i        # 截断
            if (b[i + k] & 0xC0) != 0x80:
                return False, i
        i += need + 1
    return True, -1


def extract_entities(query, max_ent=8):
    """对应 extract_entities()：停用词当分隔符切片段，每片段取 2~5 字窗口。

    ★ 全部按 bytes 走，因为 C 那边是 memcmp/字节下标。

    ★★ 2026-10-04 补的**兜底段**（对应extract.c 同名改动）：
       停用词当分隔符会**切碎含单字停用词的名字**——
       「之」「有」「中」都在停用表里，于是
           「沙之杖有什么效果」->片段 ['沙','杖','效果']
       「沙之杖」整个消失，而 #1085/#1086 段里确实写着「沙之杖」。
       → 所以额外把**原始问句**（去空白）的 2~5 字 n-gram 也列进来。
    """
    qb = query.encode("utf-8")
    bl = len(qb)
    buf = bytearray()
    i = 0
    while i < bl:
        skip = False
        for s in STOPSUB:
            sb = s.encode("utf-8")
            if qb[i:i + len(sb)] == sb:
                i += len(sb)
                skip = True
                break
        if skip:
            buf.append(0x01)
            continue
        for s in STOPEX:
            sb = s.encode("utf-8")
            if qb[i:i + len(sb)] == sb:
                i += len(sb)
                skip = True
                break
        if skip:
            buf.append(0x01)
        else:
            buf.append(qb[i])
            i += 1

    out = []
    w = len(buf)
    start = 0
    for i in range(w + 1):
        if i == w or buf[i] == 0x01:
            s, e = start, i
            # 跳过片段首尾空白（注意 C 里 s/e 是可变的）
            while s < e and buf[s] == 0x20:
                s += 1
            while e > s and buf[e - 1] == 0x20:
                e -= 1
            if e - s >= 2 and len(out) < max_ent:
                for L in range(5, 1, -1):
                    if e - s < L:
                        continue
                    for j in range(s, e):
                        if len(out) >= max_ent:
                            break
                        # 取 L 个**汉字**（不是 L 字节）
                        b0, cnt = j, 0
                        while b0 < e and cnt < L:
                            l = u8_len(buf[b0])
                            if b0 + l > e:
                                break
                            b0 += l
                            cnt += 1
                        if cnt != L:
                            continue
                        nb = b0 - j
                        if nb >= ENT_BYTES:
                            continue
                        out.append(bytes(buf[j:b0]))
            start = i + 1

    # ---- ★ 兜底段：原始问句（去空白）的 2~5 字 n-gram ----
    raw = bytes(c for c in qb if c not in (0x20, 0x09, 0x0A, 0x0D))
    # 汉字起始下标表。★ 只收**完整**的汉字：末尾若只剩半个码点就不进表，
    #   否则 ns 会比真实汉字数大1，下标 st[a+L] 越界（实测 IndexError）。
    st = []
    k = 0
    while k < len(raw) and len(st) < 64:
        l = u8_len(raw[k])
        if k + l > len(raw):
            break
        st.append(k)
        k += l
    ns = len(st)
    for a in range(ns):
        for L in range(5, 1, -1):
            # ★ 必须是 `>=` 而不是 `>`：窗口是 st[a] .. st[a+L]，
            #   下标 a+L 本身就要合法，所以 a+L == ns 已经越界。
            #   写成 `>` 时，a=0 / L=5 / ns=5 这组会漏过判据，
            #   然后 st[5] IndexError（实测整个函数对 5 字问句全挂）。
            if a + L >= ns:
                continue
            b0, b1 = st[a], st[a + L]
            nb = b1 - b0
            if nb >= ENT_BYTES:
                continue
            piece = raw[b0:b1]
            if piece in out or len(out) >= max_ent:
                continue
            out.append(piece)
    return out


def is_title_like(b):
    """对应 is_title_like()：太短、或以元信息词开头且整句短。"""
    if len(b) < 10:
        return True
    for p in TITLE_PREFIX:
        pb = p.encode("utf-8")
        if b[:len(pb)] == pb and len(b) < 16 * 3:
            return True
    return False


def gkb_extract(kb, hits, query, out_cap=768):
    """对应 gkb_extract()：切句 -> 覆盖判定 -> 取前 EXT_PICK 句。

    返回 (out_bytes, n_cand, n_picked, dbg)；
    dbg 是 exttest 装的那个诊断钩子的等价物（这里只记条数）。
    """
    # ★ 容量与 C 侧 ENT_CAP 一致（48）；传 8 会让兜底词全被挤掉
    ents = extract_entities(query, 48)
    cand, cand_hit = [], []
    n_ent = len(ents)
    text = kb["text"]

    # ---- ① 逐段切句 ----
    for pid, off, plen in hits:
        if len(cand) >= EXT_CAND_MAX:
            break
        p = text[off:off + plen]
        if not p:
            continue
        start = 0
        i = 0
        last = len(p)
        while i <= last:
            at_end = (i >= last)
            is_sep = False
            c = 0 if at_end else p[i]
            if not at_end:
                if c in (0x0A, 0x0D):
                    is_sep = True
                elif (c == 0xE3 and i + 2 < len(p)
                      and p[i + 1] == 0x80 and p[i + 2] == 0x82):
                    is_sep = True                     # 。
                elif (c == 0xEF and i + 2 < len(p) and p[i + 1] == 0xBC
                      and p[i + 2] in (0x81, 0x9F)):
                    is_sep = True                     # ！ ？
            if not is_sep and not at_end:
                i += 1
                continue

            if i > start:
                slen = i - start
                if slen <= EXT_SENT_MAX:
                    tmp = bytes(p[start:start + slen])
                    # ★★ 长度门槛必须按**字符数**判，不能用 len(tmp)。
                    # 这里 tmp 是 bytes，len() 数的是**字节**；而 PC 参照
                    # local_chat.py 的 sentences(min_len=10) 里 p 是 str，
                    # len() 数的是**字符**。两边单位不同 → 候选集不同 →
                    # top-3 选取整体错位（2026-10-06 在 AI Studio 上实测
                    # 8/8 全不一致，根因之一就是这个）。
                    # 而且 10 字节只等于 3 个汉字，作门槛明显太松。
                    # → 三边（extract.c / c_side_sim.py / local_chat.py）
                    #   统一按**字符数**判。
                    try:
                        n_char = len(tmp.decode("utf-8"))
                    except UnicodeDecodeError:
                        n_char = len(tmp)   # 截断在多字节中间时退回字节数
                    if n_char >= 10 and not is_title_like(tmp):
                        if tmp not in cand and len(cand) < EXT_CAND_MAX:
                            cand.append(tmp)
            # ★ 关键：按**字节长度**推进标点，不是 +1。
            #   「。」是 3 字节（E3 80 82），
            #   +1 会把 E3 留给上一句、下一句从 80 82 起 —— 每句都带孤立续字节，
            #   而整段字节数只差 1，肉眼和"字节数"都看不出来。
            if is_sep:
                step = 1 if c in (0x0A, 0x0D) else 3
                i += step
                # 标点后可能紧跟引号/括号/空格。
                # ⚠️ 注意 0xEF 是**3 字节序列的首字节**（全角标点、以及
                # 「ā」这类拉丁扩展字母都是它开头）。按 1 字节推进会落进
                # 续字节中间，下一轮又在续字节里找不到标点 ->
                # 若后面又跟着可跳字节就可能原地打转。
                # → 按**完整序列长度**推进。
                while i < len(p):
                    d = p[i]
                    if d in (0x22, 0x27, 0x0A, 0x0D, 0x20):
                        i += 1
                    elif d == 0xEF:
                        i += u8_len(d)
                    else:
                        break
            start = i
            # ★ 兜底：at_end 且不是分隔符时，start==i 且两者都不会再推进
            #   -> while 条件仍为真 -> 死循环。
            #   C那边同样有这个洞（`if (is_sep && !at_end)` 才推进 i，
            #   而 at_end 时 start=i 不变），空段落就会挂死。
            #   正确做法：收完最后一句就退出。
            if at_end:
                break
    n_cand = len(cand)
    if n_cand == 0:
        return b"", 0, 0, ents

    # ---- ② 覆盖判定 ----
    cand_hit = []
    for k in range(n_cand):
        hit = False
        for e in ents:
            if e and e in cand[k]:
                hit = True
                break
        cand_hit.append(hit)

    # ---- ③ 取前 EXT_PICK 句；先要命中的，再补没命中的（保持原序）----
    out = bytearray()
    picked = 0
    for take_hit in (True, False):
        for k in range(n_cand):
            if picked >= EXT_PICK:
                break
            if take_hit and not cand_hit[k]:
                continue
            if (not take_hit) and cand_hit[k]:
                continue
            s = cand[k]
            l = len(s)
            # ★ 判「要不要补句号」看**当前这句**是否以 0x82 结尾，
            #   不是看已写出的 out 的最后一个字节（那是上一句的尾字节）。
            cur_ends_stop = l >= 3 and s[l - 1] == 0x82
            add_stop = (len(out) > 0) and (not cur_ends_stop)
            need = l + (3 if add_stop else 0)
            if len(out) + need >= out_cap:
                return bytes(out), n_cand, picked, ents
            if add_stop:
                out += b"\xe3\x80\x82"
            out += s
            picked += 1
    return bytes(out), n_cand, picked, ents


# ================= 索引加载 =================
def load_kb():
    blob = open(KB_BIN, "rb").read()
    assert blob[:4] == b"GKB1"
    off = 4
    n, n_game, V, text_len = struct.unpack_from("<4i", blob, off); off += 16
    avg_len, = struct.unpack_from("<f", blob, off); off += 4
    pass_off = struct.unpack_from("<%di" % (n + 1), blob, off); off += 4 * (n + 1)
    # ⚠️ pass_game 由 i8 扩到 i16 后，**偏移也要跟着 ×2**。
    #   2026-10-05 就是这样栽的：格式串改成了 "<%dh" 却还留着 `off += n`，
    #   于是后面 post_off / post_ids 整体错位、n_post 读到垃圾值 →
    #   `IndexError: tuple index out of range`（c_side_sim 挂成 FAIL）。
    #   这条正是"改格式串必须同时改偏移"的实例。
    pass_game = struct.unpack_from("<%dh" % n, blob, off); off += 2 * n   # i16: 语料>127 款
    post_off = struct.unpack_from("<%di" % (V + 1), blob, off); off += 4 * (V + 1)
    n_post = post_off[V]
    post_ids = struct.unpack_from("<%dh" % n_post, blob, off); off += 2 * n_post
    post_tf = struct.unpack_from("<%db" % n_post, blob, off); off += n_post
    text = blob[off:off + text_len]

    kb = dict(n=n, n_game=n_game, V=V, avg_len=avg_len, pass_off=pass_off,
              pass_game=pass_game, post_off=post_off, post_ids=post_ids,
              post_tf=post_tf, text=text)
    kb["text_start"] = lambda pid: text[pass_off[pid]:pass_off[pid + 1]].decode("utf-8", "replace")
    # C 侧 pass_tlen 存在索引里；PC 侧重算。这里用 token 数（C 语义）
    kb["pass_tlen"] = [0] * n      # 由调用方按词表填
    return kb


def main():
    which = "card"
    if "--vocab" in sys.argv:
        i = sys.argv.index("--vocab")
        which = sys.argv[i + 1]

    v = load_card_vocab() if which == "card" else load_ckpt_vocab()
    kb = load_kb()
    # C 侧索引里存了 pass_tlen？—— build_kb.py 若没存，就用字节长度近似。
    # 这里先用字节长度（只影响 BM25 归一化，不影响有没有命中）。
    kb["pass_tlen"] = [kb["pass_off"][i + 1] - kb["pass_off"][i] for i in range(kb["n"])]

    print("词表来源 = %s   V=%d merges=%d" % (which, v.V, len(v.merges)))
    print("索引：段落 %d  游戏 %d  V=%d  avg_len %.1f"
          % (kb["n"], kb["n_game"], kb["V"], kb["avg_len"]))
    # ── 表一致性自检：g_alias / g_game_names 有没有跟上语料 ──
    # 这正是「id 是位置，不是内容」的总闸。表一旦过期，
    # gkb_split_game 会把游戏名识别成**另一个游戏的 gid**，
    # 症状只是「检索 0 命中 / 答案张冠李戴」，不报错也不崩 ——
    # 所以必须在这里先量一遍，别让它伪装成检索质量问题。
    alias_map, game_names = parse_c_tables()
    files = corpus_files()
    print("C 侧表：g_alias %d 条 / g_game_names %d 条 / 索引 n_game=%d / 语料 %d 款"
          % (len(alias_map), len(game_names), kb["n_game"], len(files)))
    tbl_bad = None
    # ① 语料与 KB 必须同款数，否则 gid 之间根本不可比（见 corpus_gid 的注释）
    if len(files) != kb["n_game"]:
        print("  !! 语料 %d 款 vs 索引 n_game=%d → 两边 game_id 不是同一批坐标"
              % (len(files), kb["n_game"]))
        print("     → 先重跑 `python tools/guia/build_kb.py`（ rebuilt KB 与目录同步），"
              "再验别名/检索；本判据不敢拿错位坐标下结论，记为失败。")
        return 1
    if len(game_names) != kb["n_game"]:
        tbl_bad = ("g_game_names 有 %d 条，索引 n_game=%d → 表没跟着语料重跑"
                   % (len(game_names), kb["n_game"]))
    covered = set(alias_map.values())
    lost = [g for g in range(kb["n_game"]) if g not in covered]
    if lost:
        tbl_bad = ("g_alias 只覆盖 gid 0..%d，索引有 %d 款 → 缺 %d 款"
                   "（如 gid %s）→ 这些游戏**永远检索不到**，"
                   "而限定游戏时会过滤到别的游戏"
                   % (max(alias_map.values()), kb["n_game"], len(lost),
                      lost[:5]))
    if tbl_bad:
        print("  !! %s" % tbl_bad)
        print("     → 产品侧：重跑 `python tools/guia/gen_game_tables.py --write`"
              " 再构建；本判据因此记为失败。")
    print()
    print("── 检索层 ──")

    # ── 关键词 -> 落在哪些 gid（一次全表扫描，只为失败时说人话）──
    # 没有这张表的失败信息长这样：「检索 0 命中」。
    # 它把三种**完全不同的病因**压成了一行：
    #   a) 索引里根本没有这个词（数据缺失）
    #   b) 词有，但被用例指定的 gid 筛没了（**夹具里的 id 与索引不同源**）
    #   c) 词有、gid 也对，只是排名掉出 topK（真·检索质量下降）
    # 而 (b) 恰恰是「id 是位置，不是内容」最容易复发的地方：KB 一重建，
    # 硬编码的 gid 就整片位移，用例却还在用旧坐标，于是"全绿"变"全红"
    # 而让人以为是检索坏了。判据必须自己把话说清楚。
    kw_gids = {}
    for pid in range(kb["n"]):
        t = kb["text_start"](pid)
        for kw, gset in CASES_KW.items():
            if kw in t:
                gset.add(kb["pass_game"][pid])
    for kw, gset in CASES_KW.items():
        kw_gids[kw] = sorted(gset)

    def diag(kw, gid, hits, toks):
        """把失败原因归到 a/b/c…，只报**查过**的那部分。

        ★ 分支顺序是判出来的，不是顺手写出来的：
        「有命中但 top1 不含关键词」既可能是排名问题，也可能是 gid 筛选把
        含该词的那几条全剔了（漂移就是这种）。所以必须**先数命中段里有几条
        含该词**，再拿「筛的 gid 是否含该词」去区分，最后才轮到排名。

        ★ 末分支以前只是句猜测（「BPE 把词切碎」），没查过就下结论 ——
        那是「断言对了 ≠ 对的原因对了」。现在它先去 token 层数 posting：
        有 posting 却没进 topK = 打分/阈值；一个 posting 都没有 =
        查询 piece 与索引 posting 对不上（词表不同源）或查询被切碎。
        """
        gs = kw_gids.get(kw, [])
        gshow = (str(gs[:8]) + "…" if len(gs) > 8 else str(gs))
        hit_kw = [i + 1 for i, (pid, _sc) in enumerate(hits)
                  if kw in kb["text_start"](pid)]
        if hit_kw:
            return ("best_rank=%d（含「%s」的段落还在 topK 里，但没排到 #1 → "
                    "阈值/排序问题）" % (hit_kw[0], kw))
        if not gs:
            return "索引里「%s」一次都没出现 —— 语料缺这块数据" % kw
        if gid >= 0 and gid not in gs:
            return ("gid=%d 下没有「%s」（这个词在索引里落在这 %d 个 gid：%s）→ "
                    "要么该游戏语料没写这个词，要么别名表过期把范围定错"
                    % (gid, kw, len(gs), gshow))
        cand = []
        for tid in toks:
            s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
            m = 0
            for j in range(s, e):
                pid = kb["post_ids"][j]
                if gid < 0 or kb["pass_game"][pid] == gid:
                    m += 1
            if m:
                cand.append("%s×%d" % (v.piece(tid).decode("utf-8", "replace"), m))
        if cand:
            return ("查询 token 在 gid=%d 下是有 posting 的（%s）却没进 topK "
                    "→ 打分/阈值问题" % (gid, ", ".join(cand)))
        names = "/".join(v.piece(t).decode("utf-8", "replace") for t in toks)
        return ("查询 token（%s）在 gid=%d 下一个 posting 都没有 → "
                "**查询 piece 与索引 posting 对不上**（词表不同源）"
                "或查询被停用词切碎" % (names, gid))

    n_ok = n_bad = n_skip = 0
    n_alias = 0
    results = []
    for q, kw, exp_stem in CASES:
        split_gid, rest = gkb_split_game(q, alias_map)
        # exp_gid 是**真实** game_id（语料排序下标），不是手抄坐标。
        # 原来的写法 `gid = exp_gid if exp_gid >= 0 else split_gid` 只让
        # split_gid 在「<0」这一态起作用 —— 别名识别错成别的 gid 却照样跑绿。
        exp_gid = -1 if exp_stem is None else corpus_gid(exp_stem, files)
        if exp_gid < 0 or split_gid < 0:
            gid = -1
        else:
            gid = exp_gid
        if split_gid != exp_gid:
            n_alias += 1
            print("  !! 别名表不同源：%s 被 g_alias 识别成 gid=%d，"
                  "语料排序却是 gid=%d" % (q.split()[0], split_gid, exp_gid))
        hits, toks = gkb_search(kb, v, rest, gid)
        pieces = [v.piece(t).decode("utf-8", "replace") if t is not None else "?" for t in toks]
        if not hits:
            print("[%s] 检索 0 命中  gid=%d rest=[%s]" % (kw, gid, rest))
            print("      编码出的 token(%d)=%s" % (len(toks), pieces))
            print("      诊断：%s" % diag(kw, gid, hits, toks))
            n_skip += 1
            results.append(None)
            continue
        top = kb["text_start"](hits[0][0])
        ok = (kw in top)
        print("[%s] %-6s gid=%-3d top1=#%d %s"
              % (kw, "OK" if ok else "MISS", gid, hits[0][0], top[:34].replace("\n", " ")))
        if not ok:
            print("      诊断：%s" % diag(kw, gid, hits, toks))
        if ok:
            n_ok += 1
        else:
            n_bad += 1
        results.append((hits, rest))

    print()
    print("=> 检索：OK %d  MISS %d  0命中 %d  / 共 %d   （别名不同源 %d 条）"
          % (n_ok, n_bad, n_skip, len(CASES), n_alias))

    # ---- 抽取层：切句 / 覆盖判定 / UTF-8 校验（复刻 extract.c）----
    print()
    print("── 抽取层（复刻 extract.c，含 UTF-8 校验）──")
    e_ok = e_bad = e_skip = 0
    for (q, kw, exp_gid), res in zip(CASES, results):
        if res is None:
            e_skip += 1
            continue
        hits, rest = res
        # GkbHit 三元组：(pid, tok_off, tok_len)
        triples = [(pid, kb["pass_off"][pid], kb["pass_off"][pid + 1] - kb["pass_off"][pid])
                   for pid, _sc in hits]
        out, n_cand, n_pick, ents = gkb_extract(kb, triples, rest, 768)
        if n_cand == 0:
            print("[%s] FAIL 切不出句子" % kw)
            e_bad += 1
            continue
        ok_u8, badpos = utf8_ok(out)
        if not ok_u8:
            ctx = out[max(0, badpos - 4):badpos + 6]
            print("[%s] FAIL 输出不是合法 UTF-8，坏字节在 %d 附近 %r"
                  % (kw, badpos, ctx))
            e_bad += 1
            continue
        txt = out.decode("utf-8")
        has_kw = kw in txt
        if not has_kw:
            print("[%s] FAIL 答案里没有「%s」" % (kw, kw))
            print("      实得（%d 字节 / %d 句 / 实体词%d个）：%s"
                  % (len(out), n_pick, len(ents), txt))
            e_bad += 1
            continue
        print("[%s] OK  %d 字节  候选句%d 取%d 句  %s"
              % (kw, len(out), n_cand, n_pick, txt[:46].replace("\n", " ")))
        e_ok += 1

    print()
    print("=> 抽取：OK %d  FAIL %d  跳过 %d  / 共 %d"
          % (e_ok, e_bad, e_skip, len(CASES)))
    if n_skip == 0 and n_bad == 0 and n_alias == 0 and e_ok == len(CASES) and not tbl_bad:
        print("=> 与 X2 趟的判据等价：8/8，且全部输出是合法 UTF-8")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
