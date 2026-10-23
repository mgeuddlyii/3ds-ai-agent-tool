# -*- coding: utf-8 -*-
"""tools.c（算式部分）的 PC 侧孪生实现 + 判据。

⚠️ 这是**复刻品**：复刻 tools.c 的逻辑，不是跑 tools.c 本身。
C 侧一致性靠：① arm-none-eabi-gcc 编译 ② objdump 确认无 vadd.f64
③ 人工逐行比对 ④ 设备上实测（本轮未做，禁开模拟器）。

用法：python tools_twin.py
"""

EXPR_MAX = 64
DEPTH_MAX = 16
POW_MAX = 64

OPS = {"加": "+", "减": "-", "乘": "*", "除": "/"}


# ------------------------------------------------------------ 归一化
def u8_iter(s):
    """把字符串拆成 (码点, 字节串)。"""
    b = s.encode("utf-8")
    i = 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            yield c, bytes([c]), 1
            i += 1
        elif (c & 0xE0) == 0xC0 and i + 1 < len(b):
            cp = ((c & 0x1F) << 6) | (b[i + 1] & 0x3F)
            yield cp, b[i:i + 2], 2
            i += 2
        elif (c & 0xF0) == 0xE0 and i + 2 < len(b):
            cp = ((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F)
            yield cp, b[i:i + 3], 3
            i += 3
        else:
            yield -1, b[i:i + 1], 1
            i += 1


def norm_expr(inp):
    """复刻 tools.c 的 norm_expr：映射 + 截断，返回 ASCII 前缀。"""
    out = []
    chars = list(u8_iter(inp))
    i = 0
    while i < len(chars):
        cp, _raw, adv = chars[i]
        if cp >= 0xFF01 and cp <= 0xFF5E:
            c = chr(cp - 0xFEE0)
        elif cp == 0x3000:
            c = " "
        elif cp == 0x00D7:
            c = "*"
        elif cp == 0x00F7:
            c = "/"
        elif cp == 0x2212:
            c = "-"
        elif cp > 0x7F:
            c = OPS.get(chr(cp))
            if c is None:
                break                      # 别的汉字 -> 截断
            if c in "*/":
                # 注意：chars 是**字符列表**，所以跳「以」要 +1 个元素，
                # 不是 +adv（那是字节数）。写 adv 会跳过头 -> 7除以2 变语法错。
                if i + 1 < len(chars) and chars[i + 1][0] == 0x4EE5:
                    i += 1                 # 吃掉「以」
        elif cp < 0:
            break
        else:
            c = chr(cp)

        if not (c.isdigit() or c in ".-+*/()^% "):
            break                          # 白名单外 ASCII -> 截断
        out.append(c)
        i += 1
    return "".join(out)


def charset_ok(s):
    has_digit = any(ch.isdigit() for ch in s)
    has_op = any(ch in "+-*/^%" for ch in s)
    if not s or not has_digit or not has_op:
        return False
    return all(ch.isdigit() or ch in "+-*/^%.() " for ch in s)


# ------------------------------------------------------------ 求值
class Err(Exception):
    pass


def _prim(s, i):
    while i < len(s) and s[i] == " ":
        i += 1
    if i < len(s) and s[i] == "(":
        v, i = _expr(s, i + 1)
        while i < len(s) and s[i] == " ":
            i += 1
        if i >= len(s) or s[i] != ")":
            raise Err("paren")
        return v, i + 1
    if i < len(s) and s[i] == "-":
        v, i = _prim(s, i + 1)
        return -v, i
    if i < len(s) and s[i] == "+":
        return _prim(s, i + 1)
    j = i
    buf = ""
    while j < len(s) and s[j].isdigit():
        buf += s[j]
        j += 1
    if buf:
        v = float(buf)
        if j < len(s) and s[j] == ".":
            j += 1
            frac = ""
            while j < len(s) and s[j].isdigit():
                frac += s[j]
                j += 1
            if frac:
                v += float("0." + frac)
        return v, j
    raise Err("syntax")


def _pow(s, i):
    base, i = _prim(s, i)
    while i < len(s) and s[i] == " ":
        i += 1
    if i < len(s) and s[i] == "^":
        n, i = _pow(s, i + 1)
        n = int(n)
        if n < 0:
            return 0.0, i
        n = min(n, POW_MAX)
        return base ** n, i
    return base, i


def _term(s, i):
    v, i = _pow(s, i)
    while True:
        while i < len(s) and s[i] == " ":
            i += 1
        if i >= len(s) or s[i] not in "*/%":
            return v, i
        op = s[i]
        i += 1
        r, i = _pow(s, i)
        if op == "*":
            v = v * r
        elif op == "/":
            if r == 0:
                raise Err("div0")
            v = v / r
        else:
            if r == 0:
                raise Err("div0")
            v = float(int(v) % int(r))


def _expr(s, i):
    v, i = _term(s, i)
    while True:
        while i < len(s) and s[i] == " ":
            i += 1
        if i >= len(s) or s[i] not in "+-":
            return v, i
        op = s[i]
        i += 1
        r, i = _term(s, i)
        v = v + r if op == "+" else v - r


def tools_eval(expr):
    """复刻 tools_eval：1 成功 / 0 不是算式 / -1 算式但算不出来。"""
    s = norm_expr(expr)
    if not charset_ok(s):
        return 0, None
    try:
        v, i = _expr(s, 0)
    except Err as e:
        return (-1, str(e))
    while i < len(s) and s[i] == " ":
        i += 1
    if i != len(s):
        return 0, None
    return 1, v


# ------------------------------------------------------------ 判据
FAIL = []


def check(name, expr, want_rc, want_val=None):
    rc, v = tools_eval(expr)
    ok = (rc == want_rc)
    if ok and want_rc == 1:
        ok = (abs(v - want_val) < 1e-4)
    if not ok:
        FAIL.append(name)
    got = ("rc=%d,%s" % (rc, ("%.4f" % v) if rc == 1 else v))
    print("  %s %-34s -> %-18s 期望 rc=%d%s"
          % ("OK  " if ok else "FAIL", expr[:32], got, want_rc,
             (",%g" % want_val) if want_rc == 1 else ""))


print("=" * 66)
print("tools 算式孪生判据")
print("=" * 66)

print("\n[1] 基本算式（这是刚需：1+1）")
check("1+1", "1+1", 1, 2)
check("3*5", "3*5", 1, 15)
check("12/4", "12/4", 1, 3)
check("10-3", "10-3", 1, 7)
check("2+3*4", "2+3*4", 1, 14)          # 优先级
check("3*(4+5)", "3*(4+5)", 1, 27)      # 括号
check("2^10", "2^10", 1, 1024)          # 幂，右结合
check("2^3^2", "2^3^2", 1, 512)        # 右结合 2^(3^2)=512
check("10/4", "10/4", 1, 2.5)           # 非整数
check("-5+8", "-5+8", 1, 3)             # 一元负号
check("10%3", "10%3", 1, 1)             # 取模

print("\n[2] 汉字/全角运算符")
check("1加1", "1加1", 1, 2)
check("2乘3", "2乘3", 1, 6)
check("7除以2", "7除以2", 1, 3.5)
check("1＋1", "1＋1", 1, 2)              # 全角加号
check("１＋１", "１＋１", 1, 2)            # 全角数字+加号
check("3×5", "3×5", 1, 15)               # 乘号
check("6÷2", "6÷2", 1, 3)                # 除号
check("1 + 1", "1 + 1", 1, 2)            # 带空格

print("\n[3] 问句后缀（汉字在答案之后）")
check("1+1等于几", "1+1等于几", 1, 2)
check("3*5是多少", "3*5是多少", 1, 15)

print("\n[4] 必须拒绝的（抢答就是 bug）")
check("火焰纹章觉醒", "火焰纹章觉醒", 0)
check("它的boss怎么打", "它的boss怎么打", 0)
check("塞尔达 织梦岛", "塞尔达 织梦岛", 0)
check("3ds和2ds哪个好", "3ds和2ds哪个好", 0)
check("2周目怎么打", "2周目怎么打", 0)
check("雷顿教授3怎么过", "雷顿教授3怎么过", 0)
check("1+1模式", "1+1模式", 1, 2)         # 有运算符，仍算算式（已知取舍）

print("\n[5] 错误路径")
# 判据要点：明显是算式但打错了的，返回 -1（提示"算不出来"）**比**返回 0 更 helpful ——
# 返回 0 会甩给攻略检索，用户看到的是完全不相干的答案。
check("1/0", "1/0", -1)                 # 除零
check("10%0", "10%0", -1)                # 取模零
check("(1+2", "(1+2", -1)                # 括号不配对 -> 明确告诉用户
check("+*1", "+*1", -1)                  # 语法错 -> 明确告诉用户
check("1++", "1++", -1)                  # 尾随运算符

print("\n[6] 深度与指数保护")
check("((((1+2))))", "((((1+2))))", 1, 3)
check("2^1000000", "2^1000000", 1, float(2 ** POW_MAX))   # 指数被夹到 64

print("\n" + "=" * 66)
if FAIL:
    print("❌ %d 条未过：%s" % (len(FAIL), ", ".join(FAIL)))
    raise SystemExit(1)
print("✅ 全部判据通过（6 组 / 33 条）")
print("=" * 66)
