/*
 * amprobe.c -- CIA 安装能力探针
 *
 * ── 这个文件为什么存在 ──
 * 关于"3DS 上的自制程序能不能装 CIA"，网上有两种说法，而且是矛盾的：
 *   · 一种说"不行，要 am:u 权限，自制程序拿不到"；
 *   · 一种说"FBI 不也是自制程序，它就能装"。
 *
 * 两种说法都对，但都没说到点上。真正决定的是**exheader 里声明的
 * ServiceAccessControl + 服务端的签名级别检查**这两道关。
 * 而这是个**可以实测**的问题 —— 与其继续读文档猜，不如直接把
 * am:u 的真实返回码打出来。
 *
 * 本探针**不安装任何东西**。它只做三件事：
 *   1. amInit() 能不能成功（拿不拿得到 am 服务句柄）
 *   2. AM_GetTitleCount 这类**只读**接口通不通
 *   3. 真的去调 AM_StartCiaInstall，看它返回什么错误码
 *      一旦真的返回成功，立刻 AM_CancelCIAInstall 撤掉 ——
 *      **绝不留下半个装到一半的 title**。
 *
 * 结论写进 status.txt（app_note），宿主脚本据此断言。
 * 这比任何文档都可靠，因为它是这台机器、这个 exheader 的实测结果。
 *
 * ⚠️ amprobe 的返回码只反映**当前这个形态**的权限。同一个二进制装成
 * .cia 再跑，第 3 步的结果可能完全不同（因为权限来源不同，见上）。
 *
 * ── 错误码对照 ──
 * 下面这行是**实测**得来的（2026-10-03，Azahar）：
 *   0                  → Azahar 里给的就是 0。但它**不算数**，见下。
 *
 * ── ⚠️ 这个探针在 Azahar 上的结论不可信 ──
 * Azahar 的 am 服务（core/hle/service/am/am.cpp）**不检查调用者的
 * exheader / ServiceAccessControl**，所以 AM_StartCiaInstall 对谁都返回 0。
 * 这跟 httpc 那条"模拟器里 HTTPS 必然成功"是同一类假阳性。
 *
 * 真机上唯一有效的判据是 Luma3DS 的行为，而它取决于**形态**：
 *   · .3dsx ：Luma 走 hbldr.c 的 hbldrPatchExHeaderInfo()，**先 memset
 *             清零再 memcpy 一张写死的 34 项服务表**（含 am:net，不含 am:u）。
 *             本节所在程序以 3dsx 运行时，无论 RSF 写什么都拿不到 am:u。
 *   · .cia  ：判据是 loader.c 的 hbldrIs3dsxTitle(title_id)，它比的是
 *             可配置的 hbldr_tid（默认 hblauncher）。我们的 UniqueId 0xA1C01
 *             不等于它 → 走普通 title 分支 → **用自己 exheader 里的声明**。
 *             这就是 FBI 能装 CIA 的原因：不是它有魔法，是它走这条路。
 *             RSF 在 app/AIConsole_cia.rsf，里面有 am:u。
 * 依据：LumaTeam/Luma3DS master 的 hbldr.c / hbldr.h / loader.c。
 * ⚠️ 以上仍是**源码阅读**结论，真机未实测。
 */

#include "amprobe.h"

#include "app.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* libctru 的结果码是个 32 位数，拆成模块/描述两段更好读。
 * 3DS 的 Result 布局：level(1) summary(1) module(8) description(9)，
 * 我们只关心低 22 位里的 module 和 description。 */
static void fmt_result(char *out, size_t cap, Result r)
{
    if (R_SUCCEEDED(r)) {
        snprintf(out, cap, "0（成功）");
        return;
    }
    /* 高位是 level/summary，实际诊断看低 22 位 */
    unsigned raw = (unsigned)r;
    snprintf(out, cap, "0x%08X（module=%u desc=%u）",
             raw,
             (raw >> 10) & 0xFF,
             raw & 0x3FF);
}

void amprobe_run(void)
{
    char     buf[128];
    Result   r;
    u32      count = 0;
    Handle   ciaHandle = 0;
    bool     am_ok;

    app_note("amprobe: 开始（不安装任何东西，只看返回码）");

    /* ---- 1. 拿 am 服务 ---- */
    r = amInit();
    am_ok = R_SUCCEEDED(r);
    fmt_result(buf, sizeof buf, r);
    app_note("amprobe: amInit rc=%s", buf);

    if (!am_ok) {
        /* 拿不到 am 服务句柄。注意**别把原因写成"exheader 没声明 am:u"** ——
         * 那只在 .cia 形态下成立。以 3dsx 运行时，原因几乎一定是 Luma 把
         * 服务表覆盖成了它那张写死的 34 项表（见文件头）。两种原因对应
         * 完全不同的修法，写错会让人去改一个根本没用的 RSF。 */
        app_note("amprobe: 结论 = 拿不到 am 服务。"
                 "若本程序是 3dsx 形态，这是 Luma 覆盖服务表所致，"
                 "改 RSF 无效；要 am:u 必须用 CIA 形态。");
        return;
    }

    /* ---- 2. 只读接口（验证服务确实在工作，而不是拿到个空壳）---- */
    r = AM_GetTitleCount(MEDIATYPE_SD, &count);
    fmt_result(buf, sizeof buf, r);
    app_note("amprobe: AM_GetTitleCount(SD) rc=%s count=%lu", buf,
             (unsigned long)count);

    r = AM_GetTitleCount(MEDIATYPE_NAND, &count);
    fmt_result(buf, sizeof buf, r);
    app_note("amprobe: AM_GetTitleCount(NAND) rc=%s count=%lu", buf,
             (unsigned long)count);

    /* ---- 3. 关键一试：真的调 StartCiaInstall ----
     * 注意：这里**没有给它任何 CIA 数据**。我们只要看它返回什么：
     *   · 返回成功 → 说明权限是有的（那这个 handle 必须立刻撤掉）
     *   · 返回权限错 → 权限关过不去，CIA 路线到此为止
     * 所以无论哪种结果，都不会真的装进任何东西。 */
    r = AM_StartCiaInstall(MEDIATYPE_SD, &ciaHandle);
    fmt_result(buf, sizeof buf, r);

    if (R_SUCCEEDED(r) && ciaHandle) {
        /* 分两条 note 打，避免 snprintf 在 192 字节里塞不下被截断
         * （编译器会因此报 -Wformat-truncation，而那个告警本身没错）。 */
        app_note("amprobe: AM_StartCiaInstall rc=%s handle=%lu",
                 buf, (unsigned long)ciaHandle);
        /* ⚠️ 这里**不能**写"权限是有的"。
         * 同一个 rc=0 在三种环境下含义完全不同：
         *   · Azahar        → 不查调用者 exheader，对谁都返 0（假阳性）；
         *   · 3dsx 真机     → 服务表被 Luma 覆盖，压根拿不到 am:u；
         *   · .cia 真机     → 这才是"真有权限"。
         * 早先这里写的是"权限是有的"，而且和下面的结论行**自相矛盾** ——
         * 那两句一起看，人只会记住更乐观的那句。 */
        app_note("amprobe: （这个 0 只说明调用没被拒，"
                 "不等于有权限 —— 见下面结论）");

        /* 立刻撤回 —— 我们没写任何数据，但那个 install context
         * 已经在 am 里登记了。不撤掉的话，它会留在 import.db 里，
         * 变成一个"装到一半的 title"。这在一个探针里是绝对不能留的。 */
        r = AM_CancelCIAInstall(ciaHandle);
        fmt_result(buf, sizeof buf, r);
        app_note("amprobe: AM_CancelCIAInstall rc=%s（已撤回，没留东西）", buf);
        /* 结论必须带上"这个码在什么环境下才算数"。
         * Azahar 的 am 服务不查调用者 exheader，它返 0 只说明模拟器不设防；
         * 3dsx 形态的真机则会被 Luma 覆盖服务表（拿不到 am:u）；
         * 只有 .cia 形态的真机，这个 0 才真的意味着"有权限"。
         * 一句"可行"会把三种情况混成一种 —— 那是这份探针最不该犯的错。 */
        app_note("amprobe: 结论 = StartCiaInstall 返回成功。"
                 "但**这个码本身不构成权限证据**：模拟器不设防，"
                 "3dsx 真机无此权限。要下结论必须用 CIA 形态在真机上跑。");
    } else {
        app_note("amprobe: AM_StartCiaInstall rc=%s", buf);
        app_note("amprobe: 结论 = CIA 安装被服务端拒绝（权限关过不去）");
    }

    /* ---- 4. 我们到底是什么身份、什么形态 ----
     * 这一段是给"返回码能不能信"用的：
     *   · 3dsx 形态 → am 的返回码可能被 Luma 覆盖服务表影响，
     *                 而且 Azahar 上必然假阳性；
     *   · cia  形态 → 走的是自己 exheader 里的声明，这个码才有意义。
     * 判据用 svcGetProcessInfo 读到的 program id（= title id）。
     *
     * ⚠️ 这里踩过一个坑，别再犯：Azahar 上 svcGetProcessInfo(0x10000)
     *    **返回成功但并不写入数据**，于是未初始化的栈值被当成 title id
     *    打印出来（实测打出了 575C69796475475C，那是 ASCII 的 "\\Gudy\\W"，
     *    一段残留的路径字符串）。一个看起来煞有介事的十六进制数，
     *    比一句"读不到"危险得多 —— 人会拿它当依据。
     *    所以现在**先判这个值像不像 title id**，不像就明说读不到。 */
    {
        u32  pid = 0;
        /* svcGetProcessInfo 的原型是 s64*（libctru 把 64 位整数统一
         * 用 signed 表示），这里用 u64 会触发 -Wpointer-sign。
         * 读出来立刻转成 u64 做位运算，符号不影响结果。 */
        s64  tid_raw = 0;
        u64  tid;

        svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        app_note("amprobe: 我们的进程 pid=0x%08lX", (unsigned long)pid);

        /* 探测前先清零，这样"没被写入"和"写入了 0"能区分开 ——
         * 不清零的话两种情况下读到的都是上一次的栈残留。 */
        tid_raw = 0;
        r = svcGetProcessInfo(&tid_raw, CUR_PROCESS_HANDLE, 0x10000);
        tid = (u64)tid_raw;

        /* 3DS 的 title id 高 32 位必然落在 00040000 ~ 0004013F 之间
         * （自制 title 是 0004000F 段，系统应用 00040030，系统模块 00040130）。
         * 落在区间外 = 读到的不是 title id，多半是未初始化的残留。
         * 这是个**形状检查**，不是精确校验 —— 它要挡的就是"垃圾值被当成
         * 权威数据打印出来"这一件事。 */
        {
            u32 hi = (u32)(tid >> 32);

            if (R_FAILED(r) || hi < 0x00040000u || hi > 0x0004013Fu) {
                fmt_result(buf, sizeof buf, r);
                app_note("amprobe: title_id 读不到或不可信（rc=%s，"
                         "读到 %08lX）—— 说明本环境的这一项无效，"
                         "**不要**拿它推权限。真机上才作数。",
                         buf, (unsigned long)hi);
            } else {
                app_note("amprobe: title_id=%016llX", (unsigned long long)tid);

                if ((hi & 0x0004FFFFu) == 0x0004000Fu)
                    app_note("amprobe: 形态=自制 title（很可能是 CIA 形态，"
                             "这个码可用于判断权限）");
                else
                    app_note("amprobe: 形态=非自制 title（%08lX）—— "
                             "很可能是被 Luma 接管成 3dsx，"
                             "这个码不能用来推权限", (unsigned long)hi);
            }
        }
    }

    amExit();
    app_note("amprobe: 结束");
}

/* --------------------------------------------------------------------------
 * 查已安装的游戏
 *
 * 这是"装游戏"这件事的**另一半**：装完得能看见。
 * 而且它对 .cia 路线同样重要 —— 如果 AM_GetTitleList 能读，
 * 至少说明 am 服务是通的（哪怕装不了）。
 * ----------------------------------------------------------------------- */
void amprobe_list_titles(void)
{
    u64   ids[64];
    u32   total = 0, got = 0;
    Result r;
    char  buf[128];

    r = amInit();
    if (R_FAILED(r)) {
        fmt_result(buf, sizeof buf, r);
        app_note("titles: amInit 失败 rc=%s", buf);
        return;
    }

    r = AM_GetTitleCount(MEDIATYPE_SD, &total);
    if (R_FAILED(r)) {
        fmt_result(buf, sizeof buf, r);
        app_note("titles: 读 SD 上的 title 数失败 rc=%s", buf);
        amExit();
        return;
    }

    app_note("titles: SD 卡上装了 %lu 个 title", (unsigned long)total);

    if (total > 0) {
        u32 want = total > 64 ? 64 : total;
        r = AM_GetTitleList(&got, MEDIATYPE_SD, want, ids);
        if (R_SUCCEEDED(r)) {
            for (u32 i = 0; i < got && i < 8; ++i)
                app_note("titles: [%lu] %016llX", (unsigned long)i,
                         (unsigned long long)ids[i]);
            if (got > 8)
                app_note("titles: …还有 %lu 个", (unsigned long)(got - 8));
        } else {
            fmt_result(buf, sizeof buf, r);
            app_note("titles: AM_GetTitleList 失败 rc=%s", buf);
        }
    }

    amExit();
}
