#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
web_chat_server.py — 浏览器里的对话窗口（后端）

═══ 为什么不直接用 local_chat.py ═══════════════════════════════════
local_chat.load_all() 会**加载神经网络模型**（numpy，~1.2MB 权重 + 词表），
但抽取式回答**根本不用模型** —— 它只做三件事：
    分词 -> BM25 检索 -> 从命中段落里选句子
所以这个服务**只加载 BPE + 倒排索引**，启动快一个数量级。

★ 也不引任何第三方 Web 框架：标准库 http.server 就够了。
  理由和「不引 CDN」同源 —— 这个工具的定位是「断网也能用」，
  而 pip 装 flask / 前端引 React 都是「装得上但可能装不上」的东西。

── 启动 ──
    python web_chat_server.py            # http://127.0.0.1:18930
    python web_chat_server.py --port 9000
然后浏览器打开 http://127.0.0.1:18930

⚠️ **本机无 network 访问限制，但服务只绑127.0.0.1**（不给局域网暴露）。
"""
from __future__ import print_function

import argparse
import json
import os
import pickle
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as GS          # noqa: E402
import local_chat as LC# noqa: E402

WEB_HTML = os.path.join(HERE, "web_chat.html")

_state = {}
_lock = threading.Lock()

# ★ 单例缓存。**不要用 `id(kb)` 当键** ——
#   test_kb.load() 每次都重新读文件、返回**全新 dict**，
#   于是 id() 每次都变、缓存永不命中，表现为"每个查询都要 5 秒"。
#   （2026-10-04 实测踩到：每答一句就重建一次 Retriever。）
#   Retriever 的构造要把 1174 个段落全跑一遍 BPE 算 token 长度（BM25 长度归一化用）。
_singleton = {}

#★ 退化问法的判据。
#
# 为什么需要：纯标点的问法（`？？？`、`...`、`，，，`）**看起来有输出，
# 实际完全答错**。实测 `？？？`：
#     全角问号经norm_text 归一化成 `???` -> BPE 切出有意义的 token
#     -> 命中段落 861/864（宝可梦进化石）-> 实体词 `???` 确实在句子里
#     -> cov = 1（"覆盖命中"）
# 而那个"实体词"根本不是用户想问的东西 —— **cov=1 在这里是假的**。
#
# 所以：实体词里若**没有**任何一个含「字母或汉字」，就判定为"没听懂"，
# 直接回问而不是硬答。判据不能只看"有没有命中"，
# 要看"命中的是不是用户真的在问的东西"。
_PUNCT_ONLY = re.compile(
    r"^[\s\?\.!,;:~…·、，。！？；：\"'()（）\-—_=+*/\\@#$%^&]+$")


def _degenerate(q):
    """问法里没有任何有效内容（纯标点/空白/单个无意义字母）时返回 True。

    ⚠️ **不要**把「剔除游戏名后剩余为空」算成退化 ——
    「火焰纹章觉醒」这样的问法很常见，答案是游戏概述，
    而它剔除游戏名后确实什么都不剩。那种情况该**回退用游戏名检索**，
    不是回问用户。这个区别在 2026-10-04 实测踩到过：
    第一版把两者混在一起，正常用例被误判成"没听懂"。
    """
    s = (q or "").strip()
    if not s:
        return True
    if _PUNCT_ONLY.match(s):
        return True
    # 单个 ASCII 字母/数字（如 "a"、"3"）—— BPE 会切出高频 token，
    # 结果是命中一个跟问法无关的段落。
    # 判据要简单到一眼能验证：去掉空白后全是 ASCII 字母数字，且长度 <= 2。
    t = s.replace(" ", "")
    if len(t) <= 2 and all(c.isascii() and c.isalnum() for c in t):
        return True
    return False


def get_retriever():
    """取唯一的 Retriever（构造一次，约 5 秒），之后每次查询都是毫秒级。"""
    if "R" in _singleton:
        return _singleton["R"]
    t0 = time.time()
    kb = LC.TK.load()
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    from train_bpe import BPE
    bpe = BPE(vocab_size=len(d["inv"]))
    # ★ bpe.pkl 里的 inv 可能是 dict（内部表示），必须归一成按 id 排的 list。
    #   不这么做的话 R.text() 里的 bpe.encode 会拿 bytes 去查 int 键 -> KeyError。
    inv = d["inv"]
    if isinstance(inv, dict):
        inv = [inv[i] for i in range(len(inv))]
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], inv, d["merges"]
    bpe._train_ranks()

    R = GS.Retriever(kb, bpe, GS.GAME_ORDER)
    _singleton["R"] = R
    _singleton["kb"] = kb
    sys.stderr.write("[init] Retriever 就绪：%d 段落 / 平均 %.1f token，耗时 %.2fs\n"
                     % (kb["n"], R.avg_len, time.time() - t0))
    sys.stderr.flush()
    return R


def answer(query, force_game=None):
    """核心：抽取式回答。

    返回 dict：answer / game / n_cand / cov / hits（来源段落 id 与原文）。

    ⚠️ 诚实边界（前端要显示，别让人以为它在自由发挥）：
    · 答案句子是**语料原文逐字拷贝**，不是模型改写
    · 若一个候选句都没命中，会退回"取检索原序前几句"——
      那不是"答对了"，只是"没答错"
    """
    q = (query or "").strip()
    if not q:
        return {"ok": False, "error": "问题不能为空"}

    # 退化问法（纯标点、单个字母）直接回问。
    # ★ 不放在检索之后判断：那些问法会"命中"一些段落并给出看起来正常的答案，
    #   而那个答案是**错的** —— 与其给一个错答案，不如说"没听懂"。
    if _degenerate(q):
        return {
            "ok": True,
            "answer": "我没听懂这个问题。",
            "note": "我只会从 31 款游戏的攻略语料里查东西，"
                    "所以问题里得有个具体的词 —— 道具名、动作名、"
                    "地点或游戏名。比如「沙之杖怎么用」「火焰纹章觉醒 转职怎么转」。",
            "game": "", "n_cand": 0, "cov": 0.0, "hits": [], "degenerate": True,
        }

    R = get_retriever()

    # 游戏限定：前端下拉优先（它比问句里的别名更可信）
    if force_game and force_game in GS.GAME_ORDER:
        gid = GS.GAME_ORDER.index(force_game)
        # 仍然剔除问句里的游戏名，避免同一词被当实体词重复计分
        _auto, rest = LC.split_game(q)
    else:
        gid, rest = LC.split_game(q)

    if not rest.strip():
        # 整句都是游戏名 -> 用游戏名当查询
        rest = q

    hits = R.search(rest, gid, k=4)
    if not hits:
        return {
            "ok": True, "answer": "这个问题在本地语料里没有对应内容。",
            "note": "检索 0 命中 —— 语料只覆盖 31 款游戏的攻略要点。",
            "game": (force_game or ""), "n_cand": 0, "cov": 0.0, "hits": [],
        }

    cands = []
    for pid, _sc in hits:
        for s in LC.sentences(R.text(pid)):
            if s not in cands:
                cands.append(s)

    game_name = (force_game or
                 (GS.GAME_ORDER[gid] if 0 <= gid < len(GS.GAME_ORDER) else ""))

    if not cands:
        return {
            "ok": True, "answer": "命中了段落，但里面没有可抽取的完整句子。",
            "note": "段落太短或都是元信息（如「主要道具与用途。」这类标题句）。",
            "game": game_name, "n_cand": 0, "cov": 0.0,
            "hits": [{"pid": pid, "text": R.text(pid)[:300]} for pid, _ in hits],
        }

    ents = LC.entities_of(rest)
    scored = []
    for s in cands:
        cov = 1.0 if any(e in s for e in ents) else 0.0
        scored.append((cov, s))
    scored.sort(key=lambda kv: -kv[0])
    best_cov = scored[0][0]
    top = [s for c, s in scored if c >= best_cov][:3]

    note = None
    if best_cov == 0.0:
        note = ("没有任何候选句含你问的实体词 —— 已退回「取检索原序前几句」。"
                "这不是「答对了」，只是「没答错」。换个更具体的问法会准得多"
                "（带上道具名、动作名或游戏名）。")

    # ★★ 别把 best_cov 直接当成「这次答对了」对外显示（2026-10-04 实测打掉）。
    #
    # cov=1 只说明「某个实体词窗口出现在候选句里」，而**实体词可能
    # 是通用词**。实测两个反例：
    #     「适合新手吗」   cov=1 -> 实体词「新手」确实在句里，但答的是御三家
    #     「这游戏卖多少钱」cov=1 -> 实体词「多少钱」在句里，但答的是带道具
    # 而问「职业怎么转」（真正想问的是「转职」，但问句里没这两个字）时
    # cov=1 却**没提到转职** —— 也就是说 cov=1 既可能对、也可能错，
    # **它与「答对了」没有稳定对应关系**。
    #
    # → 前端不要展示 cov 当"准确度"。诚实的做法是只说
    #   「这句有没有含你问的词」，并且在没命中时明说「没命中」。
    matched_any = best_cov > 0
    return {
        "ok": True,
        "answer": "".join(top),
        "note": note,
        "game": game_name,
        "n_cand": len(cands),
        "cov": best_cov,
        "matched": matched_any,
        "n_pick": len(top),
        "hits": [{"pid": pid, "text": R.text(pid)[:300]} for pid, _ in hits[:4]],
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "AIChatLocal/1.0"

    def log_message(self, fmt, *args):
        # 静默：默认实现会往 stderr 打每条请求，干扰启动信息
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError):
            # 浏览器刷新/关闭时会正常发生，不是错误
            pass

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/") or "/"
        if path == "/":
            if not os.path.exists(WEB_HTML):
                self._send(404, "缺少 %s" % os.path.basename(WEB_HTML),
                           "text/plain; charset=utf-8")
                return
            html = open(WEB_HTML, encoding="utf-8").read()
            self._send(200, html, "text/html; charset=utf-8")
            return
        if path == "/api/games":
            with _lock:
                gs = [{"id": g, "aliases": GS.GAME_ALIAS.get(g, [])}
                      for g in GS.GAME_ORDER]
            self._send(200, json.dumps({"games": gs}, ensure_ascii=False))
            return
        if path == "/api/health":
            # 复用已建好的单例，不要为了一句 health 又去读一遍 600KB 索引
            n = _singleton.get("kb", {}).get("n", 0)
            if not n:
                try:
                    get_retriever()
                    n = _singleton.get("kb", {}).get("n", 0)
                except Exception:
                    pass
            self._send(200, json.dumps(
                {"ok": True, "passages": n, "games": len(GS.GAME_ORDER)},
                ensure_ascii=False))
            return
        self._send(404, json.dumps({"error": "没有这个路径：%s" % path},
                                   ensure_ascii=False))

    def do_POST(self):
        if self.path.split("?")[0].rstrip("/") != "/api/chat":
            self._send(404, json.dumps({"error": "没有这个接口"},
                                       ensure_ascii=False))
            return
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 64 * 1024:
            self._send(400, json.dumps({"error": "请求体大小不对"}, ensure_ascii=False))
            return
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception as e:
            self._send(400, json.dumps({"error": "JSON 解析失败：%s" % e},
                                       ensure_ascii=False))
            return
        if not isinstance(data, dict):
            self._send(400, json.dumps({"error": "请求体必须是对象"},
                                       ensure_ascii=False))
            return

        q = data.get("q")
        if not isinstance(q, str):
            self._send(400, json.dumps({"error": "缺字符串字段 q"}, ensure_ascii=False))
            return
        fg = data.get("game")
        fg = fg if isinstance(fg, str) and fg else None

        t0 = time.time()
        try:
            with _lock:
                # 检索是纯 CPU 的（BM25 over 1174 段），串行化避免多请求打满
                r = answer(q, fg)
        except Exception as e:
            self._send(500, json.dumps(
                {"ok": False, "error": "服务端出错：%s: %s"
                                    % (type(e).__name__, e)},
                ensure_ascii=False))
            return
        r["ms"] = int((time.time() - t0) * 1000)
        self._send(200, json.dumps(r, ensure_ascii=False))


def main():
    ap = argparse.ArgumentParser(description="本地攻略 AI 的浏览器对话窗口")
    ap.add_argument("--port", type=int, default=18930)
    ap.add_argument("--host", default="127.0.0.1",
                    help="默认只绑本机。改成 0.0.0.0 会暴露到局域网，别乱改。")
    args = ap.parse_args()

    print("加载索引 ...")
    get_retriever()
    kb_n = _singleton.get("kb", {}).get("n", 0)
    print("索引 %d 段落 / %d 款游戏" % (kb_n, len(GS.GAME_ORDER)))
    print()
    print("  对话窗口： http://%s:%d" % (args.host, args.port))
    print("  停止： Ctrl+C")
    print()
    print("  ★ 答案来自本地语料，逐字拷贝；不联网、不消耗任何积分。")
    print()

    if not os.path.exists(WEB_HTML):
        print("!! 缺少前端文件 %s" % WEB_HTML)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
        srv.shutdown()


if __name__ == "__main__":
    main()
