"""fix_cand.py — 用「该款语料里确实存在」的词替换候选词表

══ 为什么要重做一遍选词 ════════════════════════════════════════════

上一版候选词表里有 11 处不合格（verify_cand.py 抓出来的），
逐个查根因后发现**不是检索的问题，是我选词时凭空想的**：

    徽章   在 pokemon_bw 的语料里 0 次   （它在 frlg / sm / xy 里有）
    变身   在 zelda_albw / oot3d 里 0 次（那两款没这个系统）
    动森   animal_crossing 的语料里写的是「动物之森」，没有「动森」
    配招   BPE 切成 ['配','招'] 两块，两块都不是完整的词
    化石   BPE 切成 ['化','石']
    双剑   BPE 切成 ['双','剑']
    G级    BPE 切成 ['G','级']
    猫咪幽灵 切成 ['猫','幽灵']，且语料里写的是「猫咪幽灵」也会被切开

诊断时的判据是这一行（它比 verify 的更有用）：
    该游戏内含词段 = 0
→ 直接说明这个词**在语料里根本不存在**，换检索算法也没用。

所以这一版只用**各款语料的章节标题里出现过的词** ——
那些是人写的，一定存在。

⚠️ 另一个反复踩到的坑：**BPE 切不开的词不能当候选词**。
判据是 len(bpe.encode(w)) == 1。切开的词（「配招」「化石」「G级」）
在倒排里存的是碎片，检索时命中的是无意义的半截。

用法：python tools/guia/fix_cand.py --apply
"""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as G          # noqa: E402
import test_kb as T               # noqa: E402
from train_bpe import BPE         # noqa: E402

# 全部取自**该款语料的 ## 章节标题**，且都是 BPE 能整词切分的。
NEW = {
    "ace_attorney":    ["证词", "调查", "追问", "档案", "信息", "触发"],
    "animal_crossing": ["钓鱼", "天空", "服务处", "家具", "普通", "图鉴"],
    "fe_awakening":    ["转职", "支援", "结婚", "弓箭", "外传", "战士"],
    "fe_if":           ["命中率", "传送", "规划", "金币", "影响", "有限"],
    "kid_icarus":      ["冲刺", "障碍物", "点数", "移动", "重要", "能量"],
    "kirby":           ["心心", "金币", "冲刺", "移动", "关卡", "地形"],
    "legend_zelda":    ["回旋镖", "戒指", "试炼", "迷宫", "赛跑", "试错"],
    "luigi2":          ["吸尘器", "鬼怪", "宝石", "喷火", "家具", "模式"],
    "mario_3d_land":   ["变身", "星星", "回血", "转身", "掉血", "操作"],
    "mario_3d_land_2": ["猫套装", "星星币", "印章", "角落", "习惯", "蘑菇"],
    "mario_party":     ["库巴", "星星", "机关", "地形", "传送", "点数"],
    "mh4g":            ["弱点", "硬直", "回避", "提升", "平衡", "掉血"],
    "minecraft":       ["绿宝石", "神庙", "实用", "规划", "生物", "村民"],
    "mk7":             ["漂移", "组装", "零件", "赛道", "名次", "喷火"],
    "new_supermario_2d": ["金币", "变身", "大炮", "隐藏房间", "世界", "划算"],
    "paper_mario":     ["贴纸", "判定", "NPC", "心心", "城镇", "卡住"],
    "pokemon_bw":      ["御三家", "TM", "拔群", "冰系", "主力", "难度"],
    "pokemon_emerald": ["性格", "TM", "绿宝石", "输出", "顺序", "主力"],
    "pokemon_frlg":    ["御三家", "克制", "洞窟", "削弱", "稳定", "捕捉"],
    "pokemon_sm":      ["试炼", "克制", "物理", "存档", "版本", "条件"],
    "pokemon_xy":      ["卡洛斯", "克制", "物理", "存档", "角落", "提升"],
    "rayman":          ["公主", "机械", "隐藏区域", "撞墙", "帮助", "两种"],
    "repton":          ["NPC", "情报", "地图", "障碍物", "点数", "复杂"],
    "resident_evil":   ["射击", "连击", "弹药", "近战", "计时", "关键"],
    "retro_prince":    ["钩索", "时间重置", "潜行", "转身", "走位", "判定"],
    "steam_code":      ["指令", "机器人", "顺序", "压力", "解法", "完成"],
    "we_football":     ["阵型", "触屏", "记忆", "有效", "位置", "重要"],
    "zelda_albw":      ["心之碎片", "沙之杖", "迷宫", "支线", "魔法", "NPC"],
    "zelda_alttp":     ["黑暗世界", "弓箭", "触屏", "装备", "地牢", "手套"],
    "zelda_mm3d":      ["心之碎片", "面具", "城镇", "山区", "支线", "NPC"],
    "zelda_oot3d":     ["心之碎片", "钩索箭", "药水", "特性", "NPC", "魔法"],
}

def main():
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    kb = T.load()
    R = G.Retriever(kb, bpe, G.GAME_ORDER)

    if set(NEW) != set(G.GAME_ORDER):
        print("!! 游戏集合不一致")
        return 1

    bad = 0
    print("逐词三道判据：① BPE 能整词切分 ② 语料里真的有 "
          "③ 限定该游戏时能命中")
    print()
    for gi, stem in enumerate(G.GAME_ORDER):
        marks = []
        for w in NEW[stem]:
            n_tok = len(bpe.encode(w))
            in_game = sum(1 for p in range(kb["n"])
                          if kb["pass_game"][p] == gi and w in R.text(p))
            r = R.search(w, gi, k=1)
            top1 = bool(r) and w in R.text(r[0][0])
            if n_tok != 1:
                marks.append("%s=切%d块" % (w, n_tok)); bad += 1
            elif in_game == 0:
                marks.append("%s=语料无" % w); bad += 1
            elif not top1:
                marks.append("%s=弱" % w); bad += 1
            else:
                marks.append("%s=OK" % w)
        print("  %2d %-22s %s" % (gi, stem, "  ".join(marks)))

    print()
    if bad:
        print("仍有 %d 处不合格" % bad)
        return 1
    print("全部合格。")
    if "--apply" in sys.argv:
        # 把结果写进 verify_cand.py 的 CAND
        p = os.path.join(HERE, "verify_cand.py")
        s = open(p, encoding="utf-8").read()
        i = s.index("CAND = {")
        j = s.index("\n}\n", i) + 3
        body = ["CAND = {"]
        for gi, stem in enumerate(G.GAME_ORDER):
            body.append('    "%s":%s[%s],'
                        % (stem, " " * max(1, 20 - len(stem)),
                           ", ".join('"%s"' % w for w in NEW[stem])))
        body.append("}")
        s = s[:i] + "\n".join(body) + s[j:]
        open(p, "w", encoding="utf-8").write(s)
        print("已写回 verify_cand.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
