/*
 * ota.h -- 远程升级（离线包通道 + 校验 + 安装）
 *
 * 六条护栏见 设计规划.md §4.8，这里对应到代码：
 *   G1 更新源是编译期常量 kUpdateBase[]，不进 config.json
 *   G2 manifest 里只有文件名，没有 http 地址
 *   G3 version_code 必须严格递增（ota_compare）
 *   G4 size + sha256 双重校验，不符就丢弃（ota_verify_one）
 *   G5 写盘走 .tmp + rename
 *   G6 安装永远要人按允许 —— 那是 UI 的事（sc_update.c 的下屏只有两个按钮）
 *
 * 为什么先做**离线包通道**：
 *   1. 它本身就是有用的功能：在电脑上下载好更新包丢进卡，3DS 直接装。
 *   2. 它是可验收的。模拟器里没有网络，在线检查只能验到"连不上"；
 *      而"解析 -> 比较 -> 校验 -> 安装"这条主链路，离线通道能完整跑一遍。
 *   3. 在线下载只是换一个"manifest 从哪来"，后面的逻辑完全复用。
 *
 * ★★ 2026-10-24：**在线通道的网络层换掉了**。
 *   原来走 libctru 的 httpc，而 httpc/sslc 只到 TLS 1.1 —— 阿里云 OSS 现在
 *   只收 TLS 1.2，所以"检查更新"在真机上永远连不上（这条以前被记成了
 *   "3DS 老 TLS 连不上现代站点"，其实是我们选错了客户端）。
 *   现在跟聊天/语音一样走 curl + mbedtls（httpx.c，TLS 1.2 + 内嵌 CA），
 *   而且**恢复了证书与主机名校验**——httpc 时代因为 3DS 没有根证书库，
 *   只能 SSLCOPT_DisableVerify 关掉，那才是真正的弱点。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 当前版本。G3 就是拿它跟 manifest 里的 version_code 比。
 * 改这里时记得同步 tools/make_update.py 的 --code 起点，别发出降级包。
 *
 * 0.99.6（2026-10-24）：
 *   · **每个任务一份对话历史**：切任务/建任务，上屏立刻换成那个任务的内容，
 *     切回来还是原样（原来是全局一份，换项目在屏幕上等于什么都没发生）
 *   · 任务行右端加「×」删除键（只剩一个任务时是禁用态）
 *   · 键位重排：发送挪到「键盘」格 `Y`，第 0 格 `A` 改这一行的值，
 *     `SELECT` 换短语在任何格子都能用；十字键左右切格子、摇杆滚上屏
 *   · 最下行操作指引改成轮播（一屏放不下整套键位）
 *   · 修：按住 ZL 说话时采样没被搬进来，松手永远判「太短」
 *   · 修：面板格的页脚没人画，屏上是上一帧的残留像素
 *   · 修：从键盘格发出的消息只画了占位、请求根本没出去
 *
 * 0.99.4（2026-10-21）：
 *   · **对话历史不再丢**：落盘从"打开即清空再整份重写"改成追加写；
 *     用户那句话一发出就落盘；恢复时保留最新的（原来留的是最旧的）
 *   · 模型手册增加英文版（AGENT.en.md），界面切英文时自动使用
 *   · 下屏滑轨（工作台）：键盘 / 项目 / 文件 / 浏览
 *   · 拼音输入法（自建词表 + 候选翻页）
 *   · 可变主题色（8 色，设置屏下屏可触摸）
 *   · 修 B-08/B-09/B-10/B-11 与若干 S 类缺陷
 *
 * 0.99.2（2026-10-18）：
 *   · 修「没有 asr.txt 时内置语音 Key 不生效」（asr_reset 移到 fopen 前）
 *   · 思考强度从设置屏搬到对话页（录入凭据屏仍留入口，值不锁）
 *   · 对话页加上下文用量环 + 点击弹窗
 *
 * 0.99.5（2026-10-23）—— **首个公开测试版**：
 *   · 改名 3DS AI Agent Tool；署名 acrac 设计 / DeepSeek V4.1-Flash 制作
 *   · 修：设置屏 23 行只能逐行按方向键（加 L/R 整屏翻页）
 *   · 修：主页 B 键掉进 M1 验收页（调试入口泄漏）
 *   · 修：主页 SELECT 阻塞式网络自检（搬进设置屏诊断分类）
 *   · 修：每帧写 SD 卡导致 6.8fps（帧率提升到 15.7fps @模拟器）
 *   · 修：主页/「更多」/对话屏页脚与实际行为不符，补 [ZL] 说话提示 */
#define APP_VERSION_CODE 105
#define APP_VERSION_NAME "0.99.6"

#define OTA_MAX_ARTIFACTS 4

/* 更新源。**编译期常量，故意不进 config.json**（G1）：
 * 让 SD 卡上的文件决定从哪下载，等于把更新源的管辖权交出去。 */
extern const char kUpdateBase[];

/* 更新源候选表（只读，给验收探针和诊断用）。
 * 2026-10-24 起是**一串**：自定义域名 + bucket 规范域名，
 * 前者证书名对不上时自动落到后者。见 ota.c 顶部说明。 */
int         ota_base_count(void);
const char *ota_base_url(int i);      /* i 越界返回 NULL */
const char *ota_last_base(void);      /* 上次成功的那个 */


typedef struct {
    char kind[12];      /* 3dsx / cia */
    char file[32];
    int  size;
    char sha256[65];
    char install[12];   /* replace = 直接覆盖；fbi = 只能写到卡上让用户装 */
} OtaArtifact;

typedef struct {
    int  schema;
    int  version_code;
    char version_name[24];
    char channel[12];
    int  min_supported;
    char notes[192];
    int  count;
    OtaArtifact art[OTA_MAX_ARTIFACTS];
} OtaManifest;

typedef enum {
    OTA_NONE = 0,
    OTA_NO_PACKAGE,      /* 卡上没有更新包 */
    OTA_UP_TO_DATE,      /* 包里的版本不比当前新 */
    OTA_AVAILABLE,       /* 有可用更新，还没校验产物 */
    OTA_VERIFY_OK,       /* 摘要都对得上，可以装 */
    OTA_DONE,            /* 已安装（或已放到该放的位置） */
    OTA_ERR_PARSE,       /* manifest 读不出来 */
    OTA_ERR_TOO_OLD,     /* 降级包 —— 拒绝，这是安全信号不是报错 */
    OTA_ERR_MISMATCH,    /* 大小或摘要不符 */
    OTA_ERR_IO,          /* 读写失败 */
    OTA_ERR_NET,         /* 连不上更新源（网络/TLS/DNS/非 200）—— 与"包坏了"完全两件事 */
} OtaState;

const char *ota_state_text(OtaState s);

/* ---- 在线检查与下载（L2）----
 *
 * ★ 与离线通道的**分工**，别混：
 *   ota_scan_local  管"卡上已经有的包"（读 manifest + 校验产物）
 *   ota_fetch_remote 只读**几百字节的 manifest**，拿到"线上是什么版本"
 *   ota_download    把某个产物拉到 update/ 下的 .partial，校验后 rename
 *
 * 为什么检查与下载分成两个函数：检查便宜（几百字节）、下载贵（几 MB），
 * 而且两者的失败原因完全不同（连不上 vs 摘要不符）。合成一个的话
 * 失败提示只能写"更新失败"，等于没提示。
 *
 * ⚠️ 在线检查**只能到 OTA_AVAILABLE**，永远到不了 OTA_VERIFY_OK ——
 *    因为此时包里根本没有产物可校验。必须先 ota_download，
 *    再 ota_scan_local 把状态推到 VERIFY_OK，才允许安装。
 */
bool ota_fetch_remote(const char *channel, OtaManifest *m, OtaState *state,
                      char *detail, size_t cap);

/* 上一次 ota_fetch_remote 读到的**原始 manifest 文本**（不下发所有权，别 free）。
 *
 * 为什么需要它：下载完必须把这份文本也落到 update/manifest.json。
 * 不落的话 ota_scan_local 会拿**卡上旧的那份**去核对新下载的产物 ——
 * 大小/摘要必然对不上，用户看到的是"包坏了"，其实是我们没同步 manifest。
 * 2026-10-17 实测就是这么红的。 */
const char *ota_remote_raw(size_t *n);

/* 下载进度回调（可选）。
 *
 * pct  = 0..100（按 manifest 声明的 size 算，不是按已收字节和总数猜）
 * file = 正在下的产物名
 *
 * 为什么要有它：下载是一个**阻塞调用**，中间不会回到主循环 ——
 * 上层轮询不到进度，只有真正在收数据的那一层（file_sink）知道走到哪了。
 * 回调里可以重绘界面（同一线程，没有并发问题）。 */
typedef void (*OtaProgressCb)(int pct, const char *file);
void ota_set_progress_cb(OtaProgressCb cb);

/* 下载第 idx 个产物到 OTA_DIR。边下边算 sha256，只有校验通过才 rename 成
 * 正式文件名 —— 中途断网/断电只会留下一个 .partial，绝不会留半个"正式包"
 * 让下一次检查把它当成可安装的东西。 */
bool ota_download(const char *channel, const OtaManifest *m, int idx,
                  char *err, size_t errsz);


/* 更新包放在哪 */
const char *ota_dir(void);

/* 解析 manifest 文本。结构不对就返回 false。 */
bool ota_parse(const char *json, size_t n, OtaManifest *m);

/* 与当前版本比较：1 = 有更新，0 = 一样，-1 = 更旧（必须拒绝） */
int ota_compare(const OtaManifest *m, int cur_code);

/* 扫卡上的更新包：读 manifest -> 比版本 -> 逐个验 size 和 sha256。
 * detail 会填一句人话（哪一步、哪个文件出问题），直接给 UI 显示。 */
bool ota_scan_local(OtaManifest *m, OtaState *state, char *detail, size_t cap);

/* ★ 轻量版：同上，但**跳过 sha256**（只验存在性 + size）。
 *
 * 为什么要有它（2026-10-18「选更新卡顿」优化）：
 *   ota_scan_local 对每个产物跑 sha256_file() 全文件读一遍 ——
 *   3dsx ~5MB + cia ~4MB 就是 9MB 同步读 + 哈希，ARM11 上好几秒，
 *   而 update_enter() 是在**进屏时同步调用**的，期间主循环完全冻结。
 *   轻量版只做 O(1) 的元数据操作（fopen/ftell + manifest 解析），
 *   进屏能立刻出画面；真正的 sha256 闸门留到 do_install() 之前，
 *   用户点了「允许」才付那份代价。
 *
 * 【安全语义不变】返回 OTA_VERIFY_OK 的仍然是 ota_scan_local（完整校验）。
 *   轻量版最多推到 OTA_AVAILABLE —— 它就代表"看着像有更新，还没校验"，
 *   这是枚举里本来就有的状态，不是新造的宽松态。 */
bool ota_scan_local_fast(OtaManifest *m, OtaState *state,
                         char *detail, size_t cap);

/* 装第 idx 个产物。3dsx 直接覆盖；cia 只复制到 cias/ 并如实告诉用户
 * 「下一步得你自己去 FBI 里点一下」—— 装 CIA 走 AM 服务，自制程序没那权限。 */
bool ota_install(const OtaManifest *m, int idx, char *err, size_t errsz);
