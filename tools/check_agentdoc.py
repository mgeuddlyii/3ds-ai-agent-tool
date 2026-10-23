#!/usr/bin/env python3
"""
check_agentdoc.py —— AGENT.md 与设备端代码的对账脚本

── 这份脚本在防什么 ──
AGENT.md 是喂给云端模型的 system 消息，它每一句话都会被模型当真
（"装到 sdmc:/3ds/"、"内容 512 字节"、"动作名只有 20 个"、
 "动作结果下一轮才回灌给你"）。
而 AGENT.md 是**人手写**的，代码是**机器**的，两边必然漂移：
改了上限忘了改手册、加了动作忘了教模型、动了目录忘了同步。
漂移的后果不是"文档落后"，是**模型照着过期的规矩去说**，
然后真人按它说的做了，装错地方或以为装上了。

── 铁律 ──
所有基准值都**从被测 C 源码里抠**，不在这里硬编码一份"正确的数"：
抠不到就 FAIL，绝不写兜底值。
一个"永远绿"的对账脚本比没有更坏 —— 它只会让人以为文档是对的。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

AGENT_MD   = os.path.join(ROOT, "AGENT.md")
AGENT_EN_MD = os.path.join(ROOT, "AGENT.en.md")
ACTIONS_C  = os.path.join(ROOT, "shared", "actions.c")
ACTIONS_H  = os.path.join(ROOT, "shared", "actions.h")
PKG_H      = os.path.join(ROOT, "shared", "pkg.h")
PKG_C      = os.path.join(ROOT, "shared", "pkg.c")
LOCALE_H   = os.path.join(ROOT, "shared", "locale.h")
AGENTDOC_H = os.path.join(ROOT, "shared", "agentdoc.h")

ok_cnt  = 0
bad_cnt = 0


def get_text(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def sniff(pattern, text, what, path):
    """从源码抠一个值。抠不到 = FAIL（绝不给兜底值）。"""
    m = re.search(pattern, text)
    if not m:
        fail("%s：源码里找不到 %s（%s）→ 对账基准缺失，判 FAIL" % (what, pattern, path))
        return None
    return m.group(1)


def ok(what):
    global ok_cnt
    ok_cnt += 1
    print("  PASS  %s" % what)


def fail(what):
    global bad_cnt
    bad_cnt += 1
    print("  FAIL  %s" % what)


def expect_in_file(marker, path, what, pattern=None):
    """文档/代码里必须出现某个字面量。"""
    t = get_text(path)
    if pattern:
        hit = re.search(pattern, t)
    else:
        hit = marker in t
    if hit:
        ok(what)
    else:
        fail("%s —— %s 里找不到 %r" % (what, path, marker))


def main():
    print("== AGENT.md 对账 ==")

    if not os.path.exists(AGENT_MD):
        print("\n  FAIL  AGENT.md 不存在 → 模型这一轮没有手册（程序会静默跳过拼接）")
        return 2
    agent = get_text(AGENT_MD)
    act_c = get_text(ACTIONS_C)
    act_h = get_text(ACTIONS_H)
    pkg_h = get_text(PKG_H)
    pkg_c = get_text(PKG_C)
    loc_h = get_text(LOCALE_H)

    # ── 1. 二十个动作名必须与 hdr_match 白名单逐字一致 ──
    # ⚠️ 这份列表是**手写**的，所以它同时扮演两个角色：
    #   · 正向：模型必须被教会这每一个（文档里出现 ≥2 次）
    #   · 反向：actions.c 里多出列表外的动作 → 红（见下面 code_acts 那段）
    # 也就是说：**加动作时要连这里一起改**，否则要么红"文档没教"、
    # 要么将来删了动作这里忘了删而失去检查能力。两种红都比悄悄漏掉好。
    print("\n[1] 动作白名单（AGENT.md 与 actions.c 的 hdr_match 必须一致）")
    ACTIONS = ["file.write", "pkg.install", "locale.set", "locale.clear",
               "ui.theme", "ui.fontsize", "ui.depth", "sec.level",
               "cfg.set", "sys.info", "file.list", "file.read",
               "font.install", "model.install",
               "file.delete", "file.rename", "key.remove", "key.switch",
               "trash.purge", "svc.set"]
    for a in ACTIONS:
        # 判据是"至少出现 2 次"（一次在合法动作名列表、一次在链路/决策树），
        # 不是"全文出现过就算"。只看一次 = 判据会被文档里别处的同名字符串喂饱，
        # 于是删掉某一节也照样绿 —— 那正是我们要抓的情况。
        in_doc  = agent.count(a) >= 2
        in_code = re.search(r'hdr_match\(hdr, "%s"' % re.escape(a), act_c) is not None
        # 正向：代码里有、文档里有（否则模型不知道这个动作）
        if in_code and in_doc:
            ok("动作 %s 在代码与文档中都在" % a)
        else:
            fail("动作 %s —— 代码里 hdr_match=%s, 文档里提到=%s（必须两者都为真）"
                 % (a, in_code, in_doc))

    # 反例判据：文档不许出现"第五个"动作名，也不许代码里有而文档漏了
    code_acts = set(re.findall(r'hdr_match\(hdr, "([^"]+)"\)', act_c))
    missing   = code_acts - set(ACTIONS)
    if not missing:
        ok("actions.c 里没有文档未记载的额外动作（共 %d 条）" % len(code_acts))
    else:
        fail("actions.c 里有文档没教模型的动作：%s" % sorted(missing))

    # ── 1b. 反例判据：文档不许出现"第五个"动作名（编造的动作名） ──
    # 判据要在**坏情况下会红**，所以比的是"文档里每一个点号 token 都必须有出处"，
    # 不是"出现过四个真动作名就行"。后者抓不住这种漂移：
    #   人把文档正文里一处 `pkg.install` 手改成 `wi.install`，
    #   四个真动作名都还在、行数没变、其它判据全绿 —— 但模型已经学会说一个设备不认的动作。
    #
    # 三个"有出处"的口子（都是派生的，没有硬编码名字白名单）：
    #   ① 真动作名（从 actions.c 的 hdr_match 抠）
    #   ② 仓库其它文件里出现的 token —— 比如 `config.json` / `action.log` / `locale.txt`
    #      （这些是设备端真实文件名）、`agentdoc.py`（维护者脚本名）
    #      **排除 AGENT.md 自身**，否则"文档里有什么就算什么"，判据当场失效。
    #   ③ 文档里 `name=` 后面的值 —— 那是示例文件名（note.txt），不是动作名
    #
    # ★ 排除两个文件自身：AGENT.md（否则"文档里写了就算有出处"，判据当场失效）
    # 和本脚本（否则"把坏例子写进注释里，注释就变成了证据"——
    # 判据里拿 wi.install 当反例写进注释，文档里出现 wi.install 反而被判为合法）。
    # 这就是铁律 6「断言对了 ≠ 对的原因对了」：不止要看它红不红，要看它为什么绿。
    _exclude = {os.path.abspath(AGENT_MD), os.path.abspath(__file__)}
    _other = ""
    for _dp, _ds, _fs in os.walk(ROOT):
        _ds[:] = [d for d in _ds
                  if d not in ("app/build", ".git", "_build_voice_opt",
                               "devkitPro", "3ds") and not d.startswith(".")]
        for _f in _fs:
            if not _f.endswith((".c", ".h", ".py", ".md", ".txt", ".json", ".s")):
                continue
            _p = os.path.join(_dp, _f)
            if os.path.abspath(_p) in _exclude:
                continue
            try:
                _other += get_text(_p)
            except Exception:
                pass
    name_vals = set(re.findall(r"name=([A-Za-z0-9_.\-]+)", agent))
    # ★ 前缀用 `[a-z_]+` 而不是 `[a-z]+`：`[a-z]+` 吃不下下划线，
    #   `tools/check_agentdoc.py` 只会抠出片段 `agentdoc.py`，
    #   于是判据去告一个文档里根本不存在的"假名字"（误伤基线）。
    tokens    = set(re.findall(r"[A-Za-z0-9_-]+\.[A-Za-z0-9_]{2,}", agent))
    # ★★ 章节号要剔掉。`### 2.11 \`svc.set\`` 里的 `2.11` 长得就像一个
    #   "点号 token"，而它当然不是动作名 —— 加到 2.11 那天判据就红了。
    #   之前没暴露是因为 `2.10` 之类能被当浮点数看，而 `2.11` 这种两位小数的
    #   躲过了那层判断。剔法：**去掉紧跟在 Markdown 标题 `###` 之后、
    #   或行首编号位置的那个**。用"全由数字组成"来判，比列黑名单稳 ——
    #   将来章节写到 2.20、3.11 也照样剔掉。
    tokens = set(t for t in tokens if not t.replace(".", "").isdigit())
    repo_files = set()
    for _dp, _ds, _fs in os.walk(ROOT):
        for _f in _fs:
            repo_files.add(_f)
    # ④ 文档里**明确被称作无效写法**的例子（反例句）。AGENT.md 第 42 行就拿
    #    `pkg.installX` / `pkg-install` 当"不算"的反例讲 —— 那两个名字本来就该在文档里。
    #    判据看的是**上下文有没有否定词**，不是名字本身，所以反例句写什么都不会误伤；
    #    真把 `pkg.install 从这里取` 改成不存在的动作名时，那句话里没有否定词 → 照样红。
    NEG  = ("不算", "不认", "不要写", "禁止", "无效", "会失败", "错了", "错误")
    def is_counterexample(t):
        for m in re.finditer(re.escape(t), agent):
            ctx = agent[max(0, m.start() - 40):m.end() + 40]
            if any(k in ctx for k in NEG):
                return True
        return False

    phantom = sorted(t for t in tokens
                     if t not in code_acts and t not in name_vals
                     and t not in _other and t not in repo_files
                     and not is_counterexample(t))
    if not phantom:
        ok("文档里 %d 个点号 token 全部有出处（真动作名 / 仓库已有文件名 / 示例 name=）"
           % len(tokens))
    else:
        fail("文档里出现了代码和仓库都不认的名字（模型会照着说，设备只会弹'我不认识它'）：%s"
             % phantom)

    # ── 2. 头部字段名必须与 arg_get 一致 ──
    print("\n[2] 动作头部字段")
    for fld in ["name=", "sha256=", "tid=", "region=", "lang="]:
        in_code = ("arg_get(hdr, \"%s\"" % fld) in act_c
        in_doc  = ("%s" % fld.rstrip("=")) in agent
        if in_code and in_doc:
            ok("字段 %s 在代码与文档中都在" % fld)
        else:
            fail("字段 %s —— 代码 arg_get=%s, 文档提到=%s" % (fld, in_code, in_doc))

    # ── 3. 上限：从源码抠，与文档字面量比 ──
    print("\n[3] 上限常量（从源码抠基准值，不硬编码）")
    # 先把源码里的真值备好，稍后逐个数对账用。
    # 注意：判据是「文档里**每一个**带单位的数字都要对」，不是「有没有这个数」——
    # 同一条规则在文档里会出现好几遍（正文、示例、决策表、环境事实…），
    # 只查"出现过"的话，改错其中一处照样绿。
    _byte = sniff(r"#define\s+ACT_TEXT_MAX\s+(\d+)", act_h, "ACT_TEXT_MAX", ACTIONS_H)
    _char = sniff(r"#define\s+ACT_NAME_MAX\s+(\d+)", act_h, "ACT_NAME_MAX", ACTIONS_H)
    _hex  = sniff(r"#define\s+LOC_TID_HEX\s+(\d+)", loc_h, "LOC_TID_HEX", LOCALE_H)
    _mb   = sniff(r"#define\s+PKG_MAX_BYTES\s+([^/\n]+)", pkg_h, "PKG_MAX_BYTES", PKG_H)
    try:
        _mbv = str(eval(_mb.strip(), {"__builtins__": {}}, {}))
    except Exception:
        _mbv = None
    UNIT = {"字节": _byte, "字符": _char, "位": _hex}
    if _mbv:
        UNIT["MB"] = str(int(_mbv) // (1024 * 1024))

    # 只抓**真正在声明上限**的数字（前面有"最长/上限/不超过/恰好/≤"或"单包…"）。
    # 不这么做的话，文档里这些数会被误伤：
    #   11567 字节（维护者注：手册自身体积）、
    #   128 字节（动作**头部**长度，不是内容上限，由下面另一条 phrase 判据覆盖）、
    #   64 位（sha256 是 64 位十六进制，不是游戏 ID 位数）。
    # 判据宁可漏放一个"写着上限但没单位"的错，也不要天天误报 —— 误报修两次，
    # 人就把它关掉了，那才是真正的"永远绿"。
    #
    # 注意**不放进 `≥`**：头部长度那句是"≥128 字节就不认"，它是另一条规则，
    # 硬塞进这张表就会被拿去跟内容上限（512 字节）比，于是基线就红。
    body_text = re.sub(r"<!--.*?-->", "", agent, flags=re.S)
    # "文件名"也进前缀表：环境事实那句写的是「文件名 60 字符 / 文件内容 512 字节…」，
    # 前面没有"最长/上限"之类的动词，只有分隔符——漏了它，"文件名上限被改错"就抓不到。
    CAP = (r"((?:最长|上限|不超过|最多|恰好|≤|文件名)[^0-9]{0,4}"
           r"|单包[^0-9]{0,6})\s*([0-9]+)\s*(字节|字符|位|MB)")
    unit_bad = []
    for _pre, n, unit in re.findall(CAP, body_text):   # 三个组：前缀 / 数字 / 单位
        exp = UNIT.get(unit)
        if exp and n != exp:
            unit_bad.append("%s %s（源码是 %s %s）" % (n, unit, exp, unit))
    if unit_bad:
        fail("AGENT.md 里有与源码不符的声明：%s" % "；".join(sorted(set(unit_bad))))
    else:
        ok("文档里所有带上限单位（字节/字符/位/MB）的数字都与源码一致")
    # ★ 判据一律比"带单位的完整短语"，绝不比裸数字：
    # 文档里散落着一堆数字（3ds、512、240…），裸数字比对会被别处的数喂饱，
    # 改成别的数也照样绿 —— 一个只在坏情况下不会红的判据等于没有。
    name_max = sniff(r"#define\s+ACT_NAME_MAX\s+(\d+)", act_h, "ACT_NAME_MAX", ACTIONS_H)
    if name_max:
        if ("%s 字符" % name_max) in agent:
            ok("文件名上限 %s 与文档一致" % name_max)
        else:
            fail("文件名上限：源码 %s 字符，AGENT.md 里没有『%s 字符』这句"
                 % (name_max, name_max))

    text_max = sniff(r"#define\s+ACT_TEXT_MAX\s+(\d+)", act_h, "ACT_TEXT_MAX", ACTIONS_H)
    if text_max:
        if ("%s 字节" % text_max) in agent:
            ok("内容上限 %s 与文档一致" % text_max)
        else:
            fail("内容上限：源码 %s 字节，AGENT.md 里没有『%s 字节』这句"
                 % (text_max, text_max))

    # 源码里是 `(32 * 1024 * 1024)` 这种表达式 —— 抠**整段**再算，
    # 只抠第一个数字会读到 32，然后 "32 MB" 看起来对、其实基准是错的。
    expr = sniff(r"#define\s+PKG_MAX_BYTES\s+([^/\n]+)", pkg_h, "PKG_MAX_BYTES", PKG_H)
    if expr:
        try:
            pkg_max = str(eval(expr.strip(), {"__builtins__": {}}, {}))
        except Exception as e:
            fail("PKG_MAX_BYTES 表达式 %r 算不出来：%s" % (expr, e))
            pkg_max = None
    if pkg_max:
        mb = int(pkg_max) / (1024 * 1024)
        if ("%g MB" % mb) in agent or ("%d MB" % mb) in agent:
            ok("单包装载上限 %g MB 与文档一致" % mb)
        else:
            fail("单包上限：源码 %d 字节 (%g MB)，文档里没写这个数" % (int(pkg_max), mb))

    # 出处是 LOC_TID_HEX（不是 LOC_TID_MAX —— 那个名字不存在，抠错就 FAIL）
    # 源码若从 16 改成 8，"%s 位" 就对不上文档里的"16 位" → 红。
    # （别写成硬编码 `tid_max == "16"`，那等于把期望值塞进判据，对账就没意义了）
    tid_max = sniff(r"#define\s+LOC_TID_HEX\s+(\d+)", loc_h, "LOC_TID_HEX", LOCALE_H)
    if tid_max:
        if ("%s 位" % tid_max) in agent:
            ok("游戏 ID 长度 %s 位与文档一致" % tid_max)
        else:
            fail("游戏 ID 长度：源码 %s 位，AGENT.md 里没有『%s 位』这句"
                 % (tid_max, tid_max))

    m128 = re.search(r"char\s+hdr\[(\d+)\]", act_c)
    if m128:
        n = m128.group(1)
        if ("%s 字节" % n) in agent:
            ok("动作头部长度上限 %s 字节与文档一致" % n)
        else:
            fail("动作头部长度上限：源码 %s 字节，AGENT.md 里没有『%s 字节』这句"
                 % (n, n))
    else:
        fail("actions.c 里找不到头部缓冲声明（sizeof hdr 的判据前提）")

    # ── 4. 路径：抠宏，与文档里的字符串比 ──
    print("\n[4] 卡上路径")
    paths = [
        (ACTIONS_C,  r'#define\s+ACT_DIR\s+"([^"]+)"',     "沙箱 out/"),
        (PKG_C,      r'#define\s+PKG_DIR\s+"([^"]+)"',     "pkgs/ 待安装目录"),
        (LOCALE_H,   r'#define\s+LOC_DIR\s+"([^"]+)"',     "luma/titles"),
        (PKG_H,      r'#define\s+PKG_INSTALL_DIR\s+"([^"]+)"', "pkg 安装落点"),
    ]
    for path, pat, what in paths:
        val = sniff(pat, get_text(path), what, path)
        # 判据比"整段路径出现在文档里"，**不**用 `val.split("/")[-1] in agent`：
        # 那样只比最后一个目录名 —— 文档把沙箱写成 sdmc:/3ds/aiconsole/sandbox/ 时，
        # 末尾那个 "out" 在别处也出现，判据照样绿，错路径就这么混过去了。
        #
        # ★ 匹配要**忽略大小写**：文档里同一个路径有两种写法（正文 `sdmc:/…`、
        #   路径表 `SD:/…`）。只按字面比的话，把表里那处改错、正文那处还是对的，
        #   判据就绿了 —— 同一个真值存两处，早晚有一处改漏。
        #   （这类"路径表与正文不一致"的漂移，判据只能抓到这里写的这一种）
        # 还要**至少出现 2 次**：文档里每条卡上路径都在"环境事实/正文"和"路径表"两处写过。
        # 只出现一次的话，说明有人把其中一处改错了（另一处还是对的），而判据比的是
        # "整段路径还在不在" —— 那种单处改写照样绿，改错的那一处没人发现。
        # 宏里写的是带协议的 `sdmc:/…`，文档正文和路径表一个写 `sdmc:/…`、一个写 `SD:/…`
        # —— 比较前把协议前缀去掉，否则「同一条路径的两种写法」会被当成两条不同路径。
        key = re.sub(r"^(sdmc|sd):/", "", val.strip("/")) if val else ""
        _cnt = agent.lower().count(key.lower())
        if key and _cnt >= 2:
            ok("%s = %s 与文档一致（出现 %d 次，正文与路径表都在）" % (what, key, _cnt))
        elif key:
            fail("%s：源码是 %s，AGENT.md 里没完整出现这个路径（只出现 %d 次）"
                 % (what, key, _cnt))
        else:
            fail("%s：抠不到路径常量" % what)

    # pkgs 里的目录必须真的是 pkg.install 的源（文档讲的前置条件）
    if "pkgs" in agent and "pkgs" in get_text(PKG_C):
        ok("文档说明了包必须先放在 pkgs/ 目录")
    else:
        fail("文档没提 pkgs/ 前置条件（模型会以为它能直接装不存在的包）")

    # ── 5. 协议标记必须与 MARK_* 一致 ──
    print("\n[5] 协议标记")
    for pat, what in [
        (r'#define\s+MARK_OPEN\s+"([^"]+)"',     "开标记"),
        (r'#define\s+MARK_CLOSE\s+"([^"]+)"',    "闭标记"),
        (r'#define\s+MARK_END\s+"([^"]+)"',      "头部结束"),
    ]:
        val = sniff(pat, act_c, what, ACTIONS_C)
        if val and val in agent:
            ok("%s %s 与文档一致" % (what, val))
        elif val:
            fail("%s：源码是 %s，AGENT.md 里没出现" % (what, val))
        else:
            fail("%s：抠不到" % what)

    # ── 6. 体积上限：AGENT.md 必须小于 AGENTDOC_MAX ──
    # ⚠️ 超限的后果**不是**"截断成半份"，而是 agentdoc_load() **整份返回失败**
    #    （宁可这一轮没有手册，也不给一份看起来完整的半份 —— 半份会让模型
    #    以为"没写到的动作就是不许的"）。这句话早先写反了，改掉。
    print("\n[6] 装载体积（超了 agentdoc_load 会**整份拒绝** → 这一轮模型没有手册）")
    cap_expr = sniff(r"#define\s+AGENTDOC_MAX\s+([^/\n]+)", get_text(AGENTDOC_H),
                     "AGENTDOC_MAX", AGENTDOC_H)
    cap = None
    if cap_expr:
        try:
            cap = str(eval(cap_expr.strip(), {"__builtins__": {}}, {}))
        except Exception as e:
            fail("AGENTDOC_MAX 表达式 %r 算不出来：%s" % (cap_expr, e))
    size = os.path.getsize(AGENT_MD)
    if cap:
        if size < int(cap):
            ok("AGENT.md %d 字节 < AGENTDOC_MAX %s（留 %d 字节余量）"
               % (size, cap, int(cap) - size))
        else:
            fail("AGENT.md %d 字节 >= AGENTDOC_MAX %s → 装载时整份被拒（模型这一轮没有手册）"
                 % (size, cap))
    else:
        fail("抠不到 AGENTDOC_MAX")

    # ── 6b. 内嵌副本：烧进程序的那份手册必须与 AGENT.md 逐字节一致 ──
    # AGENT.md 改了但没重新构建 → 程序里那份还是旧的，
    # 而运行时**优先用内嵌那份**，于是"改了文档没反应"还会表现得很正常。
    print("\n[6b] 内嵌副本（烧进程序、运行时优先用的就是它）")
    # 跟着 AIC_BUILD_DIR 走。并行构建时中间产物在私有目录里，
    # 而这份判据要是死盯着 app/build/，就会拿**上一次**的装配来比 ——
    # 那时它报"不一致"其实是比错了对象，报"一致"才是真的比错
    # （另一个会话刚编好的新装配被无视了）。两种都是假结论。
    _bdir = os.environ.get("AIC_BUILD_DIR") or os.path.join(ROOT, "app", "build")
    emb_s = os.path.join(_bdir, "agentdoc_md.s")
    if os.path.exists(emb_s):
        _txt = get_text(emb_s)
        _bad = [l for l in _txt.splitlines() if "%%" in l]
        _vals = []
        for _m in re.finditer(r"^\t\.byte\s+(.*)$", _txt, re.M):
            _vals += [int(x, 16) for x in _m.group(1).split(",")]
        _raw = open(AGENT_MD, "rb").read()
        if _bad:
            fail("agentdoc_md.s 里残留 %%（构建脚本转义写错，gas 会报语法错）：%s" % _bad)
        elif bytes(_vals) != _raw:
            fail("内嵌副本与 AGENT.md 不一致：装配里 %d 字节，文档 %d 字节"
                 % (len(_vals), len(_raw)))
        else:
            ok("内嵌副本 %d 字节与 AGENT.md 逐字节一致" % len(_vals))
    else:
        fail("%s 不存在 —— 构建没跑，或构建脚本漏了这一步"
             "（运行时会静默改读卡上文件，手册来源就从内嵌变成 sdmc）"
             % os.path.relpath(emb_s, ROOT).replace("\\", "/"))

    # ── 6c. 接线：内嵌那条路必须是接上的，否则上面那份字节对了也没用 ──
    print("\n[6c] 编译接线（AGENTDOC_EMBEDDED 有没有真的连上）")
    _bc = get_text(os.path.join(ROOT, "tools", "build.py"))
    _ac = get_text(os.path.join(ROOT, "shared", "agentdoc.c"))
    _sc = get_text(os.path.join(ROOT, "app", "source", "ui", "sc_chat.c"))
    for what, cond in [
        ("build.py 里 emit_agentdoc_asm 被构建主流程调用",
         "emit_agentdoc_asm(" in _bc and "emit_agentdoc_asm(agent_md" in _bc),
        # 要比**完整的那段宏名**（带前后引号），不是只比子串：
        # 把 `-DAGENTDOC_EMBEDDED` 改成 `-DAGENTDOC_EMBEDDED_OFF`，
        # 子串照样在，判据就绿了 —— 而那条路已经悄悄断了。
        ("build.py 给 agentdoc.c 加了 -DAGENTDOC_EMBEDDED",
         '"-DAGENTDOC_EMBEDDED"' in _bc),
        ("agentdoc.c 有内嵌分支（#ifdef AGENTDOC_EMBEDDED）",
         "#ifdef AGENTDOC_EMBEDDED" in _ac),
        ("agentdoc.c 的 agentdoc_load 是四参数（出参 src 报来源）",
         "bool agentdoc_load(char *buf, size_t cap, size_t *len, const char **src)"
         in _ac),
        ("sc_chat.c 按新签名调用（少了这个参数会直接编译不过）",
         # ★ 判据只认**调用形状**，不认变量名：早先这里写死了
         #   "agentdoc_load(g_agdoc, sizeof g_agdoc, &adl, &asrc)"，
         #   后来调用处变量改名成 adbuf，判据就永久红了 —— 红线原因是
         #   "判据绑了实现细节"，不是接线真的断了。这类假红比漏检更坏：
         #   它训练人忽略红。用正则匹配四个实参的形状即可。
         re.search(r"agentdoc_load\s*\(\s*\w+\s*,\s*sizeof\s+\w+\s*,"
                   r"\s*&\w+\s*,\s*&\w+\s*\)", _sc) is not None),
        ("探针报手册来源（doc=%s，能分清内嵌/卡上过期的）",
         'doc=%s' in _sc),
    ]:
        if cond:
            ok(what)
        else:
            fail("接线断了：%s" % what)

    # ── 7. 红线条款：文档必须真的教了这几条 ──
    #
    # ★ 2026-10-16 改判据本身：旧版盯的是"不许声称执行成功"，理由是
    #   "你看不到结果"。而设备**早就把上一轮结果回灌给模型了**
    #   （app_act_result_store → 下一轮 system 消息），于是那条老规矩
    #   变成了**假约束**：模型被教成"我是瞎的"，就再也不去规划第二步 ——
    #   强模型来了也干不了活。新的不变式是：
    #     · 结果**能**看到，但只在下一轮，且**只能引用、不许编**；
    #     · 发动作的**同一轮**不许做任何结果推断。
    #   所以判据也换成盯这两条 —— 光删掉旧句子而没写新句子，这里要红。
    print("\n[7] 关键行为约束（漏了就是模型会瞎说）")
    must_haves = [
        ("结果下一轮才回灌、只能引用不许编", ["回灌", "只能引用", "不许编"]),
        ("发动作同一轮不许推断结果",       ["这一轮", "同一轮", "不许推断", "一个字都不许"]),
        ("不许编造 sha256",         ["sha256", "摘要"]),
        ("不许一次多个动作",        ["多个动作", "第一个"]),
        ("不许编路径",              ["裸文件名", "路径是设备拼的", "不编"]),
        ("装完不能说已装好",        ["装好了", "已装好", "已经装好"]),
        ("拒绝/失败后要换一条路",   ["拒绝了", "换一条", "不要再发"]),
    ]
    for what, pats in must_haves:
        hit = any(p in agent for p in pats)
        if hit:
            ok("文档表达了：%s" % what)
        else:
            fail("文档缺少关键约束：%s" % what)

    print("\n[8] 英文手册（AGENT.en.md）—— 界面切成 English 时喂给模型的就是它")
    # ★ 为什么英文版也要对账：它**不是翻译参考**，而是真的会被发给模型
    #   （shared/agentdoc.c 的 pick_doc 按 ui.language 挑）。
    #   英文版少了动作名 → 英文界面下模型根本不知道有这个动作；
    #   英文版写了过期的上限 → 模型按错的数去说。
    #   这类漏检的症状是"英文用户那边 AI 变笨"，而中文这边全绿。
    if not os.path.exists(AGENT_EN_MD):
        fail("AGENT.en.md 不存在 —— 英文界面会回退中文手册（行为正确但语言不对）")
    else:
        agent_en = get_text(AGENT_EN_MD)
        ok("AGENT.en.md 存在（%d 字节）" % os.path.getsize(AGENT_EN_MD))
        for a in ACTIONS:
            # 英文版同样要求出现 ≥2 次：一次在合法动作名列表，一次在链路/决策树。
            if agent_en.count(a) >= 2:
                ok("英文版含动作 %s" % a)
            else:
                fail("英文版缺动作 %s（出现 %d 次，至少 2 次）"
                     % (a, agent_en.count(a)))
        # 关键数字必须与中文版一致（这些是设备端硬上限，翻译时最容易漏）
        for num in ("512", "32 MB", "60", "24 KB", "128"):
            if num in agent_en:
                ok("英文版含关键数字 %s" % num)
            else:
                fail("英文版缺关键数字 %s" % num)
        # 关键行为约束（英文版必须有等价表述，否则英文用户会遇到中文版早修过的坑）
        must_en = [
            ("不许编造结果", ["fabricate", "pretend"]),
            ("不许编动作名", ["invent action", "do not invent"]),
            ("只给裸名字/值", ["bare file name", "bare names"]),
            ("结果下一轮才回灌", ["next turn"]),
            ("拒绝后换一条路", ["different route"]),
        ]
        for what, pats in must_en:
            if any(p in agent_en for p in pats):
                ok("英文版表达了：%s" % what)
            else:
                fail("英文版缺少关键约束：%s" % what)

    print("\n== %d ok, %d problem(s) ==" % (ok_cnt, bad_cnt))
    return 0 if bad_cnt == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
