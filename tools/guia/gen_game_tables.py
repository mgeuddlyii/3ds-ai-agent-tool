#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_game_tables.py — 生成游戏名/别名表

══ 为什么这个脚本存在 ══════════════════════════════════════════════

game_id 是**按语料文件名字母序**分配的下标（见 build_kb.py 的
`for f in sorted(glob("guide_*.txt"))`）。所以：

    game_id 不是语义标识，而是**一个必须与语料目录保持同步的序号**。

而历史上这两张表（Python 的 GAME_ORDER/GAME_ALIAS、C 的 g_alias/
g_game_names）是**手抄**的，抄的是 19 款版本的语料。
后来语料扩到 31 款，两张表**都没跟着改** —— 于是：

    全部 19 款的下标**错位**（fe_awakening 真实 id=2，表里写 0）
    另外 12 款**根本不在表里**

后果不是"少支持几款游戏"，而是**限定游戏时过滤到别的游戏**：
`gkb_search(kb, query, gid, ...)` 拿 gid 去比 `pass_game[pid]`，
gid 错位 → 命中的是另一个游戏的段落。而现象只是「答案不对」，
不报错、也不崩。

⚠️ 这是「定长表/下标表只能加末尾、不能重排」的一个真实代价：
game_id 一旦分配就固化在 build_kb.bin 里，改语料清单会让**所有 id 平移**。
所以加游戏时**只能追加字母序靠后的文件名**才能保证老 id 不变 ——
而这次扩语料显然没满足这个前提。

→ 这个脚本从**语料目录**重新生成三张表，让它们不可能再错位。
   改完语料后必须重跑它，并把结果贴回源码。

⚠️ 历史坑：这个脚本的 docstring 曾经写过「--write 直接改三个源文件」，
   而 main() 里**根本没有 argv 处理**，一个字节都不写。跑完看着像成功，
   实际什么都没改 —— 于是「改完语料去重跑它」这条流程**静默失效**，
   表就一直停在旧版本。这是「永远绿的守卫比没有更坏」的同族病：
   一个写着能干活的入口，比没有这个入口更坑人。
   （2026-10-05 已补上真正的 --write，见 apply_writes()）

用法：
    python tools/guia/gen_game_tables.py            # 只打印 + 自检（默认）
    python tools/guia/gen_game_tables.py --write    # 落盘，并先备份成 .bak-<时间戳>
"""
import glob
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# 写盘目标根目录。默认就是项目根；设 AIC_TABLES_ROOT 可以指向一份**副本**，
# 用来在不动真源码的前提下把 --write 全流程演练一遍（升级前必做的自证：
# 「这个 --write 真的会产出正确结果吗」，而不是等写完再看）。
ROOT = (os.environ.get("AIC_TABLES_ROOT")
        or os.path.abspath(os.path.join(HERE, "..", "..")))
# ⚠️ 必须是**绝对路径**。这里原来写的是相对路径 "tools/guia/corpus"，
#    于是这个脚本**只在项目根目录下跑才对**：换个 cwd 跑，glob 返回空，
#    collect() 得到 0 款，apply_writes 就会把 guide_kb.c 里的两张表
#    **清成空表**再落盘（虽然有写回自检会 raise，但那时字节已经写进去了，
#    只剩 .bak 能救）。一个「看起来能跑、换个目录就清空源码」的入口，
#    比直接报错危险得多。
CORPUS = os.path.join(HERE, "corpus")

# 文件基名 -> (屏幕显示名, 别名列表)
#
# 显示名从语料正文第一个《》里取（见脚本自动提取），这里只补**别名**。
# 别名的作用：让「火焰纹章觉醒 转职怎么转」这种带游戏名的问句能把
# 游戏名**从打分里剔除**（只用来定范围）。
#
# ⚠️ 别名不要包含**别的游戏的词**。实测把「三角力量」放进
# legend_zelda 之后，它会抢走 zelda_alttp / zelda_albw / zelda_mm3d /
# zelda_oot3d 的问句 —— 因为「塞尔达传说 三角力量 重制版」里也有
# 「三角力量」四个字，而 g_alias 是**先长后短**匹配，
# 短的那个先命中就把范围定错了。
#
# 所以别名只放**该游戏独有**的片段。
# ⚠️ 别名必须**互不为子串**（跨游戏）。
# 上面 ALIAS 里有几处是故意留下的"坏例子"，脚本末尾的自检会抓出来：
#   · 「3D 大陆」是「3D 大陆 2」的子串
#   · 「三角力量」是「众神三角力量2」「三角力量 重制版」的子串
# 自检报出冲突时，**把短的那个删掉**（短的更容易撞车），
# 而不是把长的删掉 —— 长别名本来就更能唯一定位一款游戏。
#
# ALIAS 里带 ✔ 的是"已经过自检的最终形态"；不带的是反例，留着当文档。
# 显示名（底屏上列出的那一行）。
#
# ⚠️ 不能只靠 re.search(r"《...》") 自动取第一个 —— 语料扩到 66 款后实测出两类问题：
#   1. 有 7 款文件的正文里**一个 《》 都没有**（bravely_default / danganronpa_1 /
#      inazuma_eleven_go / mega_man_legacy / pokemon_dppt / vc_nes_classics /
#      yokai_watch），自动提取直接退回文件名，底屏会显示 guide_xxx 这种英文串。
#   2. 更隐蔽的：pokemon_xy 正文第一个 《》 是《宝可梦X》（Y 在后面），
#      提取结果就成了「宝可梦X」，底屏看着像只有一版。
#      zelda_oracle_ages 同理第一个 《》 是《织梦岛》（那是 links_awakening 的名字）。
# 所以显示名**必须显式给**，自动提取只留作兜底并在末尾自检（见 NAME_AUTO_OK）。
NAME = {
    "ace_attorney": "逆转裁判4",
    "animal_crossing": "动物之森 新的开始",
    "bravely_default": "勇气默示录",
    "castlevania_vc": "恶魔城 历险记",
    "danganronpa_1": "弹丸论破 枪弹辩驳",
    "donkey_kong_country_returns_3d": "森喜刚之国 归来 3D",
    "dragon_quest_vii_3d": "勇者斗恶龙 VII 伊甸的战士们",
    "fe_awakening": "火焰纹章：觉醒",
    "fe_if": "火焰之纹章 if",
    "fire_emblem_echoes": "火焰纹章 外传",
    "inazuma_eleven_go": "闪电十一人 GO",
    "kid_icarus": "米特罗德：伊卡洛斯之翼",
    "kingdom_hearts_ddx": "王国之心 II",
    "kirby": "星之卡比 起源",
    "kirby_extra": "星之卡比 机械星球",
    "layton_miracle_mask": "雷顿教授与奇迹之假面",
    "legend_zelda": "塞尔达传说 三角力量",
    "luigi2": "路易吉洋馆 2",
    "mario_3d_land": "超级马力欧 3D 大陆",
    "mario_3d_land_2": "超级马里奥 3D 大陆 2",
    "mario_party": "超级马力欧派对",
    "mario_party_star_rush": "马力欧派对 星际大冲刺",
    "mega_man_legacy": "洛克人 遗产合集",
    "mh4g": "怪物猎人 4G",
    "mh_generations": "怪物猎人 世代",
    "minecraft": "Minecraft",
    "mk7": "马力欧卡丁车 7",
    "new_super_mario_bros_u": "新超级马力欧兄弟 U",
    "new_supermario_2d": "新超级马力欧兄弟 2D",
    "paper_mario": "纸片马力欧 RPG 超级贴纸",
    "pokemon_bw": "宝可梦 黑·白",
    "pokemon_conquest": "宝可梦 征服者",
    "pokemon_dppt": "宝可梦 钻石·珍珠·白金",
    "pokemon_emerald": "宝可梦 绿宝石",
    "pokemon_frlg": "宝可梦 红宝石·蓝宝石",
    "pokemon_gold_silver": "宝可梦 金·银",
    "pokemon_sm": "宝可梦太阳·月亮",
    "pokemon_xy": "宝可梦X·Y",
    "rayman": "雷曼：疯狂兔子 3D",
    "re_revelations": "生化危机 启示录",
    "repton": "雷顿教授与不可思议的小镇",
    "resident_evil": "生化危机：双重迷宫",
    "retro_prince": "波斯王子 时之砂",
    "smash_bros_3ds": "任天堂明星大乱斗 for 3DS",
    "sonic_3ds": "3D 索尼克",
    "sonic_generations_3ds": "索尼克 世代",
    "star_fox_64_3d": "星际火狐 64 3D",
    "steam_code": "CodeName: S.T.E.A.M.",
    "steel_diver_sub_wars": "钢铁潜水员 潜艇大战",
    "super_mario_64_3ds": "超级马力欧 64",
    "super_mario_maker_3ds": "超级马力欧制造者",
    "taiko_no_tatsujin_3ds": "太鼓达人 小龙人与不可思议的宝珠",
    "tomodachi_life": "朋友聚会 生活",
    "vc_nes_classics": "经典 FC 怀旧合集",
    "vc_snes_classics": "经典 SFC 怀旧合集",
    "we_football": "WE 足球",
    "yokai_watch": "妖怪手表 2",
    "yoshi_new_island": "耀西的新岛",
    "zelda_albw": "塞尔达传说：众神的三角力量2",
    "zelda_alttp": "塞尔达传说 三角力量 重制版",
    "zelda_links_awakening": "塞尔达传说 织梦岛",
    "zelda_minish_cap": "塞尔达传说 缩小帽",
    "zelda_mm3d": "塞尔达传说：梅祖拉的假面",
    "zelda_oot3d": "塞尔达传说：时之笛 3D",
    "zelda_oracle_ages": "塞尔达传说 时代之笛",
    "zelda_oracle_seasons": "塞尔达传说 四季之笛",
    # ---- 2026-10-05 第二波（并行 agent 采集）新增 23 款 ----
    "mario_tennis_open": "马力欧网球 Open",
    "mario_golf_world_tour": "马力欧高尔夫 World Tour",
    "warioware_gold": "瓦力欧制造 Gold",
    "bomberman_blitz": "炸弹人 Blitz",
    "mh3g": "怪物猎人 3G",
    "mhxx": "怪物猎人 XX",
    "terraria_3ds": "泰拉瑞亚 3DS",
    "shovel_knight": "铲子骑士",
    "ace_attorney_5": "逆转裁判 5",
    "ace_attorney_6": "逆转裁判 6",
    "persona_q": "女神异闻录 Q",
    "danganronpa_2": "弹丸论破 2",
    "zero_escape_vlr": "极限脱出 VLR",
    "pokemon_usum": "宝可梦 究极之日·月",
    "pokemon_mystery_dungeon_3ds": "宝可梦 不可思议的迷宫 3DS",
    "pokemon_shuffle": "宝可梦 Shuffle",
    "pokemon_rumble_world": "宝可梦 Rumble World",
    "pokemon_b2w2": "宝可梦 黑2·白2",
    "mgs3_snake_eater_3d": "合金装备 3D 食蛇者的复仇",
    "asphalt_3d": "狂野飙车 3D",
    "pacman_ghostly_adventures": "吃豆人人与幽灵历险记",
    "bionic_commando_3d": "生物突击队 3D",
    "angry_birds_trilogy": "愤怒的小鸟 三合一",
    # ---- 2026-10-05 第三波扩到 211 款，补 122 款的中文显示名 ----
    # 取名依据一律是**语料正文第一段自己写的中文/英文原名**（逐款读过语料），
    # 没有一款是凭印象编的；语料里既无中文译名也没有英文原名的（如 two_fast_4_gnomz）
    # 就用正文里出现的英文原名 —— 底屏显示英文总比显示 guide_xxx 强。
    # ⚠️ gyakuten_saiban_5/6 写「逆転裁判」（日文汉字），不是「逆转裁判」：
    #    它们和 ace_attorney_5/6 其实是**同一款游戏**（日式名 vs 西式名，
    #    语料被重复收了两遍），两边显示名都写简体「逆转裁判5」的话，
    #    底屏上会出现两个长得一模一样的条目，用户没法区分。
    "ace_attorney_4":                                 "逆转裁判4 阿波罗",
    "ace_combat_ah_legacy":                           "皇牌空战 突击地平线 遗产",
    "after_burner_2_3d":                              "After Burner II 3D",
    "anchor_point":                                   "Anchor Point",
    "another_code":                                   "Another Code",
    "binding_of_isaac_rebirth_3ds":                   "以撒的结合：重生",
    "bittrip_runner_3ds":                             "BIT.TRIP RUNNER",
    "blazblue_continuum_shift_2":                     "苍翼默示录：连续变换 II",
    "bokujou_origin_no_daichi":                       "牧场物语 起源的大地",
    "bokujou_tsunagaru_shintenchi":                   "牧场物语 连接新天地",
    "bokumono_3_villages":                            "牧场物语 三个小村的珍贵朋友",
    "boxboy":                                         "BoxBoy!",
    "brunswick_pro_bowling":                          "Brunswick Pro Bowling",
    "chase_cold_case_investigations":                 "Chase 冷案调查",
    "corpse_party_3ds":                               "尸体派对",
    "dai_gyakuten_saiban":                            "大逆转裁判 成步堂龙之介的冒险",
    "dai_gyakuten_saiban_2":                          "大逆转裁判2 成步堂龙之介的觉悟",
    "dasshutsu_adventure_akumu_no_shinigami_ressha":  "脱出冒险 噩梦的死神列车",
    "dasshutsu_adventure_kami_oroshi_no_uranai_ban":  "脱出冒险 神降临的占卜盘",
    "dasshutsu_adventure_kyuukousya_no_syoujo":       "脱出冒险 旧校舍的少女",
    "dasshutsu_adventure_majo_no_sumu_yakata":        "脱出冒险 魔女的栖馆",
    "dasshutsu_adventure_shiawase_no_akai_ishi":      "脱出冒险 幸福的红石",
    "dasshutsu_adventure_shuuen_no_kuroikiri":        "脱出冒险 终焉的黑雾",
    "dasshutsu_adventure_zetsubou_yosai":             "脱出冒险 绝望要塞",
    "dead_or_alive_dimensions":                       "死或生：多重维度",
    "detective_conan_marionette_symphony":            "名侦探柯南 傀儡交响曲",
    "detective_conan_phantom_rhapsody":               "名侦探柯南 幻影狂诗曲",
    "detective_pikachu":                              "名侦探皮卡丘",
    "dodonpachi_resurrection":                        "怒首领蜂 复活",
    "dooors":                                         "DOOORS",
    "dragon_quest_1_2_3_3d":                          "勇者斗恶龙 罗德三部曲 3D",
    "dragon_quest_monsters_2_iru_luca":               "勇者斗恶龙怪兽篇2 伊鲁与露卡",
    "dragon_quest_monsters_joker_3":                  "勇者斗恶龙怪兽篇 Joker3",
    "dragon_quest_monsters_terry_wonderland_3d":      "勇者斗恶龙怪兽篇 泰瑞的奇幻乐园 3D",
    "dragon_quest_viii_3d":                           "勇者斗恶龙VIII：被诅咒王的冒险",
    "escape_vektor":                                  "escapeVektor",
    "etrian_odyssey_4":                               "ay a 地牢4",
    "f1_2011":                                        "F1 2011",
    "fantasy_life":                                   "幻想生活",
    "fantasy_zone_3d":                                "Fantasy Zone 3D",
    "final_fantasy_explorers":                        "最终幻想 探索者",
    "fire_emblem_warriors":                           "火焰纹章无双",
    "fractured_soul":                                 "Fractured Soul",
    "galaxy_force_2_3d":                              "Galaxy Force II 3D",
    "gunman_clive":                                   "枪手克莱夫",
    "gyakuten_saiban_123_selection":                  "逆转裁判123 成步堂精选集",
    "gyakuten_saiban_5":                              "逆転裁判5",
    "gyakuten_saiban_6":                              "逆転裁判6",
    "hatsune_miku_project_diva_f":                    "初音未来 Project DIVA f",
    "hotel_dusk":                                     "Hotel Dusk",
    "hyrule_warriors_3d":                             "塞尔达无双 传奇",
    "kamen_rider_travelers_senki":                    "假面骑士 旅行者战记",
    "karous":                                         "Karous",
    "last_window":                                    "Last Window",
    "layton_vs_gyakuten_saiban":                      "雷顿教授 VS 逆转裁判",
    "laytons_mystery_journey":                        "雷顿教授的神秘旅程",
    "london_2012":                                    "伦敦奥运会 2012 官方游戏",
    "mario_sonic_rio_2016":                           "马力欧与索尼克 里约 2016",
    "mario_sports_superstars":                        "马力欧体育超级明星",
    "metal_slug_7":                                   "Metal Slug 7",
    "mighty_switch_force":                            "Mighty Switch Force!",
    "mutant_mudds":                                   "变异泥怪",
    "naruto_shippuden_3d_the_new_era":                "火影忍者疾风传 忍立体绘卷",
    "need_for_speed_the_run_3d":                      "极品飞车：亡命狂飙",
    "ninja_gaiden_dragon_sword":                      "忍者龙剑传 龙剑",
    "olliolli":                                       "OlliOlli",
    "one_piece_unlimited_cruise_sp":                  "海贼王 无限巡航 SP",
    "parascientific_escape_crossing_at_the_farthest_horizon": "超科学脱出 最果て的十字眼",
    "parascientific_escape_cruise_in_distant_seas":   "超科学脱出 绝海豪华客船",
    "parascientific_escape_gear_detective":           "超科学脱出 齿轮侦探",
    "persona_q2":                                     "女神异闻录Q2 新 cinema 迷宫",
    "picross_3d":                                     "Picross 3D",
    "pokemon_picross":                                "宝可梦 像素解谜",
    "pokemon_rumble_blast":                           "宝可梦 大乱战 爆裂",
    "project_diva_f_2nd":                             "初音未来 Project DIVA f 2nd",
    "project_mirai":                                  "初音未来 Project Mirai DX",
    "punch_out_3d":                                   "Punch-Out 3D",
    "pushmo":                                         "Pushmo",
    "radiant_historia_perfect_chronology":            "Radiant Historia 完美编年史",
    "rhythm_heaven":                                  "节奏天堂 Megamix",
    "rhythm_heaven_met":                              "节奏天国 The Best+",
    "ridge_racer_3d":                                 "山脊赛车 3D",
    "runbow":                                         "彩虹酷跑",
    "sega_3d_classics_collection":                    "SEGA 3D 经典合集",
    "sengoku_musou_chronicle":                        "战国无双 编年史",
    "sengoku_musou_chronicle_2nd":                    "战国无双 编年史 2nd",
    "sengoku_musou_chronicle_3":                      "战国无双 编年史 3",
    "shantae_pirates_curse_3ds":                      "山塔 海盗诅咒",
    # smt_iv 显示名比这里多一个空格 —— 见下方 ALIAS 里那条冲突说明
    "shin_megami_tensei_4":                           "真·女神转生IV",
    # smt_iv 显示名多一个空格（见下方 ALIAS 里那条冲突说明）
    "shin_sangokumusou_ds":                           "真·三国无双 DS 斗士之战",
    "smt_deep_no_m":                                  "真·女神转生IV FINAL",
    "smt_iv":                                         "真·女神转生 IV",
    "smt_iv_apocalypse":                              "真·女神转生IV Apocalypse",
    "smt_soul_hackers":                               "真·女神转生 恶魔召唤师 灵魂黑客",
    "smt_strange_journey":                            "真·女神转生 奇异之旅",
    "sonic_allstars_racing":                          "索尼克与全明星赛车 变形",
    "star_fox_command":                               "星际火狐 Command",
    "steamworld_dig":                                 "蒸汽世界 挖掘",
    "steamworld_dig_2":                               "蒸汽世界 挖掘2",
    "steamworld_heist":                               "蒸汽世界 抢劫",
    "stella_glow":                                    "Stella Glow",
    "story_of_seasons":                               "Story of Seasons 牧场物语",
    "style_boutique":                                 "New Style Boutique",
    "super_monkey_ball_3d":                           "超级猴子球 3D",
    "super_street_fighter_iv_3d":                     "超级街头霸王IV 3D",
    "tales_of_the_abyss":                             "深渊传说",
    "tekken_3d_prime_edition":                        "铁拳3D Prime Edition",
    "the_alliance_alive":                             "The Alliance Alive",
    "the_legend_of_dark_witch":                       "暗黑女巫传说",
    "theatrhythm_ff":                                 "Theatrhythm 最终幻想",
    "thunder_blade_3d":                               "Thunder Blade 3D",
    "toki_tori_3d":                                   "小鸡快跑3D",
    "twinbee_3d":                                     "TwinBee 3D",
    "two_fast_4_gnomz":                               "2 Fast 4 Gnomz",
    "vvvvvv_3ds":                                     "VVVVVV",
    "winter_sports_2012":                             "冬季运动 2012 感受激情",
    "wwe_all_stars":                                  "WWE 全明星",
    "xenoblade_chronicles_3d":                        "异度之刃 3D",
    "xevious_3d":                                     "Xevious 3D",
    "yugioh_zexal_duel_carnival":                     "游戏王 ZEXAL 激突 决斗嘉年华",
    "zelda_phantom_hourglass":                        "塞尔达传说 幻影沙漏",
    "zero_escape_zero_time_dilemma":                  "极限脱出 零时困境",
}

ALIAS = {
    "ace_attorney":      ["逆转裁判4", "逆转裁判 4", "逆转裁判"],
    "animal_crossing":   ["动物之森", "动森", "动物森格"],
    "fe_awakening":      ["火焰纹章觉醒", "火焰纹章 觉醒", "火焰纹章：觉醒", "火焰纹章", "FE觉醒"],
    "fe_if":             ["火焰之纹章", "FEIF", "火纹IF"],
    # ⚠️ 2026-10-05 修：「恶魔城」是抄错的 —— kid_icarus 其实是
    # 「米特罗德：伊卡洛斯之翼」（Metroid: Zero Mission），跟恶魔城没关系。
    # 留着它，问「恶魔城 历险记怎么打」时「恶魔城」会被当成游戏名剔除掉
    # （而 castlevania_vc 的别名更长、按先长后短应该赢），一旦它先命中，
    # 剩下「历险记怎么打」会跑飞。所以「恶魔城」现在独家归 castlevania_vc。
    "kid_icarus":        ["伊卡洛斯之翼", "伊卡洛斯", "米特罗德", "Kid Icarus", "pit", "PIT"],
    "castlevania_vc":    ["恶魔城", "恶魔城 历险记", "恶魔城历险记", "血之轮回", "Castlevania"],
    "kirby":             ["星之卡比", "卡比", "Kirby"],
    # 只留全称。「三角力量」不能留：它是「众神三角力量2」和
    # 「三角力量 重制版」的子串，而那两款也都在语料里。
    "legend_zelda":      ["塞尔达传说 三角力量"],
    "luigi2":            ["路易吉洋馆2", "路易吉洋馆 2", "路易吉洋馆", "洋馆", "路易吉"],
    "mario_3d_land":     ["超级马力欧 3D 大陆", "马力欧 3D 大陆", "马力欧3D大陆", "猫咪幽灵"],
    # 不留「3D 大陆」「3D大陆」：它们是本款自己的子串，
    # 会被 3D 大陆 2 抢走（而 3D 大陆 2 才是更长的那个名字）。
    "mario_3d_land_2":   ["3D 大陆 2", "3D大陆2", "猫咪幽灵2", "马力欧马里奥"],
    "mario_party":       ["马力欧派对", "超级马力欧派对", "Mario Party"],
    "mh4g":              ["怪物猎人4G", "怪物猎人 4G", "怪物猎人", "怪猎4G", "怪猎"],
    "minecraft":         ["Minecraft", "MC", "我的世界", "mc"],
    "mk7":               ["马力欧卡丁车7", "马力欧卡丁车 7", "马卡丁车7", "马力欧卡丁车", "卡丁车", "赛车"],
    "new_supermario_2d": ["新超级马力欧兄弟 2D", "新超级马力欧", "新马力欧", "新超级马力欧兄弟", "NSMB", "2D马力欧", "一百万金币"],
    "paper_mario":       ["纸片马力欧", "纸片马力欧RPG", "超级贴纸"],
    "pokemon_bw":        ["宝可梦黑", "宝可梦白", "宝可梦 黑", "宝可梦 白", "宝可梦黑白", "BW"],
    "pokemon_emerald":   ["宝可梦绿宝石", "绿宝石"],
    "pokemon_frlg":      ["宝可梦红宝石", "宝可梦蓝宝石", "红宝石", "蓝宝石", "红宝石蓝宝石", "RSE"],
    "pokemon_sm":        ["宝可梦太阳", "宝可梦 太阳", "宝可梦月亮", "太阳月亮", "SM"],
    "pokemon_xy":        ["宝可梦XY", "宝可梦X", "宝可梦Y", "宝可梦 X", "宝可梦 Y", "XY"],
    "rayman":            ["雷曼", "疯狂兔子", "Rayman"],
    "repton":            ["雷顿教授", "不可思议的小镇", "雷顿"],
    # ⚠️ 2026-10-05 修：去掉「启示录」—— 那是 re_revelations（生化危机：
    # 启示录 / Revelations），跟「生化危机：双重迷宫」是两款不同的游戏。
    # 留着它会让「生化危机 启示录 怎么解锁」里的「启示录」被本款吃掉，
    # 剩下「生化危机 怎么解锁」，范围直接跑到另一款上。
    "resident_evil":     ["生化危机", "双重迷宫"],
    "retro_prince":      ["波斯王子", "时之砂"],
    "steam_code":        ["STEAM", "蒸汽", "CodeName"],
    "we_football":       ["WE足球", "WE 足球", "实况足球"],
    "zelda_albw":        ["众神三角力量2", "众神三角力量 2", "ALBW"],
    "zelda_alttp":       ["塞尔达传说 三角力量 重制版", "重制版"],
    "zelda_mm3d":        ["梅祖拉的假面", "梅祖拉假面", "梅祖拉"],
    "zelda_oot3d":       ["时之笛3D", "时之笛 3D", "时之笛"],
    # ---- 2026-10-05 扩到 66 款时新增的 35 款 ----------------------------
    # 规则同旧款：只放**游戏名**，不放道具名（否则「沙之杖怎么用」会被
    # 整句消解成空串）；别名之间互不成为子串，冲突靠下面 `clean()` 自动删。
    "bravely_default":        ["勇气默示录", "勇气默示", "BRAVELY DEFAULT"],
    "danganronpa_1":          ["弹丸论破", "枪弹辩驳", "Danganronpa", "黑白熊"],
    "donkey_kong_country_returns_3d": ["森喜刚", "森喜刚之国", "大金刚",
                                       "DK Country Returns"],
    "dragon_quest_vii_3d":    ["勇者斗恶龙", "勇者斗恶龙 VII", "勇者斗恶龙7",
                               "DQ7", "伊甸的战士们"],
    "fire_emblem_echoes":     ["火焰纹章 外传", "火焰纹章外传", "圣魔光石"],
    "inazuma_eleven_go":      ["闪电十一人", "闪电十一人 GO", "闪电十一人GO",
                               "Inazuma Eleven"],
    "kingdom_hearts_ddx":     ["王国之心", "王国之心 II", "王国之心II", "KH2"],
    "kirby_extra":            ["星之卡比 机械星球", "卡比 机械星球", "卡比机械星球",
                               "卡比三重彩"],
    "layton_miracle_mask":    ["雷顿教授与奇迹之假面", "雷顿教授 奇迹之假面",
                               "奇迹之假面", "LaytonMM"],
    "mario_party_star_rush":  ["马力欧派对 星际大冲刺", "马力欧派对星际大冲刺",
                               "派对 星际大冲刺"],
    "mega_man_legacy":        ["洛克人", "洛克人 遗产", "Mega Man", "洛克人 X"],
    "mh_generations":         ["怪物猎人 世代", "怪物猎人世代", "怪物猎人X",
                               "怪猎X", "MHX"],
    "new_super_mario_bros_u": ["新超级马力欧兄弟 U", "新超级马力欧兄弟 U 版",
                               "新超级马力欧 U", "NSMBU"],
    "pokemon_conquest":       ["宝可梦 征服者", "宝可梦征服者", "征服者",
                               "Pokemon Conquest"],
    "pokemon_dppt":           ["宝可梦 钻石", "宝可梦 珍珠", "宝可梦 白金",
                               "宝可梦 钻石珍珠", "珍珠钻石", "DPPt"],
    "pokemon_gold_silver":    ["宝可梦 金", "宝可梦 银", "宝可梦 金·银",
                               "宝可梦金银", "宝可梦 金银", "GSC"],
    "re_revelations":         ["生化危机 启示录", "生化危机启示录",
                               "生化危机 启示", "Revelations"],
    "smash_bros_3ds":         ["任天堂明星大乱斗", "明星大乱斗", "大乱斗", "Smash"],
    "sonic_3ds":              ["3D 索尼克", "索尼克3D", "索尼克"],
    "sonic_generations_3ds":  ["索尼克 世代", "索尼克世代", "索尼克 3D 世代"],
    "star_fox_64_3d":         ["星际火狐", "星际火狐 64", "星际火狐64", "Star Fox"],
    "steel_diver_sub_wars":   ["钢铁潜水员", "钢铁潜水员 潜艇大战", "潜艇大战"],
    "super_mario_64_3ds":     ["超级马力欧 64", "超级马力欧64", "马力欧 64", "SM64"],
    "super_mario_maker_3ds":  ["超级马力欧制造者", "马力欧制造者", "超级马力欧制造",
                               "SMM"],
    "taiko_no_tatsujin_3ds":  ["太鼓达人", "太鼓の達人", "太鼓", "Taiko"],
    "tomodachi_life":         ["朋友聚会", "朋友聚会 生活", "朋友聚会生活",
                               "Tomodachi"],
    # ⚠️ 这两款的名字别写成「xx 怀旧合集」这种互为子串的形式：
    # 「SFC 怀旧合集」里含「FC 怀旧合集」，clean() 只能删一个，
    # 而删了长的是白瞎一个正确别名。所以两边都用「经典 xx 合集」开头。
    "vc_nes_classics":        ["经典 FC 合集", "Virtual Console", "VC 怀旧"],
    "vc_snes_classics":       ["经典 SFC 合集", "魂斗罗 3", "魂斗罗精神",
                               "Super Famicom"],
    "yokai_watch":            ["妖怪手表", "妖怪手表 2", "妖怪手表2", "妖怪"],
    "yoshi_new_island":       ["耀西的新岛", "耀西岛", "耀西", "Yoshi"],
    "zelda_links_awakening":  ["塞尔达传说 织梦岛", "织梦岛", "织梦岛 DX",
                               "织梦岛DX"],
    "zelda_minish_cap":       ["塞尔达传说 缩小帽", "缩小帽", "Minish Cap"],
    "zelda_oracle_ages":      ["塞尔达传说 时代之笛", "时代之笛", "Oracle of Ages"],
    "zelda_oracle_seasons":   ["塞尔达传说 四季之笛", "四季之笛",
                               "Oracle of Seasons"],
    # ---- 第二波新增 23 款的别名 ----
    "mario_tennis_open":      ["马力欧网球", "马里奥网球", "Mario Tennis"],
    "mario_golf_world_tour":  ["马力欧高尔夫", "马里奥高尔夫", "Mario Golf"],
    "warioware_gold":         ["瓦力欧制造", "WarioWare"],
    "bomberman_blitz":        ["炸弹人", "炸弹人 Blitz", "Bomberman"],
    "mh3g":                   ["怪物猎人3G", "怪物猎人 3G", "怪物猎人Tri",
                               "怪猎3G"],
    "mhxx":                   ["怪物猎人XX", "怪物猎人 XX", "怪物猎人GU",
                               "怪物猎人Generations", "怪猎XX"],
    "terraria_3ds":           ["泰拉瑞亚", "Terraria"],
    "shovel_knight":          ["铲子骑士", "Shovel Knight"],
    "ace_attorney_5":         ["逆转裁判5", "逆转裁判 5", "逆转裁判 V"],
    "ace_attorney_6":         ["逆转裁判6", "逆转裁判 6", "逆转裁判 VI"],
    "persona_q":              ["女神异闻录Q", "女神异闻录 Q", "Persona Q"],
    "danganronpa_2":          ["弹丸论破2", "弹丸论破 2", "超级弹丸论破2"],
    "zero_escape_vlr":        ["极限脱出", "virtue's last reward", "零时刻",
                               "VLR"],
    "pokemon_usum":           ["宝可梦 究极之日", "宝可梦 究极之月",
                               "宝可梦 究极", "究极之日", "USUM"],
    "pokemon_mystery_dungeon_3ds": ["宝可梦 不可思议的迷宫", "不可思议的迷宫",
                                    "宝可梦迷宫", "迷宫 3DS"],
    "pokemon_shuffle":        ["宝可梦 Shuffle", "宝可梦碰撞",
                               "Pokemon Shuffle"],
    "pokemon_rumble_world":   ["宝可梦 Rumble World", "宝可梦 乱战",
                               "Pokemon Rumble World"],
    "pokemon_b2w2":           ["宝可梦黑2", "宝可梦白2", "宝可梦 黑2",
                               "宝可梦 白2", "宝可梦 黑白2", "B2W2"],
    "mgs3_snake_eater_3d":    ["合金装备", "合金装备3D", "MGS3", "Snake Eater",
                               "食蛇者"],
    "asphalt_3d":             ["狂野飙车", "狂野飙车 3D", "Asphalt 3D",
                               "Asphalt3D"],
    "pacman_ghostly_adventures": ["吃豆人", "吃豆人 幽灵历险记",
                                  "Pac-Man"],
    "bionic_commando_3d":     ["生物突击队", "生化臂", "Bionic Commando",
                               "High Voltage"],
    "angry_birds_trilogy":    ["愤怒的小鸟", "Angry Birds"],
    # ---- 2026-10-05 第三波（扩到 211 款）新增 122 款的别名 ----
    # 规则同上：只放**该游戏独有**的片段，且尽量让别名之间是**前缀**关系
    # （前缀关系由 clean() 自动放过，只有"中间子串"才判真冲突）。
    # ⚠️ 两处特别处理：
    #   · gyakuten_saiban_5/6 用日文汉字「逆転裁判」开头 —— 它们和
    #     ace_attorney_5/6 是**同一款游戏**（日式名 vs 西式名，语料被重复收了），
    #     两边都写「逆转裁判5」会让同一串别名指向两个 gid，先长后短也分不开。
    #     用日文写法在字面上一分为二，各自的表才干净。
    #   · 牧场物语三款（origin/tsunagaru/bokumono）共用前缀「牧场物语」，
    #     统一靠「牧场物语 X」的长别名撑住，短的「牧场物语」不单独出现。
    "ace_attorney_4":         ["逆转裁判4 阿波罗", "逆转裁判4 Apollo Justice",
                               "Apollo Justice", "阿波罗·杰特", "Apollo"],
    "ace_combat_ah_legacy":   ["皇牌空战 突击地平线 遗产",
                               "皇牌空战 AH 遗产", "Ace Combat 突击地平线",
                               "Ace Combat Assault Horizon Legacy",
                               "皇牌空战 突击地平线"],
    "after_burner_2_3d":      ["After Burner II 3D", "3D After Burner II",
                               "After Burner II", "After Burner",
                               "After Burner 2"],
    "anchor_point":           ["Anchor Point", "アンカーポイント",
                               "AnchorPoint", "Anchor Point 悬疑 ADV"],
    "another_code":           ["Another Code", "アナザーコード",
                               "Another Code 二つの記憶", "二つの記憶",
                               "Another Code 两个记忆"],
    "binding_of_isaac_rebirth_3ds": ["以撒的结合 重生", "以撒的结合",
                                     "The Binding of Isaac Rebirth",
                                     "Binding of Isaac", "以撒"],
    "bittrip_runner_3ds":     ["BIT.TRIP RUNNER", "Bit.Trip Runner",
                               "BIT TRIP RUNNER", "BIT.TRIP SAGA",
                               "Bit.Trip Runner 跑酷"],
    "blazblue_continuum_shift_2": ["苍翼默示录 连续变换 II",
                                   "苍翼默示录 连续变换II",
                                   "BlazBlue Continuum Shift II",
                                   "苍翼默示录 连续变换", "苍翼默示录"],
    "bokujou_origin_no_daichi": ["牧场物语 起源的大地", "牧场物语起源的大地",
                                 "牧场物语起源", "Bokujou Origin no Daichi",
                                 "牧场物语 起源"],
    "bokujou_tsunagaru_shintenchi": ["牧场物语 连接新天地",
                                     "牧场物语连接新天地",
                                     "Bokujou Tsunagaru Shintenchi",
                                     "牧场物语 结界新天地"],
    "bokumono_3_villages":    ["牧场物语 三个小村的珍贵朋友",
                               "牧场物语三个小村的珍贵朋友",
                               "牧场物语 三个小村", "Bokumono 3 Villages",
                               "牧场物语 珍贵朋友"],
    "boxboy":                 ["BoxBoy", "BoxBoy!", "Qbby", "方块男孩",
                               "Boxboy! 方块"],
    "brunswick_pro_bowling":  ["Brunswick Pro Bowling", "Brunswick",
                               "保龄球", "Pro Bowling"],
    "chase_cold_case_investigations": ["Chase 冷案调查", "Chase Cold Case",
                                       "冷案调查", "Distant Memories",
                                       "Chase Cold Case Investigations"],
    "corpse_party_3ds":       ["尸体派对", "尸体派对 血之覆盖", "Corpse Party",
                               "コープスパーティ", "尸体派对3DS"],
    "dai_gyakuten_saiban":    ["大逆转裁判", "大逆转裁判 成步堂龙之介的冒险",
                               "成步堂龙之介", "Dai Gyakuten Saiban",
                               "大逆转裁判 冒险"],
    "dai_gyakuten_saiban_2":  ["大逆转裁判2", "大逆转裁判2 成步堂龙之介的觉悟",
                               "成步堂龙之介的觉悟", "Dai Gyakuten Saiban 2",
                               "大逆转裁判 觉悟"],
    "dasshutsu_adventure_akumu_no_shinigami_ressha": ["脱出冒险 噩梦的死神列车",
                                                       "噩梦的死神列车",
                                                       "脱出冒险 死神列车",
                                                       "Escape Adventure Death God"],
    "dasshutsu_adventure_kami_oroshi_no_uranai_ban": ["脱出冒险 神降临的占卜盘",
                                                       "神降临的占卜盘",
                                                       "脱出冒险 占卜盘",
                                                       "脱出冒险 神降臨"],
    "dasshutsu_adventure_kyuukousya_no_syoujo": ["脱出冒险 旧校舎の少女",
                                                   "脱出冒险 旧校舍的少女",
                                                   "旧校舍的少女", "旧校舎",
                                                   "脱出冒险 旧校舎"],
    "dasshutsu_adventure_majo_no_sumu_yakata": ["脱出冒险 魔女的栖馆",
                                                  "魔女的栖馆", "脱出冒险 魔女",
                                                  "Escape Adventure Mansion"],
    "dasshutsu_adventure_shiawase_no_akai_ishi": ["脱出冒险 幸福的红石",
                                                    "幸福的红石", "脱出冒险 赤い石",
                                                    "脱出冒险 红石"],
    "dasshutsu_adventure_shuuen_no_kuroikiri": ["脱出冒险 终焉的黑雾",
                                                  "终焉的黑雾", "脱出冒险 黒霧",
                                                  "脱出冒险 黑雾"],
    "dasshutsu_adventure_zetsubou_yosai": ["脱出冒险 绝望要塞", "绝望要塞",
                                            "脱出アドベンチャー 絶望要塞",
                                            "脱出冒险 要塞"],
    "dead_or_alive_dimensions": ["死或生 多重维度", "死或生 Dimensions",
                                 "DOA Dimensions", "死或生 维度",
                                 "Dead or Alive Dimensions"],
    "detective_conan_marionette_symphony": ["名侦探柯南 傀儡交响曲",
                                             "柯南 傀儡交响曲",
                                             "Detective Conan Marionette",
                                             "コナン マリオネット交響曲"],
    "detective_conan_phantom_rhapsody": ["名侦探柯南 幻影狂诗曲",
                                          "柯南 幻影狂诗曲",
                                          "Detective Conan Phantom Rhapsody",
                                          "コナン ファントム狂詩曲"],
    "detective_pikachu":      ["名侦探皮卡丘", "Detective Pikachu",
                               "ピカチュウ 探偵", "皮卡丘 侦探"],
    "dodonpachi_resurrection": ["怒首领蜂 复活", "怒首领蜂复活",
                                "DoDonPachi Resurrection", "怒首领蜂",
                                "怒首领蜂  redemption"],
    "dooors":                 ["DOOORS", "DOOORS room escape", "DOOORS 脱出",
                               "脱出 解谜"],
    "dragon_quest_1_2_3_3d":  ["勇者斗恶龙 罗德三部曲", "DQ 罗德三部曲",
                               "勇者斗恶龙 I II III", "罗德三部曲",
                               "勇者斗恶龙 1 2 3"],
    "dragon_quest_monsters_2_iru_luca": ["勇者斗恶龙怪兽篇2", "伊鲁与露卡",
                                          "DQ Monsters 2", "勇者斗恶龙怪兽篇 2",
                                          "不可思议钥匙"],
    "dragon_quest_monsters_joker_3": ["勇者斗恶龙怪兽篇 Joker3", "DQ Joker3",
                                       "Joker3", "勇者斗恶龙怪兽篇Joker3",
                                       "怪兽篇 Joker3"],
    "dragon_quest_monsters_terry_wonderland_3d": ["勇者斗恶龙怪兽篇 泰瑞的奇幻乐园",
                                                    "泰瑞的奇幻乐园",
                                                    "DQ Monsters Terry",
                                                    "勇者斗恶龙怪兽篇 泰瑞"],
    "dragon_quest_viii_3d":   ["勇者斗恶龙VIII 3D", "勇者斗恶龙 VIII 3D",
                               "DQ8 3D", "被诅咒王的冒险", "勇者斗恶龙8 3D"],
    "escape_vektor":          ["escapeVektor", "Escape Vektor", "Vektor",
                               "escape Vektor"],
    "etrian_odyssey_4":       ["ay a 地牢4", "ay a 地牢", "Etrian Odyssey IV",
                               "Etrian Odyssey 4", "ay a 地牢 IV"],
    "f1_2011":                ["F1 2011", "F1", "一级方程式 2011", "F1 赛车 2011"],
    "fantasy_life":           ["幻想生活", "Fantasy Life", "FantasyLife",
                               "幻想生活 职业"],
    "fantasy_zone_3d":        ["Fantasy Zone 3D", "3D Fantasy Zone",
                               "Opa-Opa Bros", "Fantasy Zone", "FantasyZone"],
    "final_fantasy_explorers": ["最终幻想 探索者", "最终幻想探索者",
                                 "Final Fantasy Explorers", "FF 探索者",
                                 "最终幻想 探险者"],
    "fire_emblem_warriors":   ["火焰纹章无双", "火焰之纹章 无双",
                               "Fire Emblem Warriors", "火焰纹章 无双"],
    "fractured_soul":         ["Fractured Soul", "FracturedSoul", "破碎灵魂",
                               "Fractured Soul 3DS"],
    "galaxy_force_2_3d":      ["Galaxy Force II 3D", "3D Galaxy Force II",
                               "Galaxy Force II", "Galaxy Force 2"],
    "gunman_clive":           ["枪手克莱夫", "Gunman Clive", "Clive",
                               "Gunman Clive 西部"],
    "gyakuten_saiban_123_selection": ["逆转裁判123", "逆转裁判 123",
                                       "逆转裁判123 成步堂精选集",
                                       "成步堂精选集", "Gyakuten Saiban 123"],
    # ⚠️ 用日文汉字「逆転」，与 ace_attorney_5 的简体「逆转裁判 V」字面分开。
    # ⚠️ 两条都不能留裸的「逆転裁判」：C 端 gkb_split_game 是 `al > best_len`
    #    **严格**最长匹配，等长只认表序第一个 —— 两条裸串落在两个 gid 上，
    #    「逆転裁判」问句会永远归给表序靠前那个（5），6 永远认不出来。
    #    所以两边都写死带编号的「逆転裁判5/6」。
    "gyakuten_saiban_5":      ["逆転裁判5", "逆転裁判 V", "Gyakuten Saiban 5"],
    "gyakuten_saiban_6":      ["逆転裁判6", "逆転裁判 VI", "Gyakuten Saiban 6"],
    "hatsune_miku_project_diva_f": ["初音未来 Project DIVA f",
                                     "Project DIVA f", "初音ミク Project DIVA f",
                                     "Hatsune Miku Project DIVA f",
                                     "初音未来 DIVA f"],
    "hotel_dusk":             ["Hotel Dusk", "ホテルダスク", "Hotel Dusk ルーム402",
                               " hotels 侦探"],
    "hyrule_warriors_3d":     ["塞尔达无双 传奇", "塞尔达无双",
                               "Hyrule Warriors Legends", "塞尔达无双 传说"],
    "kamen_rider_travelers_senki": ["假面骑士 旅行者战记",
                                     "假面骑士旅行者战记", "假面骑士 铠武",
                                     "仮面ライダー トラベラーズ"],
    "karous":                 ["Karous", "Karous The Beast of Re Eden",
                               "The Beast of Re Eden", "KAROUS"],
    "last_window":            ["Last Window", "ラストウィンドウ",
                               "Last Window 声の聖域", "last window 侦探"],
    "layton_vs_gyakuten_saiban": ["雷顿教授 VS 逆转裁判", "雷顿教授VS逆転裁判",
                                   "Layton vs Gyakuten Saiban",
                                   "雷顿教授 vs 逆转裁判", "雷顿 逆转裁判"],
    "laytons_mystery_journey": ["雷顿教授的神秘旅程", "卡图莉耶与百万富翁的阴谋",
                                 "LAYTONS MYSTERY JOURNEY",
                                 "雷顿教授 神秘旅程", "雷顿教授 百万富翁"],
    "london_2012":            ["伦敦奥运会 2012", "2012 伦敦奥运",
                               "London 2012", "伦敦奥运 2012"],
    "mario_sonic_rio_2016":   ["马力欧与索尼克 里约 2016", "马力欧与索尼克里约奥运会",
                               "Mario Sonic Rio 2016", "里约 2016",
                               "马力欧与索尼克 里约"],
    "mario_sports_superstars": ["马力欧体育超级明星", "马力欧体育 超级明星",
                                 "Mario Sports Superstars", "马力欧体育"],
    "metal_slug_7":           ["Metal Slug 7", "メタルスラッグ 7",
                               "合金弹头7", "Metal Slug"],
    "mighty_switch_force":    ["Mighty Switch Force", "Mighty Switch Force!",
                               "强力开关", "Mighty Switch Force 3D"],
    "mutant_mudds":           ["变异泥怪", "Mutant Mudds", "MutantMudds",
                               "变异泥怪 平台"],
    "naruto_shippuden_3d_the_new_era": ["火影忍者疾风传 忍立体绘卷",
                                         "火影忍者 疾风传 忍立体绘卷",
                                         "最强忍界决战", "Naruto 3D 新时代"],
    "need_for_speed_the_run_3d": ["极品飞车 亡命狂飙", "极品飞车亡命狂飙",
                                   "Need for Speed The Run", "NFS 亡命狂飙",
                                   "极品飞车 狂飙"],
    "ninja_gaiden_dragon_sword": ["忍者龙剑传 龙剑", "忍者龙剑传龙剑",
                                   "Ninja Gaiden Dragon Sword",
                                   "忍者龙剑传 剑"],
    "olliolli":               ["OlliOlli", "滑板", "OlliOlli 3DS",
                               "OlliOlli 超难"],
    "one_piece_unlimited_cruise_sp": ["海贼王 无限巡航 SP", "ONE PIECE 无限巡航 SP",
                                       "One Piece Unlimited Cruise SP",
                                       "无限巡航 SP", "海贼王 无限巡航"],
    "parascientific_escape_crossing_at_the_farthest_horizon": ["超科学脱出 最果てのクロスアイズ",
                                                                 "超科学脱出 最果て",
                                                                 "Parascientific Escape Crossing",
                                                                 "超科学脱出 最果ての"],
    "parascientific_escape_cruise_in_distant_seas": ["超科学脱出 絶海の豪華客船",
                                                       "超科学脱出 绝海豪华客船",
                                                       "Parascientific Escape Cruise",
                                                       "超科学脱出 绝海"],
    "parascientific_escape_gear_detective": ["超科学脱出 齿轮侦探",
                                              "超科学脱出 アドベンチャー",
                                              "Parascientific Escape Gear Detective",
                                              "超科学脱出 齿轮"],
    "persona_q2":             ["女神异闻录Q2", "女神异闻录 Q2", "Persona Q2",
                               "新 cinema 迷宫", "女神异闻录 Q2 迷宫"],
    "picross_3d":             ["Picross 3D", "立体 Picross", "立体数独",
                               "Picross 3D 立体"],
    "pokemon_picross":        ["宝可梦 像素解谜", "宝可梦 Picross",
                               "Pokemon Picross", "宝可梦数织", "宝可梦 拼图"],
    "pokemon_rumble_blast":   ["宝可梦 大乱战 爆裂", "宝可梦大乱战爆裂",
                               "Pokemon Rumble Blast", "宝可梦大乱战",
                               "发条玩具宝可梦"],
    "project_diva_f_2nd":     ["初音未来 Project DIVA f 2nd",
                               "Project DIVA f 2nd",
                               "Hatsune Miku Project DIVA f 2nd",
                               "初音未来 DIVA f 2nd"],
    "project_mirai":          ["初音未来 Project Mirai DX", "Project Mirai",
                               "Hatsune Miku Project Mirai",
                               "初音未来 未来组装"],
    "punch_out_3d":           ["Punch-Out 3D", "Mike Tyson Punch Out",
                               "Punch Out 3D", "拳击 3D"],
    "pushmo":                 ["Pushmo", "Pullblox", "推箱解谜", "Pushmo 推箱"],
    "radiant_historia_perfect_chronology": ["Radiant Historia 完美编年史",
                                             "Radiant Historia", "完美编年史",
                                             "光辉编年史"],
    "rhythm_heaven":          ["节奏天堂 Megamix", "Rhythm Heaven Megamix",
                               "Rhythm Paradise Megamix", "节奏天堂",
                               "リズム天国 ザ・ベスト"],
    "rhythm_heaven_met":      ["节奏天国 The Best+", "リズム天国 ザ・ベスト+",
                               "Rhythm Heaven The Best", "节奏天国 最佳",
                               "Rhythm Paradise"],
    "ridge_racer_3d":         ["山脊赛车 3D", "山脊赛车3D", "Ridge Racer 3D",
                               "山脊赛车"],
    "runbow":                 ["彩虹酷跑", "Runbow", "Run Bow", "彩虹酷跑 Runbow"],
    "sega_3d_classics_collection": ["SEGA 3D 经典合集", "3D 经典合集",
                                     "Sega 3D Classics", "SEGA 3D 经典"],
    "sengoku_musou_chronicle": ["战国无双 编年史", "战国无双编年史",
                                 "Sengoku Musou Chronicle", "战国无双 编年"],
    "sengoku_musou_chronicle_2nd": ["战国无双 编年史 2nd", "战国无双编年史2nd",
                                     "Sengoku Musou Chronicle 2nd",
                                     "战国无双 编年史 2"],
    "sengoku_musou_chronicle_3": ["战国无双 编年史 3", "战国无双编年史3",
                                   "Sengoku Musou Chronicle 3",
                                   "战国无双 编年史 3 代"],
    "shantae_pirates_curse_3ds": ["山塔海盗诅咒", "Shantae and the Pirates Curse",
                                   "半精灵 海盗诅咒", "Shantae",
                                   "山塔 海盗诅咒"],
    # ⚠️ shin_megami_tensei_4 与 smt_iv 是**同一款游戏**（语料收了两遍），
    #    显示名与别名都必须靠字面差异分开，否则：
    #    · 底屏出现两个一模一样的条目，用户分不清；
    #    · 「真·女神转生IV」这条裸串同时落在两个 gid 上，而 C 端是严格最长
    #      匹配，裸串等长 → 只认表序第一个 → 另一个永远认不出。
    #    这里让旧款 shin 独占中文裸写法，新款 smt_iv 全用带空格/带后缀的写法。
    "shin_megami_tensei_4":   ["真·女神转生IV", "真女神转生4", "SMT4",
                               "真·女神转生 IV 3DS"],
    "shin_sangokumusou_ds":   ["真·三国无双 DS", "斗士之战", "真三国无双DS斗士之战",
                               "Shin Sangokumusou DS", "真·三国无双 DS 斗士"],
    "smt_deep_no_m":          ["真·女神转生IV FINAL", "真·女神转生IV最终篇",
                               "SMT IV FINAL", "真·女神转生 IV FINAL",
                               "Deep No M"],
    "smt_iv":                 ["真·女神转生 IV", "真女神转生IV", "SMT IV"],
    "smt_iv_apocalypse":      ["真·女神转生IV Apocalypse", "真·女神转生IV 觉醒",
                               "SMT IV Apocalypse", "SMT4 Apocalypse",
                               "真·女神转生IV 最终"],
    "smt_soul_hackers":       ["真·女神转生 恶魔召唤师 灵魂黑客",
                               "恶魔召唤师 灵魂黑客", "Devil Summoner Soul Hackers",
                               "灵魂黑客", "真·女神转生 灵魂黑客"],
    "smt_strange_journey":    ["真·女神转生 奇异之旅", "真·女神转生Strange Journey",
                               "Strange Journey", "SMT Strange Journey",
                               "奇异之旅"],
    "sonic_allstars_racing":  ["索尼克与全明星赛车", "索尼克全明星赛车 变形",
                               "Sonic All Stars Racing", "索尼克 赛车",
                               "索尼克与全明星赛车 变形"],
    "star_fox_command":       ["星际火狐 Command", "星际火狐Command",
                               "Star Fox Command", "星际火狐 命令"],
    "steamworld_dig":         ["蒸汽世界 挖掘", "SteamWorld Dig",
                               "蒸汽世界挖掘", "蒸汽世界 Dig"],
    "steamworld_dig_2":       ["蒸汽世界 挖掘2", "SteamWorld Dig 2",
                               "蒸汽世界挖掘2", "蒸汽世界 Dig 2"],
    "steamworld_heist":       ["蒸汽世界 抢劫", "SteamWorld Heist",
                               "蒸汽世界抢劫", "蒸汽世界 Heist"],
    "stella_glow":            ["Stella Glow", "ステラ グロウ", "星辉",
                               "Stella Glow 星之辉"],
    "story_of_seasons":       ["Story of Seasons", "牧场物语 Story of Seasons",
                               "牧场物语 四季", "牧场物语 四季旅程"],
    "style_boutique":         ["New Style Boutique", "Style Savvy",
                               "New Style Boutique 换装", "换装 经营 ",
                               "换装 Boutique"],
    "super_monkey_ball_3d":   ["超级猴子球 3D", "超级猴子球3D",
                               "Super Monkey Ball 3D", "猴子球"],
    "super_street_fighter_iv_3d": ["超级街头霸王IV 3D", "超级街霸IV 3D",
                                    "Super Street Fighter IV 3D", "超级街霸",
                                    "超级街头霸王 3D"],
    "tales_of_the_abyss":     ["深渊传说", "Tales of the Abyss", "TOT A",
                               "深渊传说 音素"],
    "tekken_3d_prime_edition": ["铁拳3D", "铁拳 3D Prime Edition",
                                 "Tekken 3D Prime Edition", "铁拳6 血之复仇",
                                 "铁拳 3D"],
    "the_alliance_alive":     ["The Alliance Alive", "同盟 Alive",
                               "同盟存活", "The Alliance Alive 3DS"],
    "the_legend_of_dark_witch": ["暗黑女巫传说", "The Legend of Dark Witch",
                                  "魔法少女 传说", "暗黑女巫"],
    "theatrhythm_ff":         ["Theatrhythm Final Fantasy", "最终幻想 节奏剧场",
                               "FF 节奏剧场", "最终幻想 节奏"],
    "thunder_blade_3d":       ["Thunder Blade 3D", "3D Thunder Blade",
                               "Thunder Blade", "Thunder Blade 3D 空战"],
    "toki_tori_3d":           ["小鸡快跑3D", "Toki Tori 3D", "小鸡快跑",
                               "Toki Tori 小鸡"],
    "twinbee_3d":             ["TwinBee 3D", "3D 经典 TwinBee",
                               "TwinBee", "TwinBee 3D 射击"],
    "two_fast_4_gnomz":       ["2 Fast 4 Gnomz", "2FAST4GNOMZ", "小矮人",
                               "2 Fast 4 Gnomz 跑酷"],
    "vvvvvv_3ds":             ["VVVVVV", "VVVVVV 3DS", "Captain Viridian",
                               "重力翻转"],
    "winter_sports_2012":     ["冬季运动 2012", "冬季运动2012",
                               "Winter Sports 2012", "冬季运动 感受激情"],
    "wwe_all_stars":          ["WWE 全明星", "WWE All Stars", "WWE",
                               "WWE 摔角"],
    "xenoblade_chronicles_3d": ["异度之刃 3D", "异度之刃", "Xenoblade Chronicles 3D",
                                 "异度神剑 3D", "异度之刃 移植"],
    "xevious_3d":             ["Xevious 3D", "3D 经典 Xevious", "Xevious",
                               "ゼビウス 3D"],
    "yugioh_zexal_duel_carnival": ["游戏王 ZEXAL 激突 决斗嘉年华",
                                    "游戏王ZEXAL 激突决斗嘉年华",
                                    "游戏王 决斗嘉年华", "Yu-Gi-Oh Zexal",
                                    "游戏王 激突"],
    "zelda_phantom_hourglass": ["塞尔达传说 幻影沙漏", "幻影沙漏",
                                 "Phantom Hourglass", "塞尔达 幻影沙漏"],
    "zero_escape_zero_time_dilemma": ["极限脱出 零时困境",
                                       "Zero Escape Zero Time Dilemma",
                                       "零时困境", "Zero Time Dilemma",
                                       "极限脱出 零时"],
}


def collect():
    """返回 [(基名, 正式名, 别名list)]，顺序 = sorted() = game_id 顺序。"""
    out = []
    for f in sorted(glob.glob(os.path.join(CORPUS, "guide_*.txt"))):
        stem = os.path.basename(f)[len("guide_"):-len(".txt")]
        txt = open(f, encoding="utf-8").read()
        m = re.search(r"《([^》]+)》", txt)
        auto = m.group(1) if m else None
        name = NAME.get(stem, auto or stem)
        out.append((stem, name, list(ALIAS.get(stem, []))))
    return out


def clean(items):
    """删掉会互相抢问句的短别名，返回被删的清单。

    为什么自动删而不是报错让人手改：
      别名的作用是从问句里**剔掉游戏名**，只留提问部分。
      别名 A 被别的游戏（较长的）B 包含、且 A 不是 B 的前缀时，
      无论先长后短还是先短后长，A 都会把 B 的名字吃掉一块，
      剩下的提问就残缺了 —— 这种错**不会崩，只是检索跑飞**。
      短别名（通常是简称）比长别名更容易撞车，所以删短不删长。
    """
    pool = []
    for stem, name, al in items:
        pool.append((name, stem, name))
        for a in al:
            pool.append((a, stem, a))

    drop = []
    for i, (stem, name, al) in enumerate(items):
        if not al:
            continue
        keep = []
        for a in al:
            bad = None
            for other, owner, _ in pool:
                if owner == stem or len(other) < 3:
                    continue
                if other in a and not a.startswith(other):
                    bad = (other, owner)
                    break
            if bad:
                drop.append((stem, a, bad[0], bad[1]))
            else:
                keep.append(a)
        items[i] = (stem, name, keep)
    return drop


def _alias_cells(i, al):
    """一个 gid 的别名会生成几个 `{ "别名", gid }` 元素（**逗号由调用方加**）。"""
    return ['{ "%s", %d }' % (a, i) for a in al]


def _sub_block(src, decl, inner):
    """把 `decl ... { <inner> };` 里花括号之间的内容整块换掉。"""
    pat = re.compile(r"(" + decl + r")(.*?)(\};)", re.S)

    def f(m):
        return m.group(1) + "\n" + inner + m.group(3)
    return pat.sub(f, src, count=1)


def _sub_py_block(src, head, tail_re, inner):
    """把 `head` 之后、下一个行首 `tail_re` 之前的内容换掉（头尾都保留）。

    ⚠️ 不能用 `src.index("]")` 找尾巴：GAME_ALIAS 的每个值都是
    `["...", "..."]`，行内本来就有 `]`。必须**行首锚定**才找得对。
    用错的下场是替换范围从 GAME_ORDER 一路吃到 GAME_ALIAS 中间，
    两个表合体成语法错误的怪东西。
    """
    i = src.index(head)
    m = re.compile(tail_re, re.M).search(src, i + len(head))
    if m is None:
        raise RuntimeError("找不到 %r 的结尾标记 %r" % (head, tail_re))
    return src[:i + len(head)] + "\n" + inner + src[m.start():]


def render_game_order(items, width=68):
    """生成 guide_search.py 里 GAME_ORDER 的正文（与 --print 出来的同格式）。"""
    lines, cur = [], "   "
    for stem, _, _ in items:
        t = ' "%s",' % stem
        if len(cur) + len(t) > width:
            lines.append(cur)
            cur = "   "
        cur += t
    if cur.strip():
        lines.append(cur)
    return "\n".join(lines) + "\n"


def render_game_alias(items):
    """生成 guide_search.py 里 GAME_ALIAS 的正文。

    ⚠️ 别名列表为空的款**必须兜底填显示名**。否则那款游戏的 GAME_ALIAS 是
    `[]`，split_game() 遍历它匹配不到任何东西 —— 玩家带游戏名提问时
    游戏名剔不掉，会被当成普通词参与打分，把别的游戏的段落挤上来。
    症状和「别名表停在 31 项」一模一样：不报错，只是认不出游戏。

    ⚠️ 别名列表**非空时原样输出，不重排、不插显示名**。顺序会影响
    split_game 的匹配结果（长别名先匹配才能压住短别名），
    自作主张把显示名提到第一位会**悄悄改掉现有 89 款的匹配行为**。
    这一版只做「空的兜底」，保证对已同步的款是**幂等**的。
    """
    lines = []
    for stem, name, al in items:
        use = list(al) if al else [name]
        cells = ", ".join('"%s"' % a for a in use)
        lines.append('    "%s": [%s],' % (stem, cells))
    return "\n".join(lines) + "\n"


def apply_writes(items):
    """把表落回源文件。返回 [(路径, 摘要)]。

    只动两处**声明本身**（guide_kb.c 的两张表 + guide_kb.h 的 GKB_GAME_N），
    不动注释与其他代码。写之前整份备份成 .bak-<时间戳>。
    """
    N = len(items)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    done = []

    def load(p):
        return open(p, encoding="utf-8").read()

    def save(p, s):
        # ★ 同一个文件**只备份第一次**。apply_writes 会往 guide_kb.c 写两次
        #   （g_alias 一次、g_game_names 一次），如果每次都备份，
        #   回滚时按 done 顺序恢复会把「新 g_alias + 旧 g_game_names」
        #   的中间态当成初态 —— 越回滚越脏。
        if not any(d[0] == p for d in done):
            bak = p + ".bak-" + stamp
            shutil.copyfile(p, bak)
            done.append((p, os.path.basename(bak)))
        open(p, "w", encoding="utf-8").write(s)

    # ---- ① guide_kb.c: g_alias[] ----
    p = os.path.join(ROOT, "shared", "nnet", "guide_kb.c")
    src = load(p)
    lines = []
    for i, (stem, name, al) in enumerate(items):
        cur = "   "
        for c in _alias_cells(i, al):
            # ★ 每个元素**后面**都要跟逗号（含行尾那一个 —— C 允许初始化器
            #   尾逗号，不允许**元素之间缺**逗号）。
            #   2026-10-05 这里原来写的是 `cur += c + " "`（空格分隔），
            #   写出来的是 `{ "逆转裁判4", 0 } { "逆转裁判 4", 0 } ...`，
            #   gcc 直接 `error: expected '}' before '{'`（行内元素没隔开）。
            if len(cur) + len(c) + 2 > 74 and cur.strip():
                lines.append(cur.rstrip())
                cur = "   "
            cur += c + ", "
        if cur.strip():
            lines.append(cur.rstrip())
    src = _sub_block(src, r"static\s+const\s+GkbAlias\s+g_alias\[\]\s*=\s*\{",
                     "".join("    %s\n" % l for l in lines))
    save(p, src)

    # ---- ② guide_kb.c: g_game_names[] ----
    p2 = os.path.join(ROOT, "shared", "nnet", "guide_kb.c")
    src = load(p2)
    lines = []
    cur = "   "
    for i, (stem, name, _) in enumerate(items):
        t = ' "%s",' % name
        if len(cur) + len(t) > 68:
            lines.append(cur)
            cur = "   "
        cur += t
    if cur.strip():
        lines.append(cur)
    src = _sub_block(src, r"static\s+const\s+char\s+\*g_game_names\[\]\s*=\s*\{",
                     "".join("    %s\n" % l for l in lines))
    save(p2, src)

    # ---- ③ guide_kb.h: GKB_GAME_N ----
    h = os.path.join(ROOT, "shared", "nnet", "guide_kb.h")
    hs = load(h)
    hs2, n = re.subn(r"#define\s+GKB_GAME_N\s+\d+",
                     "#define GKB_GAME_N %d" % N, hs, count=1)
    if n != 1:
        raise RuntimeError("guide_kb.h 里找不到 GKB_GAME_N —— 不敢写")
    save(h, hs2)

    # ---- ④ guide_search.py: GAME_ORDER ----
    #
    # 为什么必须一起写（2026-10-05 加）：
    #   这张表原来**只能手抄**（脚本把内容 print 出来让人贴）。结果
    #   C 侧写好了、Python 侧忘了 —— 语料 118 款时 GAME_ORDER 停在 89。
    #   更糟的是 C 侧那次是**照 31 款时代的顺序**生成的：g_alias[1] 是
    #   「动物之森」，而索引里 gid=1 是 ace_attorney_5 —— 整表平移，
    #   只要玩家带游戏名提问，检索就会过滤到**别的游戏**上。
    #   这就是「game_id 是位置，不是内容」那条铁律的又一次复发。
    gs = os.path.join(HERE, "guide_search.py")
    src_py = load(gs)
    src_py = _sub_py_block(src_py, "GAME_ORDER = [", r"^\]",
                           render_game_order(items))

    # ---- ⑤ guide_search.py: GAME_ALIAS ----
    src_py = _sub_py_block(src_py, "GAME_ALIAS = {", r"^\}",
                           render_game_alias(items))
    save(gs, src_py)

    # ---- ⑥ 写回自检：必须能 parse 回 N 条，且覆盖 0..N-1 ----
    src2 = load(p2)
    ma = re.search(r"static\s+const\s+GkbAlias\s+g_alias\[\]\s*=\s*\{(.*?)\};",
                   src2, re.S)
    mn = re.search(r"static\s+const\s+char\s+\*g_game_names\[\]\s*=\s*\{(.*?)\};",
                   src2, re.S)
    errs = []          # ★ 必须早于下面所有 append（原来定义晚了一行，
                       #   加的守卫一跑就 NameError）
    # 这条正则只有**一个**捕获组 —— 上面 re.findall 若按 (_, gid) 解包就会
    # 「expected 2, got 1」。第一次实现正是这么写的，于是表写进去了、
    # 自检炸了、回滚后留下一个无人负责的 .bak。拆开写才看得出来。
    #
    # ⚠️ 另一个更隐蔽的洞（2026-10-05）：原来只捞 `"...", \s*(-?\d+)\s*\}` 里的
    #    **gid 数字**。而「元素之间**缺逗号**」时写成
    #       { "逆转裁判4", 0 } { "逆转裁判 4", 0 } { "逆转裁判", 0 }
    #    三个元素各自都能匹配到那条正则 → gids 集合照样完整 → 自检全绿，
    #    可 gcc 直接 `error: expected '}' before '{'`，构建当场挂。
    #    所以除了数 gid，必须再数**完整元素**（`{ ... }` 一个不多一个不少）。
    total_alias = sum(len(al) for _s, _n, al in items)
    cells = re.findall(r"\{\s*\"[^\"]*\"\s*,\s*-?\d+\s*\}", ma.group(1))
    if len(cells) != total_alias:
        err_cells = ("g_alias 写回后是 %d 个完整元素（应 %d）—— "
                     "多半是元素之间缺逗号被粘成了一个" % (len(cells), total_alias))
        errs.append(err_cells)
    gids = {int(g) for g in re.findall(r"\"[^\"]*\"\s*,\s*(-?\d+)\s*\}",
                                       ma.group(1))}
    names = re.findall(r"\"([^\"]*)\"", mn.group(1))
    if len(names) != N:
        errs.append("g_game_names 写回后 %d 条（应 %d）" % (len(names), N))
    missing = sorted(set(range(N)) - gids)
    if missing:
        # ★ 必须把**缺哪几个**打出来。只报 min/max 会让人以为「覆盖到了
        #   就是对的」，而漏一个 gid 正是别名表里某款游戏没别名的典型症状。
        errs.append("g_alias 写回后缺 gid %s（共 %d 款，应覆盖 0..%d）"
                    % (missing[:8], N, N - 1))
    hd = re.search(r"#define\s+GKB_GAME_N\s+(\d+)", load(h), re.S)
    if not hd or int(hd.group(1)) != N:
        errs.append("GKB_GAME_N 写回后 != %d" % N)

    # ---- ⑦ 写回自检：Python 侧两个表 ----（真要 import 一次才作数）
    py2 = load(gs)
    try:
        import importlib
        sys.path.insert(0, HERE)
        if "guide_search" in sys.modules:
            gmod = importlib.reload(sys.modules["guide_search"])
        else:
            gmod = importlib.import_module("guide_search")
        if list(gmod.GAME_ORDER) != [s for s, _, _ in items]:
            errs.append("GAME_ORDER 写回后 != 语料顺序")
        if set(gmod.GAME_ALIAS) != {s for s, _, _ in items}:
            miss = sorted({s for s, _, _ in items} - set(gmod.GAME_ALIAS))
            errs.append("GAME_ALIAS 写回后缺 %d 款 %s" % (len(miss), miss[:8]))
        empty = [s for s, v in gmod.GAME_ALIAS.items() if not v]
        if empty:
            errs.append("GAME_ALIAS 有 %d 款别名为空 %s（无法从问句认出）"
                        % (len(empty), empty[:8]))
    except Exception as e:
        errs.append("guide_search.py 写回后 import 失败：%r" % (e,))
    if errs:
        # 回滚**三个**文件，不能只回滚 C 的 —— 否则会留下
        # 「Python 换了、C 没换」的半截状态，比全不改更难看。
        for path, _bak in done:
            b = os.path.join(os.path.dirname(path), _bak)
            if os.path.exists(b):
                shutil.copyfile(b, path)
        raise RuntimeError("写回自检没过（已回滚全部 %d 个文件）：%s"
                           % (len(done), "; ".join(errs)))
    return done


def main():
    items = collect()
    dropped = clean(items)
    N = len(items)
    do_write = "--write" in sys.argv
    print("语料 %d 款（game_id = 0..%d，按文件名字母序）\n" % (N, N - 1))

    print("=" * 74)
    print("PYTHON 侧：GAME_ORDER")
    print("=" * 74)
    print("GAME_ORDER = [")
    line = "   "
    for i, (stem, _, _) in enumerate(items):
        t = ' "%s",' % stem
        if len(line) + len(t) > 68:
            print(line)
            line = "   "
        line += t
    if line.strip():
        print(line)
    print("]")

    print()
    print("=" * 74)
    print("C 侧：g_alias  与  g_game_names")
    print("=" * 74)
    print("static const GkbAlias g_alias[] = {")
    for i, (stem, name, al) in enumerate(items):
        # 一行放不下 3 个以上别名就换行，人读的版本。
        # ⚠️ 同样要带逗号（见 apply_writes 里那句注释）：照着这张表
        #    手动贴进 .c 的人，贴的还是"元素用空格分开"的写法就会编译不过。
        cur = "   "
        for c in _alias_cells(i, al):
            if len(cur) + len(c) + 2 > 74 and cur.strip():
                print(cur.rstrip())
                cur = "   "
            cur += c + ", "
        if cur.strip():
            print(cur.rstrip())
    print("};")

    print()
    print("static const char *g_game_names[] = {")
    cur = "   "
    for i, (stem, name, _) in enumerate(items):
        t = ' "%s",' % name
        if len(cur) + len(t) > 68:
            print(cur)
            cur = "   "
        cur += t
    if cur.strip():
        print(cur)
    print("};")

    # 自检：别名不得包含别的游戏的名称。
    #
    # 分两类，第二类可以放过：
    #   A. **真冲突** —— 短别名 A 被长别名 B 包含，而 A 不是 B 的前缀。
    #      无论先长后短还是先短后长都会撞车。
    #      例：猫咪幽灵 / 猫咪幽灵2 —— 后者不是前者的前缀。
    #   B. **可由匹配顺序解决** —— 短别名 A 是长别名 B 的**前缀**。
    #      只要匹配时**先长后短**，就一定是 B 先命中，A 拿不到。
    #      例：塞尔达传说 三角力量 / 塞尔达传说 三角力量 重制版。
    #      C 端 gkb_split_game 本来就是"取最长命中"（见 guide_kb.c），
    #      所以这类是安全的 —— 但必须**验证**那个实现真的是取最长，
    #      下面会把这条要求写进输出里提醒。
    print()
    print("=" * 74)
    print("自检：别名交叉污染（clean() 已自动删掉短的那个）")
    print("=" * 74)
    for stem, a, other, owner in dropped:
        print("  [已删] %s 的别名「%s」会吃掉 %s 的「%s」-> 删短留长"
              % (stem, a, owner, other))
    if dropped:
        print("  上面 %d 条被自动删掉。若某款游戏因此一个别名都不剩，"
              "要手动在 ALIAS 里补一个更长的。" % len(dropped))
    else:
        print("  无需删。")
    hard, soft = [], []
    for i, (stem, name, al) in enumerate(items):
        cands = [(name2, None) for _, name2, _ in items if name2] + \
                [(b2, stem2) for stem2, _, b2s in items for b2 in b2s]
        for a in al:
            for other, owner in cands:
                if other == a or len(other) < 3:
                    continue
                if owner == stem:
                    continue          # 自己的别名不算
                if other in a:
                    (soft if a.startswith(other) else hard).append(
                        (stem, a, owner, other))

    for stem, a, owner, other in hard:
        print("  [真冲突] %s 的别名「%s」含 %s 的「%s」，"
              "且不是其前缀 -> 删掉「%s」" % (stem, a, owner, other, a))
    for stem, a, owner, other in soft:
        print("  [可解]   %s 的别名「%s」含 %s 的「%s」，"
              "但它是前缀 -> 只要先长后短匹配就安全" % (stem, a, owner, other))

    if hard:
        print("\n  %d 处真冲突 —— 表不能直接用" % len(hard))
        return 1
    print("\n  OK：无真冲突。")

    # ── 自检：同一条别名不得落在**两个不同的 game_id** 上 ──
    #
    # 为什么这条必须单独查（2026-10-05 踩到）：gkb_split_game 用
    # `al > best_len` 取**严格**最长命中，等长就只认表序第一个。于是
    # 「真·女神转生IV」同时挂在 shin_megami_tensei_4 和 smt_iv 两个 gid 上时，
    # 用户输入「真·女神转生IV 怎么玩」，永远归给表序靠前的那个，
    # 另一个 gid 的语料（内容其实一模一样）**永远检索不到**，
    # 而且 silent —— 上面那套"别名污染"自检抓不到这种（两条串完全相等，
    # `other in a` 成立但 startswith 也成立，判为安全）。
    #
    # 症状也不是"少一款"，而是"某一款认不出游戏名" —— 与 §no_alias 同类，
    # 但成因完全不同，所以单开一条。
    owner = {}
    clash = []
    for stem, name, al in items:
        for a in list(al) + [name]:
            if owner.get(a, stem) != stem:
                clash.append((a, owner[a], stem))
            else:
                owner[a] = stem
    for a, s1, s2 in clash:
        print("  [同串两主] 「%s」同时挂在 %s 和 %s 上 —— C 端严格最长匹配下"
              "只有表序靠前的能命中，要么删掉一边的这条，要么让两边字面不同"
              % (a, s1, s2))
    if clash:
        print("\n  %d 条同串两主 —— 必须修，否则另一半语料永远检索不到" % len(clash))
        return 1
    print("  OK：无同串两主。")
    if soft:
        print("  ⚠️ 但有 %d 处依赖「先长后短匹配」。"
              "请确认 gkb_split_game() 取的是**最长**命中 ——"
              "它若按表序第一个命中就中招了。" % len(soft))

    # ── 自检：有没有整款游戏**一条别名都没有** ──
    # 这跟"少支持几款"不是一个量级：别名是**从问句里剔除**游戏名的依据，
    # 一款游戏没别名 = 用户说「XX 的 YY 怎么拿」时它根本进不了筛选范围，
    # 而且**不报错、不崩**，只是检索永远找不着。
    # 两种成因（下面分头说清）：ALIAS 字典漏配 / clean() 把它的别名删光了。
    no_alias = [i for i, (_s, _n, al) in enumerate(items) if not al]
    if no_alias:
        print("\n  ⚠️ %d 款游戏一条别名都没有（gid %s）"
              % (len(no_alias), no_alias[:8] + (["…"] if len(no_alias) > 8 else [])))
        print("     → 问句里认不出这些游戏，检索永远找不着（不问就报错）：")
        print("       要么 ALIAS 字典漏配；要么 clean() 把它们唯一的别名删了")
        print("       （删掉说明那条别名与别款游戏撞车，得换一个独有的说法）。")

    if not do_write:
        print("\n（默认只打印。要落盘请加 --write；落盘前会备份成 .bak-<时间戳>）")
        return 0

    print("\n写入源文件：")
    try:
        for p, bak in apply_writes(items):
            print("  %s\n     备份 -> %s" % (os.path.relpath(p, ROOT), bak))
    except Exception as e:
        print("  !! 写入失败：%s" % e)
        print("     → 已备份的 .bak-* 在，源文件多半没被写坏；请检查后重跑。")
        return 1
    print("\n⚠️ 落盘后**还有三件事没做**，别以为这就完事：")
    print("   1. `python tools/guia/gen_cand_rows.py` 重出 g_cand_set[] —— "
          "sc_guide.c 有编译期断言 `sizeof(g_cand_set) == GKB_GAME_N`，")
    print("      游戏从 31 变 %d 条，行数不同就**编译不过**（好，会当场暴露）。" % N)
    print("   2. `python tools/build.py` 重新构建 —— 本机无工具链时这个必须人工做。")
    print("   3. 真机/模拟器趟次验一遍：游戏列表能列出 %d 款且检索不错位。" % N)
    if no_alias:
        print("   4. 给 gid %s 这 %d 款补别名，否则它们至今检索不到。"
              % (no_alias[:6], len(no_alias)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
