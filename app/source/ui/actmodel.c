/*
 * actmodel.c -- model.install 动作的执行（app 层）
 *
 * 这一节原本是「内容更新」屏里的 do_one()：用户在那屏上选一条、按 A。
 * 现在同一段逻辑也由 AI 通过 [[ACT:model.install]] 触发。
 *
 * 为什么要抽出来共用，而不是在动作层再写一遍：
 * 安装是**有副作用**的流程（先备份 → 再覆盖 → 版本号最后写）。
 * 抄第二份的后果不是"多几行代码"，而是两处的备份与回滚行为将来会漂移 ——
 * 而这种漂移只在"某一份装坏了、需要回滚"的时候才显形，那时已经晚了。
 * 所以这里**照搬内容屏的调用序列**（cu_scan_local → cu_install），
 * 一个字都没改逻辑。
 *
 * ⚠️ 尚未在模拟器/真机上实测（积分约束）。cu_install 那条路本身是
 * V 趟验过的，但"经由动作触发"这条新路没有。
 */

#include "actmodel.h"

#include "actions.h"
#include "app.h"
#include "cupd.h"

#include <stdio.h>
#include <stdlib.h>   /* strtol —— idx 序号 */
#include <string.h>
#include "uistr.h"

/* kind 名 → CuKind。取值表在 actions.c（actions_model_kind），
 * 这里**按那张表**逐个试，不自己另写映射 ——
 * 两张映射表迟早有一处漏改，而漏改的表现是"手册教了 model、
 * 代码只认 model2"这种最难查的不对称。 */
static bool kind_to_cu(const char *kind, CuKind *out)
{
    static const CuKind all[] = { CU_MODEL, CU_KB, CU_HARDWARE };
    const char *n;

    for (unsigned i = 0; i < sizeof all / sizeof all[0]; ++i) {
        n = cu_kind_name(all[i]);
        if (n && !strcmp(n, kind)) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

bool model_install_action(const char *kind, const char *idx_str,
                          char *msg, size_t cap)
{
    CuManifest man;
    CuState   st;
    char      detail[192];
    CuKind    k;
    long      idx = 0;
    int       avail;

    if (!msg || cap == 0)
        return false;
    msg[0] = '\0';

    if (!kind_to_cu(kind, &k)) {
        /* 这一条本该在 actions_check_install 里就被挡住。
         * 走到这里说明两张映射表对不上 —— 那是代码漏改，
         * 不能报成"用户写错了类别"，那会把人引到错的方向去查。 */
        snprintf(msg, cap, S(S0027), kind);
        return false;
    }

    if (idx_str && *idx_str)
        idx = strtol(idx_str, NULL, 10);

    /* 与内容屏 rescan() 同一序列：先扫卡上的更新包清单。 */
    avail = cu_scan_local(&man, &st, detail, sizeof detail);
    if (avail <= 0) {
        snprintf(msg, cap, S(S0028),
                 cu_state_text(st), detail);
        return false;
    }
    if (idx < 0 || idx >= (long)man.count) {
        /* idx 越界要说清"一共几条" —— 模型知道自己传错了，
         * 但它看不到界面，报一句"越界"等于让它盲猜。 */
        snprintf(msg, cap, S(S0029), man.count, idx);
        return false;
    }

    if (cu_install(&man, (int)idx, msg, cap)) {
        app_note("actmodel: installed kind=%s idx=%ld", kind, idx);
        return true;
    }
    app_note("actmodel: install failed kind=%s idx=%ld msg=%s", kind, idx, msg);
    return false;
}
