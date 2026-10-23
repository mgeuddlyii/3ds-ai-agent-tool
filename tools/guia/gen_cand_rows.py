#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_cand_rows.py — 给每个游戏生成「底屏候选词」（C 端 g_cand_set 的每一行）

为什么要有这个文件
------------------
C 端 `g_cand_set[]` 必须一行一个游戏，行数 == GKB_GAME_N，
而且**编译期断言只卡行数、不卡内容**（见 sc_guide.c 那句
`cand_rows_must_equal_game_count`）。历史上真实出过一次：
表只有 19 行，语料扩到 31 款后 gcc 一个字不报，新 12 款在底屏全空白 ——
运行时越界读到的是合法的下一个数组元素，跑起来不崩、只是那个游戏下面没词。

所以这张表不能手抄（手抄必然和语料脱节一次），要从**语料本身**算出来。

同源要求（本项目铁律）
----------------------
- 段落切分直接复用 build_kb.py 的规则（SKIP_SECTIONS / strip_qa / split_long），
  不能自己再写一遍 —— 切法不同，候选词就不在索引里，验证会假绿。
- 命中验证直接复用 guide_search.Retriever（就是卡上那一套 BM25 + 游戏过滤），
  不能自己写个近似打分器冒充「能命中」。

用法：
    python tools/guia/gen_cand_rows.py            # 全量生成 + 逐条验证（不落盘）
    python tools/guia/gen_cand_rows.py --rows     # 只要 C 侧行文本（贴进源码用）
    python tools/guia/gen_cand_rows.py --check 89 # 只验证 gid>=89 的新增款
    python tools/guia/gen_cand_rows.py --write    # 落盘进 sc_guide.c（先备份）

⚠️ --write 是 2026-10-05 才真正实现的。在这之前 docstring 与末尾提示
   都写着「请加 --write」，而 main() 里没有这个分支 —— 传进去会被无视、
   照常跑完、返回 0。**静默失效**，和 gen_game_tables.py 那个坑同族。
   落盘有前置核验（每行必须正好 6 个词、gid 必须连续覆盖 0..N-1）与
   写回自检（重新 parse 回来核对行数与词数，不过就回滚）。
"""

import os
import sys
import pickle
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# 当前这一轮 build_passages() 的结果 [(text, game_id)]，pid 就是下标。
# 放在模块级是因为 pick_words() 要按 pid 去查倒排表。
KB_PASSAGES = []

import build_kb as KB                       # noqa: E402  段落规则同源
from guide_search import Retriever, STOP_EXACT, STOP_SUBSTR   # 打分同源  # noqa: E402


def build_passages():
    """与 build_kb.main() 完全一致的段落切分，返回 [(text, game_id)]。"""
    corpus = os.path.join(HERE, "corpus")
    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))
    passages, game_id = [], 0
    for fn in files:
        cur_title, buf, blocks = None, [], []

        def flush():
            if cur_title is None:
                return
            body = "\n".join(buf).strip()
            if body:
                blocks.append((cur_title, body))

        for line in open(os.path.join(corpus, fn), encoding="utf-8"):
            line = line.rstrip("\n")
            if line.startswith("## "):
                flush()
                cur_title, buf = line[3:].strip(), []
            else:
                buf.append(line)
        flush()

        for title, body in blocks:
            if any(s in title for s in KB.SKIP_SECTIONS):
                continue
            body = KB.strip_qa(body)
            if len(body) < 20:
                continue
            for piece in KB.split_long(title + "。" + body):
                passages.append((piece, game_id))
        game_id += 1
    return passages


BAD_CHARS = "。！？；，、：,.!?;:()（）《》「」『』【】\n\t '\""
# 开头的虚词片段：BPE 会把它们和后面的实词粘成「就是」「或者」
# 「的是」「上的」这种**跨句碎片**，看着像词，实际是句子接缝。
BAD_HEAD = ("的", "了", "是", "就", "或", "而", "这", "那", "会", "要",
            "可", "在", "和", "与", "也", "都", "很", "上", "下", "里",
            "有", "个", "不", "没", "被", "把", "让", "给", "到", "过")


def WORD_OK(piece):
    """BPE 出来的"词"里有一大类是碎屑，不能当候选词按钮。

    三类必须排掉：
      1. 带标点的（`.` `所以` `属性。`）—— 标点会被单独切成 token，
         按下去检索到的是噪声。
      2. 短拉丁碎片（`Le` `oo` `ec` `ed` `PC`）—— BPE 把英文单词切开了，
         这些片段在全语料里到处都是，没有检索价值。
      3. 以虚词开头的（`就是` `或者` `的是`）—— 它们是句子接缝，
         不是实体。
    """
    if not piece or len(piece) < 2:
        return False
    if "�" in piece:                  # BPE 丢字符后的解码残片，别当词
        return False
    if any(c in BAD_CHARS for c in piece):
        return False
    if piece[0] in BAD_HEAD:
        return False
    if all(ord(c) < 128 for c in piece) and len(piece) <= 3:
        return False
    return True


MIN_HITS = 2        # 一个候选词至少要在本游戏里打出这么多条命中
MIN_DF = 3          # 本游戏内至少出现在这么多个段落里
MAX_DF = 60         # 本游戏内最多出现在这么多段落里（太多说明是通用词）
MAX_GDF = 160       # 全语料 df 上限：超过就更容易撞 MAX_POSTING 截断

# ⚠️ 2026-10-05 修：候选池原来也只取 CAND_N=6 个，而 verify 是在**这 6 个之外**
# 做的 —— 任何一个词 verify 挂掉，keep 就少一个，且**没有第 7 个词可换**，
# 于是「语料扩到 211 款」后冒出 18 款「只有 1~5 个」、--write 直接被拒。
# 症状很像"这游戏语料薄"，其实是**取候选的池子太窄**。
# 所以池子放大到 CAND_N 的三倍，verify 过的收进 keep、没过的跳过换下一个；
# 真正能红的守卫生死线变成「keep != CAND_N」（见 main 的 fails）。
CAND_N = 6                    # 必须与 sc_guide.c 的 `#define CAND_N 6` 一致
CAND_POOL = CAND_N * 3        # 候选池大小（verify 挂了能换，不是最终行数）


def pick_words(gid, bpe, kb, k=6, min_hits=None, max_df=None, min_df=None):
    global MIN_HITS, MAX_DF, MIN_DF
    old = (MIN_HITS, MAX_DF, MIN_DF)
    MIN_HITS = min_hits or MIN_HITS
    MAX_DF = max_df or MAX_DF
    MIN_DF = min_df or MIN_DF
    """从单款游戏的段落里挑 k 个「确实能打出命中」的候选词。

    ⚠️ 这里踩过一个坑，别再退回去（2026-10-05）：
    最初的版本按**游戏内 df 倒序**取前 k 个高频词，结果大量词
    验证时 hits=0。原因是倒排有全局上限 MAX_POSTING=64（每个词
    最多只留 tf 最高的 64 条），而高 df 词在全语料里出现上百次，
    它的 posting 列表里**根本没有本游戏的段落** —— 于是按 df 挑出来的
    词看着"这游戏明明有很多处"，真去检索却是 0 命中。
    症状是候选按钮按下去底屏空白，不报错。

    所以现在不看 df 猜，直接数：
      hits(token) = 该词的 posting 里、属于本游戏的段落数
    只保留 hits >= MIN_HITS 的词，再按 (hits 多、全语料 df 小) 排序。
    hits 多 = 玩家按下去有内容；全语料 df 小 = 区分度高、且不容易被截断。
    """
    V = kb["V"]
    # build_passages() 返回 [(text, game_id)]，pid 就是列表下标（与
    # build_kb 的枚举顺序一致，所以能直接拿下标当 pid 去查倒排）
    game_pids = set(i for i, (_, g) in enumerate(KB_PASSAGES) if g == gid)

    tok_pids = collections.defaultdict(set)
    for pid, (_t, g) in enumerate(KB_PASSAGES):
        if g != gid:
            continue
        for tid in bpe.encode(_t):
            if tid >= V - 3:                 # BOS/EOS/SEP/噪声，与 build_kb 一致
                continue
            tok_pids[tid].add(pid)

    scored = []
    for tid, pids in tok_pids.items():
        if len(pids) < MIN_DF or len(pids) > MAX_DF:
            continue
        piece = bpe.inv[tid].decode("utf-8", "replace")
        if not WORD_OK(piece):
            continue
        # ★ 关键那一步：在这个词**截断后剩下的 posting** 里，
        # 到底还有几条属于本游戏。
        # 早先漏的就是这一步 —— 只数了「本游戏里出现过这个词」的段落，
        # 而那恒等于 df，跟倒排有没有存下它毫无关系。
        po = kb["post_off"]
        s, e = po[tid], po[tid + 1]
        if s >= len(kb["post_ids"]):
            continue
        e = min(e, len(kb["post_ids"]))
        hits = sum(1 for p in kb["post_ids"][s:e] if p in game_pids)
        if hits < MIN_HITS:
            continue
        gdf = e - s
        if gdf > MAX_GDF:
            continue
        scored.append((-hits, gdf, piece))
    scored.sort()
    out = [p for _, _, p in scored[:k]]
    MIN_HITS, MAX_DF, MIN_DF = old
    return out


# 放宽阶梯：有些游戏（尤其是语料偏薄的）在严格档凑不满 6 个，
# 逐级松一点再挑。注意**最后仍然要逐条 verify**，放宽只是"多给候选"，
# 不是放水 —— verify 才是那个能红的守卫。
#
# ⚠️ 2026-10-05 修：这一行原来是 `((2, 60), (1, 80), (1, 120), (1, 200))`,
#   只放宽了 (min_hits, max_df) 两个数，**MIN_DF 恒等于 3**。
#   于是「本游戏段落数 < 6」的游戏（211 款语料里有 18 款）**一条候选都挑不出来**
#   —— 一条词要"出现在本游戏 ≥3 个段落里"，这游戏总共才 5 个段落。
#   症状是 `--write` 报「只有 1~5 个」，看着像"语料薄"，
#   其实是**筛选条件本身把薄语料锁死了**（和"断言自己筛错数据源"同族）。
#   现在把 min_df 一并纳入阶梯，从 3 → 2 → 1 逐级松。
RELAX_LADDER = ((2, 60, 3), (1, 60, 2), (1, 80, 2), (1, 120, 1), (1, 200, 1))


def pick_with_ladder(gid, bpe, kb, k=CAND_POOL):
    # ⚠️ 这里**不要**再传 texts_by_game[gid] 进来：那个值是 list，
    # 再按 [gid] 取一次会在「段落数 < gid 的游戏」上直接 IndexError
    # （比如某款只有 13 个段落，gid=17 就炸）。pick_words 现在只认
    # KB_PASSAGES，压根不需要那个参数。
    for mh, mdf, mdfd in RELAX_LADDER:
        ws = pick_words(gid, bpe, kb, k=k, min_hits=mh, max_df=mdf,
                        min_df=mdfd)
        if len(ws) >= k:
            return ws, (mh, mdf, mdfd)
    mh, mdf, mdfd = RELAX_LADDER[-1]
    return pick_words(gid, bpe, kb, k=k, min_hits=mh, max_df=mdf,
                      min_df=mdfd), (mh, mdf, mdfd)


def verify(R, word, gid):
    """这个词以该游戏为范围去搜，能不能真打出命中（且命中就该是本游戏的）。

    ⚠️ search() 返回的是 `[(pid, score), ...]` 元组，**不是** dict，
    里面没有 game 字段 —— 所以「命中是不是本游戏的」只能拿 pid 去
    `kb["pass_game"][pid]` 查。别写成 `h.get("game")`，那会直接炸；
    而如果哪天 search() 开始返回 dict，这个也得跟着改（否则 own 恒为 0，
    所有词都被判 FAIL，等于把守卫变成"永远红的废话"）。
    """
    hits = R.search(word, gid=gid, k=4)
    if not hits:
        return 0, 0
    own = sum(1 for pid, _ in hits if R.kb["pass_game"][pid] == gid)
    return len(hits), own


# ══════════════════════════════════════════════════════════════════════
# 落盘：把 rows 写回 app/source/ui/sc_guide.c 的 g_cand_set[]
#
# ⚠️ 2026-10-05 之前这个文件**根本没有 --write**：docstring 第 26 行
#    写了「要写进 g_cand_set 请加 --write」，main() 里却只解析了 --rows。
#    传 --write 进去会被无视，脚本照常跑完、打印一句「请加 --write」、
#    返回 0 —— 看着完全成功，一个字节都不写。
#    这和 gen_game_tables.py 历史上那个坑是同一个病：**一个写着能干活的
#    入口，比没有这个入口更坑人**。升级流程里有它就等于第②步静默失效。
# ══════════════════════════════════════════════════════════════════════

SC_GUIDE = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                        "app", "source", "ui", "sc_guide.c")


def _disp_width(s):
    """按终端显示宽度算长度（汉字算 2 列），用来对齐注释。"""
    import unicodedata
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


def render_rows(rows, cand_n=CAND_N):
    """生成 g_cand_set[] 的正文（不含花括号）。"""
    stem_w = max(_disp_width(stem) for _, stem, _ in rows) if rows else 1
    lines = []
    for gid, stem, ws in rows:
        cells = ", ".join('"%s"' % w for w in ws)
        body = '    { { %s } },' % cells
        pad = max(1, 78 - _disp_width(body))
        lines.append("%s%s/* %3d %s */"
                     % (body, " " * pad, gid, stem))
    return "\n".join(lines) + "\n"


def apply_write(rows, cand_n=CAND_N):
    """写回 sc_guide.c。返回 (路径, 备份名)。任何自检不过就 raise。"""
    import re
    import shutil
    import time

    # ---- 前置①：每行必须**正好** cand_n 个词 ----
    # `const char *words[6]` 少给了就是 NULL 尾巴，底屏那几个格子空白 ——
    # 正是历史上「选了那个游戏，下面全空白」的样子。宁可不写。
    short = [(g, s, len(w)) for g, s, w in rows if len(w) != cand_n]
    if short:
        raise RuntimeError(
            "有 %d 行凑不满 %d 个候选词，拒绝落盘（短行会让底屏空白）：\n%s"
            % (len(short), cand_n,
               "\n".join("   gid=%-3d %-26s 只有 %d 个" % t for t in short[:12])))

    # ---- 前置②：gid 必须连续覆盖 0..N-1 ----
    gids = [g for g, _, _ in rows]
    if gids != list(range(len(rows))):
        miss = sorted(set(range(len(rows))) - set(gids))
        raise RuntimeError("gid 不连续，缺 %s —— 拒绝落盘" % miss[:10])

    p = SC_GUIDE
    if not os.path.exists(p):
        raise RuntimeError("找不到 %s" % p)
    src = open(p, encoding="utf-8").read()

    pat = re.compile(
        r"(static\s+const\s+CandSet\s+g_cand_set\[\]\s*=\s*\{)(.*?)(\};)", re.S)
    if len(pat.findall(src)) != 1:
        raise RuntimeError("sc_guide.c 里 g_cand_set[] 的 `{ ... };` 不是唯一一处，"
                           "不敢自动替换（找到 %d 处）" % len(pat.findall(src)))

    inner = render_rows(rows, cand_n)
    new, n = pat.subn(lambda m: m.group(1) + "\n" + inner + m.group(3), src, count=1)
    if n != 1:
        raise RuntimeError("替换失败")

    stamp = time.strftime("%Y%m%d_%H%M%S")
    bak = p + ".bak-" + stamp
    shutil.copyfile(p, bak)
    open(p, "w", encoding="utf-8").write(new)

    # ---- 写回自检：重新 parse 一遍，必须能读回同样多的行、每行 6 个 ----
    back = open(p, encoding="utf-8").read()
    m = pat.search(back)
    if not m:
        raise RuntimeError("写回后 parse 不出 g_cand_set[] —— 文件可能已损坏，"
                           "备份在 %s" % bak)
    got = re.findall(r"\{\s*\{([^}]*)\}\s*\}", m.group(2))
    errs = []
    if len(got) != len(rows):
        errs.append("写回后 %d 行（应 %d）" % (len(got), len(rows)))
    bad = [i for i, g in enumerate(got)
           if len(re.findall(r'"[^"]*"', g)) != cand_n]
    if bad:
        errs.append("写回后这些行不是 %d 个词: %s" % (cand_n, bad[:10]))
    if errs:
        shutil.copyfile(bak, p)
        raise RuntimeError("写回自检没过（已回滚）：%s" % "; ".join(errs))
    return p, os.path.basename(bak)



def main():
    global KB_PASSAGES
    argv = sys.argv[1:]
    only_rows = "--rows" in argv
    do_write = "--write" in argv
    only_check = None
    for i, a in enumerate(argv):
        if a == "--check":
            only_check = int(argv[i + 1]) if i + 1 < len(argv) else 0
    if do_write and only_check is not None:
        print("!! --write 与 --check 不能同时用 —— --check 只看一部分，")
        print("   落盘必须用全量结果（否则 g_cand_set 会缺行，编译期断言会拦）")
        return 2

    passages = build_passages()
    KB_PASSAGES = passages
    texts_by_game = collections.defaultdict(list)
    for t, g in passages:
        texts_by_game[g].append(t)

    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = KB.load_bpe()

    import test_kb as T
    kb = T.load()
    R = Retriever(kb, bpe)

    stems = sorted(os.path.basename(x)[len("guide_"):-len(".txt")]
                   for x in os.listdir(os.path.join(HERE, "corpus"))
                   if x.startswith("guide_") and x.endswith(".txt"))

    from guide_search import GAME_ORDER
    if len(GAME_ORDER) != len(stems):
        print("对照表!! guide_search.GAME_ORDER = %d 款，语料 = %d 款" %
              (len(GAME_ORDER), len(stems)))
        return 1

    rows, fails, drops = [], [], []
    for gid, stem in enumerate(stems):
        if only_check is not None and gid < only_check:
            continue
        ws, relax = pick_with_ladder(gid, bpe, kb)
        keep = []
        for w in ws:
            if len(keep) >= CAND_N:
                break
            n, own = verify(R, w, gid)
            if n == 0:
                drops.append((gid, stem, w, 0, 0))
                continue
            if own == 0:
                drops.append((gid, stem, w, n, own))
                continue
            keep.append(w)
        # ★ 生死的判据只有一条：凑满 CAND_N 个"确实能命中本游戏"的词。
        #   池子里那些没验过的（drops）**不阻止**落盘 —— 换个词就是了；
        #   但凑不满就是真红（底屏会点出空结果）。
        if len(keep) != CAND_N:
            fails.append((gid, stem, "(只挑到 %d 个能命中的词)" % len(keep), 0, 0))
            continue
        rows.append((gid, stem, keep))

    if drops:
        print("  （%d 个候选词没验过，已跳过换下一个，不阻止落盘）:" % len(drops))
        for gid, stem, w, n, own in drops[:12]:
            print("   gid=%-3d %-24s %-10s hits=%s own=%s" % (gid, stem, w, n, own))
        if len(drops) > 12:
            print("   ... 另有 %d 条" % (len(drops) - 12))
    if fails:
        print("!! %d 款凑不满 %d 个候选词（这是真 FAIL，这些行不能落盘）:" % (len(fails), CAND_N))
        for gid, stem, w, n, own in fails[:12]:
            print("   gid=%-3d %-24s %-10s hits=%s own=%s" % (gid, stem, w, n, own))
        if len(fails) > 12:
            print("   ... 另有 %d 款" % (len(fails) - 12))
        return 1

    if only_rows:
        for gid, stem, ws in rows:
            cells = ", ".join('"%s"' % w for w in ws)
            print('    { { %s } },                        /* %d %s */'
                  % (cells, gid, stem))
        return 0

    if do_write:
        # ★ 落盘前先把「这张表要替换掉什么」说清楚。g_cand_set 的**行数**
        #   有编译期断言卡住（sc_guide.c: cand_rows_must_equal_game_count），
        #   但断言只卡行数、不卡内容 —— 所以这里必须自己再核一遍内容。
        print("语料 %d 款 / 段落 %d" % (len(stems), len(passages)))
        print("-" * 72)
        try:
            p, bak = apply_write(rows)
        except RuntimeError as e:
            print("!! 落盘失败：%s" % e)
            return 1
        print("已写入 %s" % p)
        print("  备份 %s" % bak)
        print("  %d 行 × %d 词，gid 覆盖 0..%d" % (len(rows), CAND_N, len(rows) - 1))
        print()
        print("⚠️ 下一步：确认 guide_kb.h 的 GKB_GAME_N 也等于 %d（行数有编译期断言），"
              % len(rows))
        print("   然后 `python tools/build.py` 重建；只改这张表不重建 = 卡上没变。")
        return 0

    print("语料 %d 款 / 段落 %d" % (len(stems), len(passages)))
    print("-" * 72)
    print("%-4s %-26s %s" % ("gid", "stem", "候选词（已逐个验证命中）"))
    for gid, stem, ws in rows:
        print("%-4d %-26s %s" % (gid, stem, " ".join(ws)))
    print("-" * 72)
    # bad 已在上面统一判过了，这里只报结果（FAIL 明细只有那一处，
    # 两份打印会让人以为可以只信其中一个）。
    print("全部 %d 款候选词均验证命中本游戏 -> 要写进 g_cand_set 请加 --write"
          % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
