/*
 * sc_guide.c -- 离线攻略（检索版）
 *
 * 双屏分工沿用项目惯例：上屏是**只读的事实区**，下屏是**操作区**。
 *
 *   上屏：当前游戏名 + 命中段落的原文（能滚）+ 出处
 *   下屏：游戏选择条 / 输入框 / 检索结果（top-4）
 *
 * ── 为什么这一屏的核心是「原文照抄」 ──────────────────────────────
 * 检索版没有语言模型参与，所以它的全部价值就是**原文准确**。
 * 一旦在这里做同义改写、摘要或者"顺便补一句解释"，用户就没法核对
 * 屏上这句话到底出自哪一段，出处标注也就失去了意义。
 * 所以段落文本按字节原样搬过来，只做 UTF-8 安全的截断，不做任何改写。
 *
 * ── 为什么默认「全部游戏」 ────────────────────────────────────────
 * BPE 词表是从语料学出来的，游戏名恰好没被学成完整 token
 * （详见 guide_kb.h 顶部的说明），所以「先选游戏、再提问」是这个
 * 功能唯一稳的用法。但**默认锁死一款游戏**会让"我就想随手搜个道具"
 * 变成一次额外的换游戏操作，于是默认放开范围；用户手动选过之后
 * 才收窄，而且手动选的优先级高于问句里写的游戏名（见 do_search）。
 *
 * ── 中文怎么输 ──────────────────────────────────────────────────
 * 主路径是系统自带的 swkbd（和 sc_chat.c 同一套做法），它自带中文
 * 输入法。但候选词那一路不是冗余：swkbd 在模拟器里不好用，真机上
 * 一个常用问题也不值得打十几个字。所以下屏列出该游戏的高频查询词，
 * 上下选、A 直接提问 —— 一次软键盘都不碰也能把功能用起来。
 *
 * ── 键位 ────────────────────────────────────────────────────────
 *   ←/→      换游戏（19 款 + 全部游戏）
 *   ↑/↓      移动下屏列表光标（候选词 / 结果）
 *   L/R      翻上屏正文（肩键；和 sc_viewer.c 一个路子）
 *   A        选中：候选词=直接提问，结果=推到上屏全文
 *   Y        调出软键盘   X 用当前输入重新检索   B 返回
 * 上下滚动正文**不占 ↑/↓**：无结果时 ↑/↓ 要留给候选词，
 * 有结果时要留给结果列表，两个列表共用一个方向键比再加一层模式切换好。
 */
#include "screen.h"

#include "app.h"
#include "guide_kb.h"
#include "guide_nn.h"
#include "extract.h"
#include "conv_state.h"
#include "orch.h"
#include "hwk.h"
#include "nnet_text.h"
#include "render.h"
#include "text.h"
#include "widgets.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* 一条问句的上限（字节）。 */
#define GUIDE_QUERY 192

/* 权重文件在 SD 卡上的位置。
 *
 * ⚠️ 为什么不在 .cia 里：权重 1.2MB，内嵌会让固件大 4 倍，
 * 而它在开发期是**每次训练都要换**的东西 —— 内嵌就意味着
 * 每次改模型都要重编整个 .cia。放 SD 卡的好处是
 * 「换模型 = 拷一个文件」，不动代码。
 *
 * 离线要求：这是唯一的外部依赖，缺了会退化成纯检索
 * （见 guide_nn.c 的失败策略），不会崩、也不会显示不出东西。 */
#define GUIDE_MODEL_PATH "sdmc:/3ds/aiconsole/guide_model.bin"

/* 段落正文缓冲。语料里最长的一段 633 字节，留到 768 足够。 */
#define GUIDE_PASSAGE 768

/* 模型组织出来的句子。比段落短（要能在 7 行内读完），
 * 但仍留 512 —— 候选集限制下实测 20~40 token，约 120 字节 UTF-8，
 * 512 是三倍余量。 */
/*
 * 答案缓冲。★ 从 512 扩到 1600，理由不是"想多存点"：
 * gkb_extract 最多拼 EXT_PICK(3) 句，每句的候选缓冲 EXT_SENT_MAX 是 512，
 * 所以最坏情况需要 3*512+分隔 ≈ 1600 字节。缓冲不够时会在**半句**处截断，
 * 而"半句"看起来仍然像句话 —— 症状是答案被腰斩，不报错（extract.h 里明写了）。
 * 多占 1.1KB .bss，对 3DS 的 100MB RAM 可忽略。
 * 显示仍然只画 G_ANSWER_LINES(3) 行，多存的部分不占屏幕。
 */
#define GUIDE_ANSWER  1600

/* 上屏正文同时显示几行。26 + 7*18 = 152，下面还放得下出处行与滚动提示，
 * 再上面就是 200 处的页脚。
 *
 * ⚠️ 这 7 行是给**原文**留的。模型生成时正文区会缩到
 * G_ANSWER_LINES 行，剩下的给上面那句整理后的话 ——
 * 答案在上、佐证在下，人读起来才是「结论 → 出处」的顺序。
 * 反过来（原文在上、结论在下）会让人先花 20 秒读完原文
 * 才看到那句总结，而总结才是他要的。 */
#define G_BODY_LINES 7

/* 模型生成那一句占的行数。3 行 = 54px，够放约 40 个汉字
 * （实测生成 20~40 token ≈ 30~60 汉字，两行；3 行是余量）。 */
#define G_ANSWER_LINES 3

/* BPE 词表工作区。gkb_vocab_bytes(4096,4096) 约 224KB，
 * 必须放静态区：main.c 把 app 搬进了 256KB 的工作线程栈，
 * 这块塞栈上会把 gkb_search 剩下的 20KB 挤爆。 */
static uint8_t g_vocab_work[224 * 1024];

/* ---- 状态 ---------------------------------------------------------------
 * 屏幕实例是单例，状态全放文件级 static（约定见 screen.h 顶部）。
 * ----------------------------------------------------------------------- */
static Gkb      g_kb;
static bool     g_kb_ok;         /* gkb_bind 成功过没有 */
static bool     g_vocab_ok;      /* 词表加载成功过没有 */

static char     g_query[GUIDE_QUERY];   /* 输入框里的字 */
static int      g_game;          /* 当前选中的游戏 id，-1 = 全部游戏 */

static GkbHit   g_hits[GKB_HIT];
static int      g_nhits;
static int      g_pick;          /* 结果列表光标，0..g_nhits-1 */
static int      g_shown;         /* 上屏正在显示第几条命中，-1 = 没显示 */
static int      g_scroll;        /* 上屏滚动的视觉行号 */
static int      g_lines;         /* 当前段落折行后的总行数 */

static int      g_cand;          /* 候选词光标 */
static int      g_cand_top;      /* 候选词窗口首行 */
static int      g_widened;       /* 上一次检索是否放宽了游戏范围 */

/* ---- 模型生成的回答 ----------------------------------------------------
 *
 * 与段落正文**分开存**，因为两者的生命周期不同：
 *   g_passage 随选中的命中变（用户可以翻 4 条命中）
 *   g_answer  随最近一次生成变（用户翻命中时它不变）
 * 分开存才能做到「翻命中时回答还挂在上面」，那才是有用的对照 ——
 * 用户能自己看出模型那句是不是从原文里来的。
 */
static char     g_answer[GUIDE_ANSWER];
static int      g_has_answer;    /* 1 = g_answer 里有有效句子 */

/* ---- 硬件知识这一路 ----------------------------------------------------
 *
 * 为什么和攻略共用这一屏而不是单开一屏：用户问「3ds 能超频吗」时，
 * 他期待的就是在这同一个输入框里得到答案。为一个「问句的另一个来源」
 * 单开一屏，等于要求他先想清楚该问哪一屏再找过去。
 *
 * 代价是两路的显示形态不同（攻略是可滚的段落原文，硬件是一整条
 * 固定答案），所以要有 g_from_hw 这个互斥标志。它是**互斥**不是叠加：
 * 两边同时显示会让用户以为是同一条内容的两个版本，
 * 而它们其实一个来自 31 款游戏语料、一个来自 25 条硬件结论。
 */
static HwkResult g_hwk;
static int       g_from_hw;      /* 1 = 上屏显示的是硬件知识，不是攻略段落 */

/* --------------------------------------------------------------------------
 * 候选词：每款游戏的高频查询词
 *
 * 素材来自 tools/guia/corpus/guide_*.txt 的 `##` 章节标题与
 * guide_search.py 的 CASES —— 也就是这些词都是**确实能命中**的，
 * 不是拍脑袋想的（那 12 条用例是 PC 上实测过的）。
 *
 * 每款 6 个。底屏一次显示 5 行，第 6 个靠滚动才看得到。
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *words[6];
} CandSet;

/* 下标与 guide_kb.c 的 g_game_names 一一对应（也就是 build_kb.py 里
 * sorted(corpus) 的顺序）。加游戏时两处必须一起改。
 *
 * ⚠️⚠️ 这张表**曾经只有 19 行**，而语料扩到 31 款后没跟着加 ——
 * 新增的 12 款在底屏上**一个字都没有**（cand_at 越界返回空串），
 * 现象是「选了那个游戏，下面全空白」。不报错。
 *
 * 词是**逐个验证过可检索性**的（tools/guia/verify_cand.py，
 * 判据：限定该游戏时 top1 正文里含这个词本身）。
 * ⚠️ 不要用"自动取高频词"来生成这张表 —— 试过，
 * 产出大量「简单说」「通常情况下」这种**跨句碎片**
 * （频率高是因为句子结构重复，不是因为它是有意义的实体）。
 *
 * ⚠️ 已知的一类不合格：`配招`/`双剑`/`时间旅行` 这些 BPE 切不开的词，
 * 以及 `关卡`/`流程`/`强化` 这些撞上 GKB_MAX_POSTING=24 上限的词
 * —— 限定游戏时会 0 命中。根因在索引的 MAX_POSTING 太小，
 * 要改就得同时改 build_kb.py 与 guide_kb.h。 */
static const CandSet g_cand_set[] = {
    { { "法庭", "证物", "证词", "调查", "档案", "证据" } },                   /*   0 ace_attorney */
    { { "证物", "法庭", "直接", "调查", "出来", "提示" } },                   /*   1 ace_attorney_4 */
    { { "法庭", "玩家", "提醒", "证物", "调查", "一个" } },                   /*   2 ace_attorney_5 */
    { { "玩家", "法庭", "提醒", "系统", "证据", "经验" } },                   /*   3 ace_attorney_6 */
    { { "状态", "格斗", "能量", "距离", "一段", "镜头" } },                   /*   4 ace_combat_ah_legacy */
    { { "提前", "地面", "目标", "BOSS", "反应", "一段" } },                   /*   5 after_burner_2_3d */
    { { "同一", "时间", "一遍", "后面", "玩家", "视角" } },                   /*   6 anchor_point */
    { { "结构", "飞行", "石头", "触屏", "操作", "特殊" } },                   /*   7 angry_birds_trilogy */
    { { "铃钱", "岛上", "家具", "图鉴", "狸克", "之后" } },                   /*   8 animal_crossing */
    { { "东西", "拿到", "顺序", "对话", "房间", "触摸" } },                   /*   9 another_code */
    { { "赛道", "杯赛", "漂移", "升级", "解锁", "星级" } },                   /*  10 asphalt_3d */
    { { "房间", "游戏", "炸弹", "boss", "地图", "物品栏" } },                 /*  11 binding_of_isaac_rebirth_3ds */
    { { "高处", "关卡", "能力", "远处", "镜头", "收集" } },                   /*  12 bionic_commando_3d */
    { { "动作", "障碍", "之后", "固定", "普通", "boss" } },                   /*  13 bittrip_runner_3ds */
    { { "对手", "攻击", "直接", "招式", "方向", "必杀" } },                   /*  14 blazblue_continuum_shift_2 */
    { { "设计", "次数", "之后", "解锁", "累计", "动物" } },                   /*  15 bokujou_origin_no_daichi */
    { { "动物", "直接", "黄金", "石头", "自己的", "一次" } },                 /*  16 bokujou_tsunagaru_shintenchi */
    { { "三个", "第一", "本作", "材料", "阶段", "所以" } },                   /*  17 bokumono_3_villages */
    { { "炸弹", "强化", "联机", "自己", "速度", "爆炸" } },                   /*  18 bomberman_blitz */
    { { "方块", "卡住", "方向", "一块", "关卡", "平台" } },                   /*  19 boxboy */
    { { "职业", "BOSS", "经验", "剧情", "支线", "骑士" } },                   /*  20 bravely_default */
    { { "方向", "之后", "同一", "对手", "还能", "出手" } },                   /*  21 brunswick_pro_bowling */
    { { "主机", "平台", "手感", "动作", "回头", "节奏" } },                   /*  22 castlevania_vc */
    { { "线索", "档案", "调查", "一次", "一点", "什么" } },                   /*  23 chase_cold_case_investigations */
    { { "房间", "一条", "一次", "每个", "直接", "捡到" } },                   /*  24 corpse_party_3ds */
    { { "法庭", "直接", "第一", "第二", "一段", "本作" } },                   /*  25 dai_gyakuten_saiban */
    { { "法庭", "剧情", "第二", "部分", "人物", "出来" } },                   /*  26 dai_gyakuten_saiban_2 */
    { { "提醒", "调查", "经验", "建议", "日常", "证据" } },                   /*  27 danganronpa_1 */
    { { "提醒", "经验", "调查", "建议", "碎片", "关键" } },                   /*  28 danganronpa_2 */
    { { "剧情", "对话", "系列", "自己", "开门", "人物" } },                   /*  29 dasshutsu_adventure_akumu_no_shinigami_ressha */
    { { "五十", "出来", "发现", "纸片", "分类", "箱子" } },                   /*  30 dasshutsu_adventure_kami_oroshi_no_uranai_ban */
    { { "机关", "剧情", "探索", "解谜", "零件", "说明" } },                   /*  31 dasshutsu_adventure_kyuukousya_no_syoujo */
    { { "零件", "对话", "房间", "拿到", "系列", "一遍" } },                   /*  32 dasshutsu_adventure_majo_no_sumu_yakata */
    { { "线索", "情报", "东西", "机关", "证词", "件事" } },                   /*  33 dasshutsu_adventure_shiawase_no_akai_ishi */
    { { "机关", "每个", "系列", "场景", "第一", "还是" } },                   /*  34 dasshutsu_adventure_shuuen_no_kuroikiri */
    { { "钥匙", "情报", "调查", "编号", "开门", "原地" } },                   /*  35 dasshutsu_adventure_zetsubou_yosai */
    { { "反击", "对手", "高度", "场地", "对战", "立体" } },                   /*  36 dead_or_alive_dimensions */
    { { "背景", "解锁", "一次", "出来", "同一", "回头" } },                   /*  37 detective_conan_marionette_symphony */
    { { "指令", "战斗", "一遍", "完全", "收集", "系统" } },                   /*  38 detective_conan_phantom_rhapsody */
    { { "证物", "出来", "对话", "提示", "情报", "细节" } },                   /*  39 detective_pikachu */
    { { "首领", "BOSS", "输出", "杂兵", "炸弹", "路线" } },                   /*  40 dodonpachi_resurrection */
    { { "关卡", "香蕉", "护盾", "难度", "气球", "翻滚" } },                   /*  41 donkey_kong_country_returns_3d */
    { { "机器", "画面", "起来", "物理", "存档", "机关" } },                   /*  42 dooors */
    { { "职业", "装备", "Boss", "顺序", "入门", "随机" } },                   /*  43 dragon_quest_1_2_3_3d */
    { { "怪物", "钥匙", "世界", "系统", "素材", "合成" } },                   /*  44 dragon_quest_monsters_2_iru_luca */
    { { "怪物", "区域", "素材", "大师", "蓄力", "地形" } },                   /*  45 dragon_quest_monsters_joker_3 */
    { { "怪物", "才能", "技能", "合成", "属性", "队伍" } },                   /*  46 dragon_quest_monsters_terry_wonderland_3d */
    { { "职业", "碎片", "怪物", "经验", "后期", "提醒" } },                   /*  47 dragon_quest_vii_3d */
    { { "技能", "战斗", "同伴", "怪物", "金属", "BOSS" } },                   /*  48 dragon_quest_viii_3d */
    { { "格子", "弹药", "路线", "敌人", "第二", "能力" } },                   /*  49 escape_vektor */
    { { "地牢", "队伍", "采集", "任务", "技能", "职业" } },                   /*  50 etrian_odyssey_4 */
    { { "轮胎", "赛道", "策略", "名次", "前先", "位置" } },                   /*  51 f1_2011 */
    { { "采集", "战斗", "装备", "技能", "经验", "任务" } },                   /*  52 fantasy_life */
    { { "火力", "BOSS", "商店", "武器", "射击", "一段" } },                   /*  53 fantasy_zone_3d */
    { { "职业", "等级", "武器", "外传关卡", "战斗", "结婚" } },               /*  54 fe_awakening */
    { { "武器", "转职", "职业", "等级", "支援", "地形" } },                   /*  55 fe_if */
    { { "委托", "素材", "装备", "武器", "召唤", "任务" } },                   /*  56 final_fantasy_explorers */
    { { "经验", "提醒", "治疗", "迷宫", "地下", "章节" } },                   /*  57 fire_emblem_echoes */
    { { "攻击", "伤害", "特性", "杂兵", "敌人", "必杀" } },                   /*  58 fire_emblem_warriors */
    { { "关卡", "计时", "一半", "平台", "屏幕", "动作" } },                   /*  59 fractured_soul */
    { { "BOSS", "反应", "输出", "立体", "方向", "深处" } },                   /*  60 galaxy_force_2_3d */
    { { "boss", "射击", "平台", "方向", "最容易", "机关" } },                 /*  61 gunman_clive */
    { { "证物", "法庭", "证词", "档案", "一段", "什么" } },                   /*  62 gyakuten_saiban_123_selection */
    { { "法庭", "证物", "证词", "调查", "场景", "先把" } },                   /*  63 gyakuten_saiban_5 */
    { { "法庭", "证词", "证物", "先把", "两条", "调查" } },                   /*  64 gyakuten_saiban_6 */
    { { "判定", "连击", "难度", "分数", "位置", "反复" } },                   /*  65 hatsune_miku_project_diva_f */
    { { "对方", "真正", "人物", "几乎", "东西", "后面" } },                   /*  66 hotel_dusk */
    { { "杂兵", "BOSS", "特殊", "攻击", "妖精", "魔法" } },                   /*  67 hyrule_warriors_3d */
    { { "必杀", "之石", "球员", "比赛", "主角", "核心" } },                   /*  68 inazuma_eleven_go */
    { { "形态", "变身", "骑士", "杂兵", "推进", "必杀" } },                   /*  69 kamen_rider_travelers_senki */
    { { "任务", "BOSS", "炸弹", "容错", "近战", "辅助" } },                   /*  70 karous */
    { { "Boss", "能量", "飞行", "射击", "地面", "移动" } },                   /*  71 kid_icarus */
    { { "世界", "指令", "提醒", "触屏", "培养", "BOSS" } },                   /*  72 kingdom_hearts_ddx */
    { { "能力", "卡比", "地形", "心心关卡", "风柱", "boss" } },               /*  73 kirby */
    { { "卡比", "能力", "本作", "boss", "方块", "贴纸" } },                   /*  74 kirby_extra */
    { { "线索", "跳过", "关键", "同一", "回头", "交互" } },                   /*  75 last_window */
    { { "谜题", "硬币", "面具", "场景", "提示", "卡住" } },                   /*  76 layton_miracle_mask */
    { { "法庭", "迷宫", "调查", "证物", "点数", "两条" } },                   /*  77 layton_vs_gyakuten_saiban */
    { { "谜题", "场景", "线索", "检查", "金币", "每日" } },                   /*  78 laytons_mystery_journey */
    { { "回旋镖", "林克", "四个", "三个", "机关", "宝箱" } },                 /*  79 legend_zelda */
    { { "时机", "游泳", "判定", "动作", "加速", "节奏" } },                   /*  80 london_2012 */
    { { "吸尘器", "鬼怪", "房间", "暗光灯", "吸住", "拉扯" } },               /*  81 luigi2 */
    { { "缩地", "火球", "星星", "镜头", "回旋镖", "特殊" } },                 /*  82 mario_3d_land */
    { { "狸猫", "超级", "马力欧", "星星币", "平台", "尾巴" } },               /*  83 mario_3d_land_2 */
    { { "城堡", "落点", "旋转", "力量", "地形", "挑战" } },                   /*  84 mario_golf_world_tour */
    { { "星星", "位置", "大星星", "掷骰子", "对手", "小游戏" } },             /*  85 mario_party */
    { { "小游戏", "金币", "星星", "boss", "掷骰子", "因为" } },               /*  86 mario_party_star_rush */
    { { "跟着", "特别", "按键", "训练", "比赛", "四个" } },                   /*  87 mario_sonic_rio_2016 */
    { { "杯赛", "马力欧", "训练", "决定", "特殊", "还有" } },                 /*  88 mario_sports_superstars */
    { { "杯赛", "落点", "机会", "按键", "马力欧", "特别" } },                 /*  89 mario_tennis_open */
    { { "蓄力", "能量", "boss", "平台", "挑战", "各有" } },                   /*  90 mega_man_legacy */
    { { "分数", "还是", "一眼", "位置", "补给", "火力" } },                   /*  91 metal_slug_7 */
    { { "瞄准", "发现", "草丛", "生存", "体力", "情报" } },                   /*  92 mgs3_snake_eater_3d */
    { { "猎人", "怪物", "素材", "防具", "任务", "状态" } },                   /*  93 mh3g */
    { { "怪物", "任务", "素材", "部位", "强化", "破坏" } },                   /*  94 mh4g */
    { { "猎人", "风格", "素材", "狩猎", "联机", "村庄" } },                   /*  95 mh_generations */
    { { "猎人", "风格", "素材", "极限", "怪物", "狩猎" } },                   /*  96 mhxx */
    { { "方块", "障碍", "背景", "切换", "发射", "作品" } },                   /*  97 mighty_switch_force */
    { { "生存", "村民", "方块", "火把", "工具", "附魔" } },                   /*  98 minecraft */
    { { "赛道", "加速", "漂移", "金币", "速度", "排量" } },                   /*  99 mk7 */
    { { "精灵", "早期", "一条", "平台", "支线", "跳跃" } },                   /* 100 mutant_mudds */
    { { "金币", "能力", "冲刺", "地形", "购买", "一共" } },                   /* 101 naruto_shippuden_3d_the_new_era */
    { { "检查", "漂移", "加速", "位置", "挑战", "提前" } },                   /* 102 need_for_speed_the_run_3d */
    { { "路易吉", "变身", "马力欧", "完全", "设计", "砖块" } },               /* 103 new_super_mario_bros_u */
    { { "金币", "隐藏", "蘑菇", "刷分", "世界", "砖块" } },                   /* 104 new_supermario_2d */
    { { "Boss", "一下", "屏幕", "防御", "翻滚", "往上" } },                   /* 105 ninja_gaiden_dragon_sword */
    { { "落地", "招式", "空中", "真正", "其实", "挑战" } },                   /* 106 olliolli */
    { { "Boss", "第二", "素材", "角色", "往下", "打法" } },                   /* 107 one_piece_unlimited_cruise_sp */
    { { "变身", "跳跃", "幽灵", "能力", "一点", "特殊" } },                   /* 108 pacman_ghostly_adventures */
    { { "贴纸", "纸片", "世界的", "东西", "彗星", "一张" } },                 /* 109 paper_mario */
    { { "选项", "工具", "第三", "关系", "线索", "走路" } },                   /* 110 parascientific_escape_crossing_at_the_farthest_horizon */
    { { "钥匙", "房间", "人物", "属于", "炸弹", "小时" } },                   /* 111 parascientific_escape_cruise_in_distant_seas */
    { { "谜题", "东西", "时间", "道具", "一颗", "选项" } },                   /* 112 parascientific_escape_gear_detective */
    { { "迷宫", "面具", "技能", "素材", "二周目", "陷阱" } },                 /* 113 persona_q */
    { { "弱点", "属性", "行动", "对应", "技能", "治疗" } },                   /* 114 persona_q2 */
    { { "方块", "谜题", "保留", "处理", "失误", "进度" } },                   /* 115 picross_3d */
    { { "宝可梦", "图鉴", "道馆", "一只", "版本", "队伍" } },                 /* 116 pokemon_b2w2 */
    { { "道馆", "宝可梦", "一只", "队伍", "等级", "龙系" } },                 /* 117 pokemon_bw */
    { { "宝可梦", "宝可梦的", "能量", "天气", "强力", "进化" } },             /* 118 pokemon_conquest */
    { { "道馆", "功能", "画面", "珍珠", "一样", "宝可梦" } },                 /* 119 pokemon_dppt */
    { { "宝可梦", "绿宝石", "特性", "能力值", "天气", "主力" } },             /* 120 pokemon_emerald */
    { { "道馆", "宝可梦的", "一只", "火系", "前期", "地面" } },               /* 121 pokemon_frlg */
    { { "宝可梦", "传说", "进化", "覆盖", "道馆", "出现" } },                 /* 122 pokemon_gold_silver */
    { { "超级", "迷宫", "委托", "力量", "结构", "背包" } },                   /* 123 pokemon_mystery_dungeon_3ds */
    { { "能量", "区域", "格子", "解锁新", "后续", "一张" } },                 /* 124 pokemon_picross */
    { { "齿轮", "招式", "宝可梦", "宝可梦的", "体力", "主力" } },             /* 125 pokemon_rumble_blast */
    { { "排名", "岛屿", "招式", "免费", "传说", "主力" } },                   /* 126 pokemon_rumble_world */
    { { "好感", "资源", "稀有", "宝可梦", "强化", "超级" } },                 /* 127 pokemon_shuffle */
    { { "试炼", "捕捉", "队伍", "岛屿", "飞行", "招式" } },                   /* 128 pokemon_sm */
    { { "图鉴", "试炼", "霸主", "太阳", "月亮", "阿罗拉" } },                 /* 129 pokemon_usum */
    { { "宝可梦", "妖精", "交换", "使用", "招式", "属性" } },                 /* 130 pokemon_xy */
    { { "判定", "连击", "中心", "机器", "模式", "新增" } },                   /* 131 project_diva_f_2nd */
    { { "判定", "连击", "提前", "命中", "刷分", "家具" } },                   /* 132 project_mirai */
    { { "循环", "防御", "体力", "对手", "星星", "快速" } },                   /* 133 punch_out_3d */
    { { "结构", "方块", "一块", "开关", "通路", "观察" } },                   /* 134 pushmo */
    { { "连击", "同一", "一条", "支援", "两条", "一种" } },                   /* 135 radiant_historia_perfect_chronology */
    { { "Teensies", "Lums", "滑翔", "机械", "关卡", "特定" } },               /* 136 rayman */
    { { "弹药", "扫描", "限制", "升级", "血量", "火力" } },                   /* 137 re_revelations */
    { { "谜题", "委托", "小镇", "徽章", "场景里", "场景" } },                 /* 138 repton */
    { { "评价", "分数", "弹药", "队友", "生存", "连击" } },                   /* 139 resident_evil */
    { { "沙漠", "重置", "钩索", "正面", "守卫", "箱子" } },                   /* 140 retro_prince */
    { { "小游戏", "触屏", "判定", "时机", "中间", "反复" } },                 /* 141 rhythm_heaven */
    { { "小游戏", "跟着", "评价", "切换", "前面", "音乐" } },                 /* 142 rhythm_heaven_met */
    { { "漂移", "赛道", "名次", "机器", "计时赛", "提前" } },                 /* 143 ridge_racer_3d */
    { { "多人", "竞速", "颜色", "反应", "单人", "组合" } },                   /* 144 runbow */
    { { "魔法", "简单", "规则", "立体", "作品", "奖励" } },                   /* 145 sega_3d_classics_collection */
    { { "支线", "四个", "别人", "杂兵", "属于", "评价" } },                   /* 146 sengoku_musou_chronicle */
    { { "结局", "杂兵", "支线", "评价", "硬直", "补给" } },                   /* 147 sengoku_musou_chronicle_2nd */
    { { "满了", "报酬", "一片", "往上", "刷分", "往前" } },                   /* 148 sengoku_musou_chronicle_3 */
    { { "boss", "道具", "迷路", "考验", "地图", "循环" } },                   /* 149 shantae_pirates_curse_3ds */
    { { "行动", "弱点", "提升", "Boss", "进入", "尝试" } },                   /* 150 shin_megami_tensei_4 */
    { { "障碍", "杂兵", "黄金", "一片", "石头", "主机" } },                   /* 151 shin_sangokumusou_ds */
    { { "骑士", "章节", "boss", "翻滚", "检查", "地图上" } },                 /* 152 shovel_knight */
    { { "角色", "对战", "强度", "专门", "各种", "单人" } },                   /* 153 smash_bros_3ds */
    { { "弱点", "行动", "Boss", "次数", "覆盖", "规划" } },                   /* 154 smt_deep_no_m */
    { { "弱点", "选择", "支线", "物理", "覆盖", "克制" } },                   /* 155 smt_iv */
    { { "弱点", "克制", "自动", "物理", "两只", "覆盖" } },                   /* 156 smt_iv_apocalypse */
    { { "合成", "魔法", "之前", "迷宫", "电脑", "召唤" } },                   /* 157 smt_soul_hackers */
    { { "弱点", "图鉴", "探索", "管理", "标记", "辅助" } },                   /* 158 smt_strange_journey */
    { { "旋转", "经典", "冲刺", "boss", "选择", "一关" } },                   /* 159 sonic_3ds */
    { { "赛道", "名次", "加速", "漂移", "稳定", "形态" } },                   /* 160 sonic_allstars_racing */
    { { "经典", "形态", "冲刺", "主机", "旋转", "Boss" } },                   /* 161 sonic_generations_3ds */
    { { "单人", "boss", "队友", "条件", "模式", "第一次" } },                 /* 162 star_fox_64_3d */
    { { "策略", "阶段", "一圈", "队伍", "习惯", "一直" } },                   /* 163 star_fox_command */
    { { "单位", "近战", "远程", "回合", "利用", "射击" } },                   /* 164 steam_code */
    { { "工具", "地面", "往下", "资源", "商店", "升级" } },                   /* 165 steamworld_dig */
    { { "资源", "能力", "捷径", "商店", "隐藏房间", "传送" } },               /* 166 steamworld_dig_2 */
    { { "火力", "回合", "瞄准", "游戏的", "资源", "走位" } },                 /* 167 steamworld_heist */
    { { "免费", "多人", "单人", "发射", "别人", "任务" } },                   /* 168 steel_diver_sub_wars */
    { { "自由", "关系", "行动", "回合", "格子", "时间" } },                   /* 169 stella_glow */
    { { "工具", "好感", "每天", "结婚", "动物", "季节" } },                   /* 170 story_of_seasons */
    { { "风格", "能拿", "先看", "稀有", "搭配", "属于" } },                   /* 171 style_boutique */
    { { "马力欧", "力欧", "路易吉", "城堡", "星星", "金属" } },               /* 172 super_mario_64_3ds */
    { { "马力欧", "风格", "超级", "一百", "本地", "硬币" } },                 /* 173 super_mario_maker_3ds */
    { { "超级", "香蕉", "迷你", "边缘", "多人", "单人" } },                   /* 174 super_monkey_ball_3d */
    { { "必杀", "防御", "格斗", "练到", "反击", "指令" } },                   /* 175 super_street_fighter_iv_3d */
    { { "连击", "分数", "机器", "简单", "第一", "主题" } },                   /* 176 taiko_no_tatsujin_3ds */
    { { "加成", "二周目", "成长", "另外", "异常", "临时" } },                 /* 177 tales_of_the_abyss */
    { { "特别", "生存", "屏幕", "二十", "临时", "结束" } },                   /* 178 tekken_3d_prime_edition */
    { { "材料", "召唤", "房子", "增益", "金属", "附近" } },                   /* 179 terraria_3ds */
    { { "一个", "加成", "世界", "武器", "触发", "阵型" } },                   /* 180 the_alliance_alive */
    { { "魔法", "射击", "boss", "打不动", "增强", "一关" } },                 /* 181 the_legend_of_dark_witch */
    { { "判定", "音乐", "连击", "简单", "队伍", "失败" } },                   /* 182 theatrhythm_ff */
    { { "远处", "出去", "射击", "判断", "看到", "持续" } },                   /* 183 thunder_blade_3d */
    { { "传送", "开关", "解谜", "道具", "砖块", "岩石" } },                   /* 184 toki_tori_3d */
    { { "每天", "他们", "朋友", "关系", "岛上", "问题" } },                   /* 185 tomodachi_life */
    { { "火力", "铃铛", "走位", "往前", "分数", "射击" } },                   /* 186 twinbee_3d */
    { { "形态", "按住", "障碍", "滑翔", "检查", "方向键" } },                 /* 187 two_fast_4_gnomz */
    { { "双人", "血量", "首领", "能量", "容易被", "控制" } },                 /* 188 vc_nes_classics */
    { { "控制", "作品", "超级", "之一", "藏在", "朋友" } },                   /* 189 vc_snes_classics */
    { { "解锁", "死亡", "房间", "回到", "集齐", "主线" } },                   /* 190 vvvvvv_3ds */
    { { "章节", "游戏", "力欧", "指令", "小游戏", "操控" } },                 /* 191 warioware_gold */
    { { "球员", "比赛", "雷达", "跑位", "阵型", "防守" } },                   /* 192 we_football */
    { { "落地", "加速", "五十", "杯赛", "赛道", "快速" } },                   /* 193 winter_sports_2012 */
    { { "血条", "按住", "力量", "对手", "倒地", "重打" } },                   /* 194 wwe_all_stars */
    { { "宝石", "地区", "队友", "快速", "用来", "物理" } },                   /* 195 xenoblade_chronicles_3d */
    { { "地面", "炸弹", "空中", "目标", "落点", "捡到" } },                   /* 196 xevious_3d */
    { { "朋友", "限定", "强力", "主线", "硬币", "伙伴" } },                   /* 197 yokai_watch */
    { { "觉得", "水下", "金属", "变身", "敌人", "关于" } },                   /* 198 yoshi_new_island */
    { { "召唤", "效果", "动画", "陷阱", "魔法", "回合" } },                   /* 199 yugioh_zexal_duel_carnival */
    { { "沙之杖", "首领", "暗之石", "流沙", "心之碎片", "高处" } },           /* 200 zelda_albw */
    { { "黑暗世界", "地牢", "沙漠神殿", "神殿", "力量", "精灵" } },           /* 201 zelda_alttp */
    { { "林克", "击败", "首领", "集齐", "提供", "色版" } },                   /* 202 zelda_links_awakening */
    { { "林克", "之剑", "风之", "海拉鲁", "塞尔达", "火焰" } },               /* 203 zelda_minish_cap */
    { { "面具", "城镇", "首领", "变身面具", "返回", "环境" } },               /* 204 zelda_mm3d */
    { { "心之碎片", "碎片", "大师之剑", "童年", "神殿", "林克" } },           /* 205 zelda_oot3d */
    { { "林克", "时之", "时代", "塞尔达", "守护", "竖琴" } },                 /* 206 zelda_oracle_ages */
    { { "季节", "林克", "交换", "力量", "传送", "戒指" } },                   /* 207 zelda_oracle_seasons */
    { { "林克", "神殿", "岛屿", "之剑", "翻滚", "谜题" } },                   /* 208 zelda_phantom_hourglass */
    { { "结局", "规则", "组合", "谜题", "累计", "每次" } },                   /* 209 zero_escape_vlr */
    { { "碎片", "分钟", "一格", "一块", "各自", "三十" } },                   /* 210 zero_escape_zero_time_dilemma */
};

#define CAND_N 6

/*
 * ⚠️ 编译期自检：候选词表的行数必须等于游戏数。
 *
 * 这不是"稳妥起见"——历史上**真的**脱节过：g_cand_set 只有 19 行，
 * 语料扩到 31 款后没跟着加，新 12 款在底屏上全空白，
 * 而 gcc 一个字都不报（越界读的是合法的下一个数组元素）。
 *
 * 所以用**编译失败**来暴露它：宁可在构建时红，也不要在设备上
 * 表现为"那款游戏下面什么词都没有"。
 * 改语料而忘了改这张表时，这一行会直接断掉构建。
 */
typedef char cand_rows_must_equal_game_count[
    (sizeof(g_cand_set) / sizeof(g_cand_set[0]) == (size_t)GKB_GAME_N)
        ? 1 : -1];

/* 「全部游戏」下没有专属词表，给一组跨游戏都查得到的。
 * 全部取自 guide_search.py 的 CASES —— 那些是实测过能命中的。
 *
 * ⚠️ 2026-10-05：「boss战」换成了「Boss」。BPE 把「boss战」切成 boss + 战
 *    两个 token，检索时压根没有完整这个词，字面判据恒不成立
 *    （verify_cand.py 报「boss战=弱」）。「Boss」是独立 token，
 *    top1 落在 #3273「Boss 战打法。Boss 战通常在关卡结尾。Boss 血很厚
 *    而且有护盾。硬打打不动」—— 正是玩家想看的东西。
 */
static const char *const g_cand_all[CAND_N] = {
    "心之碎片", "大师之剑", "沙之杖", "御三家", "Boss", "道具"
};

static const char *cand_at(int i)
{
    if (i < 0 || i >= CAND_N)
        return "";
    if (g_game < 0)
        return g_cand_all[i];
    if (g_game >= (int)(sizeof(g_cand_set) / sizeof(g_cand_set[0])))
        return "";
    return g_cand_set[g_game].words[i];
}

/* --------------------------------------------------------------------------
 * 底部三段的几何
 *
 * 底屏内容区 y=30..214（184px）。常量集中在这里，render 与 handle
 * （触摸命中）共用同一份 —— 别在两处手写坐标，sc_home.c 的两个
 * 按钮就是因为这样才需要 azahar_test.py 里那两个对齐的坐标常量。
 * ----------------------------------------------------------------------- */
#define G_BAR_H     24.0f                                          /* 30 */
#define G_INPUT_Y   ((float)W_BOT_CONTENT_Y + G_BAR_H + 6.0f)      /* 60 */
#define G_INPUT_H   34.0f
#define G_LIST_Y    (G_INPUT_Y + G_INPUT_H + 6.0f)                  /* 100 */
#define G_ROW_H     22.0f
#define G_LIST_ROWS 5      /* 5*22 = 110；100+110 = 210 < 214 */

static Rect list_row(int i)
{
    return (Rect){ W_BOT_MARGIN, G_LIST_Y + (float)i * G_ROW_H,
                   W_BOT_USABLE, G_ROW_H, 0 };
}

/* 输入框矩形：整个框都可点，等价于「调出键盘」 */
static const Rect G_INPUT_RECT = { W_BOT_MARGIN, G_INPUT_Y,
                                   W_BOT_USABLE, G_INPUT_H, 0 };

/* --------------------------------------------------------------------------
 * 段落原文：按字节搬到可 NUL 结尾的缓冲
 *
 * kb->text 是全部段落首尾相接的连续 blob，**段落末尾不是 NUL**
 * （见 guide_kb.h）。所以必须按 tok_len 显式拷，不能用 strncpy。
 *
 * 截断时把长度向下取整到 4 的倍数：正文里全是 3 字节汉字，
 * 腰斩一个会留下半个码点，屏上是个碎块。768 字节的缓冲最多
 * 少显示一个多字，对全文阅读的影响可以忽略。
 * ----------------------------------------------------------------------- */
static char g_passage[GUIDE_PASSAGE];

/*
 * 把一段 UTF-8 文本拷进定长缓冲，**不腰斩字符**，且保证 NUL 结尾。
 *
 * 为什么不用 snprintf("%s")：gcc 会报 -Wformat-truncation
 *（源 769 字节进 768 的缓冲），而 snprintf **按字节截断**，
 * 3 字节汉字被砍掉 1~2 个字节时，末尾就是一个乱码方块。
 * 这不是"少显示一个字"，是屏上出现一个看不懂的字符。
 *
 * 与 load_passage 的对齐逻辑同源（那里处理段落、这里处理硬件答案），
 * 但多了一步：snprintf 至少保证 NUL 结尾，手写拷贝必须自己写，
 * 忘了就是"读越界一个字节" —— 那在 3DS 上是随机崩，不一定崩在这行。
 */
static void copy_aligned(char *dst, int cap, const char *src)
{
    int n = (int)strlen(src);

    if (n > cap - 1)
        n = cap - 1;
    /* 砍掉末尾的 UTF-8 续接字节（10xxxxxx）。
     * 判据看**源**字节：看目标缓冲的话读到的是上一段内容，
     * 于是"截断是否对齐"取决于上一段讲了什么。 */
    while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u)
        --n;

    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

static void load_passage(const GkbHit *h)
{
    const unsigned char *src = (const unsigned char *)g_kb.text + h->tok_off;
    int n = h->tok_len;

    if (n > GUIDE_PASSAGE - 1)
        n = GUIDE_PASSAGE - 1;

    /* 3 字节汉字不能腰斩：砍掉末尾的续接字节（10xxxxxx）。
     * 判据必须看**源**字节 —— 看目标缓冲的话读到的是上一段的内容，
     * 于是"截断是否对齐"取决于上一段讲了什么。 */
    while (n > 0 && (src[n] & 0xC0u) == 0x80u)
        --n;

    memcpy(g_passage, src, (size_t)n);
    g_passage[n] = '\0';
}

/* --------------------------------------------------------------------------
 * 上屏：按折行后的**视觉行**滚动
 *
 * 为什么不能直接抄 sc_viewer.c：那一屏的滚动单位是 g_doc.line[] 的
 * 条目（一条 HtmlLine），而攻略段落是**无结构的长文本** —— 一段最长
 * 633 字节（约 211 个汉字），折行后 9 行，上屏正文区只放得下 7 行。
 * 所以必须按视觉行滚。
 *
 * 为什么不去改 text.c 加 skip 参数：那要动共享绘制层。这里需要的
 * 只是"从第 skip 行起画"，用 r_clip_push 加一个上移的矩形就能做到，
 * text_draw 自己会再裁一次。共享绘制层保持原样是刻意的选择。
 * ----------------------------------------------------------------------- */
static void draw_passage(float x, float y, float w, float h)
{
    /* 有答案时正文区被压缩过，裁剪框的高度必须跟着变 ——
     * 不然 text_draw 会把多出来的行画到「依据」那行上面，
     * 两段文字叠在一起。 */
    int vis = g_has_answer ? (G_BODY_LINES - G_ANSWER_LINES) : G_BODY_LINES;

    /* 夹住游标。段落换了、行数变了之后游标可能越界，
     * 不夹的话会画出一屏空白（往上滚过头就是这样）。 */
    if (g_scroll > g_lines - 1)
        g_scroll = (g_lines > 1) ? g_lines - 1 : 0;
    if (g_scroll < 0)
        g_scroll = 0;

    /* 矩形上移 g_scroll 行、并按总行数拉高：text_draw 从矩形顶
     * 开始逐行画，前 g_scroll 行正好落在裁剪框外面被裁掉。 */
    Rect draw = { x, y - (float)g_scroll * (float)TEXT_LINE_H, w,
                  (float)(g_lines + vis) * (float)TEXT_LINE_H, 0 };

    r_clip_push(&(Rect){ x, y, w, h, 0 });
    text_draw(&draw, g_passage, w_c(C_SLOT_FG), TEXT_LINE_H);
    r_clip_pop();
}

/* --------------------------------------------------------------------------
 * 模型生成的答案
 *
 * 与 draw_passage 分开是因为语义不同：原文可以滚动、要显示出处，
 * 答案只有两三行、且**永远从第一行开始显示**（它是结论，
 * 结论不需要滚动 —— 滚掉了结论还叫什么结论）。
 *
 * 颜色用 C_SLOT_GEN 而不是 C_SLOT_FG：答案与原文的可信度不同，
 * 视觉上就该有差别。用户扫一眼就能看出「这句是模型说的」。
 * ----------------------------------------------------------------------- */
static float draw_answer(float x, float y, float w, float h)
{
    Rect draw = { x, y, w, h, 0 };
    r_clip_push(&(Rect){ x, y, w, h, 0 });
    text_draw(&draw, g_answer, w_c(C_SLOT_GEN), TEXT_LINE_H);
    r_clip_pop();
    /* 返回下一行的 y。答案不会超过 h 行（clipped），
     * 但**布局要按实际画了几行推进**，否则答案短的时候
     * 下面会空出一大块。 */
    {
        int n = text_line_count(g_answer, (int)w);
        if (n < 1) n = 1;
        if (n > G_ANSWER_LINES) n = G_ANSWER_LINES;
        return y + (float)n * (float)TEXT_LINE_H;
    }
}

/* --------------------------------------------------------------------------
 * 检索
 * ----------------------------------------------------------------------- */
static void show_hit(int i);

/*
 * 硬件知识这一路要不要抢在攻略前面显示。
 *
 * ⚠️ 这个判断是整个改动里最容易做错的一处，所以把判据写清楚：
 *
 *   问题不是「硬件得分高不高」，而是「**两个得分能不能比**」。
 *   攻略那边是 BM25（idf 加权的段落打分，量纲没有上界），
 *   硬件这边是 bigram 覆盖率（0~100 的整数，还带 +40 的包含加分）。
 *   这两个数**不可比** —— 拿它们比大小等于拿米和英尺比大小。
 *
 * 所以真正的判据是**来源置信度**，不是分数：
 *   1. 硬件这一路必须自己"够格"（hwk_search 内部已卡了 20% 门槛）
 *   2. 且问句里**出现了硬件词**（g_hw_hint，见下）
 *
 * 第 2 条是关键：没有它，「火焰纹章觉醒 转职怎么转」这种纯游戏问题
 * 也会被某条硬件条目蹭到 20% 覆盖率（"怎么""3ds"这类 bigram
 * 在硬件问句里到处都是），然后用户问攻略却拿到一条硬件知识。
 * 而那看起来完全像个答案 —— 比"没找到"坏得多。
 */
#define HW_HINT_N 12
static const char *const g_hw_hint[HW_HINT_N] = {
    "3ds", "3DS", "nds", "CPU", "cpu", "gpu", "GPU",
    "内存", "缓存", "多核", "超频", "线程"
};

/* 问句里有没有硬件词。有就允许硬件那一路抢答。 */
static int query_is_hardware(const char *q)
{
    int i;
    for (i = 0; i < HW_HINT_N; i++) {
        if (strstr(q, g_hw_hint[i]))
            return 1;
    }
    return 0;
}

static void note_game(void)
{
    app_note("guide: game=%d name=%s", g_game,
             g_game >= 0 ? gkb_game_name(g_game) : "全部游戏");
}

static void do_search(const char *query)
{
    char     rest[GUIDE_QUERY + 2];
    GkbHit   hits[GKB_HIT];
    int32_t  toks[GKB_MAX_QUERY];
    int      gid, n, ntok;
    int      hw_ok = 0;

    /* ---- 先跑硬件这一路 -------------------------------------------------
     *
     * 放在攻略检索**之前**，不是为了省时间，而是为了让"要不要显示
     * 硬件答案"这个决定在 g_hits 还没被写脏之前就做完。
     * 反过来的话，硬件命中时 g_hits 里还留着攻略的结果，
     * 渲染时必须靠 g_from_hw 去忽略它们 —— 多一个必须永远同步的标志。 */
    memset(&g_hwk, 0, sizeof g_hwk);
    g_hwk.hit = -1;
    if (hwk_ready()) {
        hwk_search(query, &g_hwk);
        /* 两个条件缺一不可，理由见 query_is_hardware 的注释。
         * 记进探针是因为"为什么没显示硬件答案"是个真会发生的疑问，
         * 而屏上看不出来。 */
        if (g_hwk.hit >= 0 && query_is_hardware(query)) {
            hw_ok = 1;
        }
        app_note("guide: hw hit=%d score=%d hint=%d",
                 g_hwk.hit, g_hwk.score, query_is_hardware(query));
    }

    g_from_hw = hw_ok;
    if (hw_ok) {
        /* 硬件答案整条放进 g_passage：上屏的绘制、滚动、出处标注
         * 全都复用现成的那一套，不新增一条显示路径。
         * g_nhits 置 0 而不是 1 —— 下屏的结果列表里不该出现一条
         * "硬件知识"，因为它不是可翻的段落列表，而是唯一答案。 */
        copy_aligned(g_passage, sizeof g_passage, g_hwk.best);
        g_lines = text_line_count(g_passage, (int)W_TOP_USABLE);
        if (g_lines < 1) g_lines = 1;
        g_scroll = 0;
        g_shown = -1;
        g_pick  = 0;
        g_nhits = 0;
        g_answer[0] = '\0';
        g_has_answer = 0;
        app_status_set("3DS 硬件知识 · 命中 %s",
                       g_hwk.ask[0] ? g_hwk.ask : "(未命名条目)");
        return;
    }

    /* ---- M1 统一路由：工具层（算式/日期）与闲聊层在这里抢答 -------------
     *
     * 优先级收在 orch_route() 里，而不是散成几条 if —— 因为**顺序本身
     * 就是需要被测的东西**，散在 UI 函数里改错了只有真机能发现。
     * 当前顺序：工具 -> 闲聊 -> 攻略。
     *
     * 两道防护栏各管一件事：
     *   tools_try   判不准就放行（「3ds」不会被答成「3」）
     *   chitchat_try 整句得真是寒暄（「谢谢你，火焰纹章怎么打」放行给攻略）
     * 宁可漏答不可抢答：抢答会把真正的攻略问题吃掉。 */
    {
        char ans[160];
        OrchKind ok = orch_route(query, ans, (int)sizeof ans);
        if (ok != ORCH_GUIDE) {
            /* 复用现成显示路径（与硬件答案那一路同款），不新增一条 */
            copy_aligned(g_passage, sizeof g_passage, ans);
            g_lines = text_line_count(g_passage, (int)W_TOP_USABLE);
            if (g_lines < 1) g_lines = 1;
            g_scroll = 0;
            g_shown = -1;
            g_pick  = 0;
            g_nhits = 0;
            g_answer[0] = '\0';
            g_has_answer = 0;
            g_from_hw = 0;
            app_status_set(ok == ORCH_TOOL ? "本地计算 · 不联网" : "闲聊");
            app_note("guide: orch kind=%d ans=%s", (int)ok, ans);
            return;
        }
    }

    /* 问句里的游戏名先剥掉。剥不掉（返回 -1）时 rest 就是问句原文，
     * 那是 gkb_split_game 的约定，不是错误。 */
    gid = gkb_split_game(query, rest, (int)sizeof rest);

    /* 用户手动选的优先于问句里写的 —— 选择条是显式操作，比问句里
     * 顺带一提更可信。这一句是整个交互的关键：问句里带个游戏名
     * 不该覆盖掉用户刚做的选择。 */
    if (g_game >= 0)
        gid = g_game;

    /* ---- M0 多轮槽位：把问句里出现过的游戏名记住，供下一轮补全 ----
     *
     * 修的是这个具体毛病：
     *   「火焰纹章觉醒」 -> 「它的boss 怎么打」
     * 第二句剥不出游戏名，gid 成了 -1，检索放开到全库，答案多半
     * 来自**别的游戏**，而且不报错、退出码正常、屏上看不出来。
     *
     * 认不出游戏名 + 有指代词 + 槽位还空着 -> 反问澄清。
     * 这里刻意**不**退化成全库检索：全库能返回结果，但那是错答案，
     * 比「我没找到」更糟 —— 用户会以为系统在认真回答他。 */
    {
        int r = conv_resolve_game(query, gid);
        if (r == CONV_GAME_CLARIFY) {
            char msg[128];
            conv_clarify_prompt(msg, (int)sizeof msg);
            /* 复用现成显示路径（与硬件答案那一路同款），不新增一条 */
            copy_aligned(g_passage, sizeof g_passage, msg);
            g_lines = text_line_count(g_passage, (int)W_TOP_USABLE);
            if (g_lines < 1) g_lines = 1;
            g_scroll = 0;
            g_shown = -1;
            g_pick  = 0;
            g_nhits = 0;
            g_answer[0] = '\0';
            g_has_answer = 0;
            app_status_set("需要先确认是哪款游戏");
            app_note("guide: clarify (pronoun without antecedent)");
            return;
        }
        gid = r;
    }

    /* token 数单独取一次：命中 0 条时要能分清"库里没有"和
     * "查询全被停用词吃掉了"，而屏上看不出区别。 */
    ntok = gkb_query_tokens(&g_kb, rest, toks, GKB_MAX_QUERY);
    n    = gkb_search(&g_kb, rest, gid, hits, GKB_HIT);

    /* 限定游戏却 0 命中时，自动在全库范围再搜一次。
     *
     * 这不是锦上添花：19 款语料的用词并不统一。实测「时之笛 3D 道具使用
     * 方法」在 game18（时之笛 3D）的 24 段里 0 命中 —— 它的正文写的是
     * 「大师之剑」「沙雷特长靴」而不是「道具」「方法」；而同一句在全库
     * 范围能命中 4 条。不放宽的话用户看到的就是一句「没有找到相关段落」，
     * 库里明明有答案。放宽后结果里的游戏名会如实显示，不会假装是时之笛的
     * 攻略。
     *
     * 只放宽一次，不递归：全库也 0 命中就是真没有。 */
    g_widened = 0;
    if (n <= 0 && gid >= 0 && ntok > 0) {
        n = gkb_search(&g_kb, rest, -1, hits, GKB_HIT);
        if (n > 0) g_widened = 1;
    }

    memcpy(g_hits, hits, sizeof(GkbHit) * GKB_HIT);
    g_nhits = n;
    g_pick  = 0;
    g_shown = -1;
    g_scroll = 0;

    /* 探针：一次检索的全部可核对量都在这一行里。
     * score 用整数（×100），%.0f 会把 newlib 的 dtoa 拖进运行期路径，
     * 在 Azahar 的小主栈上炸过（见 widgets.c 的同类注释）。
     * 段落 id 是 int32_t，显式转成 int 再给 %d。
     * scope= 是**实际生效**的范围：玩家选的 game / auto=问句里剥出来的 /
     * wide=限定游戏0 命中后放宽到全库。score 记的是放宽之前的原始分，
     * 放宽不改变排序，只是把候选集从「某游戏」换成「全库」。 */
    app_note("guide: q=%s game=%d scope=%s toks=%d hits=%d top=%d score=%d",
             rest, gid,
             g_widened ? "wide" : (gid >= 0 ? "game" : "auto"),
             ntok, n,
             n > 0 ? (int)hits[0].id : -1,
             n > 0 ? (int)(hits[0].score * 100.0f + 0.5f) : 0);

    if (n <= 0) {
        app_status_set(ntok > 0 ? "没有找到相关段落 · 换个说法再试"
                                 : "这个问题里没有可检索的词");
        return;
    }

    /* 检索完直接把 top-1 推上屏：不要求用户再按一次 A ——
     * "问完就看见答案"是这一屏该有的默认体验。 */
    show_hit(0);

    if (g_widened)
        app_status_set("%s里没有 · 已放宽到全部游戏", gkb_game_name(g_game));

    /* ---- 让模型把命中的段落组织成一句话 --------------------------
     *
     * 放在 show_hit(0) **之后**：先保证「一定看得到原文」，
     * 再尝试「看得到整理后的版本」。生成是同步阻塞的
     * （1.2M 参数在 804MHz 上约 25~40ms/token，48 token 最坏
     * 1.5 秒），而模型可能加载失败、词表没加载、检索退化 ——
     * 任何一种情况下用户都该先拿到段落，而不是卡在空白上。
     *
     * 注意传的是**放宽之后**的命中（g_hits），不是 hits：
     * 候选集必须与实际显示的段落同源，否则模型会拿 A 段的词
     * 去组织 B 段的内容 —— 那是胡编的一种，而且很难被看出。 */
    /* ---- 抽取式组装（M4）：**抽取优先，模型退为退路** ------------------
     *
     * 为什么不直接用模型：1.18M 的小模型教师强迫 top-1 只有 6.1%，
     * 说出来的话又短又容易跑题。而 gkb_extract 是从**命中的原文里挑句子**，
     * 句子必然出自检索结果 —— 零编造、零延迟、长度可控（EXT_PICK 句）。
     *
     * ⚠️ 这条路径以前**根本没接进卡上**（BACKLOG 第 4 项）：
     * gkb_extract 全仓只有 test/exttest.c 一个调用点，也就是
     * 「PC 侧能用抽取式、卡上一直只能看模型的短答案」。
     * 它本身早就写好，并通过 extract_parity.py 的 C↔Python 一致性验证。
     *
     * 顺序：抽取优先；**抽不到才退回模型**；两者都不行就显示
     * 「没有找到相关段落」，不硬凑。 */
    g_answer[0] = '\0';
    g_has_answer = 0;
    {
        int w = gkb_extract(&g_kb, g_hits, n, rest, g_answer, (int)sizeof g_answer);
        /* 探针：0 = 一句都没取到（库里没有能直接回答这句的句子） */
        app_note("guide: extract bytes=%d hits=%d", w, n);
        g_has_answer = (w > 0 && g_answer[0] != '\0');
    }
    if (!g_has_answer && gnn_available()) {
        int rc = gnn_generate(&g_kb, rest, gid, g_hits, n,
                              g_answer, (int)sizeof g_answer,
                              0.7f, NULL);
        g_has_answer = (rc == GNN_OK && g_answer[0] != '\0');
        /* 探针：rc 记的是 GNN_* 错误码（0=成功），bytes 记生成的长度。
         * 两者分开是因为「失败」和「生成了但为空」要能区分 ——
         * 前者是 bug，后者是模型没学到东西。 */
        app_note("guide: gen rc=%d bytes=%d", rc, (int)strlen(g_answer));
    } else if (!g_has_answer) {
        app_note("guide: gen skip=%s (extract 也没抽到)", gnn_last_error());
    }
}

static void show_hit(int i)
{
    if (i < 0 || i >= g_nhits)
        return;

    g_shown  = i;
    g_pick   = i;
    g_scroll = 0;
    load_passage(&g_hits[i]);
    g_lines = text_line_count(g_passage, (int)W_TOP_USABLE);
    if (g_lines < 1)
        g_lines = 1;

    app_status_set("段落 #%d · %d 字节 · 共 %d 行",
                   (int)g_hits[i].id, (int)g_hits[i].tok_len, g_lines);
}

/* --------------------------------------------------------------------------
 * 输入：系统软键盘（和 sc_chat.c 同一套做法）
 * ----------------------------------------------------------------------- */
static void open_keyboard(void)
{
    SwkbdState  swkbd;
    char        buf[GUIDE_QUERY];
    SwkbdButton btn;

    /* 第四参是**字符数**不是字节数。GUIDE_QUERY 是字节上限（192），
     * 按字节传会让用户敲满 192 个汉字（576 字节）直接撑爆 buf。
     * 60 个字对一句问句绰绰有余。 */
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 60);
    swkbdSetHintText(&swkbd, "想问什么？比如 转职怎么转");
    swkbdSetInitialText(&swkbd, g_query);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);

    if (btn != SWKBD_BUTTON_CONFIRM) {
        app_status_set("已取消输入");
        return;
    }

    buf[GUIDE_QUERY - 1] = '\0';
    snprintf(g_query, sizeof g_query, "%s", buf);

    if (g_query[0] == '\0') {
        app_status_set("没输入内容");
        return;
    }
    do_search(g_query);
}

/* 用第 i 个候选词提问。共用的那一小段：填输入框 + 检索。 */
static void ask_cand(int i)
{
    if (i < 0 || i >= CAND_N)
        return;
    g_cand = i;
    snprintf(g_query, sizeof g_query, "%s", cand_at(i));
    do_search(g_query);
}

/* --------------------------------------------------------------------------
 * 生命周期
 * ----------------------------------------------------------------------- */
static void guide_enter(void)
{
    g_nhits = 0;
    g_shown = -1;
    g_scroll = 0;
    g_lines  = 0;
    g_pick = 0;
    g_cand = 0;
    g_cand_top = 0;
    g_game = -1;              /* 默认「全部游戏」，理由见文件头 */
    conv_reset();             /* 多轮槽位同理：进屏即清空，不跨屏继承 */
    g_query[0] = '\0';
    g_passage[0] = '\0';
    g_answer[0] = '\0';
    g_has_answer = 0;
    g_widened = 0;
    g_from_hw = 0;
    memset(&g_hwk, 0, sizeof g_hwk);
    g_hwk.hit = -1;

    /* 硬件知识库。**失败不阻塞这一屏** —— 攻略检索是这一屏的主体，
     * 硬件知识是加分项。缺了它，用户问游戏攻略照常能用；
     * 反过来（为它牺牲攻略）才是本末倒置。
     *
     * SD 卡优先、失败回退内嵌（见 hwk.c），所以 hwk_load 失败
     * 意味着"内嵌那份也没了"，那才是真的没救。 */
    {
        int n = hwk_load();
        if (n > 0) {
            app_note("guide: hw ok n=%d from=%s", n,
                     hwk_from_sd() ? "sd" : "embedded");
        } else {
            app_note("guide: hw failed rc=%d", n);
        }
    }

    /* 词表：内嵌在 .cia 里，加一次常驻。
     * 加载失败必须**响亮地报出来** —— 词表缺失会让检索静默变成
     * "永远 0 命中"，屏上看起来像"库里没这条"，实际是检索没跑起来。 */
    {
        GkbVocab vocab;
        int rc = gkb_vocab_load_embedded(&vocab, g_vocab_work,
                                         (int)sizeof g_vocab_work);
        if (rc != 0) {
            app_note("guide: vocab failed rc=%d", rc);
            app_note("guide: vocab load failed rc=%d", rc);
        app_status_set("词表加载失败 · 检索不可用");
        } else {
            g_vocab_ok = true;
            /* 报词表规模而不是报 nnet_vocab_size()：那个声明在
             * nnet.h（推理内核的头），为一处打印把推理头拉进 UI
             * 不划算 —— 而这次功能不接模型。 */
            app_note("guide: vocab ok v=%d merges=%d",
                     (int)vocab.vocab, (int)vocab.n_merge);
        }
    }

    /* 绑定内嵌索引。gkb_bind 唯一的运行期分配是 n_passage 个 int16
     *（836 段 = 1.7KB），所以 bind 一次能用整个会话。 */
    {
        int n = gkb_bind(&g_kb);
        if (n > 0) {
            g_kb_ok = true;
            app_note("guide: bound n=%d", n);
        } else {
            app_note("guide: bind failed rc=%d", n);
            app_note("guide: index load failed rc=%d", n);
        app_status_set("索引加载失败");
        }
    }

    /* 打开模型。**失败不阻塞这一屏** —— 检索层是纯查表，
     * 完全可靠；模型层要读 1.2MB 文件、做量化、有 1.20M 参数，
     * 任何一环都可能失败。让可靠的一层被不可靠的一层拖垮
     * 是本末倒置。所以这里只记状态，do_search 里按可用性分支。
     *
     * gnn_open 内部会**记住失败不再重试**，所以模型文件缺失时
     * 反复进攻略页不会反复吃 SD 卡 IO。 */
    {
        int rc = gnn_open(GUIDE_MODEL_PATH);
        if (rc == GNN_OK)
            app_note("guide: model ok");
        else
            app_note("guide: model unavailable rc=%d (%s)", rc,
                     gnn_last_error());
    }

    if (g_kb_ok && g_vocab_ok)
        app_status_set("就绪 · ←→ 选游戏 · ↑↓ 选词 · A 提问");
    else if (!g_kb_ok)
        app_status_set("索引没加载成功");
}

static void guide_leave(void)
{
    /* 退出也记一条 —— 验收脚本据此断言这一屏确实被进过并退出过，
     * 而不只是"渲染过一帧"。 */
    app_note("guide: screen=guide end");

    /* gkb_free 只放 gkb_bind 那一次 calloc。词表工作区是静态区，
     * 不释放：重新进屏时不必再解析一遍 114KB 词表。 */
    if (g_kb_ok) {
        gkb_free(&g_kb);
        g_kb_ok = false;
    }

    /* 硬件知识库**要释放**（与词表相反）。理由：它可能来自 SD 卡，
     * 而内容更新通道能在两屏之间把它换掉 —— 留着不释放的话，
     * 用户装完更新再进攻略页，看到的还是旧内容，而且没有任何迹象
     * 说明它该被重读。guide_enter 每次都会 hwk_load()（内部先 free），
     * 所以这里放掉是安全的。
     *
     * 词表为什么不放：它只有编译期内嵌那一份，没有更新通道，
     * 内容不可能在两屏之间变。判据是"这份东西会不会被换掉"，
     * 不是"它大不大"。 */
    hwk_free();
}

/* --------------------------------------------------------------------------
 * 上屏
 * ----------------------------------------------------------------------- */
static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    const char *gname = (g_game >= 0) ? gkb_game_name(g_game) : "全部游戏";
    char foot[72];
    /* 有生成结果时正文让出 3 行；没有就整块给原文。
     * 用一个变量而不是到处写 if，是为了让「正文从哪一行开始」
     * 只有一个定义点 —— 下面 draw_passage / 出处行 / 滚动提示
     * 三处都要用它。 */
    const int   body_lines = g_has_answer ? (G_BODY_LINES - G_ANSWER_LINES)
                                          : G_BODY_LINES;
    const float body_h = (float)body_lines * (float)TEXT_LINE_H;
    float       body_y = (float)W_TOP_CONTENT_Y;

    /* 放宽检索时右端不能写「已放宽范围」：页眉右侧只有 126px
     *（R_TOP_W - 标题占的260 - W_TOP_MARGIN），这句要168px，会溢出。
     * 改成只把右端换成「含其它游戏」（80px），并在下方的出处行上标出来
     * —— 出处那一行本来就是核对用的，在那里标最贴切。
     *
     * ⚠️ 硬件那一路的右端也不能写长句。同样的 126px 上限，
     * 「3DS 硬件知识」是 6 个汉字 96px，能放下；写成「硬件知识库（SD 卡）」
     * 就会溢出 —— 溢出计数进 status.txt，验收会抓，但那属于
     * "改文案忘了量宽度"，不该在设计时就把它写进来。 */
    w_top_header("离线攻略", g_from_hw ? "硬件知识" : (g_widened ? "含其它游戏" : gname));

    /* ---- 硬件知识这一路 ----
     *
     * 判据是 g_from_hw 而不是 g_hits：硬件命中时 g_nhits 故意是 0
     * （没有可翻的段落列表），所以拿 g_hits/g_shown 去判会走进
     * "没结果"那个分支，屏上会写「上屏显示模型整理的答案」——
     * 而硬件答案恰恰**不经过模型**，那句话就成了假话。 */
    if (g_from_hw) {
        float y = (float)W_TOP_CONTENT_Y;

        /* 命中的是哪一条。这是硬件知识**唯一**的"出处"：
         * 攻略那边有段落号可核对，硬件这边只能靠回显问句。
         * 所以这行必须有 —— 没有它，用户无法判断这条答案是
         * 针对他这个问题挑的，还是库里随便一条。 */
        if (g_hwk.ask[0]) {
            y = w_line(x, y, w, g_hwk.ask, w_c(C_SLOT_ACC_DK));
            y = w_line(x, y, w, "3DS 硬件知识库（实测结论）", w_c(C_SLOT_MUTED));
            y += 6.0f;
            w_hr(x, y, w, w_c(C_SLOT_LINE));
            y += 8.0f;
        }

        /*
         * 正文**复用 draw_passage**，不自己画。
         *
         * 一开始这里是直接 text_draw + 手写 clip 的，结果做出一个
         * 死键：L/R 两个键无条件改 g_scroll，但这一支从来不读它 ——
         * 按 R 有反应（status 里的行数变了？不，status 不变），
         * 屏上纹丝不动。这类"键有反馈但画面不变"的 bug 极难在
         * 截图验收里发现，因为**每张截图单独看都是对的**。
         * draw_passage 已经处理了夹取与滚动，复用它就没有第二条路径。
         */
        draw_passage(x, y, w, (float)W_TOP_CONTENT_B - y - 20.0f);

        /* 不显示来源（内嵌 / SD），因为对用户没有意义 ——
         * 用户要的是"这条知识从哪来"，不是"这个文件从哪来"。
         * 来源信息在 status.txt 的 hw from= 里，验收脚本能查。 */
        w_top_footer(app_status(), "[L][R] 滚动  [X] 重新检索  [B] 返回");
        return;
    }

    if (g_shown < 0 || g_passage[0] == '\0') {
        /* 没结果时上屏不编内容 —— 一屏大字描述"该怎么用"会让人
         * 以为功能坏了。留白 + 页脚一行提示更诚实。 */
        float y = (float)W_TOP_CONTENT_Y + 10.0f;
        y = w_line(x, y, w, "上屏显示模型整理的答案，"
                            "下面是检索到的原文。", w_c(C_SLOT_MUTED));
        w_line(x, y, w, "下屏选游戏、选词提问。", w_c(C_SLOT_FAINT));
        w_top_footer(app_status(), "[Y] 键盘  [A] 提问  [B] 返回");
        return;
    }

    /* ---- 模型整理的答案（在上） ---- */
    if (g_has_answer) {
        float y = body_y;
        y = draw_answer(x, y, w, (float)G_ANSWER_LINES);
        /* 答案与原文之间的分界。视觉上必须有 —— 否则两段文字
         * 会被当成连续的一段，而它们的可信度完全不同。
         * 用户需要一眼看出「上面那句是模型说的，下面是原文」。 */
        y = w_line(x, y, w, "—— 依据 ——", w_c(C_SLOT_FAINT));
        body_y = y;
    }

    draw_passage(x, body_y, w, body_h);

    /* 出处行放在正文区下面那一行的位置（152 + 2）。
     * 出处不是可选信息：用户要靠它回到语料核对。上面那句是
     * 模型改写过的，这行是唯一的「原文在哪」的凭据 ——
     * 改写越是流畅，就越需要这行冷冰冰的编号。 */
    snprintf(foot, sizeof foot, "出处：段落 #%d（列表第 %d/%d 条）%s",
             (int)g_hits[g_shown].id, g_shown + 1, g_nhits,
             g_widened ? " · 已放宽范围" : "");

    {
        float y = body_y + body_h + 4.0f;
        w_line(x, y, w, foot, w_c(C_SLOT_MUTED));

        /* 真的还有下文才提示能翻。平白写一句"可上下翻"却翻不动，
         * 比不写更招人烦。 */
        if (g_scroll + body_lines < g_lines)
            w_line(x, y + (float)TEXT_LINE_H, w, "［L］［R］ 还能往下看",
                   w_c(C_SLOT_FAINT));
    }

    w_top_footer(app_status(), "[L][R] 滚动  [A] 看这条  [B] 返回");
}

/* --------------------------------------------------------------------------
 * 下屏
 * ----------------------------------------------------------------------- */
static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    char l[96];

    w_bot_header(g_from_hw ? "硬件知识 · 换个问题" : "选游戏 · 输入 · 看结果");

    /* ---- 游戏选择条 ----
     * 当前项高亮用整条主色底 + 反色字：20 个选项里定位"我现在在
     * 第几个"靠的就是这一条背景色，比画个箭头可靠。 */
    snprintf(l, sizeof l, "‹  %s  ›",
             (g_game >= 0) ? gkb_game_name(g_game) : "全部游戏");
    r_rect(&(Rect){ x, (float)W_BOT_CONTENT_Y, w, G_BAR_H, 0 },
           w_c(C_SLOT_ACCENT));
    w_center((float)R_BOT_W / 2.0f, (float)W_BOT_CONTENT_Y + 4.0f,
             l, w_c(C_SLOT_BG));

    /* ---- 输入框 ---- */
    w_panel(&G_INPUT_RECT, w_c(C_SLOT_PANEL), w_c(C_SLOT_LINE));
    r_text(&(Rect){ G_INPUT_RECT.x + 6, G_INPUT_RECT.y + 9,
                    G_INPUT_RECT.w - 12, 16, 0 },
           g_query[0] ? g_query : "（按 Y 打开键盘，或 ↓ 选下面的词）",
           g_query[0] ? w_c(C_SLOT_FG) : w_c(C_SLOT_FAINT));

    /* ---- 列表：有结果看结果，没结果看候选词 ----
     * 两种列表共用同一片区域和同一个光标（A 的含义随列表变），
     * 这样上下键只有一种含义，少一层模式切换。 */
    if (g_nhits > 0) {
        for (int i = 0; i < g_nhits && i < G_LIST_ROWS; ++i) {
            Rect row = list_row(i);
            if (i == g_pick) {
                r_rect(&row, w_c(C_SLOT_ACC_LT));
                r_frame_rect(&row, w_c(C_SLOT_ACCENT), 1);
            }
            /* 一行只放「段落号 + 游戏名 + 得分」，正文在上屏。
             * 段落最长 633 字节，截成 20 个字看着像标题，
             * 读者会以为那就是答案。 */
            int s10 = (int)(g_hits[i].score * 10.0f + 0.5f);
            snprintf(l, sizeof l, "%d. #%d  %s  %d.%d",
                     i + 1, (int)g_hits[i].id, gkb_game_name(g_hits[i].game),
                     s10 / 10, s10 % 10);
            r_text(&(Rect){ row.x + 6, row.y + 3, row.w - 12, 16, 0 },
                   l, i == g_pick ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));
        }
        if (g_nhits < G_LIST_ROWS) {
            /* 结果不足 5 条时补一行说明，别让剩下的空白看着像坏了 */
            r_text(&(Rect){ x, G_LIST_Y + (float)g_nhits * G_ROW_H,
                            w, 16, 0 },
                   "↑ 还有别的候选词，按 X 重新检索",
                   w_c(C_SLOT_FAINT));
        }
    } else {
        r_text(&(Rect){ x, G_LIST_Y - (float)TEXT_LINE_H, w, 16, 0 },
               "候选词（↓ 选 · A 直接问）", w_c(C_SLOT_MUTED));
        for (int i = 0; i < G_LIST_ROWS; ++i) {
            int idx = g_cand_top + i;
            Rect row = list_row(i);
            if (idx >= CAND_N)
                break;
            if (idx == g_cand) {
                r_rect(&row, w_c(C_SLOT_ACC_LT));
                r_frame_rect(&row, w_c(C_SLOT_ACCENT), 1);
            }
            snprintf(l, sizeof l, "%d. %s", idx + 1, cand_at(idx));
            r_text(&(Rect){ row.x + 6, row.y + 3, row.w - 12, 16, 0 },
                   l, idx == g_cand ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));
        }
    }

    w_bot_footer("[Y] 键盘  [A] 选中  [B] 返回");
}

/* --------------------------------------------------------------------------
 * 按键
 * ----------------------------------------------------------------------- */
static void guide_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸只有按下沿那一帧，先查它再看按键。
     * 三个热区：输入框 = 调键盘；列表行 = 有结果选结果段、
     * 没结果用该候选词提问。 */
    int tx, ty;
    if (app_touch_down(&tx, &ty)) {
        if (w_hit(&G_INPUT_RECT, tx, ty)) {
            open_keyboard();
            return;
        }
        for (int i = 0; i < G_LIST_ROWS; ++i) {
            Rect row = list_row(i);
            if (!w_hit(&row, tx, ty))
                continue;
            if (g_nhits > 0) {
                if (i < g_nhits)
                    show_hit(i);
            } else {
                int idx = g_cand_top + i;
                if (idx < CAND_N)
                    ask_cand(idx);
            }
            return;
        }
    }

    /* ---- 换游戏 ---- */
    if (down & KEY_LEFT) {
        if (g_game > -1) {
            --g_game;
            note_game();
        }
        return;
    }
    if (down & KEY_RIGHT) {
        if (g_game < gkb_game_count() - 1) {
            ++g_game;
            note_game();
        }
        return;
    }

    /* ---- 翻上屏正文（肩键，与 sc_viewer.c 同一个路子） ----
     * 用 L/R 而不是 ↑/↓：无结果时 ↑/↓ 要留给候选词，
     * 两个列表共用方向键比再加一层模式切换好。 */
    if (down & KEY_L) {
        if (g_scroll > 0)
            --g_scroll;
        return;
    }
    if (down & KEY_R) {
        if (g_scroll + G_BODY_LINES < g_lines)
            ++g_scroll;
        return;
    }

    /* ---- 列表光标 ---- */
    if (down & KEY_UP) {
        if (g_nhits > 0) {
            if (g_pick > 0)
                --g_pick;
        } else if (g_cand > 0) {
            --g_cand;
            /* 光标上移到窗口外就把窗口跟着挪 —— 否则光标会画在
             * 看不见的那一行上，看着像按了没反应。 */
            if (g_cand < g_cand_top)
                g_cand_top = g_cand;
        }
        return;
    }
    if (down & KEY_DOWN) {
        if (g_nhits > 0) {
            if (g_pick < g_nhits - 1)
                ++g_pick;
        } else if (g_cand < CAND_N - 1) {
            ++g_cand;
            if (g_cand > g_cand_top + G_LIST_ROWS - 1)
                g_cand_top = g_cand - G_LIST_ROWS + 1;
        }
        return;
    }

    /* ---- A：选中 ---- */
    if (down & KEY_A) {
        if (g_nhits > 0)
            show_hit(g_pick);
        else
            ask_cand(g_cand);
        return;
    }

    /* ---- X：用当前输入重新检索。
     * 「重新」是真的重新：换游戏之后同一个问句的范围变了，
     * 不重跑的话屏上还是旧游戏的结果，而下屏的游戏条已经变了 ——
     * 那是最难查的一类不一致。 */
    if (down & KEY_X) {
        if (g_query[0])
            do_search(g_query);
        else
            app_status_set("先按 Y 输入问题，或用 ↓ 选候选词");
        return;
    }
    if (down & KEY_Y) { open_keyboard(); return; }
    if (down & KEY_B)  { app_pop(); return; }
}

const Screen g_screen_guide = {
    .id            = SCREEN_GUIDE,
    .name          = "SCREEN_GUIDE",
    .enter         = guide_enter,
    .leave         = guide_leave,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = guide_handle,
};