/*
 * guide_kb.c — 离线攻略知识库检索（3DS 端）
 *
 * 算法与 tools/guia/guide_search.py 的 Retriever.search() **逐行对应**。
 * 改一边必须改另一边，否则 PC 上测好的命中率在 3DS 上会悄悄失效。
 *
 * 数据来源（**两条，按优先级**）：
 *
 *   1) SD 卡 sdmc:/3ds/aiconsole/guide_kb.bin —— 可通过内容更新通道替换
 *      （shared/cupd.c 的 CU_KB）。文件是**未压缩的 GKB1**（408,004 字节）。
 *   2) 编译期内嵌的 nnet_kb_data.c / gkb_data[] —— **GKZ 压缩**（164,614 字节）。
 *
 *   1 找不到、或格式不对，自动回退 2。**内嵌那份永远保留在二进制里**，
 *   所以「SD 卡拔了也能用」这条老约束没有被破坏 —— 代价只是 CIA 大了 165KB。
 *
 * ⚠️ 2026-10-04 之前只读内嵌，于是 cupd.c 把新攻略库装到卡上**也不生效**，
 * 是一个「假更新」。这类断点只有把每类内容的**实际加载路径**逐条查一遍才会暴露：
 * 模型走 nnet_load_file() 读 SD，攻略库走内嵌，硬件知识库当时根本没有读者。
 * 教训记在 cupd.h 里：**新增一种可更新内容，必须同时改它的读者。**
 *
 * 对应关系：
 *   split_game()   -> gkb_split_game()
 *   query_tokens() -> gkb_query_tokens()
 *   BM25 + idf^2   -> gkb_search()
 *   覆盖度^0.5      -> 同
 *   概述段降权     -> 同
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>

#include "guide_kb.h"
#include "nnet_text.h"
#include "nnet.h"     /* nnet_text_encode / nnet_text_decode 声明在这里 */

extern const unsigned char gkb_data[];
extern const unsigned int  gkb_data_len;

/* ---------------- BM25 参数（必须与 guide_search.py 一致） ---------------- */
#define K1 2.2f
#define BB 0.75f
#define COV_ALPHA   0.5f       /* 覆盖度指数 */
#define OVERVIEW_F  0.35f      /* 「游戏概述」段落降权 */

#define STOPSUB_N 19
static const char *g_stopsub[STOPSUB_N] = {
    "怎么", "如何", "哪里", "在哪", "什么", "为什么", "为何", "多少", "要点", "用处", "教程",
    "方法", "介绍", "意思", "能否", "应该", "什么用", "有啥", "咋",
};

#define STOPEX_N 29
static const char *g_stopex[STOPEX_N] = {
    "的", "了", "吗", "呢", "吧", "啊", "和", "与", "在", "是", "我", "会", "要",
    "个", "这", "那", "上", "下", "中", "之", "其", "有", "以及", "还有", "可以", "需要",
    "没有", "有没有", "哪",
};

/* UTF-8 子串匹配。needle / haystack 都不保证以\0 开头有效，
 * 传长度。用不上 strlen 是因为语料是连续 blob，段落末尾不是 NUL。 */
static int gkb_has(const char *hay, int hlen, const char *ned)
{
    int nl = (int)strlen(ned);
    int i, j;
    if (nl == 0 || nl > hlen) return 0;
    for (i = 0; i <= hlen - nl; i++) {
        for (j = 0; j < nl; j++)
            if (hay[i + j] != ned[j]) break;
        if (j == nl) return 1;
    }
    return 0;
}

/* ---------------- 游戏名 -> game_id ----------------
 *
 * ⚠️⚠️ **下标顺序必须与 build_kb.py 的 sorted(glob("guide_*.txt")) 一致**，
 * 而且这张表**曾经错位了 12 款**（2026-10-04 修）：原来只有 19 款，
 * 语料扩到 31 款后没跟着改，于是全部 19 款的下标都是错的
 * （fe_awakening 真实 id=2，表里写 0），另外 12 款根本不在表里。
 *
 * 后果不是"少支持几款"，而是**限定游戏时过滤到别的游戏** ——
 * `gkb_search` 拿 gid 比 `pass_game[pid]`，gid 错位就命中别的游戏的段落，
 * 而现象只是「答案不对」，不报错也不崩。
 *
 * → 现在这张表与 g_game_names 都由 **tools/guia/gen_game_tables.py
 *   从语料目录生成**，别再手抄。改完语料先跑那个脚本，把输出贴回来。
 *   它的自检还会抓「别名跨游戏互相包含」的问题（见下）。
 *
 * 自检：GAME_N 必须等于 gkb_data 头里的 n_game，不等就是又错位了。
 *
 * 只放**游戏名**，绝不放道具名 —— 别名是从问句里**剔除**的，
 * 放了「沙之杖」会导致「沙之杖怎么用」被整句消解成空串。
 *
 * ⚠️ 别名必须**互不为子串**（跨游戏）。踩过两次：
 *   「三角力量」抢 zelda_albw / zelda_alttp 的问句（名字里都含这四个字）
 *   「3D 大陆」抢 mario_3d_land_2 的问句（后者是前者加个 2）
 * 判据：别名 A 被 B 包含时**删掉短的那个**（短的更容易撞车）。
 * 剩下的少数「长包含短」靠**取最长命中**化解 —— gkb_split_game 里的
 * `al > best_len` 正是这个作用，那一行不能改成「第一个命中就返回」。
 */
/* ⚠️ game 必须 int16：game_id 是按语料文件名字母序分配的下标，
 * 语料扩到 211 款时最大值 210，int8 的 127 会回绕成负数。同 pass_game 那条理由。 */
typedef struct { const char *alias; int16_t game; } GkbAlias;

static const GkbAlias g_alias[] = {
       { "逆转裁判4", 0 }, { "逆转裁判 4", 0 }, { "逆转裁判", 0 },
       { "逆转裁判4 阿波罗", 1 }, { "逆转裁判4 Apollo Justice", 1 },
       { "Apollo Justice", 1 }, { "阿波罗·杰特", 1 }, { "Apollo", 1 },
       { "逆转裁判5", 2 }, { "逆转裁判 5", 2 }, { "逆转裁判 V", 2 },
       { "逆转裁判6", 3 }, { "逆转裁判 6", 3 }, { "逆转裁判 VI", 3 },
       { "皇牌空战 突击地平线 遗产", 4 }, { "皇牌空战 AH 遗产", 4 },
       { "Ace Combat 突击地平线", 4 }, { "Ace Combat Assault Horizon Legacy", 4 },
       { "皇牌空战 突击地平线", 4 },
       { "After Burner II 3D", 5 }, { "3D After Burner II", 5 },
       { "After Burner II", 5 }, { "After Burner", 5 },
       { "After Burner 2", 5 },
       { "Anchor Point", 6 }, { "アンカーポイント", 6 }, { "AnchorPoint", 6 },
       { "Anchor Point 悬疑 ADV", 6 },
       { "愤怒的小鸟", 7 }, { "Angry Birds", 7 },
       { "动物之森", 8 }, { "动森", 8 }, { "动物森格", 8 },
       { "Another Code", 9 }, { "アナザーコード", 9 }, { "Another Code 二つの記憶", 9 },
       { "二つの記憶", 9 }, { "Another Code 两个记忆", 9 },
       { "狂野飙车", 10 }, { "狂野飙车 3D", 10 }, { "Asphalt 3D", 10 },
       { "Asphalt3D", 10 },
       { "以撒的结合 重生", 11 }, { "以撒的结合", 11 },
       { "The Binding of Isaac Rebirth", 11 }, { "Binding of Isaac", 11 },
       { "以撒", 11 },
       { "生物突击队", 12 }, { "生化臂", 12 }, { "Bionic Commando", 12 },
       { "High Voltage", 12 },
       { "BIT.TRIP RUNNER", 13 }, { "Bit.Trip Runner", 13 },
       { "BIT TRIP RUNNER", 13 }, { "BIT.TRIP SAGA", 13 },
       { "Bit.Trip Runner 跑酷", 13 },
       { "苍翼默示录 连续变换 II", 14 }, { "苍翼默示录 连续变换II", 14 },
       { "BlazBlue Continuum Shift II", 14 }, { "苍翼默示录 连续变换", 14 },
       { "苍翼默示录", 14 },
       { "牧场物语 起源的大地", 15 }, { "牧场物语起源的大地", 15 }, { "牧场物语起源", 15 },
       { "Bokujou Origin no Daichi", 15 }, { "牧场物语 起源", 15 },
       { "牧场物语 连接新天地", 16 }, { "牧场物语连接新天地", 16 },
       { "Bokujou Tsunagaru Shintenchi", 16 }, { "牧场物语 结界新天地", 16 },
       { "牧场物语 三个小村的珍贵朋友", 17 }, { "牧场物语三个小村的珍贵朋友", 17 },
       { "牧场物语 三个小村", 17 }, { "Bokumono 3 Villages", 17 },
       { "牧场物语 珍贵朋友", 17 },
       { "炸弹人", 18 }, { "炸弹人 Blitz", 18 }, { "Bomberman", 18 },
       { "BoxBoy", 19 }, { "BoxBoy!", 19 }, { "Qbby", 19 }, { "方块男孩", 19 },
       { "Boxboy! 方块", 19 },
       { "勇气默示录", 20 }, { "勇气默示", 20 }, { "BRAVELY DEFAULT", 20 },
       { "Brunswick Pro Bowling", 21 }, { "Brunswick", 21 }, { "保龄球", 21 },
       { "Pro Bowling", 21 },
       { "恶魔城", 22 }, { "恶魔城 历险记", 22 }, { "恶魔城历险记", 22 }, { "血之轮回", 22 },
       { "Castlevania", 22 },
       { "Chase 冷案调查", 23 }, { "Chase Cold Case", 23 }, { "冷案调查", 23 },
       { "Distant Memories", 23 }, { "Chase Cold Case Investigations", 23 },
       { "尸体派对", 24 }, { "尸体派对 血之覆盖", 24 }, { "Corpse Party", 24 },
       { "コープスパーティ", 24 }, { "尸体派对3DS", 24 },
       { "成步堂龙之介", 25 }, { "Dai Gyakuten Saiban", 25 },
       { "成步堂龙之介的觉悟", 26 }, { "Dai Gyakuten Saiban 2", 26 },
       { "弹丸论破", 27 }, { "枪弹辩驳", 27 }, { "Danganronpa", 27 }, { "黑白熊", 27 },
       { "弹丸论破2", 28 }, { "弹丸论破 2", 28 },
       { "脱出冒险 噩梦的死神列车", 29 }, { "噩梦的死神列车", 29 }, { "脱出冒险 死神列车", 29 },
       { "Escape Adventure Death God", 29 },
       { "脱出冒险 神降临的占卜盘", 30 }, { "神降临的占卜盘", 30 }, { "脱出冒险 占卜盘", 30 },
       { "脱出冒险 神降臨", 30 },
       { "脱出冒险 旧校舎の少女", 31 }, { "脱出冒险 旧校舍的少女", 31 }, { "旧校舍的少女", 31 },
       { "旧校舎", 31 }, { "脱出冒险 旧校舎", 31 },
       { "脱出冒险 魔女的栖馆", 32 }, { "魔女的栖馆", 32 }, { "脱出冒险 魔女", 32 },
       { "Escape Adventure Mansion", 32 },
       { "脱出冒险 幸福的红石", 33 }, { "幸福的红石", 33 }, { "脱出冒险 赤い石", 33 },
       { "脱出冒险 红石", 33 },
       { "脱出冒险 终焉的黑雾", 34 }, { "终焉的黑雾", 34 }, { "脱出冒险 黒霧", 34 },
       { "脱出冒险 黑雾", 34 },
       { "脱出冒险 绝望要塞", 35 }, { "绝望要塞", 35 }, { "脱出アドベンチャー 絶望要塞", 35 },
       { "脱出冒险 要塞", 35 },
       { "死或生 多重维度", 36 }, { "死或生 Dimensions", 36 },
       { "DOA Dimensions", 36 }, { "死或生 维度", 36 },
       { "Dead or Alive Dimensions", 36 },
       { "名侦探柯南 傀儡交响曲", 37 }, { "柯南 傀儡交响曲", 37 },
       { "Detective Conan Marionette", 37 }, { "コナン マリオネット交響曲", 37 },
       { "名侦探柯南 幻影狂诗曲", 38 }, { "柯南 幻影狂诗曲", 38 },
       { "Detective Conan Phantom Rhapsody", 38 }, { "コナン ファントム狂詩曲", 38 },
       { "名侦探皮卡丘", 39 }, { "Detective Pikachu", 39 }, { "ピカチュウ 探偵", 39 },
       { "皮卡丘 侦探", 39 },
       { "怒首领蜂 复活", 40 }, { "怒首领蜂复活", 40 },
       { "DoDonPachi Resurrection", 40 }, { "怒首领蜂", 40 },
       { "怒首领蜂  redemption", 40 },
       { "森喜刚", 41 }, { "森喜刚之国", 41 }, { "大金刚", 41 },
       { "DK Country Returns", 41 },
       { "DOOORS", 42 }, { "DOOORS room escape", 42 }, { "DOOORS 脱出", 42 },
       { "脱出 解谜", 42 },
       { "勇者斗恶龙 罗德三部曲", 43 }, { "DQ 罗德三部曲", 43 }, { "勇者斗恶龙 I II III", 43 },
       { "罗德三部曲", 43 }, { "勇者斗恶龙 1 2 3", 43 },
       { "勇者斗恶龙怪兽篇2", 44 }, { "伊鲁与露卡", 44 }, { "DQ Monsters 2", 44 },
       { "勇者斗恶龙怪兽篇 2", 44 }, { "不可思议钥匙", 44 },
       { "勇者斗恶龙怪兽篇 Joker3", 45 }, { "DQ Joker3", 45 }, { "Joker3", 45 },
       { "勇者斗恶龙怪兽篇Joker3", 45 }, { "怪兽篇 Joker3", 45 },
       { "勇者斗恶龙怪兽篇 泰瑞的奇幻乐园", 46 }, { "泰瑞的奇幻乐园", 46 },
       { "DQ Monsters Terry", 46 }, { "勇者斗恶龙怪兽篇 泰瑞", 46 },
       { "勇者斗恶龙", 47 }, { "勇者斗恶龙 VII", 47 }, { "勇者斗恶龙7", 47 }, { "DQ7", 47 },
       { "伊甸的战士们", 47 },
       { "勇者斗恶龙VIII 3D", 48 }, { "勇者斗恶龙 VIII 3D", 48 }, { "DQ8 3D", 48 },
       { "被诅咒王的冒险", 48 }, { "勇者斗恶龙8 3D", 48 },
       { "escapeVektor", 49 }, { "Escape Vektor", 49 }, { "Vektor", 49 },
       { "escape Vektor", 49 },
       { "ay a 地牢4", 50 }, { "ay a 地牢", 50 }, { "Etrian Odyssey IV", 50 },
       { "Etrian Odyssey 4", 50 }, { "ay a 地牢 IV", 50 },
       { "F1 2011", 51 }, { "F1", 51 }, { "一级方程式 2011", 51 },
       { "F1 赛车 2011", 51 },
       { "幻想生活", 52 }, { "Fantasy Life", 52 }, { "FantasyLife", 52 },
       { "幻想生活 职业", 52 },
       { "Fantasy Zone 3D", 53 }, { "3D Fantasy Zone", 53 },
       { "Opa-Opa Bros", 53 }, { "Fantasy Zone", 53 }, { "FantasyZone", 53 },
       { "火焰纹章觉醒", 54 }, { "火焰纹章 觉醒", 54 }, { "火焰纹章：觉醒", 54 },
       { "火焰纹章", 54 }, { "FE觉醒", 54 },
       { "火焰之纹章", 55 }, { "FEIF", 55 }, { "火纹IF", 55 },
       { "最终幻想 探索者", 56 }, { "最终幻想探索者", 56 },
       { "Final Fantasy Explorers", 56 }, { "FF 探索者", 56 },
       { "最终幻想 探险者", 56 },
       { "火焰纹章 外传", 57 }, { "火焰纹章外传", 57 }, { "圣魔光石", 57 },
       { "火焰纹章无双", 58 }, { "火焰之纹章 无双", 58 }, { "Fire Emblem Warriors", 58 },
       { "火焰纹章 无双", 58 },
       { "Fractured Soul", 59 }, { "FracturedSoul", 59 }, { "破碎灵魂", 59 },
       { "Fractured Soul 3DS", 59 },
       { "Galaxy Force II 3D", 60 }, { "3D Galaxy Force II", 60 },
       { "Galaxy Force II", 60 }, { "Galaxy Force 2", 60 },
       { "枪手克莱夫", 61 }, { "Gunman Clive", 61 }, { "Clive", 61 },
       { "Gunman Clive 西部", 61 },
       { "逆转裁判123", 62 }, { "逆转裁判 123", 62 }, { "逆转裁判123 成步堂精选集", 62 },
       { "成步堂精选集", 62 }, { "Gyakuten Saiban 123", 62 },
       { "逆転裁判5", 63 }, { "逆転裁判 V", 63 }, { "Gyakuten Saiban 5", 63 },
       { "逆転裁判6", 64 }, { "逆転裁判 VI", 64 }, { "Gyakuten Saiban 6", 64 },
       { "初音未来 Project DIVA f", 65 }, { "Project DIVA f", 65 },
       { "初音ミク Project DIVA f", 65 }, { "Hatsune Miku Project DIVA f", 65 },
       { "初音未来 DIVA f", 65 },
       { "Hotel Dusk", 66 }, { "ホテルダスク", 66 }, { "Hotel Dusk ルーム402", 66 },
       { " hotels 侦探", 66 },
       { "塞尔达无双 传奇", 67 }, { "塞尔达无双", 67 },
       { "Hyrule Warriors Legends", 67 }, { "塞尔达无双 传说", 67 },
       { "闪电十一人", 68 }, { "闪电十一人 GO", 68 }, { "闪电十一人GO", 68 },
       { "Inazuma Eleven", 68 },
       { "假面骑士 旅行者战记", 69 }, { "假面骑士旅行者战记", 69 }, { "假面骑士 铠武", 69 },
       { "仮面ライダー トラベラーズ", 69 },
       { "Karous", 70 }, { "Karous The Beast of Re Eden", 70 },
       { "The Beast of Re Eden", 70 }, { "KAROUS", 70 },
       { "伊卡洛斯之翼", 71 }, { "伊卡洛斯", 71 }, { "米特罗德", 71 },
       { "Kid Icarus", 71 }, { "pit", 71 }, { "PIT", 71 },
       { "王国之心", 72 }, { "王国之心 II", 72 }, { "王国之心II", 72 }, { "KH2", 72 },
       { "星之卡比", 73 }, { "卡比", 73 }, { "Kirby", 73 },
       { "星之卡比 机械星球", 74 }, { "卡比 机械星球", 74 }, { "卡比机械星球", 74 },
       { "卡比三重彩", 74 },
       { "Last Window", 75 }, { "ラストウィンドウ", 75 }, { "Last Window 声の聖域", 75 },
       { "last window 侦探", 75 },
       { "雷顿教授与奇迹之假面", 76 }, { "雷顿教授 奇迹之假面", 76 }, { "奇迹之假面", 76 },
       { "LaytonMM", 76 },
       { "雷顿教授VS逆転裁判", 77 }, { "Layton vs Gyakuten Saiban", 77 },
       { "雷顿教授的神秘旅程", 78 }, { "卡图莉耶与百万富翁的阴谋", 78 },
       { "LAYTONS MYSTERY JOURNEY", 78 }, { "雷顿教授 神秘旅程", 78 },
       { "雷顿教授 百万富翁", 78 },
       { "塞尔达传说 三角力量", 79 },
       { "伦敦奥运会 2012", 80 }, { "2012 伦敦奥运", 80 }, { "London 2012", 80 },
       { "伦敦奥运 2012", 80 },
       { "路易吉洋馆2", 81 }, { "路易吉洋馆 2", 81 }, { "路易吉洋馆", 81 }, { "洋馆", 81 },
       { "路易吉", 81 },
       { "超级马力欧 3D 大陆", 82 }, { "马力欧 3D 大陆", 82 }, { "马力欧3D大陆", 82 },
       { "猫咪幽灵", 82 },
       { "3D 大陆 2", 83 }, { "3D大陆2", 83 }, { "猫咪幽灵2", 83 }, { "马力欧马里奥", 83 },
       { "马力欧高尔夫", 84 }, { "马里奥高尔夫", 84 }, { "Mario Golf", 84 },
       { "马力欧派对", 85 }, { "超级马力欧派对", 85 }, { "Mario Party", 85 },
       { "马力欧派对 星际大冲刺", 86 }, { "马力欧派对星际大冲刺", 86 }, { "派对 星际大冲刺", 86 },
       { "Mario Sonic Rio 2016", 87 }, { "里约 2016", 87 },
       { "马力欧体育超级明星", 88 }, { "马力欧体育 超级明星", 88 },
       { "Mario Sports Superstars", 88 }, { "马力欧体育", 88 },
       { "马力欧网球", 89 }, { "马里奥网球", 89 }, { "Mario Tennis", 89 },
       { "洛克人", 90 }, { "洛克人 遗产", 90 }, { "Mega Man", 90 }, { "洛克人 X", 90 },
       { "Metal Slug 7", 91 }, { "メタルスラッグ 7", 91 }, { "合金弹头7", 91 },
       { "Metal Slug", 91 },
       { "合金装备", 92 }, { "合金装备3D", 92 }, { "MGS3", 92 },
       { "Snake Eater", 92 }, { "食蛇者", 92 },
       { "怪物猎人3G", 93 }, { "怪物猎人 3G", 93 }, { "怪物猎人Tri", 93 },
       { "怪猎3G", 93 },
       { "怪物猎人4G", 94 }, { "怪物猎人 4G", 94 }, { "怪物猎人", 94 }, { "怪猎4G", 94 },
       { "怪猎", 94 },
       { "怪物猎人 世代", 95 }, { "怪物猎人世代", 95 }, { "怪物猎人X", 95 }, { "怪猎X", 95 },
       { "MHX", 95 },
       { "怪物猎人XX", 96 }, { "怪物猎人 XX", 96 }, { "怪物猎人GU", 96 },
       { "怪物猎人Generations", 96 }, { "怪猎XX", 96 },
       { "Mighty Switch Force", 97 }, { "Mighty Switch Force!", 97 },
       { "强力开关", 97 }, { "Mighty Switch Force 3D", 97 },
       { "Minecraft", 98 }, { "MC", 98 }, { "我的世界", 98 }, { "mc", 98 },
       { "马力欧卡丁车7", 99 }, { "马力欧卡丁车 7", 99 }, { "马卡丁车7", 99 },
       { "马力欧卡丁车", 99 }, { "卡丁车", 99 }, { "赛车", 99 },
       { "变异泥怪", 100 }, { "Mutant Mudds", 100 }, { "MutantMudds", 100 },
       { "变异泥怪 平台", 100 },
       { "火影忍者疾风传 忍立体绘卷", 101 }, { "火影忍者 疾风传 忍立体绘卷", 101 },
       { "最强忍界决战", 101 }, { "Naruto 3D 新时代", 101 },
       { "极品飞车 亡命狂飙", 102 }, { "极品飞车亡命狂飙", 102 },
       { "Need for Speed The Run", 102 }, { "NFS 亡命狂飙", 102 },
       { "极品飞车 狂飙", 102 },
       { "新超级马力欧兄弟 U", 103 }, { "新超级马力欧兄弟 U 版", 103 }, { "新超级马力欧 U", 103 },
       { "NSMBU", 103 },
       { "新超级马力欧兄弟 2D", 104 }, { "新超级马力欧", 104 }, { "新马力欧", 104 },
       { "新超级马力欧兄弟", 104 }, { "NSMB", 104 }, { "2D马力欧", 104 },
       { "一百万金币", 104 },
       { "忍者龙剑传 龙剑", 105 }, { "忍者龙剑传龙剑", 105 },
       { "Ninja Gaiden Dragon Sword", 105 }, { "忍者龙剑传 剑", 105 },
       { "OlliOlli", 106 }, { "滑板", 106 }, { "OlliOlli 3DS", 106 },
       { "OlliOlli 超难", 106 },
       { "海贼王 无限巡航 SP", 107 }, { "ONE PIECE 无限巡航 SP", 107 },
       { "One Piece Unlimited Cruise SP", 107 }, { "无限巡航 SP", 107 },
       { "海贼王 无限巡航", 107 },
       { "吃豆人", 108 }, { "吃豆人 幽灵历险记", 108 }, { "Pac-Man", 108 },
       { "纸片马力欧", 109 }, { "纸片马力欧RPG", 109 }, { "超级贴纸", 109 },
       { "超科学脱出 最果てのクロスアイズ", 110 }, { "超科学脱出 最果て", 110 },
       { "Parascientific Escape Crossing", 110 }, { "超科学脱出 最果ての", 110 },
       { "超科学脱出 絶海の豪華客船", 111 }, { "超科学脱出 绝海豪华客船", 111 },
       { "Parascientific Escape Cruise", 111 }, { "超科学脱出 绝海", 111 },
       { "超科学脱出 齿轮侦探", 112 }, { "超科学脱出 アドベンチャー", 112 },
       { "Parascientific Escape Gear Detective", 112 }, { "超科学脱出 齿轮", 112 },
       { "女神异闻录Q", 113 }, { "女神异闻录 Q", 113 }, { "Persona Q", 113 },
       { "女神异闻录Q2", 114 }, { "女神异闻录 Q2", 114 }, { "Persona Q2", 114 },
       { "新 cinema 迷宫", 114 }, { "女神异闻录 Q2 迷宫", 114 },
       { "Picross 3D", 115 }, { "立体 Picross", 115 }, { "立体数独", 115 },
       { "Picross 3D 立体", 115 },
       { "宝可梦黑2", 116 }, { "宝可梦白2", 116 }, { "宝可梦 黑2", 116 },
       { "宝可梦 白2", 116 }, { "宝可梦 黑白2", 116 }, { "B2W2", 116 },
       { "宝可梦黑", 117 }, { "宝可梦白", 117 }, { "宝可梦 黑", 117 }, { "宝可梦 白", 117 },
       { "宝可梦黑白", 117 }, { "BW", 117 },
       { "宝可梦 征服者", 118 }, { "宝可梦征服者", 118 }, { "征服者", 118 },
       { "Pokemon Conquest", 118 },
       { "宝可梦 钻石", 119 }, { "宝可梦 珍珠", 119 }, { "宝可梦 白金", 119 },
       { "宝可梦 钻石珍珠", 119 }, { "珍珠钻石", 119 }, { "DPPt", 119 },
       { "宝可梦绿宝石", 120 }, { "绿宝石", 120 },
       { "宝可梦红宝石", 121 }, { "宝可梦蓝宝石", 121 }, { "红宝石", 121 }, { "蓝宝石", 121 },
       { "红宝石蓝宝石", 121 }, { "RSE", 121 },
       { "宝可梦 金", 122 }, { "宝可梦 银", 122 }, { "宝可梦 金·银", 122 },
       { "宝可梦金银", 122 }, { "宝可梦 金银", 122 }, { "GSC", 122 },
       { "宝可梦 不可思议的迷宫", 123 }, { "不可思议的迷宫", 123 }, { "宝可梦迷宫", 123 },
       { "迷宫 3DS", 123 },
       { "宝可梦 像素解谜", 124 }, { "宝可梦 Picross", 124 },
       { "Pokemon Picross", 124 }, { "宝可梦数织", 124 }, { "宝可梦 拼图", 124 },
       { "宝可梦 大乱战 爆裂", 125 }, { "宝可梦大乱战爆裂", 125 },
       { "Pokemon Rumble Blast", 125 }, { "宝可梦大乱战", 125 },
       { "发条玩具宝可梦", 125 },
       { "宝可梦 Rumble World", 126 }, { "宝可梦 乱战", 126 },
       { "Pokemon Rumble World", 126 },
       { "宝可梦 Shuffle", 127 }, { "宝可梦碰撞", 127 }, { "Pokemon Shuffle", 127 },
       { "宝可梦太阳", 128 }, { "宝可梦 太阳", 128 }, { "宝可梦月亮", 128 },
       { "太阳月亮", 128 }, { "SM", 128 },
       { "宝可梦 究极之日", 129 }, { "宝可梦 究极之月", 129 }, { "宝可梦 究极", 129 },
       { "究极之日", 129 }, { "USUM", 129 },
       { "宝可梦XY", 130 }, { "宝可梦X", 130 }, { "宝可梦Y", 130 }, { "宝可梦 X", 130 },
       { "宝可梦 Y", 130 }, { "XY", 130 },
       { "Project DIVA f 2nd", 131 }, { "初音未来 DIVA f 2nd", 131 },
       { "初音未来 Project Mirai DX", 132 }, { "Project Mirai", 132 },
       { "Hatsune Miku Project Mirai", 132 }, { "初音未来 未来组装", 132 },
       { "Punch-Out 3D", 133 }, { "Mike Tyson Punch Out", 133 },
       { "Punch Out 3D", 133 }, { "拳击 3D", 133 },
       { "Pushmo", 134 }, { "Pullblox", 134 }, { "推箱解谜", 134 },
       { "Pushmo 推箱", 134 },
       { "Radiant Historia 完美编年史", 135 }, { "Radiant Historia", 135 },
       { "完美编年史", 135 }, { "光辉编年史", 135 },
       { "雷曼", 136 }, { "疯狂兔子", 136 }, { "Rayman", 136 },
       { "生化危机 启示录", 137 }, { "生化危机启示录", 137 }, { "生化危机 启示", 137 },
       { "Revelations", 137 },
       { "雷顿教授", 138 }, { "不可思议的小镇", 138 }, { "雷顿", 138 },
       { "生化危机", 139 }, { "双重迷宫", 139 },
       { "波斯王子", 140 }, { "时之砂", 140 },
       { "节奏天堂 Megamix", 141 }, { "Rhythm Heaven Megamix", 141 },
       { "Rhythm Paradise Megamix", 141 }, { "节奏天堂", 141 },
       { "リズム天国 ザ・ベスト", 141 },
       { "节奏天国 The Best+", 142 }, { "リズム天国 ザ・ベスト+", 142 },
       { "Rhythm Heaven The Best", 142 }, { "节奏天国 最佳", 142 },
       { "Rhythm Paradise", 142 },
       { "山脊赛车 3D", 143 }, { "山脊赛车3D", 143 }, { "Ridge Racer 3D", 143 },
       { "山脊赛车", 143 },
       { "彩虹酷跑", 144 }, { "Runbow", 144 }, { "Run Bow", 144 },
       { "彩虹酷跑 Runbow", 144 },
       { "SEGA 3D 经典合集", 145 }, { "3D 经典合集", 145 },
       { "Sega 3D Classics", 145 }, { "SEGA 3D 经典", 145 },
       { "战国无双 编年史", 146 }, { "战国无双编年史", 146 },
       { "Sengoku Musou Chronicle", 146 }, { "战国无双 编年", 146 },
       { "战国无双 编年史 2nd", 147 }, { "战国无双编年史2nd", 147 },
       { "Sengoku Musou Chronicle 2nd", 147 }, { "战国无双 编年史 2", 147 },
       { "战国无双 编年史 3", 148 }, { "战国无双编年史3", 148 },
       { "Sengoku Musou Chronicle 3", 148 }, { "战国无双 编年史 3 代", 148 },
       { "山塔海盗诅咒", 149 }, { "Shantae and the Pirates Curse", 149 },
       { "半精灵 海盗诅咒", 149 }, { "Shantae", 149 }, { "山塔 海盗诅咒", 149 },
       { "真·女神转生IV", 150 }, { "真女神转生4", 150 }, { "SMT4", 150 },
       { "真·女神转生 IV 3DS", 150 },
       { "真·三国无双 DS", 151 }, { "斗士之战", 151 }, { "真三国无双DS斗士之战", 151 },
       { "Shin Sangokumusou DS", 151 }, { "真·三国无双 DS 斗士", 151 },
       { "铲子骑士", 152 }, { "Shovel Knight", 152 },
       { "任天堂明星大乱斗", 153 }, { "明星大乱斗", 153 }, { "大乱斗", 153 },
       { "Smash", 153 },
       { "真·女神转生IV FINAL", 154 }, { "真·女神转生IV最终篇", 154 },
       { "SMT IV FINAL", 154 }, { "真·女神转生 IV FINAL", 154 },
       { "Deep No M", 154 },
       { "真·女神转生 IV", 155 }, { "真女神转生IV", 155 }, { "SMT IV", 155 },
       { "真·女神转生IV Apocalypse", 156 }, { "真·女神转生IV 觉醒", 156 },
       { "SMT IV Apocalypse", 156 }, { "SMT4 Apocalypse", 156 },
       { "真·女神转生IV 最终", 156 },
       { "真·女神转生 恶魔召唤师 灵魂黑客", 157 }, { "恶魔召唤师 灵魂黑客", 157 },
       { "Devil Summoner Soul Hackers", 157 }, { "灵魂黑客", 157 },
       { "真·女神转生 灵魂黑客", 157 },
       { "真·女神转生 奇异之旅", 158 }, { "真·女神转生Strange Journey", 158 },
       { "Strange Journey", 158 }, { "SMT Strange Journey", 158 },
       { "奇异之旅", 158 },
       { "3D 索尼克", 159 }, { "索尼克3D", 159 }, { "索尼克", 159 },
       { "索尼克与全明星赛车", 160 }, { "索尼克全明星赛车 变形", 160 },
       { "Sonic All Stars Racing", 160 }, { "索尼克 赛车", 160 },
       { "索尼克与全明星赛车 变形", 160 },
       { "索尼克 世代", 161 }, { "索尼克世代", 161 }, { "索尼克 3D 世代", 161 },
       { "星际火狐", 162 }, { "星际火狐 64", 162 }, { "星际火狐64", 162 },
       { "Star Fox", 162 },
       { "星际火狐 Command", 163 }, { "星际火狐Command", 163 },
       { "Star Fox Command", 163 }, { "星际火狐 命令", 163 },
       { "STEAM", 164 }, { "蒸汽", 164 }, { "CodeName", 164 },
       { "蒸汽世界 挖掘", 165 }, { "SteamWorld Dig", 165 }, { "蒸汽世界挖掘", 165 },
       { "蒸汽世界 Dig", 165 },
       { "蒸汽世界 挖掘2", 166 }, { "SteamWorld Dig 2", 166 }, { "蒸汽世界挖掘2", 166 },
       { "蒸汽世界 Dig 2", 166 },
       { "蒸汽世界 抢劫", 167 }, { "SteamWorld Heist", 167 }, { "蒸汽世界抢劫", 167 },
       { "蒸汽世界 Heist", 167 },
       { "钢铁潜水员", 168 }, { "钢铁潜水员 潜艇大战", 168 }, { "潜艇大战", 168 },
       { "Stella Glow", 169 }, { "ステラ グロウ", 169 }, { "星辉", 169 },
       { "Stella Glow 星之辉", 169 },
       { "Story of Seasons", 170 }, { "牧场物语 Story of Seasons", 170 },
       { "牧场物语 四季", 170 }, { "牧场物语 四季旅程", 170 },
       { "New Style Boutique", 171 }, { "Style Savvy", 171 },
       { "New Style Boutique 换装", 171 }, { "换装 经营 ", 171 },
       { "换装 Boutique", 171 },
       { "超级马力欧 64", 172 }, { "超级马力欧64", 172 }, { "马力欧 64", 172 },
       { "SM64", 172 },
       { "超级马力欧制造者", 173 }, { "马力欧制造者", 173 }, { "超级马力欧制造", 173 },
       { "SMM", 173 },
       { "超级猴子球 3D", 174 }, { "超级猴子球3D", 174 },
       { "Super Monkey Ball 3D", 174 }, { "猴子球", 174 },
       { "超级街头霸王IV 3D", 175 }, { "超级街霸IV 3D", 175 },
       { "Super Street Fighter IV 3D", 175 }, { "超级街霸", 175 },
       { "超级街头霸王 3D", 175 },
       { "太鼓达人", 176 }, { "太鼓の達人", 176 }, { "太鼓", 176 }, { "Taiko", 176 },
       { "深渊传说", 177 }, { "Tales of the Abyss", 177 }, { "TOT A", 177 },
       { "深渊传说 音素", 177 },
       { "铁拳3D", 178 }, { "铁拳 3D Prime Edition", 178 },
       { "Tekken 3D Prime Edition", 178 }, { "铁拳6 血之复仇", 178 },
       { "铁拳 3D", 178 },
       { "泰拉瑞亚", 179 }, { "Terraria", 179 },
       { "The Alliance Alive", 180 }, { "同盟 Alive", 180 }, { "同盟存活", 180 },
       { "The Alliance Alive 3DS", 180 },
       { "暗黑女巫传说", 181 }, { "The Legend of Dark Witch", 181 },
       { "魔法少女 传说", 181 }, { "暗黑女巫", 181 },
       { "Theatrhythm Final Fantasy", 182 }, { "最终幻想 节奏剧场", 182 },
       { "FF 节奏剧场", 182 }, { "最终幻想 节奏", 182 },
       { "Thunder Blade 3D", 183 }, { "3D Thunder Blade", 183 },
       { "Thunder Blade", 183 }, { "Thunder Blade 3D 空战", 183 },
       { "小鸡快跑3D", 184 }, { "Toki Tori 3D", 184 }, { "小鸡快跑", 184 },
       { "Toki Tori 小鸡", 184 },
       { "朋友聚会", 185 }, { "朋友聚会 生活", 185 }, { "朋友聚会生活", 185 },
       { "Tomodachi", 185 },
       { "TwinBee 3D", 186 }, { "3D 经典 TwinBee", 186 }, { "TwinBee", 186 },
       { "TwinBee 3D 射击", 186 },
       { "2 Fast 4 Gnomz", 187 }, { "2FAST4GNOMZ", 187 }, { "小矮人", 187 },
       { "2 Fast 4 Gnomz 跑酷", 187 },
       { "经典 FC 合集", 188 }, { "Virtual Console", 188 }, { "VC 怀旧", 188 },
       { "经典 SFC 合集", 189 }, { "魂斗罗 3", 189 }, { "魂斗罗精神", 189 },
       { "Super Famicom", 189 },
       { "VVVVVV", 190 }, { "VVVVVV 3DS", 190 }, { "Captain Viridian", 190 },
       { "重力翻转", 190 },
       { "瓦力欧制造", 191 }, { "WarioWare", 191 },
       { "WE足球", 192 }, { "WE 足球", 192 }, { "实况足球", 192 },
       { "冬季运动 2012", 193 }, { "冬季运动2012", 193 },
       { "Winter Sports 2012", 193 }, { "冬季运动 感受激情", 193 },
       { "WWE 全明星", 194 }, { "WWE All Stars", 194 }, { "WWE", 194 },
       { "WWE 摔角", 194 },
       { "异度之刃 3D", 195 }, { "异度之刃", 195 },
       { "Xenoblade Chronicles 3D", 195 }, { "异度神剑 3D", 195 },
       { "异度之刃 移植", 195 },
       { "Xevious 3D", 196 }, { "3D 经典 Xevious", 196 }, { "Xevious", 196 },
       { "ゼビウス 3D", 196 },
       { "妖怪手表", 197 }, { "妖怪手表 2", 197 }, { "妖怪手表2", 197 }, { "妖怪", 197 },
       { "耀西的新岛", 198 }, { "耀西岛", 198 }, { "耀西", 198 }, { "Yoshi", 198 },
       { "游戏王 ZEXAL 激突 决斗嘉年华", 199 }, { "游戏王ZEXAL 激突决斗嘉年华", 199 },
       { "游戏王 决斗嘉年华", 199 }, { "Yu-Gi-Oh Zexal", 199 }, { "游戏王 激突", 199 },
       { "众神三角力量2", 200 }, { "众神三角力量 2", 200 }, { "ALBW", 200 },
       { "塞尔达传说 三角力量 重制版", 201 }, { "重制版", 201 },
       { "塞尔达传说 织梦岛", 202 }, { "织梦岛", 202 }, { "织梦岛 DX", 202 },
       { "织梦岛DX", 202 },
       { "塞尔达传说 缩小帽", 203 }, { "缩小帽", 203 }, { "Minish Cap", 203 },
       { "梅祖拉的假面", 204 }, { "梅祖拉假面", 204 }, { "梅祖拉", 204 },
       { "时之笛3D", 205 }, { "时之笛 3D", 205 }, { "时之笛", 205 },
       { "塞尔达传说 时代之笛", 206 }, { "时代之笛", 206 }, { "Oracle of Ages", 206 },
       { "塞尔达传说 四季之笛", 207 }, { "四季之笛", 207 }, { "Oracle of Seasons", 207 },
       { "塞尔达传说 幻影沙漏", 208 }, { "幻影沙漏", 208 }, { "Phantom Hourglass", 208 },
       { "塞尔达 幻影沙漏", 208 },
       { "极限脱出", 209 }, { "virtue's last reward", 209 }, { "零时刻", 209 },
       { "VLR", 209 },
       { "极限脱出 零时困境", 210 }, { "Zero Escape Zero Time Dilemma", 210 },
       { "零时困境", 210 }, { "Zero Time Dilemma", 210 }, { "极限脱出 零时", 210 },
};
#define ALIAS_N ((int)(sizeof(g_alias) / sizeof(g_alias[0])))

static const char *g_game_names[] = {
        "逆转裁判4", "逆转裁判4 阿波罗", "逆转裁判 5", "逆转裁判 6", "皇牌空战 突击地平线 遗产",
        "After Burner II 3D", "Anchor Point", "愤怒的小鸟 三合一", "动物之森 新的开始",
        "Another Code", "狂野飙车 3D", "以撒的结合：重生", "生物突击队 3D",
        "BIT.TRIP RUNNER", "苍翼默示录：连续变换 II", "牧场物语 起源的大地", "牧场物语 连接新天地",
        "牧场物语 三个小村的珍贵朋友", "炸弹人 Blitz", "BoxBoy!", "勇气默示录",
        "Brunswick Pro Bowling", "恶魔城 历险记", "Chase 冷案调查", "尸体派对",
        "大逆转裁判 成步堂龙之介的冒险", "大逆转裁判2 成步堂龙之介的觉悟", "弹丸论破 枪弹辩驳", "弹丸论破 2",
        "脱出冒险 噩梦的死神列车", "脱出冒险 神降临的占卜盘", "脱出冒险 旧校舍的少女", "脱出冒险 魔女的栖馆",
        "脱出冒险 幸福的红石", "脱出冒险 终焉的黑雾", "脱出冒险 绝望要塞", "死或生：多重维度",
        "名侦探柯南 傀儡交响曲", "名侦探柯南 幻影狂诗曲", "名侦探皮卡丘", "怒首领蜂 复活",
        "森喜刚之国 归来 3D", "DOOORS", "勇者斗恶龙 罗德三部曲 3D", "勇者斗恶龙怪兽篇2 伊鲁与露卡",
        "勇者斗恶龙怪兽篇 Joker3", "勇者斗恶龙怪兽篇 泰瑞的奇幻乐园 3D", "勇者斗恶龙 VII 伊甸的战士们",
        "勇者斗恶龙VIII：被诅咒王的冒险", "escapeVektor", "ay a 地牢4", "F1 2011",
        "幻想生活", "Fantasy Zone 3D", "火焰纹章：觉醒", "火焰之纹章 if", "最终幻想 探索者",
        "火焰纹章 外传", "火焰纹章无双", "Fractured Soul", "Galaxy Force II 3D",
        "枪手克莱夫", "逆转裁判123 成步堂精选集", "逆転裁判5", "逆転裁判6",
        "初音未来 Project DIVA f", "Hotel Dusk", "塞尔达无双 传奇", "闪电十一人 GO",
        "假面骑士 旅行者战记", "Karous", "米特罗德：伊卡洛斯之翼", "王国之心 II", "星之卡比 起源",
        "星之卡比 机械星球", "Last Window", "雷顿教授与奇迹之假面", "雷顿教授 VS 逆转裁判",
        "雷顿教授的神秘旅程", "塞尔达传说 三角力量", "伦敦奥运会 2012 官方游戏", "路易吉洋馆 2",
        "超级马力欧 3D 大陆", "超级马里奥 3D 大陆 2", "马力欧高尔夫 World Tour", "超级马力欧派对",
        "马力欧派对 星际大冲刺", "马力欧与索尼克 里约 2016", "马力欧体育超级明星", "马力欧网球 Open",
        "洛克人 遗产合集", "Metal Slug 7", "合金装备 3D 食蛇者的复仇", "怪物猎人 3G",
        "怪物猎人 4G", "怪物猎人 世代", "怪物猎人 XX", "Mighty Switch Force!",
        "Minecraft", "马力欧卡丁车 7", "变异泥怪", "火影忍者疾风传 忍立体绘卷", "极品飞车：亡命狂飙",
        "新超级马力欧兄弟 U", "新超级马力欧兄弟 2D", "忍者龙剑传 龙剑", "OlliOlli",
        "海贼王 无限巡航 SP", "吃豆人人与幽灵历险记", "纸片马力欧 RPG 超级贴纸", "超科学脱出 最果て的十字眼",
        "超科学脱出 绝海豪华客船", "超科学脱出 齿轮侦探", "女神异闻录 Q", "女神异闻录Q2 新 cinema 迷宫",
        "Picross 3D", "宝可梦 黑2·白2", "宝可梦 黑·白", "宝可梦 征服者", "宝可梦 钻石·珍珠·白金",
        "宝可梦 绿宝石", "宝可梦 红宝石·蓝宝石", "宝可梦 金·银", "宝可梦 不可思议的迷宫 3DS",
        "宝可梦 像素解谜", "宝可梦 大乱战 爆裂", "宝可梦 Rumble World", "宝可梦 Shuffle",
        "宝可梦太阳·月亮", "宝可梦 究极之日·月", "宝可梦X·Y", "初音未来 Project DIVA f 2nd",
        "初音未来 Project Mirai DX", "Punch-Out 3D", "Pushmo",
        "Radiant Historia 完美编年史", "雷曼：疯狂兔子 3D", "生化危机 启示录",
        "雷顿教授与不可思议的小镇", "生化危机：双重迷宫", "波斯王子 时之砂", "节奏天堂 Megamix",
        "节奏天国 The Best+", "山脊赛车 3D", "彩虹酷跑", "SEGA 3D 经典合集", "战国无双 编年史",
        "战国无双 编年史 2nd", "战国无双 编年史 3", "山塔 海盗诅咒", "真·女神转生IV",
        "真·三国无双 DS 斗士之战", "铲子骑士", "任天堂明星大乱斗 for 3DS", "真·女神转生IV FINAL",
        "真·女神转生 IV", "真·女神转生IV Apocalypse", "真·女神转生 恶魔召唤师 灵魂黑客",
        "真·女神转生 奇异之旅", "3D 索尼克", "索尼克与全明星赛车 变形", "索尼克 世代", "星际火狐 64 3D",
        "星际火狐 Command", "CodeName: S.T.E.A.M.", "蒸汽世界 挖掘", "蒸汽世界 挖掘2",
        "蒸汽世界 抢劫", "钢铁潜水员 潜艇大战", "Stella Glow", "Story of Seasons 牧场物语",
        "New Style Boutique", "超级马力欧 64", "超级马力欧制造者", "超级猴子球 3D",
        "超级街头霸王IV 3D", "太鼓达人 小龙人与不可思议的宝珠", "深渊传说", "铁拳3D Prime Edition",
        "泰拉瑞亚 3DS", "The Alliance Alive", "暗黑女巫传说", "Theatrhythm 最终幻想",
        "Thunder Blade 3D", "小鸡快跑3D", "朋友聚会 生活", "TwinBee 3D",
        "2 Fast 4 Gnomz", "经典 FC 怀旧合集", "经典 SFC 怀旧合集", "VVVVVV",
        "瓦力欧制造 Gold", "WE 足球", "冬季运动 2012 感受激情", "WWE 全明星", "异度之刃 3D",
        "Xevious 3D", "妖怪手表 2", "耀西的新岛", "游戏王 ZEXAL 激突 决斗嘉年华",
        "塞尔达传说：众神的三角力量2", "塞尔达传说 三角力量 重制版", "塞尔达传说 织梦岛", "塞尔达传说 缩小帽",
        "塞尔达传说：梅祖拉的假面", "塞尔达传说：时之笛 3D", "塞尔达传说 时代之笛", "塞尔达传说 四季之笛",
        "塞尔达传说 幻影沙漏", "极限脱出 VLR", "极限脱出 零时困境",
};
#define GAME_N ((int)(sizeof(g_game_names) / sizeof(g_game_names[0])))

/* 与头文件里那个**编译期**常量互锁。两处脱节过一次（19 vs 31），
 * 而症状是"某款游戏在 UI 上不存在"—— 不报错、不崩。
 * 双向断言：任一侧改动而另一侧没跟上，都在编译期断。 */
typedef char game_n_must_equal_GKB_GAME_N[
    (GAME_N == GKB_GAME_N) ? 1 : -1];

const char *gkb_game_name(int id)
{
    if (id < 0 || id >= GAME_N) return "";
    return g_game_names[id];
}
int gkb_game_count(void) { return GAME_N; }

/* ---------------- 绑定静态数据 ---------------- */
/*
 * 知识库是 zlib 压缩的（见 tools/guia/export_kb_zlib.py）：
 *     "GKZ" | u32 raw_len | u32 comp_len | zlib 数据
 * 语料是中文攻略正文，实测压到原大小的 40%（408,004 -> 164,614 字节）。
 *
 * 为什么值得压：gkb_data[] 曾是 CIA 里最大的一块（2.5 MB C 源）。
 * 诚实说明：**这不会让 3DS 跑得更快** —— CIA 在 SD 卡上，不占运行内存。
 * 省的是安装时间和SD 占用。
 *
 * 为什么用 zlib：3ds-curl 本来就依赖 libz，已经链进二进制了，
 * 不增加任何体积。
 */
/*
 * 知识库有两种封装：
 *   "GKZ" | u32 raw_len | u32 comp_len | zlib   —— 编译期内嵌那份（压到 40%）
 *   "GKB1" | ...（裸索引）                            —— SD 卡上那份（cupd.c 装的）
 *
 * 为什么不统一成一种：内嵌那份走 zlib 是因为它当年是 2.5MB C 源文本，
 * 压完 165KB 塞进 .cia 舒服；而 SD 卡上的那份是二进制内容包，
 * 压了反而多一层 CPU 和一份体积，图什么。
 * → **两种都认，且认哪个由来源决定**，而不是让打包工具去迁就读取端。
 */
static unsigned char *gkb_unpack_src(const unsigned char *p, unsigned int src_len,
                                     unsigned int *out_len)
{
    uint32_t raw_len, comp_len;
    uLongf  dst_len;
    unsigned char *dst;

    if (!p || src_len < 20) return NULL;

    /* --- 情况 A：裸 GKB1，直接接管这块内存 --- */
    if (memcmp(p, "GKB1", 4) == 0) {
        /* 合理性上限：现在是 408 KB。给 16 MB 余量，
         * 免得数据损坏时 malloc 一个荒谬大小把堆吃光。 */
        if (src_len > (16u << 20)) return NULL;
        dst = (unsigned char *)malloc(src_len);
        if (!dst) return NULL;
        memcpy(dst, p, src_len);
        *out_len = src_len;
        return dst;
    }

    /* --- 情况 B：GKZ，解压 ---
     *
     * ⚠️⚠️ **头部是 11 字节，不是 12。**
     *
     *   "GKZ"(3) | u32 raw_len | u32 comp_len | zlib 流
     *    3  +     4     +     4      =  11
     *
     * 而 zlib 流的**头两个字节 78 da 也算在 comp_len 里面**。
     * 实测：gkb_data_len=164614、comp_len=164603、164614-11=164603 ✓。
     *
     * → 判据必须是 `comp_len <= src_len - 11`。
     *
     * ⚠️ 写成 12 会**差一个字节**，而那一个字节足以让整条路返回 NULL。
     *
     * 这个 bug 是 2026-10-04 加第二条来源时**自己引入的**：
     * 原来那版写 `12u + comp_len > gkb_data_len`，数值上等价于
     * 11+164603 > 164614 为假 → **放行**（那个 12 本来就是错值，
     * 它的宽容恰好掩盖了打包端的 11 字节事实）。
     * 我"顺手清理"成 `comp_len > src_len`（src_len 已减 12）之后，
     * 变成 164603 > 164602 为真 → **拒绝**。
     *
     * 失败的形态极其安静：两条来源都返回 NULL → gkb_bind 返 -9 →
     * 屏上只是「索引没加载成功」。
     *
     * **教训：把一处判据"写得更严格"之前，要先确认旧判据实际放行了什么。**
     * 这里旧判据的常数是错的，而它的错与打包端的错方向相反，
     * 两个错相加就一直是对的 —— 直到有人"顺手"把它改精确。
     *
     * TODO(打包端)：export_kb_zlib.py 应当明确头长 11
     *（或者把 comp_len 记为 len(comp) - 2，把 zlib 头单独算），
     * 并重新导出内容数据。之后这里的 11 才可改成 12。
     * 在那之前读端必须认 11。
     */
    if (src_len < 11 || memcmp(p, "GKZ", 3) != 0) return NULL;
    p += 3;
    memcpy(&raw_len, p, 4);   p += 4;
    memcpy(&comp_len, p, 4);  p += 4;

    if (raw_len == 0 || raw_len > (16u << 20)) return NULL;
    if (comp_len == 0 || comp_len > src_len - 11) return NULL;

    dst = (unsigned char *)malloc(raw_len);
    if (!dst) return NULL;

    dst_len = raw_len;
    /* 注意是**原始** zlib 流（不是 gzip），所以 windowBits 取负值。 */
    if (uncompress(dst, &dst_len, p, comp_len) != Z_OK ||
        dst_len != raw_len) {
        free(dst);
        return NULL;
    }
    *out_len = (unsigned int)raw_len;
    return dst;
}

/* SD 卡上的攻略库路径。**编译期常量**（设计规划 §4.8 G1：更新源不来自 AI）。 */
#define GKB_SD_PATH "sdmc:/3ds/aiconsole/guide_kb.bin"

/* 上一次 gkb_bind 实际用的是哪一份。UI 用来显示「正在用卡上那份 / 内嵌那份」。 */
static int g_gkb_from_sd;

/* pass_tlen 已分配的条数。见下面 gkb_bind_one 里的注释 —— 它必须存在，
 * 否则「指针非空就复用」会在换来源、段落数变多时堆溢出。 */
static int g_tlen_cap;

int gkb_source_is_sd(void) { return g_gkb_from_sd; }
const char *gkb_sd_path(void) { return GKB_SD_PATH; }

/*
 * 读 SD 卡上那份知识库。**读不到就返回 NULL，由调用方回退内嵌**。
 *
 * 内存：这里 malloc 的是 src_len（408KB 量级），成功之后 blob 归 gkb_bind 管，
 * 失败路径上这里必须自己释放 —— 因为 blob 一旦被 gkb_bind 接管，
 * 它只认得「自己分配的」这一种所有权。
 */
static unsigned char *gkb_load_sd(unsigned int *out_len)
{
    FILE *f;
    long   sz;
    unsigned char *buf;
    size_t got;
    unsigned char *raw;

    f = fopen(GKB_SD_PATH, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    /* 16MB 上限与 gkb_unpack_src 内部一致：宁可拒绝也不能 malloc 荒谬大小。 */
    if (sz <= 0 || sz > (16L << 20)) { fclose(f); return NULL; }
    rewind(f);

    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }

    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }

    raw = gkb_unpack_src(buf, (unsigned int)sz, out_len);
    /* buf 是压缩态的话 raw 是新分配的一块，buf 可以放；
     * 裸 GKB1 的话 gkb_unpack_src 拷了一份，buf 同样可以放。
     * 两条路都至少拷过一次，所以这里无条件 free 是对的 ——
     * 如果哪天改成「零拷贝接管 buf」，这一行必须跟着删。 */
    free(buf);
    return raw;
}

/*
 * 从一份已解开封装的映像绑定。blob 由本函数接管（成功则归 kb->blob）。
 * from_sd 只用于设置来源标记。返回段落数或负的错误码。
 *
 * 单独拆出来是因为现在有两条来源，而**解析逻辑必须只有一份** ——
 * 复制一遍的话，改了 SD 那份的判据就会漏改内嵌那份，
 * 然后表现为"更新装上了但检索结果没变"，是最难查的那一类。
 */
static int gkb_bind_one(Gkb *kb, unsigned char *blob, unsigned int raw_len,
                        int from_sd)
{
    const unsigned char *p;
    int32_t n, n_game, V, text_len;
    float avg;

    if (!blob) return -9;

    /*
     * ⚠️ 进函数先把指针字段清掉。
     *
     * gkb_bind_one 会读写 kb->blob / kb->pass_tlen，而这两个字段的
     * 初值**取决于调用方怎么声明那个 Gkb**：
     *   - 声明成 static（sc_guide.c 就是）-> 零初始化，安全；
     *   - 声明成栈上局部变量（perfprobe.c 一开始就是）-> **是垃圾值**。
     *
     * 垃圾值的后果不是"读到乱数据"这么温和：pass_tlen 是野指针，
     * `if (!kb->pass_tlen || g_tlen_cap < n)` 里 g_tlen_cap 是 0，
     * 所以会走 realloc 分支 —— 而 realloc(野指针) 是未定义行为，
     * 在 3DS 上表现为堆损坏或直接崩，且**崩溃点与真凶隔得很远**。
     *
     * 为什么不在每个调用点 memset：调用点是**别人**写的代码，
     * 下一个新增 Gkb 的模块还会再踩一次。让被调方保证自己的前置条件
     * 是唯一可靠的做法（"被调方比调用方清楚自己的字段"）。
     */
    kb->blob     = NULL;
    kb->pass_tlen = NULL;

    kb->blob = blob;
    p = kb->blob;

    if (raw_len < 20) { free(kb->blob); kb->blob = NULL; return -1; }
    if (memcmp(p, "GKB1", 4) != 0) {
        free(kb->blob); kb->blob = NULL; return -2;
    }
    p += 4;
    memcpy(&n, p, 4);      p += 4;
    memcpy(&n_game, p, 4);  p += 4;
    memcpy(&V, p, 4);       p += 4;
    memcpy(&text_len, p, 4); p += 4;
    memcpy(&avg, p, 4);     p += 4;

    if (n <= 0 || n > GKB_MAX_PASSAGES || V <= 0 || text_len <= 0) {
        free(kb->blob); kb->blob = NULL; return -3;
    }
    if (n_game != GAME_N) {
        free(kb->blob); kb->blob = NULL; return -4;   /* 别名表不同步 */
    }
    if (GAME_N > 255) {
        free(kb->blob); kb->blob = NULL; return -5;
    }

    /* 全部按小端读。3DS 是小端，ARM 上直接 memcpy 即可，
     * 不做字节序转换（写出端和读出端同为小端）。 */
    kb->pass_off = (const int32_t *)p;
    p += 4 * (n + 1);
    kb->pass_game = (const int16_t *)p;   /* i16：语料 >127 款，i8 会回绕 */
    p += n;
    kb->post_off = (const int32_t *)p;
    p += 4 * (V + 1);
    {
        int32_t n_post = kb->post_off[V];
        if (n_post < 0 || n_post > (1 << 24)) {
            free(kb->blob); kb->blob = NULL; return -6;
        }
        kb->n_post = n_post;
        kb->post = (const int16_t *)p;
        p += 2 * n_post;
        kb->post_tf = (const int8_t *)p;
        p += n_post;
    }
    kb->text= (const char *)p;
    p += text_len;
    if ((const unsigned char *)p > kb->blob + raw_len) {
        free(kb->blob); kb->blob = NULL; return -7;
    }

    kb->text_len = text_len;
    kb->n_passage = n;
    kb->n_game= n_game;
    kb->vocab    = V;
    kb->avg_len  = avg;
    /* 段落 token 长度：build_kb.bin 里没存（存的是字节偏移），
     * 而 BM25 长度归一化必须用 token 数。运行时按需算并缓存。
     *
     * ⚠️ 容量必须跟着 n 走，不能只看指针非空。
     * 原来写的是 `if (!kb->pass_tlen)` —— 那只在**首次**绑定时成立。
     * 现在有两条来源，而 SD 上那份的段落数与内嵌那份**可以不同**
     * （语料扩了但二进制没重编，就是这种局面）。
     * 于是"第二次绑定换了段落数更多的那份"时，calloc 会被跳过，
     * 紧接着的 for 循环按新的 n 写进按旧 n 分配的数组 → **堆溢出**。
     * 而堆溢出的表现是随机崩在别处，几乎不可能归因到这一行。
     * 记录已分配的条数，不够就重新分配（宁可多一次 malloc）。
     */
    if (!kb->pass_tlen || g_tlen_cap < n) {
        int16_t *nt = (int16_t *)realloc(kb->pass_tlen,
                                          (size_t)n * sizeof(int16_t));
        /* realloc 失败时 kb->pass_tlen 仍是旧的那块（realloc 不释放原块），
         * 所以必须原样留着，不能置 NULL —— 置了会漏掉旧内存。 */
        if (!nt) {
            free(kb->blob);
            kb->blob = NULL;
            return -8;
        }
        kb->pass_tlen = nt;
        g_tlen_cap = n;
    }

    {
        int i, sum = 0;
        for (i = 0; i < n; i++) {
            int a = kb->pass_off[i], z = kb->pass_off[i + 1];
            int len;
            /* 粗估 token 数：汉字 UTF-8 是 3 字节，取字节/2 足够作为
             * 长度归一化的量纲（BM25 只关心相对长度，不要求精确）。
             * 精确值需要 BPE 编码全部 836 段，启动时太慢。
             * 若要精确，见 TODO。 */
            len = (z - a) / 2;
            if (len < 1) len = 1;
            if (len > 32000) len = 32000;
            kb->pass_tlen[i] = (int16_t)len;
            sum += len;
        }
        kb->avg_tlen = (float)sum / (float)n;
        if (kb->avg_tlen < 1.0f) kb->avg_tlen = 1.0f;
    }
    /* 只在**成功**时写来源标记。失败路径上的值由 gkb_bind 决定 ——
     * 中途失败后回退到内嵌，那这句就必须能表达"最终用的是内嵌"。 */
    g_gkb_from_sd = from_sd;
    return n;
}

int gkb_bind(Gkb *kb)
{
    unsigned int raw_len = 0;
    int rc;

    /*
     * 绑定顺序：SD 卡优先，失败回退内嵌。
     *
     * ⚠️ 回退覆盖的是**所有**失败码，不只是"文件读不到"。
     * 最要紧的是 -4（游戏数与别名表不同步）：SD 上那份是外部产的，
     * 它完全可能来自一个游戏集不同的构建；而别名表 g_alias[] 是编译进
     * 本二进制的。两者不一致时**只有一条路是对的** —— 用内嵌那份，
     * 因为屏幕上的游戏列表、gkb_split_game 的拆分结果全都按编译期的
     * 别名表在算。硬用 SD 那份的话，检索会在一个和 UI 说法不一致的
     * 编号空间里打分，答出来的东西会对不上选中的游戏，而且**不报错**。
     *
     * 静默回退 vs 报错：SD 上没文件是正常状态（从没更新过的人就这样），
     * 不该报错；"卡上有但用不了"是异常，靠 gkb_source_is_sd() 返回 0
     * 配合界面上的来源显示能看出来，不必打断用户。
     */
    rc = gkb_bind_one(kb, gkb_load_sd(&raw_len), raw_len, 1);
    if (rc >= 0) return rc;

    /* SD 那条路失败了。上面那次已经把 blob 和它分配的东西都释放干净，
     * 这里可以安全地从头再来 —— 但来源标记要显式置 0，
     * 因为 gkb_bind_one 只在成功时才写它。 */
    g_gkb_from_sd = 0;
    rc = gkb_bind_one(kb, gkb_unpack_src(gkb_data, gkb_data_len, &raw_len),
                      raw_len, 0);
    return rc;
}

void gkb_free(Gkb *kb)
{
    if (kb) {
        if (kb->pass_tlen) {
            free(kb->pass_tlen);
            kb->pass_tlen = NULL;
            g_tlen_cap = 0;   /* 必须一起清：容量值描述的是刚被放掉的那块 */
        }
        /* 解压缓冲区。全部字段都指向它，所以必须最后释放，
         * 而且置NULL —— 万一调用方在 free 之后又调一次 bind，
         * 会重新走 gkb_unpack，不会撞上已释放的指针。 */
        if (kb->blob) {
            free(kb->blob);
            kb->blob = NULL;
        }
    }
}

/* ---------------- 游戏名拆分 ----------------
 * 把问句拆成 (game_id, 剩余问句)。剩余问句写进 out（需>=qlen+1 字节）。
 * 最长匹配优先：避免「宝可梦」抢走「宝可梦太阳」。
 * 返回 game_id，-1 = 没认出游戏。
 */
int gkb_split_game(const char *query, char *out, int out_sz)
{
    int qlen = (int)strlen(query);
    int best_len = 0, best_game = -1, best_pos = 0;
    int i;

    if (out_sz < qlen + 2) return -1;

    for (i = 0; i < ALIAS_N; i++) {
        const char *a = g_alias[i].alias;
        int al = (int)strlen(a);
        const char *hit = strstr(query, a);
        if (hit && al > best_len) {
            best_len= al;
            best_game = g_alias[i].game;
            best_pos  = (int)(hit - query);
        }
    }
    if (best_game < 0) {
        memcpy(out, query, qlen);
        out[qlen] = '\0';
        return -1;
    }
    /* out = query[:pos] + ' ' + query[pos+len:] */
    {
        int tail = qlen - (best_pos + best_len);
        int k = 0, j;
        for (j = 0; j < best_pos; j++) out[k++] = query[j];
        out[k++] = ' ';
        for (j = 0; j < tail; j++) out[k++] = query[best_pos + best_len + j];
        out[k] = '\0';
    }
    return best_game;
}

/* ---------------- 查询 -> token ----------------
 * 先用 nnet_text_encode() 整句BPE 编码（预切分只按空格，中文整串是一个块），
 * 再逐个 token 做停用词判定。
 * 判定必须在 token 层面：BPE 会把「在哪里」粘成一个独立 token，
 * 按空格切词看不到它，而它恰恰是噪声源
 * （实测 `在哪里` df=2 idf=5.35，压过了 df=3 的 `大师之剑` idf=5.01）。
 */
int gkb_query_tokens(const Gkb *kb, const char *query,
                     int32_t *toks, int max_tok)
{
    int32_t raw[GKB_MAX_QUERY * 2];
    int nraw, i, k, n = 0;

    if (!kb || !query || !toks || max_tok <= 0) return 0;
    nraw = nnet_text_encode(query, raw, (int)(sizeof(raw) / sizeof(raw[0])));
    if (nraw <= 0) return 0;

    for (i = 0; i < nraw && n < max_tok; i++) {
        int32_t tid = raw[i];
        const uint8_t *s = NULL;
        int plen, skip = 0;

        if (tid < 0 || tid >= kb->vocab - 3) continue;   /* 跳 BOS/EOS/SEP */
        if (kb->post_off[tid + 1] <= kb->post_off[tid]) continue;  /* 倒排空 */

        plen = nnet_vocab_piece(tid, &s);
        if (plen <= 0 || !s) continue;

        for (k = 0; k < STOPEX_N; k++) {
            if ((int)strlen(g_stopex[k]) == plen &&
                memcmp(s, g_stopex[k], (size_t)plen) == 0) { skip = 1; break; }
        }
        if (!skip) {
            for (k = 0; k < STOPSUB_N; k++) {
                if (gkb_has((const char *)s, plen, g_stopsub[k])) { skip = 1; break; }
            }
        }
        if (skip) continue;
        toks[n++] = tid;
    }
    return n;
}

/* ---------------- 检索 ---------------- */
int gkb_search(const Gkb *kb, const char *query, int game_filter,
               GkbHit *out, int max_out)
{
    int32_t toks[GKB_MAX_QUERY];
    /*
     * 这两个原本是栈上数组。GKB_MAX_PASSAGES 从 4096 抬到 16384 后
     * 就是 80KB 栈 —— 3DS 主线程栈装不下，会把报错换成更难查的栈溢出。
     * 改成本函数内的 static：本程序单线程、gkb_search 不会被重入，
     * 代价只是多占 80KB .bss（3DS 的 RAM 有 100MB 量级，可忽略）。
     * 若将来真的要多线程调用 gkb_search，必须改回栈上或按线程切分。
     */
    static float score[GKB_MAX_PASSAGES];
    static uint8_t hitc[GKB_MAX_PASSAGES];
    int ntok, i, j, nout = 0;
    float N, max_idf = 0.0f;

    if (!kb || !out || max_out <= 0) return 0;
    if (max_out > GKB_HIT) max_out = GKB_HIT;

    ntok = gkb_query_tokens(kb, query, toks, GKB_MAX_QUERY);
    if (ntok == 0) return 0;

    for (i = 0; i < kb->n_passage; i++) { score[i] = 0.0f; hitc[i] = 0; }
    N = (float)kb->n_passage;

    for (i = 0; i < ntok; i++) {
        int32_t tid = toks[i];
        int s = kb->post_off[tid], e = kb->post_off[tid + 1];
        float df = (float)(e - s);
        float idf = logf(1.0f + (N - df + 0.5f) / (df + 0.5f));
        float w = idf * idf;              /* idf^2：实体词优先 */
        if (idf > max_idf) max_idf = idf;
        for (j = s; j < e; j++) {
            int pid = kb->post[j];
            float tf, L, num, den;
            if (pid < 0 || pid >= kb->n_passage) continue;
            if (game_filter >= 0 && kb->pass_game[pid] != game_filter) continue;
            tf = (float)kb->post_tf[j];
            L  = (float)kb->pass_tlen[pid];
            num = tf * K1;
            den = tf + K1 * (1.0f - BB + BB * L / kb->avg_tlen);
            score[pid] += w * num / den;
            hitc[pid]++;
        }
    }
    (void)max_idf;

    /* 取 top-max_out：朴素插入排序，n=836 足够快 */
    {
        int32_t best_pid[GKB_HIT];
        float best_sc[GKB_HIT];
        int nb = 0;
        for (i = 0; i < kb->n_passage; i++) {
            float sc;
            if (hitc[i] == 0) continue;
            sc = score[i] * powf((float)hitc[i] / (float)ntok, COV_ALPHA);
            /* 「游戏概述」段落降权：它是每篇语料的开场白，对任何查询都像命中，
             * 但不含具体操作信息。判据是段落文本以「游戏概述」开头。 */
            if (gkb_has(kb->text + kb->pass_off[i],
                        kb->pass_off[i + 1] - kb->pass_off[i], "游戏概述"))
                sc *= OVERVIEW_F;
            if (nb < max_out) {
                int k = nb++;
                while (k > 0 && best_sc[k - 1] < sc) {
                    best_sc[k] = best_sc[k - 1]; best_pid[k] = best_pid[k - 1]; k--;
                }
                best_sc[k] = sc; best_pid[k] = i;
            } else if (sc > best_sc[max_out - 1]) {
                int k = max_out - 1;
                while (k > 0 && best_sc[k - 1] < sc) {
                    best_sc[k] = best_sc[k - 1]; best_pid[k] = best_pid[k - 1]; k--;
                }
                best_sc[k] = sc; best_pid[k] = i;
            }
        }
        for (i = 0; i < nb; i++) {
            out[i].id= best_pid[i];
            out[i].score      = best_sc[i];
            out[i].tok_off    = kb->pass_off[best_pid[i]];
            out[i].tok_len    = kb->pass_off[best_pid[i] + 1] - kb->pass_off[best_pid[i]];
            out[i].game       = kb->pass_game[best_pid[i]];
        }
        nout = nb;
    }
    return nout;
}

/* ==========================================================================
 * 输出层候选集（3DS 专属优化）
 *
 * 为什么放在检索层而不是 UI：候选集的原料是「命中段落的原文」，
 * 而原文只有 kb->text 里有（首尾相接、末尾无 NUL）。UI 拿到的是
 * GkbHit，要自己再拼一遍原文就等于把 blob 布局这层知识漏出去。
 *
 * 省多少（tools/guia/cand_probe.py 对12 条实测用例量的）
 * --------------------------------------------------------------
 *     top-1  段落的 token 数：37 / 57 / 95   （min/中位/max）
 *     top-4  段落的并集：    57 / 169 / 248
 *     输出层 524288 -> 约 20000 MAC，省 96.3%
 *     单token 总算力 1212416 -> 707712，省 41.6%
 *
 * 比设计时估的 300~500 小得多。估大了 —— 因为 BPE 编码整段之后
 * token 数并不等于字数，中文常用词被 merge 掉了。
 *
 * 顺带的收益比省算力更重要：**模型没法说出原文里没有的字**。
 * 胡编的根因是它在 4096 个 token 上自由采样，而 4096 里绝大多数
 * 与当前问题无关；把采样面收进命中原文，等于把「不编」变成
 * 结构性保证，而不是靠提示词去求。这与主项目 B/R 通道的
 * 「物理确认」闸门是同一个思路。
 * ======================================================================== */

/* 打点表：vocab 个 bit，用来去重。
 *
 * 为什么用 bit 而不是排序后二分 / 哈希表：vocab=4096 时整张表
 * 只有 512 字节，一次 memset 就干净，且插入是 O(1) 无分支预测失败。
 * 候选集规模实测中位 169，线性扫 169 个 int32 找重复也不慢，
 * 但那会让插入变成 O(n^2) 且分支不可预测 —— 这段在每 token 的
 * 热路径之外，但没必要省这点内存。 */
static uint8_t g_cand_seen[GKB_CAND_MAX * 8];

/* 段落原文的临时缓冲。语料里最长一段 633 字节，768 留够。
 * 和 sc_guide.c 的 g_passage 一样要按 tok_len 显式拷贝 ——
 * kb->text 里的段落末尾没有 NUL。 */
static char g_cand_text[GKB_CAND_TEXT];

int gkb_candidate_tokens(const Gkb *kb, const GkbHit *hits, int n_hit,
                         int32_t *out, int max_out)
{
    int n = 0, h, i;

    if (!kb || !out || max_out <= 0) return 0;
    if (n_hit <= 0 || !hits) return 0;

    /* 清打点表。GKB_CAND_MAX/8 向上取整到字节 ——
     * 4096/8=512 正好整除，但别让这个除法成为隐含前提。 */
    memset(g_cand_seen, 0, (size_t)((GKB_CAND_MAX + 7) / 8));

    for (h = 0; h < n_hit; ++h) {
        const unsigned char *src;
        int nlen, ntok;

        if (n >= max_out - 4) break;   /* 给 EOS 留位置，见下 */

        src = (const unsigned char *)kb->text + hits[h].tok_off;
        nlen = hits[h].tok_len;
        if (nlen <= 0) continue;
        if (nlen > GKB_CAND_TEXT - 1) nlen = GKB_CAND_TEXT - 1;

        /* UTF-8 不能腰斩：砍掉末尾的续接字节（10xxxxxx）。
         * 判据看**源**字节 —— 看目标缓冲的话判断结果取决于上一段
         * 讲了什么（同一个坑 sc_guide.c 的 load_passage 踩过）。 */
        while (nlen > 0 && (src[nlen] & 0xC0u) == 0x80u)
            --nlen;
        if (nlen <= 0) continue;

        memcpy(g_cand_text, src, (size_t)nlen);
        g_cand_text[nlen] = '\0';

        ntok = nnet_text_encode(g_cand_text, out + n,
                                (n < max_out) ? (max_out - n) : 0);
        if (ntok <= 0) continue;

        /* 原地去重：把 out[n..n+ntok) 里的 token 收进打点表，
         * 重复的丢掉。这里**不能**边查边写 out[n]，因为 out[n]
         * 既是读游标又是写游标 —— 先把新 token 读进临时的
         * seen 判定，再决定写不写，会踩到还没判定过的位置。 */
        {
            int w = n;
            for (i = 0; i < ntok; ++i) {
                int32_t tid = out[n + i];
                if (tid < 0 || tid >= kb->vocab) continue;  /* 越界挡掉 */
                if (g_cand_seen[tid >> 3] & (1u << (tid & 7)))
                    continue;                              /* 已收过 */
                g_cand_seen[tid >> 3] |= (uint8_t)(1u << (tid & 7));
                if (w < max_out - 4)
                    out[w++] = tid;
            }
            n = w;
        }
    }

    /* ---- EOS 必须进候选集，这是正确性命门 ----
     *
     * 输出层被限死在这 n 个 token 上，而模型是靠采到 EOS 才知道
     * 「说完了」。漏掉 EOS 的后果不是「停不下来这么小一件事」：
     * 采到 max_gen 上限才截断，屏上会多出一串无意义的 token，
     * 而这一屏的全部价值就是「原文准确」。
     *
     * 顺带把 BOS 也放进去：模型内部要能重新起头（多轮或
     * 重新生成时我们自己 push 的就是 BOS，不在候选集里的话
     * 第一步就走不下去）。
     */
    {
        int32_t bos = nnet_bos_id();
        int32_t eos = nnet_eos_id();

        if (bos >= 0 && bos < kb->vocab &&
            !(g_cand_seen[bos >> 3] & (1u << (bos & 7)))) {
            g_cand_seen[bos >> 3] |= (uint8_t)(1u << (bos & 7));
            if (n < max_out) out[n++] = bos;
        }
        if (eos >= 0 && eos < kb->vocab &&
            !(g_cand_seen[eos >> 3] & (1u << (eos & 7)))) {
            g_cand_seen[eos >> 3] |= (uint8_t)(1u << (eos & 7));
            if (n < max_out) out[n++] = eos;
        }
    }

    return n;
}
