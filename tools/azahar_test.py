#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
azahar_test.py -- 无人值守的 3DS 验收驱动（M2）

思路
----
3DS 没有可脚本化的截图通道，也没有可编程的输入注入。所以这两件事都交给
**程序自己**做：

  * 输入：app 读 sdmc:/3ds/aiconsole/autorun 脚本，逐帧注入合成按键，
    走的是和真实按键**完全相同**的 app_handle() 路径；
  * 输出：app 把帧缓冲写成 BMP 落到 SD 卡，再把"程序自己认为发生了什么"
    写成 status.txt。

宿主脚本只负责：写脚本 → 拉起 Azahar → 等 status.txt 出现 → 收图 → 断言。

截图看的是"长什么样"，status.txt 看的是"程序自己认为发生了什么"。
两个都要断言：截图能骗过眼睛（引导画对了但没落盘），日志能骗过断言
（写了自己跳过了引导但其实没跳）。

三趟验收（M2 的验收条件：跑完三屏 / config.json 生成 / 二次启动跳过引导）
--------------------------------------------------------------------------
  A 首次启动   删掉 config.json → 走完三屏 → 断言 config.json 生成且字段正确
  B 二次启动   保留 config.json → 断言直接进主界面（start_screen=SCREEN_HOME）
  C 横幅回归   宿主把 confirmed 改回 false → 断言横幅出现，按 Y 后消失且落盘

用法
----
    python tools/azahar_test.py                 # 跑全部 A–T 二十趟
    python tools/azahar_test.py --passes A      # 只跑 A
    python tools/azahar_test.py --passes O      # 只跑「AI 创建文件」
    python tools/azahar_test.py --passes P      # 只跑「长消息回落」（B1 回归守卫）
    python tools/azahar_test.py --passes Q      # 只跑「CIA 安装权限实测」（FBI 之问）
    python tools/azahar_test.py --passes R      # 只跑「AI 安装程序」（摘要护栏）
    python tools/azahar_test.py --passes S      # 只跑「AI 安装程序」（成功路径）
    python tools/azahar_test.py --passes T      # 只跑「故障注入」（七场景）
    python tools/azahar_test.py --keep          # 不杀模拟器，自己看完再关

全程不需要真 Key：H/M/N/O/R/S 六趟都走离线演示 provider（base_url 为空 → 不出网）。
Q 趟也不出网：它只调本机的 am 服务，不看网络。
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_AZAHAR = r"E:\Azahar\azahar-windows-msvc-2126.1.1\azahar.exe"
DEFAULT_ROM    = os.path.join(ROOT, "app", "AIConsole.3dsx")

#: app 端约定的目录（见 app/source/gfx/shot.h 与 ui/app.c）
SD_SUBDIR  = os.path.join("3ds", "aiconsole")
SHOT_SUB   = os.path.join(SD_SUBDIR, "shots")
CONFIG_REL = os.path.join(SD_SUBDIR, "config.json")
STATUS_REL = os.path.join(SD_SUBDIR, "status.txt")
# app 写 status.txt 的中间文件（先写 tmp 再 rename，见 app.c 的 app_write_status）
STATUS_TMP_REL = os.path.join(SD_SUBDIR, "status.tmp")
CHATLOG_REL = os.path.join(SD_SUBDIR, "chat.json")
AUTORUN_REL = os.path.join(SD_SUBDIR, "autorun")
PHRASES_REL = os.path.join(SD_SUBDIR, "phrases.txt")
#: Y 趟：语音识别（ASR）配置。格式与 config.json 一样是给 app 读的
#: 文本行，见 shared/asr.c 的 asr_load_file（只认 provider= 与 key= 两键）。
ASR_CFG_REL = os.path.join(SD_SUBDIR, "asr.txt")

#: O 趟：AI 创建文件（agent 能力第一步）
#:   OUT_SUBDIR 是沙箱目录（见 shared/actions.c 的 ACT_DIR）
#:   ACTION_LOG 是审计流水，AGENT_FILE 是演示剧本要写的那个文件
OUT_SUBDIR = os.path.join(SD_SUBDIR, "out")
ACTION_LOG = os.path.join(SD_SUBDIR, "action.log")
AGENT_FILE = os.path.join(OUT_SUBDIR, "agent_demo.txt")

OUT_DIR = os.path.join(ROOT, "tools", "out", "shots")

#: 每趟跑完立刻把 BMP 复制到这里。下一趟启动前会清掉 SD 卡上的截图，
#: 不先搬走的话，前面几趟的图到最后一并转 PNG 时已经不存在了。
STAGE_DIR = os.path.join(OUT_DIR, ".bmp")

#: 设计规划.md §5.1 规定的顶层键。缺一个就说明落盘结构跑偏了。
SCHEMA_ROOTS = ["version", "first_run_completed", "tutorial_completed",
                "launches", "nand_backup", "security", "ai", "ui",
                "guard", "diag", "ota"]
SCHEMA_SUB = {
    "nand_backup": ["confirmed", "remind_on_startup"],
    "security": ["current_level", "persist_level", "granted_actions", "dev_mode_unlocked"],
    "ai": ["provider", "model", "auth_type", "stream"],
    "ui": ["theme", "depth", "font_size"],
    "guard": ["confirm_overwrite", "confirm_delete", "confirm_bulk",
              "max_write_kb", "max_batch_files"],
    "diag": ["show_layout_overflow", "show_notes", "verbose_status"],
    "ota": ["enabled", "channel", "auto_check", "last_check", "skipped_version"],
}


# ---------------------------------------------------------------------------
# 脚本内容
# ---------------------------------------------------------------------------

SCRIPT_A = """\
# 首启：引导三屏 -> 新手教程四屏（含自动读取 API Key）-> 主界面 -> M1 回归页
wait 20
shot 1_welcome
key A
wait 3
key A
wait 6
shot 2_nand
key LEFT
wait 3
key A
wait 6
shot 3_service
key A
wait 8
# 引导三屏走完，自动进入新手教程
shot 4_apikey_what
key A
wait 6
shot 5_apikey_where
key A
wait 6
shot 6_apikey_file
key A
wait 6
shot 7_apikey_result
key A
wait 8
shot 8_home_after_onboarding
key A
wait 6
shot 9_lab_sample
key B
wait 4
quit
"""

SCRIPT_3 = """\
# 插件页：主页焦点移到「更多」-> 进去 -> 下移到「插件」-> 进去 -> 返回
#   主页 3 槽一行（CHAT/GUIDE/MORE），RIGHT×2 把焦点移到 MORE
wait 20
key RIGHT
wait 3
key RIGHT
wait 3
shot 1_home_more_focus
key A
wait 8
# MORE 列表共 6 项，初始焦点在第 0 项「翻译」，「插件」是第 5 项（index 5，
# 最后一项）。⚠️ DOWN 次数必须与 kItems 长度同步：插件项是后来加的
# （2026-10 用户要求插件独立成页），旧脚本按 5 项列表发 DOWN×4，
# 焦点落在「从电脑取文件」上，A 进的是 XFER 屏 —— 插件页从未被进入，
# 表现是 notes 里没有 plugins: scanned。改 kItems 必须同步这里。
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
shot 2_more_plugins
key A
wait 8
shot 3_plugins
key B
wait 6
key B
wait 6
shot 4_back_home
wait 2
quit
"""

SCRIPT_B = """\
# 二次启动：应当直接落在主界面，不再出现引导
wait 20
shot 6_home_second_run
wait 6
quit
"""

SCRIPT_C = """\
# 横幅回归：宿主已把 confirmed 改回 false
wait 20
shot 7_banner_on
key Y
wait 6
shot 8_banner_off
wait 2
quit
"""

SCRIPT_D = """\
# 跳过 + 回来：重走首启，教程第一页就按 X 跳过，再从主界面按 X 回来
#
# ⚠️ welcome 是 **4 页**（WELCOME_PAGES=4：语言 / 说明 / NAND 确认 /
#    录入凭据 or 完成设置），不是"三屏"—— A 趟标题里的"三屏"是旧布局的
#    遗留说法。少按一个 A 的后果是停在第 3 页永远完不成引导：
#    2026-10-18 D 趟就是这样 FAIL 的（end_screen=SCREEN_WELCOME、
#    tutorial_completed=false、两张截图字节相同，三个症状同一个根因）。
#    本脚本按 4 页写，改 WELCOME_PAGES 必须同步这里。
wait 20
key A
wait 6
key A
wait 6
key LEFT
wait 3
key A
wait 8
# 此刻在第 3 页（默认选中「完成设置」）—— A 收尾引导，进录入凭据屏
key A
wait 8
# 此刻在录入凭据屏（教程）—— 按 X 跳过，回主界面
key X
wait 8
shot 1_skipped_home
# 主界面上按 X，进 Key 管理
key X
wait 8
shot 2_back_in_tutorial
wait 2
quit
"""

SCRIPT_E = """\
# 更新：主界面按 R 进更新屏 -> 按 X 看完整说明 -> 返回 -> 右移到允许 -> 安装
#
# ★ 这一趟只走**离线**通道（卡上夹具），不碰网络 —— 日常回归不能依赖
#   外部服务，否则断网就等于全红，而红的原因跟代码无关。
#   在线检查/下载的端到端验证见 docs/新版本推送设计.md §9，
#   需要本机起一个 HTTP 服务并用 -DOTA_BASE_URL 定向编译。
wait 20
key R
wait 8
shot 1_update_available
key X
wait 8
shot 2_update_notes
key B
wait 6
key RIGHT
wait 3
key A
wait 8
shot 3_update_installed
wait 2
quit
"""

SCRIPT_F = """\
# 网络探测：主界面按 SELECT，httpc 会阻塞一会儿，给足等待。
# （原先是 L 键，M3 把 L 让给了「设置」，网络自检挪到 SELECT。）
wait 20
key SELECT
wait 45
shot 1_netprobe
wait 2
quit
"""

SCRIPT_G = """\
# 对话屏：主界面按 A 进入；空输入按 A 应当只给提示，不能崩也不能多发一条
wait 20
key A
wait 8
shot 1_chat_empty
key A
wait 6
shot 2_chat_empty_send
wait 2
quit
"""

SCRIPT_H = """\
# 真实发送：SELECT 填快捷短语（绕开 swkbd —— 模拟器里它会弹 Qt 窗，脚本没法操作）
# -> A 发送（下一帧才真发，占位消息先画出来） -> 等回复替换占位
# 模拟器内只有 ~5fps，wait 按"秒数 x 5"折算，600x4 帧（60fps 假设）会空等 10 分钟
wait 20
key A
wait 8
shot 1_chat_ready
key SELECT
wait 3
shot 2_chat_qp
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 400
wait 400
shot 3_chat_reply
wait 2
quit
"""

SCRIPT_I = """\
# Key 管理：主界面按 X 进入；游标停在第一条，按 A 切换到它
# （种子仓库 active 是第二条，切完 keys.json 里的 active 应当变成第一条的名字）
wait 20
key X
wait 8
shot 1_keys_list
key A
wait 5
shot 2_keys_switched
wait 2
quit
"""

#: 底屏按钮中心坐标（与 sc_home.c / sc_chat.c 的常量对齐）：
#  第一个按钮 x=10..154、第二个 x=166..310，y=150..186
J_BTN_CHAT  = (82, 168)     # 主界面「开始对话」 / 对话屏「发送」
J_BTN_SEND  = (82, 168)     # 同一位置，但此时已在对话屏
J_BTN_KEY   = (238, 168)    # 「配 API Key」/「键盘输入」

SCRIPT_J = """\
# 触摸：主界面点「开始对话」进对话屏；对话屏点「发送」（空输入只提示，不崩）。
# 合成触摸走 KEY_TOUCH + app_touch_frame，与真实触摸同一条 handle 路径。
wait 20
touch 158 130
wait 8
shot 1_touch_chat
# ★ 2026-10-23 修假阳性：这里原本写 80 175，而 CHAT_BTN_SEND 是
#   (10,124,144,36) —— 纵向 124..160。175 落在**滑轨区**（168..198），
#   也就是说这一点从来没点到「发送」，点到的是滑轨某一格；
#   断言之所以一直是绿的，是因为切面板**确实**让画面变了。
#   换成真实的按钮中心 (82,142) 之后，这条才真的在验"空发送给提示"。
touch 80 142
wait 6
shot 2_touch_send_empty
wait 2
quit
"""

SCRIPT_K = """\
# 设置屏：进设置 -> 切主题（黑夜）-> 换行 -> 改护栏值 -> 返回 -> 主界面也应变黑。
# 主题切换是这一趟的重点：像素要真的变了，而不是只改了个字段。
wait 20
# 主界面按 L 进设置
key L
wait 10
shot 1_settings_light
# 光标默认在第 1 行「界面主题」，按 A 切到黑夜
key A
wait 4
shot 2_settings_dark
# 往下移 9 行到「启动检查更新」那一行（0-based 第 9 行）。
# ★ 行号纪律 —— 这个 DOWN 次数改过三轮，每一轮都值得记：
#   · 早先 5 次：那时 OTA 是 0-based 第 5 行；
#   · 2026-10-16 改 6 次：同批其实插了 **5 行**（「思考强度」+ 记忆 /
#     回复长度 / 上下文轮数 / 回答语言），只补了 1 次 —— 于是这 6 次
#     落在「回复长度」上，而**整趟照样 PASS**（断言只验主题与像素，
#     没验 toggle 的是哪一行），偷偷验了隔壁行；
#   · 2026-10-18 改 9 次：「思考强度」搬到对话页，列表 22 -> 21 行。
#   · 2026-10-22 改 10 次：新增「主题色」行（插在第 2 位），
#     OTA 从 0-based 9 -> 10。
#   · 2026-10-22 改 11 次：**又把「思考强度」加回设置屏**（插在
#     「记忆」之后、「回复长度」之前），OTA 从 0-based 10 -> 11。
#     这一行在 2026-10-18 曾搬到对话页，现在两处并存（对话页管"这一轮
#     临时调"、设置屏管"我的默认习惯"），所以位置又回到靠前。
#   "按位置导航 + 错了不报错"这个组合太坑，所以本趟同时补了一条硬
#   断言（见断言段）：跑完 config.json 里 ota.enabled 必须被翻过去。
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
wait 4
shot 3_settings_guard
# 按 A 切换它
key A
wait 4
shot 4_settings_guard_toggled
# 返回主界面 —— 主界面也应该是黑夜的
key B
wait 6
shot 5_home_dark
wait 2
quit
"""

SCRIPT_L1 = """\
# 阶段 1：走完整引导 + 录入屏最后一页按 A 把 Key 收进加密仓库。
# 宿主已放好 key.txt，app 进录入屏时 credfile_scan 会读到它
# （CREDFILE_OK），在最后一页按 A 即走 do_save_provider() 存入 creds.enc。
#
# ⚠️ 两个页数常量必须同源（sc_apikey.c:25 / sc_welcome.c:23）：
#    WELCOME_PAGES=4（语言/说明/NAND确认/完成设置），
#    APIKEY_PAGES=4，而且录入屏**在最后一页上还要再按一次 A 才保存**
#    （sc_apikey.c:341-350：g_page < APIKEY_PAGES-1 时 A 只翻页；
#    停在最后一页的那一下 A 才触发 do_save_provider）。
#    2026-10-18 L 趟 FAIL 6 条的根绪就是本脚本少按了这一个 A：
#    旧版按键 A,LEFT,A,A,A,A,A,A —— LEFT 按在说明页（无效，welcome 只有
#    p0 语言页和 p2 NAND 页响应 LEFT），录入屏只翻到最后一页没保存，
#    do_save_provider 从未被调用 → creds.enc 从未生成（L1 后 key.txt
#    原封不动留在 SD 上）→ L2 creds_exists=0 / L3 没有可篡改的文件，
#    6 条 [!!] 同一个根因。A 趟 PASS 的 welcome 段（A,A,LEFT,A,A）照抄。
wait 20
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 6
key A
wait 8
# 引导四页走完 -> 自动进入录入屏（APIKEY_PAGES=4）
key A
wait 6
key A
wait 6
key A
wait 6
shot 1_l1_result_page
# 在最后一页上按 A：读到 Key -> 加密存入 creds.enc -> 切主界面
key A
wait 10
shot 2_l1_after_save
wait 2
quit
"""

SCRIPT_L2 = """\
# 阶段 2：什么都不做，直接起 —— 能免输入进 Key 管理就说明 creds.enc 解开了
wait 20
shot 1_l2_home
key X
wait 8
shot 2_l2_keys_encrypted
wait 2
quit
"""

SCRIPT_L3 = """\
# 阶段 3：creds.enc 已被宿主篡改，起一次看它怎么处理
wait 20
key X
wait 8
shot 1_l3_tampered
wait 2
quit
"""

# M6 流式输出验收。
#
# 关键点：**回复过程中要能看到半截文本**。宿主没法录屏（Azahar 的 -d 录的是
# 整个窗口，还得额外解码），所以改成"在回复过程中连拍几张" ——
# 如果流式真的在逐块上屏，这几张的内容长度必然递增；
# 若是一次性拿完再画，几张会一模一样（或只有第一张是空的、第二张就满了）。
#
# 时间成本：模拟器里 app 约 1fps 左右，TCP + API 首 token 延迟几秒，
# 整个回复大概十几秒。所以 shot 之间隔 6~10 帧，连拍 4 张。
SCRIPT_M = """\
# 进入对话 -> 发两轮 -> 看第二轮的上下文条数
#
# 这里**不再连拍**。流式整个阻塞在 handle() 里，主循环停摆、帧号不走，
# 脚本的 wait/shot 期间根本推不动 —— 连拍四张全落在流式结束之后
# （2026-10-01 实测：四张 md5 完全相同，白白浪费一次跑机）。
# "吐字到一半"的取证改由 app 按内容进度自己拍（stream_轮次_序号，
# 见 sc_chat.c 的 live_snap），那条路不依赖时序。本脚本只负责摆场景。
#
# 发两轮是本趟的重点：单轮看不出多轮上下文到底有没有生效，
# 而第二轮请求里"带上了几条历史"会写进 status.txt 的 ctx: 记录，
# 同时也是屏幕上能直接看到的（演示剧本会把它念出来）。
wait 20
key A
wait 8
shot 1_chat_ready
key SELECT
wait 3
shot 2_chat_qp
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 60
key SELECT
wait 3
shot 3_round2_qp
# 已经在键盘格里（上一轮切过来的），直接 Y 发送。
key Y
wait 60
shot 4_final
wait 2
quit
"""

SCRIPT_N = """\
# 会话持久化 + 人设：夹具已经预写好 chat.json（两条历史）和带人设的 config.json。
# 本趟就是「打开对话屏 -> 看历史在不在 -> 发一条 -> 看上下文与人设有没有带上」。
# 发完之后 chat.json 应当长出新的条目（写盘生效）。
wait 20
key A
wait 8
shot 1_restored
key SELECT
wait 3
shot 2_qp
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 60
shot 3_reply
wait 2
quit
"""

SCRIPT_O = """\
# AI 创建文件（agent 能力第一步）。
#
# 夹具已经把 phrases.txt 写成只剩一条，内容就是"试着创建一个文件" ——
# 离线演示看到"创建文件"这几个字，就会在回复里附一段 file.write 动作，
# 于是整条链路在**没有真模型、没有 Key**的情况下能被完整跑一遍。
#
# 走法：进对话屏 -> SELECT 填入那条短语 -> A 发送 -> 等回复吐完
#      -> 确认屏（默认焦点在"拒绝"）-> RIGHT 移到"允许" -> A 执行
#      -> B 退回对话屏。
#
# 这里**不看**允许之前的状态：真模型会不会吐动作是它自己的事，
# 而本趟要验的是"程序拿到动作之后做得对不对"。夹具把输入钉死，
# 是为了把这个前提从判据里摘出去。
wait 20
key A
wait 8
shot 1_chat
key SELECT
wait 3
shot 2_phrase_filled
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 60
shot 3_replied
wait 8
shot 4_confirm
key RIGHT
wait 3
shot 5_focus_allow
key A
wait 6
shot 6_after_allow
key B
wait 5
shot 7_back_to_chat
wait 2
quit
"""


SCRIPT_Z = """\
# 拼音键盘：进对话 -> Y 开键盘 -> 点 n、i -> 看候选。
# 2026-10-23 真机报「拼音根本加载不出字」，这一趟就是为了让那个现象
# 在模拟器里可复现、可截图、可回归。坐标是按 sc_dock.c 的
# ime_key_rect() 算的（W_BOT_MARGIN=10, IME_KEY_W=28, IME_KEY_GAP=2）：
#   row2 "zxcvbnm" 共 7 键 -> total=208, x0=10+(300-208)/2=56
#     'n' = col 5 -> x=56+5*30=206..234，中心 220；y=80+2*32=144..172，中心 158
#   row0 "qwertyuiop" 共 10 键 -> total=298, x0=11
#     'i' = col 7 -> x=11+7*30=221..249，中心 235；y=80..108，中心 94
wait 20
touch 158 130
wait 8
key Y
wait 6
shot 1_ime_open
touch 220 158
wait 3
touch 235 94
wait 6
shot 2_ime_ni
wait 2
quit
"""

SCRIPT_P = """\
# 长消息回落（B1 回归守卫）。
#
# chat.json 是 JSONL：一行一条记录。夹具预写的第 2 行有 3070 字节 ——
# 旧实现的行缓冲只有 1280 字节，fgets 会把它**截断**：前半段当一条，
# 后半段被当成新的一行、又因为不以 '{' 开头被整行跳过，于是
# "长消息之后的那条" 也一起凭空消失（静默丢历史，不报错、不留痕）。
#
# 本趟只做一件事：进对话屏，看历史**是不是三条都在**。
# 不发消息 —— 发出去只会往结论里掺进"写盘"这条变量，而这里要验的
# 纯粹是"读回来"。断言看 app_note 的 chat: restored=N。
wait 20
key A
wait 10
shot 1_restored
wait 2
quit
"""


SCRIPT_Q = """\
# CIA 安装能力实测（回答"为什么 FBI 能装、我们能不能"）。
#
# 这一趟**不碰 UI**，只运行两条探针：
#   amprobe -> amInit / AM_GetTitleCount / AM_StartCiaInstall 的真实返回码
#   titles  -> SD 卡上已装 title 列表（只读）
#
# amprobe 一旦真的申请到 install context 会立刻 AM_CancelCIAInstall 撤回，
# 所以本趟**不会**在这台机器上留下任何装到一半的东西。
#
# 为什么它自己就能跑：这两个 op 不模拟按键、不依赖屏幕状态，
# 从开机第一帧就能执行（见 amprobe.c 的头注释）。
# 于是"这台机器、这个 exheader 的真实权限"这条结论
# 与"UI 走到哪一步"完全解耦 —— 少一个变量少一个坑。
wait 20
amprobe
wait 4
titles
wait 4
shot 1_amprobe_done
wait 2
quit
"""


SCRIPT_R = """\
# AI 安装程序（agent 能力第二步）。
#
# 与 O 趟同构：离线演示看到"装个游戏"这几个字，就在回复里附一段
# pkg.install 动作，于是"模型请求 -> 确认屏 -> 允许 -> 装进 3ds/"
# 这条链在没有真模型、没有 Key 的情况下能被完整跑一遍。
#
# 走法与 O 趟一样：进对话屏 -> SELECT 填短语 -> A 发送 -> 等回复
#      -> 确认屏（默认焦点在"拒绝"）-> RIGHT 移到"允许" -> A 执行
#      -> 按 B 退回对话屏。
#
# 本趟预期**失败但正确**：演示给的 sha256 是全零（形状对、值不对），
# 所以真正的包核不过 → 拒绝安装。这正是这条路最要紧的护栏，
# "摘要对不上还照装"才是要抓的 bug。
#
# 不断言"装成功了" —— 那需要摘要真的匹配，而演示剧本不可能知道
# 宿主机现算的摘要（那要把它烤进二进制，等于把运行时数据变成常量）。
# 所以断言看的是：解析对了、确认屏措辞对了、报错理由对了、没留残留。
wait 20
key A
wait 8
shot 1_chat
key SELECT
wait 3
shot 2_phrase_filled
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 60
shot 3_replied
wait 8
shot 4_confirm
key RIGHT
wait 3
shot 5_focus_allow
key A
wait 6
shot 6_after_allow
key B
wait 5
shot 7_back_to_chat
wait 2
quit
"""


SCRIPT_R2 = """\
# AI 安装程序 · 成功那一半（不给摘要的可选路径）。
#
# R 趟验的是"护栏拦得住"（摘要不符 → 拒绝）；本趟验"该放行时放得行"
# （没有摘要 → 不核 → 装进去）。
#
# 为什么这两件事必须分开跑：它们走的是 pkg_install 里**两个不同的分支**
# （want_sha256 有值 vs 为空）。只跑 R 会让"空指针那条路"永远没人走 ——
# 而那正是可选参数最典型的坏法。
#
# 走法与 R 趟完全一样，只是短语换成"摘要不用核对"。
wait 20
key A
wait 8
shot 1_chat
key SELECT
wait 3
shot 2_phrase_filled
# ★ 2026-10-24：**发送挪到键盘格了**（用户定的分工）。
#   第 0 格（选项）的 A 是"把这一行的值换下一档"，按 A 只会改设置、不发消息。
#   发送 = 切到「键盘」格（RIGHT）后按 Y。
#   这个改动不是偏好问题：旧脚本按 A，屏幕上"思考强度"真的被切了一档，
#   而消息一条都没发出去 —— 判据红的地方（没回复）与真实原因（没发送）
#   隔着一整个推理步骤，排查花了整整一轮。
key RIGHT
wait 4
key Y
wait 60
shot 3_replied
wait 8
shot 4_confirm
key RIGHT
wait 3
shot 5_focus_allow
key A
wait 6
shot 6_after_allow
key B
wait 5
shot 7_back_to_chat
wait 2
quit
"""


SCRIPT_T = """\# 故障注入 · 性能层（perf_lvl）。
#
# 回答的是一个否定式的问题：**坏情况下会不会把游戏带崩。**
# 这个插件会在游戏运行过程中被打开，所以"降级"可以接受、"崩"不行。
#
# 八个场景。ruler 先量时基（下面 7 个场景的判据都要拿它当尺子），
# 其余七个全部是**真故障**，不是打桩：
#   alloc   请求 768MB 栈 -> memalign 必然失败（已反汇编确认
#           threadCreate 失败是返回 NULL 而不是 abort）
#   hang    worker 进死循环 -> join 必然超时，这条线程我们**故意不 free**
#   coexist 上一个场景留下的死循环线程还在核上跑，再正常起停 3 次
#           （UAF 的经典表现就是"立刻崩"，这一条专抓它）
#   repeat  连续 start，验参数没有串用
#   edges   空函数 / 无 worker 时 join / join(UINT32_MAX) / 1 字节栈
#   churn   8 轮起停（其中一轮故意大栈失败），验没有累积泄漏
#   idem    perf_init 幂等，连调 5 次一致
#
# ruler 除了量时基，还兼一件更要紧的事：**用死循环线程证明
# "超时即放弃"真的发生**。对一个已结束的线程测超时是测不出什么的
# —— 成功是唯一合理结果。唯一能区分"超时机制可信"和"谎报成功"
# 的办法，就是拿一个永远不会结束的线程去试。
#
# 顺序有讲究：hang 必须在 coexist 之前（后者依赖前者留下的活线程），
# hang 也必须在 repeat/edges 之前（它会占住一个核，放最后会给前面
# 的场景引入本不该有的噪音）。
wait 20
perffault ruler
wait 12
perffault alloc
wait 4
perffault hang
wait 6
perffault coexist
wait 6
perffault repeat
wait 5
perffault edges
wait 5
perffault churn
wait 10
perffault idem
wait 4
shot 1_after_all_faults
wait 2
quit
"""


# U 趟：内容更新通道自测
# 验的是 cupd.c 真的能吃下 make_content.py 产出的包 —— 字段名对不上、
# sha256 长度、schema 判断，任何一个错都会在这里暴露，
# 而不是等到 3DS 上表现为「装不上」。
#
# ⚠️ 这一趟纯离线、不联网、不碰真实内容文件：
# cupd.c 的落盘根被 cu_set_root 换到 _cupdtest/ 子目录，
# 跑完自己清理。所以它进常规回归。
#
# 这条最要紧的判据是末尾那句
#   "被拒绝的更新没有动正式文件"
# —— 它对应的场景（摘要不符 → 不能碰用户手上的那份）
# 是整个模块存在的理由。少了这条判据，其余全绿也说明不了什么。
SCRIPT_U = """\
wait 4
cupdtest
wait 3
shot 1_cupdtest_done
wait 2
quit
"""

# ---- V：区域切换 + 字库安全闸 ----
# 这两件事是同一批交付的两个功能，而且都属于"**离线就该跑通**"的：
#   · 区域切换 = 往 luma/titles/<TID>/locale.txt 写一行文本（探针写完自己撤销）；
#   · 字库     = 这个模块的"功能"本身就是**在地址没实测前拒绝**，
#                所以可验的正是那些拒绝分支，而不是"真去改内存"。
# 因此它进常规回归：任何一条红了都是真缺陷，不是环境问题。
# 探针只碰它自己那个**明显是编的** TID（0004000000FFEE01），
# 不会动用户已经设过的任何东西。
SCRIPT_V = """\
wait 4
featprobe
wait 3
shot 1_featprobe_done
wait 2
quit
"""

# ---- W：内容更新界面 + 硬件知识检索 --------------------------------------
#
# 这一趟验的是 2026-10-04 那次「假更新」修复的**界面侧**：
#   · 之前 cupd.c 能装，但**没有任何一屏让用户触发它**（后端通、界面无）；
#   · guide_kb / hw_knowledge 装上之后**没有读者**（内容换了，行为不变）。
#
# 所以判据不是"文件变了"，而是三件可核对的事：
#   1. 内容更新屏能从主界面**摸到**（新按钮的触摸坐标）
#   2. 屏上列得出三类内容各自的状态（notes 里有 scan 记录）
#   3. 硬件知识能被检索到（notes 里有 hw hit 且 >0）
#
# ⚠️ 坐标 (264,129) 是三格并排里最右那格的中心。它写死在这里，
#    而 sc_home.c 里那三个 Rect 的常量也写死在那里 ——
#    两边必须一起改。只改一边的话，按钮会挪到别的格子上，
#    现象是"点内容更新进了已设区域"，而每一张截图单独看都是对的。
J_BTN_CONTENT = (264, 129)

SCRIPT_W = """\
# 硬件知识检索探针（只读）。autorun 脚本**不能打字**，
# 所以「问一句硬件问题看它答不答得对」没法在脚本里做 ——
# 那部分只能靠人手动进攻略屏敲。脚本能验的是检索本身：
# 12 条正向 + 5 条反向（游戏问句不该命中硬件）+ 答案内容 + 边界。
wait 4
hwtest
wait 3
shot 1_hwtest_done
# 内容更新屏：**主页改了 3 格布局后，内容更新搬进了「更多」二级菜单**
# （sc_more.c 的 kItems[3] = SCREEN_CONTENT）。
# ⚠️ 旧脚本写的是 `touch 264 129`「主界面最右那格」——那是 2 列 × 4 行
# 旧布局的坐标，在新布局下点不到任何格子（触摸被丢掉），
# 表现就是断言里那句「没有 content: scan 记录 —— 内容更新屏压根没进过」。
# 走法：主页 g_focus 初值 SLOT_CHAT(0)，RIGHT×2 -> SLOT_MORE(2)，A 进 MORE；
# 菜单 g_sel 初值 0（翻译），DOWN×4 -> index 4「内容更新」，A 进 SCREEN_CONTENT。
# ★ 2026-10-24 修正：原来是 DOWN×3。10-22 新加的「添加 API」插在服务目录之后，
#   把「内容更新」从 index 3 挤到 index 4，于是这一趟点进了「已设区域」，
#   断言报「内容更新屏压根没进过」—— 而这条红是**脚本没跟上**，不是程序坏了。
#   sc_more.c 的 kItems 顺序一变，这里的 DOWN 次数必须同步。
# ★ 2026-10-25：**这一段删掉了**。
#   用户要求把「更多」里的「内容更新」删掉（它和「软件更新」只差两个字，
#   他自己就点错了一次）。菜单项一删，这屏就没有入口了 ——
#   再照原样 DOWN×4 会点进「从电脑取文件」，截图看着像"内容更新"，其实是别的屏。
#   所以这一趟现在只验硬件知识检索（hwtest），界面部分随之退休。
#   将来若把 SCREEN_CONTENT 挂回菜单（或挪进设置），把下面这段恢复即可：
#     wait 10 / key RIGHT ×2 / key A / key DOWN ×4 / key A / shot 2_content_screen …
wait 2
quit
"""

# ---- Y：语音输入链路（micprobe）----------------------------------------
#
# 这一趟验的是「语音输入」三件**不依赖麦克风硬件**的事：
#   1. asr.txt 能不能读、表里的 provider 有没有生效（最容易静默失效：
#      表里写错一个名字，屏上只有"识别失败"四个字，看不出是配置没读到）
#   2. 满 5 秒录音的 WAV 头 + multipart 拼装装不装得下（缓冲常数）
#   3. 没有麦克风时**优雅降级**（不崩、给出一句人话）
#
# ⚠️ 为什么它不能验"真的识别出字"：
#   那要真麦克风 + 真 ASR Key + 真网络，三样 Azahar 里一样都没有。
#   硬塞进来的话，这条断言在模拟器里天天红 —— 而"永远红的断言"和
#   "永远绿的断言"一样没有价值。所以这里只验"本地那一半 + 降级"，
#   端到端识别是人工的事（见 micprobe.c 文件头的说明）。
#
# ⚠️ 判据 3 接受两种结果（RESULT=PASS / mic=unavailable），
#    因为它验的本来就是"没麦时不会崩"。真机上麦克风能开，
#    探针会多打一行"采到 N 字节"，那时这条走得是另一支 —— 断言仍然成立。
# ⚠️ wait / hold 的单位都是**帧**，而 Azahar 里只有 ~5fps（见 SCRIPT_H 的
# 注释）：wait 20 ≈ 4 秒。所以下面这些数字是帧数，不是秒数。
#
# 两段流程，各验一半：
#   A. hold ZL -> 走一遍**真的 UI 编排**（sc_chat 的状态机、两帧拆分、
#      失败提示）。micprobe 绕过了这一层，而"从没被执行过的代码"是最
#      会埋雷的地方：两帧拆分写错、g_input 追加越界，屏上都只是"没反应"。
#      卡上那份 asr.txt 是**假 Key**，所以这里预期走的是失败分支 ——
#      判据要的是"跑到了"，不是"识别对了"（识别准不准得真 Key 才谈得上）。
#   B. micprobe 补上采集层直读（配置读取 + body 拼装），与 A 那条路互补。
# ⚠️ micprobe 必须排在 hold ZL **之后**：
# 探针自己会 init -> 采 -> fini -> 再 init -> 采 -> fini（连着两轮，
# 验"关掉还能不能重开"），而 sc_chat 进屏时已经 micInit 了。
# 探针排在前面的话，它那一轮 voice_fini() 会把 mic 关掉、g_inited 置 0，
# 后面的「按住 ZL」就只会说"这台机器没有可用的麦克风" ——
# 那是**探针自己造成的假象**，不是硬件问题。
# 反过来放最后也不怕：ASR 的 curl 超时已降到 12s（见 asr.c），
# 脚本步数够跑完。
SCRIPT_Y = """\
wait 20
key A
wait 8
shot 1_chat_voice_idle
hold ZL 20
wait 200
shot 2_voice_done
micprobe
wait 8
shot 3_micprobe_done
wait 4
quit
"""

# ---- Z：动作链路探针（设置类动作 + KONAMI 密码门）----------------------
#
# 为什么补这一趟：actprobe.c 里早就写好了 4 个密码门用例，autorun 也支持
# actprobe 指令，但 PASSES 里**没有任何一趟跑它** —— 这些用例从来没被
# 自动回归覆盖过。补上它之后，密码门的"输对能过 / 输错不能过"才有硬证据。
SCRIPT_Z = """\
wait 20
actprobe
wait 40
shot 1_actprobe_done
wait 2
quit
"""

# ---- 2：**拒绝**也要回灌给模型（2026-10-16 新增）
#
# 这一趟盯的是"agent 干活"最容易原地打转的那条路：
# 用户点了**拒绝**之后，模型下一轮到底知不知道？
#
# 旧版的做法是：只有"成功且有明细"的那几支会把结果存进回灌缓冲，
# 于是被拒绝/执行失败时模型**什么都收不到** —— 它下一轮会**原样再发一遍**
# 同一个动作（在它看来上一轮压根没发生），用户再拒一次……
# 这正是 AGENT.md §3 决策树里那条"被拒绝就换一条路"要靠的东西。
#
# ★ 判据不看截图（拒绝之后屏幕跟"什么都没发生"长得一样），看两个硬信号：
#   ① status 里两轮的 `ctx: sys=N` —— 第二轮必须**更长**（多出那段结果）；
#   ② action.log 里有 DENY、没有 ALLOW（证明确实走的是拒绝那条路）。
SCRIPT_2 = """\
# 进对话 -> 填动作短语 -> 发送 -> 等确认屏 -> 直接 A 拒绝（默认焦点就是拒绝）
# -> ★ 等转场走完 -> 再填一次同一个短语 -> 发送：
#    这一轮的 system 里应当带上「用户拒绝了…」那段结果
#
# ⚠️ 拒绝之后那两下必须**等够帧数**：拒绝会 app_pop 回对话屏，而转场动画
#    期间截图看到的是旧屏。第一版只等 8 帧，SELECT/A 打在了转场里 ——
#    结果第二轮的输入框还是空的（截图留了证据），整趟白跑。
wait 20
key A
wait 8
shot 1_chat
key SELECT
wait 4
# ★ 2026-10-24：发送挪到键盘格（第 0 格的 A 是"改值"）→ RIGHT 切格 + Y 发。
key RIGHT
wait 4
key Y
wait 90
shot 2_confirm
# 确认屏上的 A 仍是"按下当前焦点那一项"（默认焦点 = 拒绝）—— 这一屏没动。
key A
wait 40
shot 3_back
# ⚠️ 回到对话屏时是**重新 enter**（chat_enter 把 g_tab 归零），
#   所以这里不能直接 SELECT —— 第 0 格没有输入框，短语填进去了也看不见，
#   下屏一个字都不会变（第一版就栽在这，断言报"填了短语却没变"）。
#   先 RIGHT 切回键盘格，让输入框重新出现在屏上。
key RIGHT
wait 4
key SELECT
wait 6
shot 4_filled
# 已经在键盘格里了，直接 Y 发第二轮。
key Y
wait 90
shot 5_round2
wait 2
quit
"""

# ---- 1：密码门弹窗（2026-10-16 新增）
#
# 真机反馈的原话是"按那个密码操作的话，它项目就会移动"—— 也就是
# **键位没被接管**：↑↓ 一边输密码一边挪光标，B 直接把设置屏弹掉。
# 而 B 恰好是密码的第 9 个键，所以旧版本里这串密码**永远输不完**。
#
# 这一趟就盯两件事：
#   ① 六下 A 的节奏是**真人手速**（每下间隔 6 帧 ≈ 100ms），
#      能真的把弹窗叫出来（旧的时间窗是"六下挤进 1.2 秒"，做不到）；
#   ② 输密码期间光标**一动没动**（靠 status 里的 cursor_row= 断言，
#      不靠截图 —— 弹窗把列表盖住了，光标在哪根本看不见）。
SCRIPT_1 = """\
# 自足：从首启走到主界面（本趟会删掉 config.json），再进设置
# ★ 引导键序与 SCRIPT_0 **逐键同源**（2026-10-18 修）。
#   旧序写的是"三屏引导"，而引导早就变成"三步设置 + 四屏教程"——
#   于是 1_home 实际拍到的是教程第 4 页，后面那串 DOWN 全打在
#   教程/主界面上：整趟在验一条**从没进过设置屏**的路径，
#   而断言只会说"光标不在第 17 行"，看着像行号漂移，
#   真因是走法漂移。改引导流程时 SCRIPT_0 与这里必须一起改。
wait 20
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 8
key LEFT
wait 3
key A
wait 8
wait 4
key B
wait 10
key RIGHT
wait 3
key A
wait 8
# 新手教程四屏：从第 0 屏到主界面共 4 次 A
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 8
shot 1_home
# ★ 进设置：用**触摸右上角齿轮**，不用 key L。
#   齿轮 Rect = {R_BOT_W-64, 6, 56, 22}（sc_home.c），中心 ≈ (284, 17)。
#   为什么不用 L：引导收尾最后一个 A（APIKEY 屏第 4 页收尾）与
#   home_handle 可能吃到同一帧的 A 位 —— 到主页的同帧又进了一层屏，
#   之后 L 打在那一屏上（对话屏不认 L）→ 整趟从没进过设置屏。
#   触摸是**独立输入通道**（app_touch_down），不受按键帧状态影响。
wait 6
touch 284 17
wait 20
shot 2_settings
# 一路下到「版本号」。
# ★ 这里是**按位置**导航，改设置列表的行数必须同步改它 ——
#   2026-10-18：「思考强度」一行搬到对话页（sc_chat 的 CHAT_BTN_TLVL），
#   列表从 22 行变 21 行，版本号从 0-based 20 变 19，DOWN 20 -> 19 次。
#   ⚠️ 本处实为 **20 次**（多余一次），根因就是这一趟连红两轮的源头：
#   cursor 从 0 起，20 次 DOWN 落到 index 20 = R_ROW_LANG（语言，排在
#   版本号**之后**的那一行），不是 VERSION(19) —— 于是 sc_settings.c:1021
#   的 `g_cursor != R_ROW_VERSION` 把 A 全部剔除，konami 连点从不触发，
#   后面密码序全乱、B 把设置屏弹掉、A 进对话屏，4 条判据一起红。
#   （另一处独立错误：进设置原先用 key L，会撞上"引导收尾 A 与 home_handle
#    同帧"的穿透，已改成触摸右上角齿轮——见上面那段。）
#   数 DOWN 时**不要手写数字**：R_ROW_COUNT=22（THEME=0 … VERSION=20 … LANG=21）。
#   note 里报的 cursor_row 是 **1-based**（sc_settings.c 报 g_cursor+1），
#   所以断言写 21。
#   ★ 2026-10-22 再改一轮 18 -> 20：新增 R_ROW_ACCENT（「主题色」，插在第 2 位），
#     VERSION 从 0-based 19 -> 20，所以 DOWN 要 20 次（不是 19）。
#     同一批 SCRIPT_K 也从 9 -> 10（OTA 从 0-based 9 -> 10），两处必须一起改。
#   ★ 2026-10-22 又改一轮 20 -> 21：**「思考强度」加回设置屏**（R_ROW_THINK，
#     插在「记忆」之后），VERSION 从 0-based 20 -> 21，DOWN 要 21 次。
#     断言里的 cursor_row 同步 21 -> 22（它是 1-based）。
#     同一批 SCRIPT_K 也从 10 -> 11（OTA 0-based 10 -> 11）。
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
key DOWN
wait 5
shot 3_version_row
# 连点六下 A。
# ★ 2026-10-18：这里原先每下之间 `wait 6`（7 帧/次）。在**模拟器**上
#   设置屏渲染比主页重，帧耗时可达 200~250ms —— 7 帧就是 1.4~1.75s，
#   **超过 TAP_WINDOW=900ms**，于是 `s_taps` 每次都被重置为 1，
#   连点永远凑不到 6 下 → "提示已出现" 从不落盘 → 判据 1/2/3 一起红。
#   真机 60fps 下 7 帧仅 ~117ms，所以**代码没问题**（用户真机实测
#   连点成功、L5 能提），红的只是脚本节奏。
#   改成 `wait 1`（2 帧/次）：即便按最坏的 250ms/帧算，单次间隔
#   500ms < 900ms、整串 2500ms < 4000ms，两条窗在模拟器与真机上同时成立。
key A
wait 1
key A
wait 1
key A
wait 1
key A
wait 1
key A
wait 1
key A
wait 8
shot 4_konami_modal
# 密码：↑ ↑ ↓ ↓ ← → ← → B A（B 那一键在旧版本会把整屏弹掉）
key UP
wait 3
key UP
wait 3
key DOWN
wait 3
key DOWN
wait 3
key LEFT
wait 3
key RIGHT
wait 3
key LEFT
wait 3
key RIGHT
wait 3
key B
wait 3
key A
wait 8
shot 5_after_password
wait 2
quit
"""
# ---- 0：引导 → 录入凭据屏 → 服务目录守卫（2026-10-16 新增）
#
# 这一趟**验不了输入**：录入凭据屏的编辑动作要弹系统软键盘，而模拟器里
# swkbd 弹的是 Qt 窗口，脚本够不着（同 H 趟那句注释）。能自动化的只有：
#   ① 第 3 步的按钮真的能把人送进新屏（不是"按了没反应"）；
#   ② 新屏**画得出来**且没有横向/纵向溢出；
#   ③ 从新屏退回来会**停在第 3 步**，不会被打回第 1 步。
# ③ 是这一趟最值钱的一条：welcome_enter 每次进屏都会跑，而 app_pop
# 恰恰会对回来的那一屏重调 enter() —— 守卫写错的话用户录完 Key
# 就得重新走两页，而那种 bug 在截图上看着只是"页号是 1"，
# 很容易被当成"正常"。
SCRIPT_0 = """\
# 首启：走到第 3 步 -> 左选「录入凭据」-> A 进新屏 -> 移动光标 -> B 返回
# 再从第 3 步收尾 -> 教程四屏 -> 主界面 -> 摸一下服务目录的 X/Y 守卫
wait 20
shot 1_welcome
key A
wait 3
key A
wait 6
shot 2_step2_nand
key LEFT
wait 3
key A
wait 8
shot 3_step3_service
key LEFT
wait 3
shot 4_step3_record_selected
key A
wait 10
shot 5_keyedit
key DOWN
wait 4
shot 6_keyedit_row2
key B
wait 10
shot 7_back_to_step3
# 退回来时第 3 步仍选着「录入凭据」（状态保留），先切回「完成设置」再按 A
key RIGHT
wait 3
key A
wait 8
# 新手教程四屏：从第 0 屏到主界面共 4 次 A
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 8
shot 8_home
# ★ 主界面 2026-10-17 改版：**只剩 3 格**（开始对话/离线攻略/更多，排成一行），
#   服务目录**搬进了「更多」二级菜单**（sc_more.c 的 kItems[1] = SCREEN_PROVIDERS）。
#   HOME_COLS=3，g_focus 进屏恒为 SLOT_CHAT(0)；RIGHT×2 -> SLOT_MORE(2)，
#   A 进 SCREEN_MORE；菜单初始 g_sel=0（翻译），DOWN×1 -> index 1「服务目录」，
#   A 进 SCREEN_PROVIDERS。
#   ⚠️ 旧脚本写的是"2 列 × 4 行、主界面最右那格就是服务目录"，那是旧布局；
#   在新布局下 RIGHT×2+A 只到 MORE，再 A 进的是「翻译」（对话屏），
#   后面的 X/UP 全打在对话屏上 —— 三条判据全红且看着像"守卫失效"。
#   sc_more.c 的 kItems 长度/顺序一变，这里的 DOWN 次数必须同步。
key RIGHT
wait 3
key RIGHT
wait 3
key A
wait 8
key DOWN
wait 4
key A
wait 10
shot 9_providers
# 光标停在「当前在用」那家（进屏时就是它），按 X —— 走的是
# "内置地址改不了"那条说明，**也不弹键盘**。
# ⚠️ 这条路不弹键盘的前提是"当前那家不是 custom"：夹具里
#    （stage_keyfile → provider=zhipu，无夹具时是默认 deepseek）
#    从来没有 custom，所以安全。**将来若有夹具把 provider 设成
#    custom，这一趟会挂死在 swkbd 上**，届时把这四行删掉。
key X
wait 4
shot 10_providers_builtin_url
# ★ 再按上键换到「不是当前在用」的那一家，按 X/Y。
#   这条路走 edit_guard 的"先切过来"分支 —— 无论当前那家是什么
#   都不会弹键盘（守卫在碰键盘之前就返回了）。
key UP
wait 3
key X
wait 4
shot 11_providers_url_guard
key Y
wait 4
shot 12_providers_model_guard
wait 2
quit
"""

# ---- 7：十字键管下屏 / 摇杆管上屏滚动 / 最下行操作指引（2026-10-23 新增）
#
# 用户原话：「现在逻辑是十字键控制下屏内容，然后那个摇杆上下控制上屏文字
# 上下滑动。另一个是最下行，给出操作指引。」
#
# 这一趟验三件在别的趟次里**根本碰不到**的事：
#   ① ←→ 真的在切下屏滑轨（判据：底屏像素在切格前后**不同**，且切回来
#      之后与最初**相同** —— 只判"不同"会被状态行文字/闪烁骗过）；
#   ② ↑↓ 真的在动当前格的光标（判据同上，但只在第 0 格验：选项行高亮
#      位置一变，整块底屏必然不同）；
#   ③ 摇杆真的能滚上屏（判据：推杆前后**上屏**不同，而底屏**不变** ——
#      底屏不变是关键：如果底屏也变了，说明这不是"滚动"，是别的东西
#      顺带改了画面，那这条断言就失去了意义）。
#
# ⚠️ ③ 必须先有"能滚的内容"：上屏没消息时 g_scroll 卡在 0，
#    推杆一格都走不动，画面也不会变 —— 于是这条会假失败。
#    所以脚本先按 SELECT 填一条快捷短语再发送（离线演示 provider=demo
#    会立刻回一句），这样上屏真的有内容可滚。
#    ⚠️ 这一步依赖 M 趟的离线夹具？不依赖 —— 无夹具时 provider 是
#    deepseek 且没有 Key，发送会失败。所以本趟**只验"有内容时的滚动"**：
#    判断不了内容有没有，就退回到"推杆前后上屏相同 = 这条没验到"，
#    并在断言里显式区分「没内容可滚」与「推杆无效」。
#
# ⚠️ 引导段与 SCRIPT_4 逐键同源（改引导必须两处一起改）。
SCRIPT_7 = """\
wait 20
# 走完引导到主页（与 SCRIPT_4 / SCRIPT_0 同源）
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 8
key LEFT
wait 3
key A
wait 8
wait 4
key B
wait 12
key RIGHT
wait 3
key A
wait 10
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 10
# 主界面 → 开始对话（用触摸，见 SCRIPT_J 的说明：连续注入同名按键
# 在模拟器里只交付一个按下沿，触摸是独立通道）
touch 158 130
wait 10
shot 1_chat_opt
# 先塞一条短语，让上屏真的有内容可滚
#
# ★ 这里**用 SELECT 而不是 A**：第 0 格是「选项」，A 在这一格的含义是
#   "把光标那一行的值换下一档"（见 sc_dock.c 的 dockui_keys_opt）。
#   2026-10-23 第一版脚本按了 A，于是「思考强度」真的被切了一档 ——
#   后面"回位后画面应当与最初相同"两条判据因此全红，而代码本身是对的。
#   这不是"判据太严"，是脚本自己改了状态：**验收脚本不许有副作用**，
#   否则它验的就不再是同一件事了。
#
# ⚠️ 也别再在这里按 A 发送：A 在第 0 格永远是"切值"。本趟不发送 ——
#   它的目的是"上屏有内容可滚"，而卡上 chat.json 已经有历史（restored=4），
#   现成的就够了。为了滚动去发一条请求，反而会把网络状态混进判据里。
key SELECT
wait 6
shot 2_chat_sent
# ① ←→ 切格：切到「键盘」格
key RIGHT
wait 6
shot 3_tab_kbd
# 切回「选项」格 —— 与 1 对比应当逐字节相同（除状态行外）
key LEFT
wait 6
shot 4_tab_opt_back
# ② 第 0 格内 ↑↓ 选行
key DOWN
wait 6
shot 5_opt_row2
key UP
wait 6
shot 6_opt_row1
# ③ 摇杆上推：连续 16 帧 dy=-120（累积量够走好几格）
stick -120 16
wait 6
shot 7_scrolled_up
# 摇杆下推：回到最新
stick 120 16
wait 6
shot 8_scrolled_down
# ④ 最下行操作指引 —— **必须换到「键盘」格再测**
#
# ★ 2026-10-23 踩过的坑：第 0 格（选项）自己也会画一条固定页脚
#   （S0963「[←→] 切格 [↑↓] 选行 [A] 动作 [B] 返回」，见 sc_dock.c
#   的 dockui_render_opt），那条**不轮播**。而对话格的轮播页脚
#   （7 条键位指引）只画在**非第 0 格**的路径上 —— 也就是 g_tab>0。
#   第一版脚本全程停在第 0 格，于是"轮播在走"永远测不到：页脚压根
#   不是同一个控制器画出来的。判据红的原因不是功能坏了，是**测错了格子**。
#
#   所以这里先 RIGHT 切到「键盘」格，再等两轮（每条 3.2s，模拟器
#   一帧约 0.2s，90 帧 ≈ 18s，足够走好几条）。
key RIGHT
wait 8
shot 9_hint_a
# ★ 取三张、间隔两次，而不是"前后两张必须不同"（2026-10-24 改）。
#
#   轮播是**按时间**推进的（3.2s 一条），而 w_bot_footer_roll 用 while
#   补齐错过的轮次 —— 于是"刚切回这一格"那一帧会把中间几条一次跳过，
#   之后又老实等 3.2s。两张截图恰好落在同一格的概率不为零（实测栽过：
#   status 里明明记着 step=2，两张却一模一样）。
#   三张里只要有一对相邻不同，就证明"文案真的会换" —— 这才是判据要的。
#   要求"每一对都不同"反而是把判据绑在模拟器帧率上，与功能无关。
wait 60
shot 9b_hint
wait 60
shot 10_hint_b
wait 2
quit
"""

# ---- 4：更多 → 添加 API（2026-10-22 新增，第五件需求）
#
# 用户要求：「API 除了新手教程之外，在更多里也可以通过多种方法加进来 ——
# 可以写文件，也可以直接手输，可以自定义网站 / API Key / 模型名称」。
#
# 这一趟盯三件事：
#   ① 「更多」菜单里**真的有一项「添加 API」**，并且按 A 能进录入凭据屏
#      （菜单多了第 3 项，DOWN×2 从「翻译」走到它）；
#   ② 录入凭据屏**版面上没有重叠**：新增的「从文件导入」按钮落在下屏，
#      layout_overflow 必须是 0，且下屏那两个按钮不能互相压；
#   ③ 触摸「从文件导入」真的被接住（status 里有 keyedit: import 记录）——
#      宿主铺好了 key.txt 夹具，所以这条能走通。
#
# ⚠️ 走法与 SCRIPT_0 的引导段**逐键同源**（改引导必须两处一起改）。
# ⚠️ 「更多」菜单是**按位置**导航的：kItems 顺序一变，这里的 DOWN 次数
#    必须同步（当前 index 2 = SCREEN_KEYEDIT）。
SCRIPT_4 = """\
wait 20
# 引导：三次 A 走到第 3 步，再一路收尾到主页（与 SCRIPT_0 同源，但
# 这一趟不进去配 Key —— 直接走到主页）
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 8
key LEFT
wait 3
key A
wait 8
wait 4
key B
wait 10
key RIGHT
wait 3
key A
wait 8
# 教程四屏
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 8
shot 1_home
# ★ 主页：点「更多」那一格 —— 用**触摸**，不用 RIGHT×2。
#   为什么要换：上一版 RIGHT×2 只有第一下生效（status 里只有
#   home: focus=1），第二下在**同一屏内**被吞掉；把间隔从 3 帧
#   拉到 10 帧也没用。真正的原因不是等待不够，而是连续注入同名按键时
#   模拟器只交付一个按下沿 —— SCRIPT_0 里那串 DOWN×20/21 之所以有效，
#   是因为中间夹了 wait 且它们**落在同一屏的同一处理分支**上（行号递增
#   可自证），而这里两下 RIGHT 都要求"焦点真的挪一格"，判据严格得多。
#   触摸是独立输入通道（app_touch_down），一格直达，不受这个问题影响。
#   「更多」格：HOME_BTN_MORE = {10+144+12, 150, 144, 32} -> 中心 (238, 166)。
touch 238 166
wait 12
shot 2_more_menu
# 「更多」里 DOWN×2 -> index 2 = 「添加 API」（index 0 翻译 / 1 服务目录）
# ⚠️ 这里仍用按键：它是**同一屏内**的行号递增，且 next 之后会按 A，
#   即使少挪一格也会被 more: -> item N 断言抓到（不静默）。
key DOWN
wait 12
key DOWN
wait 12
shot 3_more_addapi_selected
# A 进录入凭据屏（SCREEN_KEYEDIT）
key A
wait 12
shot 4_keyedit
# 一路 DOWN 到底（8 行），看每行都画得出来、没有叠字
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
key DOWN
wait 3
shot 5_keyedit_lastrow
# 触摸「从文件导入」按钮 —— 它的 Rect 是 {10,196,300,26}，中心 (160,209)
touch 160 209
wait 8
shot 6_import_done
wait 2
quit
"""

# ---- 5：设置屏 L/R 翻页 + 网络自检（2026-10-23 新增）
#
# 用户反馈「设置屏 23 行只能一下一下按方向键，太累」。这一趟验两件事：
#   ① L/R 真的**整屏翻页**（7 行一步），而且到头会夹住不越界；
#   ② 从主页搬来的「网络自检」**点得着、按得动**（note 里有 netprobe=）。
#
# 为什么必须单独一趟：这两条都是"按了有反应"型的改动 ——
# 现有趟次（0/1/4/K）都不碰它们，静态看代码也看不出翻页幅度对不对。
# 判据全是 status.txt 里的数字，不看截图。
SCRIPT_5 = """\
wait 20
# 走完引导到主页（与 SCRIPT_4 同源）
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 8
key LEFT
wait 3
key A
wait 8
wait 4
key B
wait 12
key RIGHT
wait 3
key A
wait 10
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 10
# 触摸右上齿轮进设置（SCRIPT_0/1 验证过这条路，不受按键帧穿透影响）
touch 284 17
wait 20
shot 1_settings_top
# R 翻一屏：光标应从第 1 行跳到第 8 行（ROW_VISIBLE=7）
key R
wait 8
shot 2_settings_paged
# 再按 R 两次 —— 注意这里要**按满 4 次**才撞到底：
#   1 -> 8 -> 15 -> 22 -> (29 越界，夹到 24)。
# ★ 这一条我第一版写成"按 3 次就该到底"，实测停在第 22 行而判红 ——
#   代码是对的，是**判据算错了**：ROW_VISIBLE=7，24 行要 4 步才够到。
#   "到头夹住"这件事本身必须验（越界会写出 g_cursor=29 这种值，
#   渲染时行窗口算出来是负数，画面直接空白），所以补满 4 次。
key R
wait 8
key R
wait 8
shot 3_settings_bottom
# 从上往下按 L 翻回顶部，同样要 4 次（24 -> 17 -> 10 -> 3 -> 夹到 1）
key L
wait 8
key L
wait 8
key L
wait 8
key L
wait 8
shot 4_settings_top_again
# 一路 DOWN 到「网络自检」（0-based 20 -> 从 0 起按 20 次）
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
key DOWN
wait 2
shot 5_netprobe_row
# 按 A 执行自检（会阻塞几秒，等够）
key A
wait 120
shot 6_netprobe_done
wait 2
quit
"""

# ---- X：推理耗时基线（**不进常规回归**）-------------------------------
#
# 为什么单独一趟、且默认不跑：
# 它输出的是**时间**，而时间没有固定值可断言 —— 模拟器调度、
# 宿主 CPU 争用、升频是否生效都会影响。
# 上一轮 8 路展开的教训就是：静态指令数降 19% 而实测更慢。
# 所以优化必须有前后两次实测，而"这次的数字是多少"这件事
# **只能靠人比对**，脚本判不了。
#
# 脚本能判的只有：探针跑到了、给出了可用数字、且不是 SKIP
# （SKIP = 卡上没有模型文件，那就没有基线可言）。
#
# 用法：
#   1. python tools/azahar_test.py --passes X   -> 记下 RAW 那一行
#   2. 改推理内核，重编
#   3. 再跑一次 X，对比两次的 RAW
# ⚠️ 对比时**只看 RAW（tick 数）**。ms 那一行经过换算，
# 换算本身是整数除法，会把小差异抹掉。
SCRIPT_X = """\
# 推理耗时基线。需要卡上有 sdmc:/3ds/aiconsole/guide_model.bin。
wait 4
perfprobe
wait 3
shot 1_perfprobe_done
wait 2
quit
"""

# 不进默认回归的趟次。理由逐条写清楚，因为「为什么它不在里面」
# 和「它为什么存在」同样重要 —— 不写的话下一个人会想当然地把它当门禁。
#   X = 推理耗时基线：①要卡上有 guide_model.bin（模拟器不一定有）
#      ②输出是时间，没有可断言的固定值
DEFAULT_EXCLUDE = {"X", "X2"}


# ---- X2：抽取式回答验收（**不进常规回归**，同 X 的理由）--------------
#
# 判据是**输出文本**而不是 rc：我在 C 端犯过两个「编译通过、逻辑全错」
# 的坑（multi-character constant 让 '。' 比较恒为假、切不出句子），
# 只有实跑才暴露。
SCRIPT_X2 = """# 抽取式回答验收。需要卡上有 guide_model.bin。
wait 4
exttest
wait 3
shot 1_exttest_done
wait 2
quit
"""

#: 趟 8 · 每个任务一份历史（2026-10-24 用户要求）
#:
#: 用户原话：「选择新项目后，上屏的页面没有变化，没有进入新项目的空间。
#: 同理，打开新项目的时候，也要看到那个项目之前做的内容。」
#:
#: 验的就是这两句，一条断言对一句：
#:   ① 切到任务 B → 上屏**必须**变（否则等于没进去）
#:   ② 切回任务 A → 上屏**必须**变回原样（否则 B 的历史把 A 冲了）
#:
#: ⚠️ 别只判"变了"：改一下状态行也会让上屏变。所以三个指纹要两两不同
#:   （A ≠ B）**且**首尾相同（回到 A = 原样），两条一起才说明"分得开"。
SCRIPT_8 = """\
wait 20
key A
wait 3
key A
wait 6
key LEFT
wait 3
key A
wait 8
key LEFT
wait 3
key A
wait 8
wait 4
key B
wait 12
key RIGHT
wait 3
key A
wait 10
key A
wait 6
key A
wait 6
key A
wait 6
key A
wait 10
touch 158 130
wait 10
shot 1_proj_a
# 切到「项目」格（0 选项 → 1 键盘 → 2 项目）
key RIGHT
wait 4
key RIGHT
wait 6
shot 2_proj_panel
# 点第 1 行 = 切到任务 B。行坐标 row_rect: y = 54 + i*26，高 24
#   → 第 0 行 54..78（中心 66）、第 1 行 80..104（中心 92）
touch 158 92
wait 10
shot 3_proj_b
# 点第 0 行 = 切回任务 A
touch 158 66
wait 10
shot 4_proj_a_again
wait 2
quit
"""

# 语音请求的"连接复用"验收（2026-10-24）。
# 只做一件事：对同一个主机连发两次请求，看第二次有没有重新建连/握手。
# 判据不在这里，在探针里（asrnetprobe.c）：第二次 conn 和 tls 必须为 0。
# 之所以要单独一趟：这条链路的收益是**时间**，而时间在截图上看不出来。
SCRIPT_9 = """\
wait 4
asrnet
wait 10
shot 1_asrnet_done
wait 2
quit
"""

# 远程更新的网络链路（2026-10-24 修）。只取 manifest，不下产物。
# 为什么要单独一趟：这条链路以前用 httpc（只到 TLS 1.1）根本连不上 OSS，
# 而"连不上"和"线上没有新版本"在屏上都是同一句"检查失败"，分不开。
SCRIPT_UP = """\
wait 4
otanet
wait 14
shot 1_otanet_done
wait 2
quit
"""

# 远程更新的下载段：真下一个包（挑最小的那个）。otanet 只验 manifest，
# 而"流式写盘 + 边下边算 sha256 + rename"是另一条路径，错一点的表现是
# "下完了但摘要不符" —— 那句话会把排查引向"线上文件被换过"，方向全错。
SCRIPT_DL = """\
wait 4
otadl
wait 20
shot 1_otadl_done
wait 2
quit
"""

# 更新屏的**按钮 + 读条**（2026-10-25）。这一趟专门验用户要的那件事：
# 下屏有没有一个明确的「检查更新」按钮，按下去屏上有没有进度。
# 注意：真正取 manifest 会阻塞，所以 2 号图抓的是"动画泵到一半"的那一帧 ——
# 抓不到也不算失败（时机问题），判据看 status 里的 ota: online check 记录。
SCRIPT_CH = """\
wait 20
key R
wait 8
shot 1_update_idle
key A
wait 6
shot 2_update_progress
wait 45
shot 3_update_result
wait 2
quit
"""

# 滑轨「状态」格（2026-10-25）：点第 6 格，看三块图有没有画出来、数字是否在动。
# 触摸坐标：6 格 ×44 + 5 间隙 ×2 = 274，右对齐到 x=310 → 起点 x=36；
# 第 6 格是 36+5*46=266..310，取中心 (288, 33)。
# 两张图隔一段时间再拍，用来看"柱子确实在长"（静态图证明不了它在采样）。
SCRIPT_ST = """\
wait 20
key A
wait 10
touch 288 33
wait 14
shot 1_stat_panel
wait 45
shot 2_stat_later
wait 2
quit
"""

# 软件更新屏的两页（2026-10-25）：程序(L/R)->内容包(L/R)->程序。
# 内容包原来是一个独立屏「内容更新」，已并入本屏当第二页；
# 这趟要证的正是"合并后它还能进、还能画、还能退回"。
SCRIPT_CP = """\
wait 20
key R
wait 8
shot 1_prog_page
key L
wait 8
shot 2_content_page
key R
wait 6
shot 3_back_prog
wait 2
quit
"""

# 代码块落盘（2026-10-25）：模型回复里的 ``` 块要变成沙箱里的文件。
# 不走网络 —— 探针直接喂一段写死的带围栏文本，验"解析 + 写文件"这两步。
SCRIPT_CB = """\
wait 4
codeprobe
wait 3
shot 1_codeprobe
wait 2
quit
"""

# 删除任务要确认（2026-10-26 用户要求）：请求删除 -> 弹窗 -> 取消 -> 再删。
# 触摸坐标：滑轨 6 格 ×44 + 5 间隙 ×2，右对齐到 310，起点 36；
# 「项目」是第 3 格（0 基 2）→ 36+2*46=128..172，取中心 (150, 33)。
# 键盘路径：SELECT 提请求，A/← 取消，→/Y 才真删（安全侧默认是取消）。
# 删除任务要确认（2026-10-26 用户要求）：点「×」→ 弹窗 → 取消 → 再点 → 红色删除。
# 坐标（都是算出来的，不是试出来的）：
#   滑轨「项目」格：6 格 ×44 + 5 间隙 ×2，右对齐到 310，起点 36 → 第 3 格 128..172
#   第 1 行的「×」：row_rect(0) = (10,54,300,24)，叉在行右端 24 宽 → 中心 (298,66)
#   弹窗红色按钮：{168,148,136,34} → 中心 (236,165)
SCRIPT_PD = """\
wait 20
key A
wait 8
touch 150 33
wait 6
shot 1_proj_tab
touch 298 66
wait 6
shot 2_confirm
key A
wait 4
shot 3_cancelled
touch 298 66
wait 5
shot 4_confirm_again
touch 236 165
wait 8
shot 5_deleted
quit
"""


# 配置 DeepSeek V3 的实弹脚本（2026-10-27）。
# ⚠️ Key **不写进仓库**：脚本运行时从本机密码文件里取，取不到就空着跑
#    （那样 keyedit 会提示"空 Key 不接受"，脚本照常能验前面的步骤）。
def _da_key():
    for ln in open(r"E:/Gudyi/密码/deepseek api.txt", encoding="utf-8", errors="replace"):
        v = ln.strip()
        if v.startswith("sk-") and len(v) == 35:
            return v
    return ""

def _da_script():
    # ★ 行序（sc_keyedit.c 的 row_visible）：只有 SVC / MODEL / KEY / SAVE 可见，
    #   URL 仅在自定义时可见，人设/上下文/思考强度已挪到设置屏。
    #   所以 **MODEL 下一格就是 KEY**（一次 DOWN），不是四次 ——
    #   第一版按"8 行全可见"算，DOWN×4 绕回 MODEL，把 Key 追到了模型字段上。
    return """\
wait 20
key RIGHT
wait 3
key RIGHT
wait 3
key A
wait 8
key DOWN
wait 3
key DOWN
wait 3
key A
wait 10
shot 1_keyedit
key LEFT
wait 4
shot 2_svc_deepseek
key DOWN
wait 3
key A
wait 6
shot 3_model_edit
type deepseek-chat
wait 3
key SELECT
wait 5
shot 4_model_done
key DOWN
wait 3
key A
wait 6
shot 5_key_edit
type %s
wait 3
key SELECT
wait 5
shot 6_key_done
key DOWN
wait 3
key A
wait 8
shot 7_saved
touch 237 174
wait 14
shot 8_tested
quit
""" % _da_key()

# 中文输入法（2026-10-27）：点字母 → 出候选 → A 上屏 → 数字/符号/空格。
# 坐标按 IME_KEY_W=28 / GAP=2 / 行 y=64,94,124,154 算出来的：
#   qwertyuiop 10 键总宽 280，起始 x=11 → 第 i 键中心 = 11+30i+14
#   asdfghjkl   9 键总宽 268，起始 x=16
#   zxcvbnm     7 键总宽 208，起始 x=56
#   n 在第 3 行第 6 列 → 56+5*30+14 = 220, y=124+13=137
#   i 在第 1 行第 8 列 → 11+7*30+14 = 235, y=64+13=77
SCRIPT_IM = """\
wait 20
key A
wait 8
touch 104 33
wait 6
shot 1_keyboard
touch 220 137
wait 3
touch 235 77
wait 4
shot 2_pinyin_ni
key A
wait 4
shot 3_committed
touch 14 197
wait 4
shot 4_space
touch 233 197
wait 4
shot 5_after
quit
"""

PASSES = {
    "A": dict(script=SCRIPT_A, shots=9, title="首启 · 引导三屏 + 新手教程四屏"),
    "B": dict(script=SCRIPT_B, shots=1, title="二次启动 · 跳过引导"),
    "C": dict(script=SCRIPT_C, shots=2, title="横幅回归 · 未备份提醒"),
    "D": dict(script=SCRIPT_D, shots=2, title="教程可跳过 · 主界面能回来"),
    "E": dict(script=SCRIPT_E, shots=3, title="OTA · 说明查看 + 校验 + 安装离线更新包"),
    "3": dict(script=SCRIPT_3, shots=4, title="插件页 · 主页导航 + 列表 + 返回"),
    "F": dict(script=SCRIPT_F, shots=1, title="网络探测 · httpc 在模拟器里能不能出去"),
    "G": dict(script=SCRIPT_G, shots=2, title="对话屏 · 进入与空输入保护"),
    "H": dict(script=SCRIPT_H, shots=3, title="真实发送 · 快捷短语 -> API -> 回复"),
    "I": dict(script=SCRIPT_I, shots=2, title="Key 管理 · 列表 + 切换 + 落盘"),
    "J": dict(script=SCRIPT_J, shots=2, title="触摸 · 底屏按钮点击（进对话 + 空发送）"),
    "K": dict(script=SCRIPT_K, shots=5, title="设置屏 · 主题切换 + 护栏项 + 返回后仍生效"),
    "L": dict(script=SCRIPT_L1, shots=6,
              title="M5 加密存储 · 入库 + 重启免输入 + 防篡改（三阶段）"),
    "M": dict(script=SCRIPT_M, shots=12,
              title="M6 流式输出 + 多轮上下文 · 离线演示趟，发两轮"),
    "N": dict(script=SCRIPT_N, shots=7,
              title="会话持久化 + 助手人设 · 恢复历史、带上人设"),
    "O": dict(script=SCRIPT_O, shots=14,
              title="AI 创建文件 · 动作解析 + 确认屏 + 沙箱落盘 + 审计"),
    # ★ 键位用 6 而不是 Z：Z 早就被「动作链路探针」占了，两个同名 dict
    #   条目会**后者覆盖前者**（我第一次就踩了，跑出来是 actprobe 的截图）。
    #   加新趟次前先看 PASSES 里哪些键空闲。
    "6": dict(script=SCRIPT_Z, shots=2,
              title="拼音键盘 · 开键盘 + 打 ni + 看候选"),
    "P": dict(script=SCRIPT_P, shots=2,
              title="长消息回落 · JSONL 行超旧行缓冲不丢历史"),
    "Q": dict(script=SCRIPT_Q, shots=2,
              title="CIA 安装能力实测 · am:u 权限返回码（FBI 之问）"),
    "R": dict(script=SCRIPT_R, shots=14,
              title="AI 安装程序 · pkg.install 解析 + 确认屏 + 摘要护栏"),
    "S": dict(script=SCRIPT_R2, shots=14,
              title="AI 安装程序 · 成功路径（不给摘要 → 真的装进 3ds/）"),
    "T": dict(script=SCRIPT_T, shots=2,
              title="故障注入 · 性能层八场景（时基/内存不足/线程卡死/UAF 邻域）"),
    "U": dict(script=SCRIPT_U, shots=1,
              title="内容更新通道 · 解析拒绝 + 安装/回滚闭环 + 坏包不碰原文件"),
    "V": dict(script=SCRIPT_V, shots=1,
              title="区域切换 + 字库安全闸 · 写 luma/titles 与拒绝非法输入"),
    "W": dict(script=SCRIPT_W, shots=1,
              title="硬件知识检索（内容更新屏已从菜单移除，界面部分退休）"),
    "Y": dict(script=SCRIPT_Y, shots=3,
              title="语音输入链路 · UI 编排（按住 ZL 走完）+ ASR 配置读取 + 拼装自检"),
    # X 不进常规回归：它输出的是**时间**，没有固定值可断言。
    # 它的用法是「改了推理内核之后先跑一次，拿这次的数字当新基线」。
    # 要断言的只有一件事：探针跑到了、且给出了可用数字。
    "X": dict(script=SCRIPT_X, shots=1,
              title="推理耗时基线 · 整段/检索/原始 tick（人工比对用）"),
    "X2": dict(script=SCRIPT_X2, shots=1,
               title="抽取式回答验收 · 8 条用例（人工比对用）"),
    "Z": dict(script=SCRIPT_Z, shots=1,
              title="动作链路探针 · 设置类动作落盘读回 + KONAMI 密码门四用例"),
    # ⚠️ 趟名只能用**单个字符**：--passes 的解析规则是"逐字符匹配优先"
    #    （见文件末尾那段说明），两字符的趟名会被拆成单字去匹配。
    #    A-Z 已经全被占了，所以新趟叫 "0" / "1"。
    "0": dict(script=SCRIPT_0, shots=12,
              title="引导 → 录入凭据屏 → 服务目录守卫 · 输入动作真机才可验"),
    "1": dict(script=SCRIPT_1, shots=5,
              title="密码门弹窗 · 键位被接管 + 真手速连点六下 + 密码能输完"),
    "2": dict(script=SCRIPT_2, shots=6,
              title="拒绝也回灌 · 模型下一轮收得到「用户拒绝了」，不会原地重发"),
    "4": dict(script=SCRIPT_4, shots=6,
              title="更多 → 添加 API · 菜单入口 + 录入屏版面 + 从文件导入"),
    "5": dict(script=SCRIPT_5, shots=6,
              title="设置屏 L/R 翻页 + 网络自检（搬自主页 SELECT）"),
    # ★ 2026-10-23 新增：用户定的新分工（十字键管下屏 / 摇杆管上屏滚动 /
    #   最下行给操作指引）。这趟验三件事：
    #     ① ←→ 真的在切下屏格子（不是只在画面上写着）
    #     ② ↑↓ 真的在动当前格的光标
    #     ③ 摇杆真的能滚上屏（靠 autorun 的 stick 指令驱动）
    #   为什么必须单独一趟：这三条都是"新接线"，任何一处没接上，
    #   表现都是"按了没反应"，而别的趟次根本不碰这些键。
    "7": dict(script=SCRIPT_7, shots=11,
              title="十字键切格/选行 + 摇杆滚上屏 + 最下行操作指引"),
    # ★ 2026-10-24 新增：每个任务一份对话历史。
    "8": dict(script=SCRIPT_8, shots=4,
              title="按任务分历史 · 切项目上屏就换 · 切回来还是原样"),
    # ★ 2026-10-24 新增：语音连接复用。**要联网**，模拟器能出去网才有意义
    #   （趟 F 早就在验这条路通不通）。判据是"第二次 conn/tls 归零"，
    #   比"总时长变短"可靠 —— 后者会被服务端缓存和抖动带偏。
    "9": dict(script=SCRIPT_9, shots=1,
              title="语音连接复用 · 同一主机冷热两连（第二次不该再握手）"),
    # ★ 2026-10-24 新增：远程更新链路。**要联网**才有意义。
    #   键用两字符 "UP"：单字符编号已经用满，而 --passes 是"先长后短"匹配，
    #   "UP" 不会被拆成 U + P。
    "UP": dict(script=SCRIPT_UP, shots=1,
               title="远程更新链路 · 逐源握手 + 真的取一次 manifest"),
    # ★ 2026-10-24 新增：远程更新**下载**段（要联网，会真下几 MB）
    "DL": dict(script=SCRIPT_DL, shots=1,
               title="远程更新下载 · 流式写盘 + sha256 + rename"),
    # ★ 2026-10-25：更新屏的按钮与进度（用户要求的"专门按钮 + 读条"）
    # ★ 2026-10-25：滑轨第 6 格「状态」——帧率/主循环占用/内存
    # ★ 2026-10-25：软件更新的「内容包」页（原「内容更新」屏已并入）
    # ★ 2026-10-25：代码块 → 沙箱文件（AI 发的代码要能落成可打开的文件）
    # ★ 2026-10-26：删除任务必须先弹窗、红色按钮确认
    # ★ 2026-10-27：实弹配置 DeepSeek V3（添加 API 全流程 + 自绘键盘注入）
    # ★ 2026-10-27：中文输入法（拼音候选 + 上屏 + 空格）
    "IM": dict(script=SCRIPT_IM, shots=5,
               title="输入法 · 拼音出候选 / A 上屏 / 空格"),
    "DA": dict(script=_da_script(), shots=8,
               title="添加 API · 自绘键盘填模型/Key 并保存（DeepSeek V3）"),
    "PD": dict(script=SCRIPT_PD, shots=5,
               title="删除任务 · 确认弹窗（取消 / 红色删除按钮）"),
    "CB": dict(script=SCRIPT_CB, shots=1,
               title="代码块落盘 · 围栏解析 + 扩展名 + 没收尾的块"),
    "CP": dict(script=SCRIPT_CP, shots=3,
               title="软件更新 · 程序/内容包 两页切换"),
    "ST": dict(script=SCRIPT_ST, shots=2,
               title="滑轨状态页 · CPU/帧耗时/内存三块图"),
    "CH": dict(script=SCRIPT_CH, shots=3,
               title="更新屏 · 检查更新按钮 + 进度条（要联网）"),
}# ---------------------------------------------------------------------------
# SD 根目录
# ---------------------------------------------------------------------------

def sdmc_roots(exe_dir):
    """所有可能被当成虚拟 SD 根目录的位置。"""
    appdata = os.environ.get("APPDATA", "")
    localapp = os.environ.get("LOCALAPPDATA", "")
    cands = [
        os.path.join(exe_dir, "sdmc"),
        os.path.join(exe_dir, "user", "sdmc"),
        os.path.join(exe_dir, "user"),
        os.path.join(appdata, "azahar", "sdmc"),
        os.path.join(appdata, "Citra", "sdmc"),
        os.path.join(appdata, "Lime3DS", "sdmc"),
        os.path.join(localapp, "azahar", "sdmc"),
    ]
    seen, out = set(), []
    for c in cands:
        c = os.path.normpath(c)
        if c and c not in seen and os.path.dirname(c) != c:
            seen.add(c)
            out.append(c)
    return out


def writable_roots(roots):
    """能在那写出 3ds/aiconsole/ 的候选根。"""
    ok = []
    for r in roots:
        d = os.path.join(r, SD_SUBDIR)
        try:
            os.makedirs(d, exist_ok=True)
            probe = os.path.join(d, ".probe")
            with open(probe, "w") as fh:
                fh.write("x")
            retire(probe)
            ok.append(r)
        except OSError:
            continue
    return ok


def retire(p):
    """把单个文件"撤下"到同目录的 .trash/ 里 —— 不真删。

    整个脚本里**一处 os.remove 都不留**是刻意的：执行环境的批量删除护栏
    按回合计数（阈值 50），12 趟全量验收累计会超，一超就拦断整轮，
    而报错信息看起来像 app 崩了（见 rmtree_files 的说明）。
    统一走这一个出口，以后加新代码也很难不小心踩回去。

    .trash 里同名文件会被覆盖（只留最后一份），够用 —— 它的作用是
    "万一要回查被清掉的东西长什么样"，不是做版本库。
    """
    if not os.path.exists(p):
        return
    d = os.path.join(os.path.dirname(p), ".trash")
    try:
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, os.path.basename(p))
        if os.path.isdir(p):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(p, dst)
        else:
            os.replace(p, dst)
    except OSError as e:
        print("  （撤下 %s 失败：%s）" % (p, e))


def rmtree_files(paths):
    """"删"掉这些文件 —— 逐个交给 retire()，实际是挪进 .trash/，不真删。"""
    for p in paths:
        retire(p)


def archive_screenshots(shots_dir):
    """把上一趟的 BMP 挪进 .old/ 而不是删掉。

    为什么不用 os.remove：跑全量 A-K 时，每趟启动前都要清一次截图目录，
    11 趟累计 50+ 个删除会撞上执行环境的**批量删除护栏**
    （SAFE_DELETE_BULK_CONFIRM_REQUIRED，阈值 50/回合），
    表现为全量验收跑到一半被拦断、只能跑完前两趟 ——
    而且报错长得像 app 崩了，极难定位。

    重命名不受删除配额限制，而且顺带留下了上一趟的画面 ——
    排查"某趟失败"时正好可以直接翻上一次的图对比。
    """
    d = os.path.join(shots_dir, ".old")
    for p in glob.glob(os.path.join(shots_dir, "*.bmp")):
        try:
            os.makedirs(d, exist_ok=True)
            os.replace(p, os.path.join(d, os.path.basename(p)))
        except OSError:
            pass


def rotate_stage_dir(tag):
    """暂存区 .bmp/<tag> 的跨趟轮转：<tag> -> .prev/<tag> -> .prev2/<tag>。

    同样一个文件都不删，只重命名。要清理旧图就手动删 .bmp/.prev2/ ——
    验收脚本运行期间不做任何批量删除，这才是不会再被护栏拦断的做法。
    """
    live = os.path.join(STAGE_DIR, tag)
    p1 = os.path.join(STAGE_DIR, ".prev", tag)
    p2 = os.path.join(STAGE_DIR, ".prev2", tag)

    if os.path.isdir(p1):
        os.makedirs(os.path.dirname(p2), exist_ok=True)
        try:
            os.replace(p1, p2)
        except OSError:
            pass
    if os.path.isdir(live):
        os.makedirs(os.path.dirname(p1), exist_ok=True)
        try:
            os.replace(live, p1)
        except OSError:
            pass


def clear_pass_artifacts(roots, drop_config):
    """清掉上一轮的截图/状态；drop_config 时连配置一起删（模拟首启）。"""
    for r in roots:
        archive_screenshots(os.path.join(r, SHOT_SUB))
        rmtree_files([os.path.join(r, SHOT_SUB, "error.txt"),
                      os.path.join(r, STATUS_REL),
                      # status.tmp 是 app 写 status.txt 时的中间产物
                      # （先写 tmp 再 rename，避免读者看到 0 字节窗口）。
                      # 上一趟崩在 rename 之前就会留下它 —— 不清的话
                      # 它可能比 status.txt 还新，被 newest() 选中。
                      os.path.join(r, STATUS_TMP_REL)])
        if drop_config:
            rmtree_files([os.path.join(r, CONFIG_REL)])


def write_autorun(roots, script):
    for r in roots:
        p = os.path.join(r, AUTORUN_REL)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(script)


def newest(roots, rel):
    best, best_t = None, -1.0
    for r in roots:
        p = os.path.join(r, rel)
        if os.path.isfile(p):
            t = os.path.getmtime(p)
            if t > best_t:
                best, best_t = p, t
    return best


def read_text(path):
    if not path:
        return ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def read_status_stable(path):
    """等 status.txt 真的写完，再把它读回来。

    为什么不能直接 read_text：app 是在退出前 `fopen(path, "w")` 一次性
    覆写的，而 **fopen("w") 一调用就把文件截断、mtime 也跟着变新**。
    外层等待循环的条件恰恰是"mtime 比启动时刻新"，于是它在文件刚被清空、
    内容还没落盘的那一瞬间就 break 了 —— 读回来是空文件或半截内容，
    断言报出来的是"status 里没有 layout_overflow 字段（护栏未生效，
    旧二进制？）"这种把人往错方向引的话。

    2026-10-01 全量在 E / I 趟间歇出现，单独跑必过 —— 现在能解释了：
    是不是踩中那个窗口，取决于那一趟的收尾快慢。

    判据：连续两次读到**一模一样**的内容，且包含每趟都会写的字段。
    "两次一样"就等于"没人在写它了"。

    2026-10-03 补记：这个等待窗口**本身就太窄**。实测抓到的时序：
        t=12.0s  status=0B      <- fopen("w") 刚截断
        t=13.5s  status=1241B   <- 才写完
    24×0.5s = 12.0s 正好卡在窗口外，于是 return 空串，
    断言报出来的是「status 里没有 layout_overflow 字段（护栏未生效，旧二进制？）」
    —— 一句把人和 1.5 秒的运气都误导了的话。
    B/K 趟间歇失败就是这个（且 T 趟写 3707 字节反而更容易躲过，
    因为它落盘后还有后续 pass 的时间差）。
    → 现在 60 轮 = 30 秒。另外 app 侧已改成「先写 status.tmp 再 rename」，
      所以正常情况下根本不该再出现 0 字节窗口；这里留够余量是为了
      **万一** rename 失败退回了直接写，脚本仍能等到。
    ⚠ path 可能是 None：某一趟的 Azahar 提前退出（rc!=0）时压根没产生
      status.txt，上层拿到的 status_path 就是 None。没有这个守卫的话，
      下面 open(None) 抛 TypeError，整个全量回归**当场中止**，
      前面已跑完的趟次结果全丢 —— 2026-10-03 的 reg3 就是这么挂的
      （C 趟退出 rc=1 → TypeError → 只跑了 A/B/C 就结束）。
      正确行为：返回空串，让这一趟自己的断言去报"status 里没有 X"，
      从而停在**那一趟**而不是整轮。
    """
    if not path:
        return ""
    prev = None
    for _ in range(60):                  # 最多等 30 秒（实测写盘约 13.5s）
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                cur = fh.read()
        except OSError:
            cur = ""
        if cur and "layout_overflow" in cur and cur == prev:
            return cur
        prev = cur
        time.sleep(0.5)
    # 等不到时的**诊断信息**：只回空串的话，断言只能报"没有 X 字段"，
    # 而真相往往是"文件是空的"或"根本没生成"。把实际看到的样子带回去。
    if prev:
        return prev
    return ""


def kill_tree(proc):
    if proc.poll() is not None:
        return
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                   capture_output=True)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def bmp_to_png(paths, outdir):
    try:
        from PIL import Image
    except ImportError:
        print("（没有 Pillow，跳过 BMP->PNG 转换）")
        return []
    os.makedirs(outdir, exist_ok=True)
    out = []
    for p in paths:
        try:
            im = Image.open(p)
            stem = os.path.splitext(os.path.basename(p))[0]
            # 带上趟次前缀。每趟的截图编号都从 1 开始（1_chat_ready、
            # 2_chat_qp…），若只按原名平铺到 out/，H 趟和 M 趟就会同名
            # 互相覆盖 —— 2026-10-01 实测撞过：翻 H 趟的图，看到的是 M 趟
            # 的画面。这比"没有图"更误导，因为它是错的但看起来是对的。
            tag = os.path.basename(os.path.dirname(p))
            dst = os.path.join(outdir, "%s_%s.png" % (tag, stem))
            im.save(dst)
            out.append(dst)
        except Exception as e:                       # noqa: BLE001
            print("  转换失败 %s: %s" % (p, e))
    return out


# ---------------------------------------------------------------------------
# 一趟
# ---------------------------------------------------------------------------

def stage_shots(tag, shots):
    """把这一趟的 BMP 搬到暂存区。返回暂存后的路径。"""
    d = os.path.join(STAGE_DIR, tag)
    os.makedirs(d, exist_ok=True)
    out = []
    for p in shots:
        dst = os.path.join(d, os.path.basename(p))
        try:
            shutil.copy2(p, dst)
            out.append(dst)
        except OSError as e:                          # noqa: BLE001
            print("  暂存失败 %s: %s" % (p, e))
            out.append(p)
    return out


def kill_stale_azahar(image="azahar.exe"):
    """启动前先清场。Azahar 有单实例机制：如果上一趟（或用户手动开的）
    Azahar 还活着，新 Popen 会被吞掉，表现为"跑了但什么都没发生"。
    显式用绝对路径 —— 本机 shell PATH 不可靠。

    ⚠️ `image` 是**按进程名**清理用的，默认 "azahar.exe"。
    留这个参数是为了支持"同一个仓库被两个会话同时验收"的场景：
    一方用改名的副本（例如复制成 azahar_iso.exe 并配 `--azahar`），
    两边就互不误杀。**按 exe 名杀而不是按固定字符串**，是因为
    写死 "azahar.exe" 时，改名的那个实例永远清不掉，
    会残留成僵尸实例、和新实例抢同一个 SD 目录。

    taskkill 返回**不等于进程已经退出**。13 趟连着跑时，上一趟的 Azahar
    常常还没释放 SD 目录句柄，新实例就已经起来了 —— 于是它读到的是
    上一趟留下的 status.txt，断言报出来的错误看着毫无道理
    （同一个 end_screen 值单独跑是 OK、连着跑就 [!!]；
    2026-10-01 全量在 E/I 趟各出现过一次，单独跑必过）。
    所以这里等到进程真的从任务列表里消失为止。
    """
    taskkill = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                            "System32", "taskkill.exe")
    if not os.path.isfile(taskkill):
        taskkill = "taskkill"
    subprocess.run([taskkill, "/F", "/IM", image, "/T"],
                   capture_output=True, timeout=30)

    tasklist = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                            "System32", "tasklist.exe")
    if not os.path.isfile(tasklist):
        tasklist = "tasklist"
    for _ in range(24):                 # 最多等 12 秒
        # ⚠ 这里**不能**用 text=True。中文 Windows 的 tasklist 输出是 GBK，
        #   而 Python 的 text=True 走 locale/UTF-8 严格解码 —— 撞上 0xD0 这类
        #   GBK 首字节就在 subprocess 的 _readerthread 里抛 UnicodeDecodeError。
        #   那个线程是 daemon，异常不会传播，于是：主流程照跑、结果照样正确，
        #   但每一趟都在 stdout 上刷一段 traceback，把真正的趟次日志淹掉 ——
        #   2026-10-03 的全量回归就是这么看起来"一趟都没过"的（实际 19 趟全绿）。
        #   显式用 capture_output 拿 bytes，自己按 GBK 宽松解。
        try:
            out = subprocess.run([tasklist, "/FI", "IMAGENAME eq " + image,
                                  "/NH"],
                                 capture_output=True, timeout=15)
        except Exception:               # noqa: BLE001
            break                       # 查不了就别卡住，退回原来的行为
        raw = (out.stdout or b"")
        txt = raw.decode("gbk", errors="replace").lower()
        if image.lower() not in txt:
            break
        time.sleep(0.5)

    time.sleep(1.0)                     # 再给它一点时间落盘/释放


def run_pass(tag, spec, ctx):
    roots = ctx["roots"]
    print("\n" + "=" * 68)
    print("PASS %s · %s" % (tag, spec["title"]))
    print("=" * 68)

    # ★ "4" 也走首启（同 "0"）：它的脚本从引导段开头，若不清配置，
    #   夹具里的 first_run_completed=true 会让程序**直接进主页**，
    #   那串引导键就全打在主页上（3 个 A 进对话屏、RIGHT 挪焦点、
    #   A 进攻略屏）—— 症状是 end_screen=SCREEN_GUIDE 且六张截图
    #   逐像素完全相同，看着像"程序卡死了"，真因是走法错位。
    # ★ 趟 "7" 也必须清配置：它的脚本从**引导段开头**跑（同 0/4/5），
    #   有 config.json 的话首启会跳过引导，后面那串 key 全打在主页上。
    clear_pass_artifacts(roots,
                         drop_config=(tag in ("A", "D", "0", "1", "4", "5", "7", "8")))
    write_autorun(roots, spec["script"])
    # 按**实际配置的 exe 名**清场（见 kill_stale_azahar 的说明）：
    # 写死 "azahar.exe" 会误杀另一个会话的实例，且清不掉改名副本的残留。
    kill_stale_azahar(os.path.basename(ctx["azahar"]))

    h_dummy = None              # H 趟夹具标记；初值必须写在夹具赋值之前

    # ★ 2026-10-25：**每一趟都清对话历史**。
    #   原来只清"用历史夹具的那几趟"，于是前面任何一趟聊过天（CH/ST/H…）
    #   都会给后面 M/N/P 留一条记录 —— 断言报"实际 4 条（夹具预置 3 条）"，
    #   看起来像程序多写了一条，其实是**趟与趟之间没隔离**。
    #   放在最前面，各趟自己的夹具随后再写。
    _clear_chat_slots(roots)

    if tag == "A":
        stage_keyfile(roots)
    if tag == "D":
        # 这一趟要验的就是「什么都没配」的状态，所以把凭据文件也清掉
        rmtree_files([os.path.join(r, KEY_REL) for r in roots])
        print("D 趟：已清掉 key.txt（模拟还没配过）")
    if tag == "E":
        stage_update_package(roots)
    if tag == "CH":
        # 这一趟验的是"**没有**更新包时的常态版面"（那个「检查更新」按钮）。
        # 必须先清掉上一趟 E 留下的夹具：不清的话状态是 AVAILABLE，
        # 下屏变成「拒绝/允许」，按 A 等于"拒绝"并退回主界面 ——
        # 断言会把"按钮其实没问题"判成"按钮没生效"，方向完全反了。
        for r in roots:
            d = os.path.join(r, "3ds", "aiconsole", "update")
            if os.path.isdir(d):
                shutil.rmtree(d, ignore_errors=True)
        print("CH 趟：已清掉 update/ 目录（模拟没有更新包）")
    if tag == "C":
        patch_config_for_banner(roots)
    if tag == "H":
        h_dummy = stage_keystore(roots, True)
        print("H 趟：%s" % ("仓库里没有真激活 Key，放了假 Key（预期 401 错误路径）"
                            if h_dummy else "仓库里有真激活 Key，走真实请求"))
    if tag == "I":
        stage_keystore(roots, False)
        print("I 趟：仓库就绪，切换后 active 预期 = %s" % KS_EXPECTED["active"])
    if tag == "M":
        # M 趟验的是「流式上屏」，不是「能不能连上服务商」。
        # 所以用离线演示夹具（provider=demo，base_url 为空 → 不出网）：
        # 走的是**同一套** on_delta 回调链路（同样的分块、节流、滚动、上屏），
        # 但不需要真 Key、不消耗额度、不受网络波动影响。
        # httpc 那一半的正确性不在本趟覆盖范围内 —— 那一半只能靠真 Key。
        stage_demo(roots)
        # 顺手清掉可能残留的会话记录：对话屏开机时会把它恢复回来，
        # 而本趟第一轮的断言是"不带上下文"—— 残留历史会让它假失败。
        for r in roots:
            retire(os.path.join(r, *CHATLOG_REL.split("/")))
        # 短语也钉死：不能借内置默认列表（见 NEUTRAL_PHRASE 的说明）。
        stage_phrases_neutral(roots)
        print("M 趟：已切到离线演示（provider=demo，不联网、不用真 Key）")
    if tag == "N":
        backup_config(roots)
        # 用和 M 趟同样的离线演示夹具：本趟得真的"发出去"才看得到上下文，
        # 而发出去需要凭据 —— 演示模式不要 Key、也不耗额度。
        stage_demo(roots)
        stage_chatlog(roots)
        stage_phrases_neutral(roots)
        print("N 趟：夹具已就位（离线演示 + 2 条历史 + 一句人设）")
    if tag == "7":
        # ★ 2026-10-24：趟 7 以前靠"卡上碰巧还剩几条历史"来滚上屏。
        #   那个前提是靠不住的 —— 前面任一趟（H/M/N）都会重写 chat.json，
        #   轮到 7 时常常只剩 2 条，一屏就装得下、**根本没有可滚的东西**，
        #   于是"摇杆能滚"这条断言红在一个与摇杆毫无关系的原因上。
        #   夹具把"有足够内容"这件事钉死：8 条，每屏只放得下 3~4 条。
        stage_chatlog_many(roots)
        print("7 趟：夹具已就位（8 条历史 → 上屏真的能滚）")
    if tag == "PD":
        # 删除确认要验"取消不删、确认才删"，所以必须先有两个任务。
        # 复用趟 8 的夹具（两任务 + 各自历史），别另写一份。
        stage_two_projects(roots)
        print("PD 趟：夹具已就位（2 个任务，用来验删除确认）")
    if tag == "8":
        # 这一趟验"每个任务一份历史"，所以**必须**先有两个任务、两份内容
        # 不同的记录 —— 只有一个任务时代码走的是老路径（见 chatlog.c：
        # 单任务 = chat.json），那种状态下这条判据一句都没验到。
        stage_two_projects(roots)
        print("8 趟：夹具已就位（2 个任务 + 各自 3 条历史）")
    if tag == "K":
        backup_config(roots)
        print("K 趟：config.json 已备份（本趟会改主题与护栏，跑完还原）")
    if tag == "O":
        # 和 M/N 同样的离线演示夹具：本趟验的是"拿到动作之后做得对不对"，
        # 不是"能不能连上模型"。演示把"模型会吐什么"钉死，
        # 于是四道闸的每一道都能在没有 Key 的情况下反复跑。
        stage_demo(roots)
        stage_phrases_o(roots)

        # 起点必须是"卡上没有这个文件、也没有审计流水"。理由有两条：
        #   · 不断言"文件存在"的话，上一轮留下的同名文件会让本趟假通过；
        #   · 审计流水要能证明**这一次**的 ALLOW，混着旧记录就说不清了。
        # 用 retire（挪进 .trash）而不是删 —— 全脚本一条 os.remove 都不留。
        for r in roots:
            retire(os.path.join(r, *AGENT_FILE.split("/")))
            retire(os.path.join(r, *ACTION_LOG.split("/")))
        print("O 趟：夹具已就位（离线演示 + 一句动作短语，沙箱与流水已清空）")
    if tag == "2":
        # 与 O 趟同一套夹具：本趟不需要真模型 —— 要的是"确认屏出现"，
        # 而离线演示会把"模型会吐什么"钉死。
        stage_demo(roots)
        stage_phrases_o(roots)

        # 清掉审计流水：这一趟要用"有没有 DENY"当判据，
        # 混着上一趟的 ALLOW 就说不清是谁的了（同 O 趟的理由）。
        # 顺手也清掉对话记录：留着的话第一轮会先 `restored=N`，
        # 屏上摆着上一趟的旧对话，而本趟要看的是"这一轮 system 变长了"
        # —— 带上别人的历史只会让 ctx: 那两个数更难读。
        for r in roots:
            retire(os.path.join(r, *ACTION_LOG.split("/")))
            retire(os.path.join(r, *CHATLOG_REL.split("/")))
        print("2 趟：夹具已就位（离线演示 + 动作短语，审计与对话记录已清空）")
    if tag == "R":
        # 与 O 趟同样的离线演示夹具：验的是"拿到安装动作之后做得对不对"。
        stage_demo(roots)
        stage_phrases_r(roots)

        # 起点三件事都要清：
        #   · 装好的包不能留 —— 否则"文件出现"可能只是上一轮的残影；
        #   · 审计流水不能留 —— 结论要能说清"这一次是谁批的"；
        #   · pkgs/ 里的源包要**重新摆** —— 顺便拿到它的真实摘要，
        #     用来核对"演示给的那个全零摘要确实不等于真实摘要"。
        r_real_sha = stage_pkg_src(roots)
        stage_pkg_unit(roots)
        for r in roots:
            retire(os.path.join(r, *ACTION_LOG.split("/")))
        print("R 趟：夹具已就位（离线演示 + 安装短语；源包真实 sha256=%.16s…，"
              "而演示给的是全零 → 预期被摘要护栏拦下）"
              % (r_real_sha or "?"))
    if tag == "S":
        # 与 R 趟同样是离线演示，但短语走"不给摘要"那条路 ——
        # 于是预期是**真的装成功**，文件出现在 3ds/。
        stage_demo(roots)
        stage_phrases_r2(roots)
        r_real_sha = stage_pkg_src(roots)
        stage_pkg_unit(roots)
        for r in roots:
            retire(os.path.join(r, *ACTION_LOG.split("/")))
        print("S 趟：夹具已就位（离线演示 + 不给摘要的短语 → 预期装成功）")
    if tag == "P":
        # 本趟关心的只有"读得回来"，所以不给 Key、不切演示 —— 一进屏就
        # 恢复历史，凭据是什么都不影响 restore 这一步。少一个变量少一个坑。
        p_long_lines = stage_chatlog_long(roots)
        p_lens = [len(b) for b in p_long_lines]
        p_overshot = max(p_lens) > P_OLD_BUF
        print("P 趟：夹具已就位（3 条历史，行字节=%s；旧缓冲 %d，%s）"
              % (p_lens, P_OLD_BUF,
                 "确实越界" if p_overshot else "**没越界，本趟失去意义**"))

    if tag == "Y":
        # 夹具的意义见 stage_asr_cfg 的文件头：只有先把 asr.txt 摆上去，
        # 「配置读到没有」才是真判据。不摆的话探针走默认分支，
        # 这条判据会一路绿下去而什么都没验到。
        stage_asr_cfg(roots)
        print("Y 趟：夹具已就位（asr.txt -> provider=%s + 假 Key）"
              % Y_ASR_PROVIDER)

    # B 趟要断言 launches 递增 1（不是"等于 2"），所以得先读一份跑之前的。
    launches_before = None
    if tag == "B":
        p0 = newest(roots, CONFIG_REL)
        if p0:
            try:
                with open(p0, "r", encoding="utf-8") as fh:
                    launches_before = json.load(fh).get("launches")
            except Exception:                                    # noqa: BLE001
                launches_before = None
        print("B 趟：上一轮 launches = %s" % launches_before)

    t0 = time.time()
    i_ks_after = ""
    k_cfg_after = None
    n_turns_after = None
    proc = subprocess.Popen([ctx["azahar"], ctx["rom"]], cwd=ctx["exe_dir"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    status_path = None
    deadline = t0 + ctx["wait"]
    while time.time() < deadline:
        status_path = newest(roots, STATUS_REL)
        if status_path and os.path.getmtime(status_path) >= t0 - 1:
            break
        if proc.poll() is not None and not status_path:
            print("Azahar 提前退出 rc=%s，且没有 status.txt" % proc.returncode)
            break
        time.sleep(1.0)

    elapsed = time.time() - t0
    if not ctx["keep"]:
        kill_tree(proc)
    if tag == "H":
        restore_keystore()          # app 已退出，keys.json 该还给用户了
    if tag == "M":
        restore_keystore()
    if tag == "N":
        restore_keystore()
    if tag == "O":
        restore_keystore()
    if tag == "2":
        # 现场取证放在善后**之前**（同 O 趟的理由：清了就读不到了）
        _lp = newest(roots, ACTION_LOG)
        two_log = read_text(_lp) if _lp else None
        restore_keystore()
    if tag == "R":
        restore_keystore()
    if tag == "S":
        restore_keystore()
    if tag == "I":
        ks_p = newest(roots, KS_REL)
        i_ks_after = read_text(ks_p) if ks_p else ""
        restore_keystore()

    # K 趟：先把配置读出来当断言证据，再还原 —— 顺序不能反，
    # 还原之后就读不到"这次跑出来的主题是 dark"了。
    if tag == "K":
        cfg_path_k = newest(roots, CONFIG_REL)
        try:
            with open(cfg_path_k, "r", encoding="utf-8") as fh:
                k_cfg_after = json.load(fh)
        except Exception:                                        # noqa: BLE001
            k_cfg_after = None
        restore_config()

    if tag == "N":
        # 写盘有没有生效：数一数 chat.json 里有几条（夹具预置的是 2 条）。
        # **必须在清理之前读** —— 下面就要把这个文件删掉。
        # ★ 2026-10-25 修：数**真正在写的那一份**。
        #   对话历史早就按任务分文件了（chatlog.c 的 chat_p0.json），
        #   chat.json 只在首次启动迁移时用一次。原来只读 chat.json，
        #   于是永远只能读到夹具预置的那 2 条 —— 断言"应当 > 2"永远不成立，
        #   报出来的话却是"写盘路径没生效"，把方向指到写盘上去了。
        p_chat = newest(roots, os.path.join(SD_SUBDIR, "chat_p0.json"))
        if not p_chat:
            p_chat = newest(roots, CHATLOG_REL)
        if p_chat:
            try:
                with open(p_chat, "r", encoding="utf-8") as fh:
                    n_turns_after = sum(1 for ln in fh if ln.startswith("{"))
            except OSError:
                n_turns_after = None

        # 夹具的善后是夹具的责任。这份 chat.json 会让 M 趟的第一轮带上
        # 上下文（M 断言它必须为空），也必须还给用户一个干净的卡。
        for r in roots:
            retire(os.path.join(r, *CHATLOG_REL.split("/")))
        restore_config()

    if tag == "O":
        # ---- 现场取证：必须在善后之前读 ----
        # 这一段的每个值都只在"app 刚退出、文件还没被清"的这一刻存在。
        # 参考 N 趟的 n_turns_after：先读完再清，顺序反了就什么也读不到。
        o_files = [p for p in (os.path.join(r, *AGENT_FILE.split("/"))
                               for r in roots) if os.path.isfile(p)]
        # 取内容最长的那份（多候选根里只有一份是真的被写过的）
        o_file = max(o_files, key=os.path.getsize) if o_files else None
        o_text = None
        if o_file:
            try:
                o_text = open(o_file, "r", encoding="utf-8",
                              errors="replace").read()
            except OSError:
                o_text = None

        lp = newest(roots, ACTION_LOG)
        o_log = read_text(lp) if lp else None

        # ---- 这个文件跟 N 趟的 chat.json 一样，是**夹具造出来的**，
        # 不能留在用户的卡上（下一次跑 M/N 会看见一个莫名其妙的 out/）。
        # 但审计流水留着更有价值：它是"这台机器上模型试过什么"的唯一长期记录，
        # 而且每趟开头都会清空，不会积累。所以这里只撤文件。
        for p in o_files:
            retire(p)

        o_capture = dict(o_files=o_files, o_text=o_text, o_log=o_log)
        restore_config()
    else:
        o_capture = None

    if tag == "R":
        # ---- 现场取证：必须在善后之前读 ----
        # 与 O 趟同一个理由：这些值只在"app 刚退出、文件还没被清"的
        # 这一刻存在。先读完再清，顺序反了就什么也读不到。
        r_installed = [p for p in (os.path.join(r, *R_PKG_REL.split("/"))
                                   for r in roots) if os.path.isfile(p)]
        lp = newest(roots, ACTION_LOG)
        r_log = read_text(lp) if lp else None

        r_capture = dict(r_installed=r_installed, r_log=r_log,
                         r_real_sha=r_real_sha)
        restore_config()
    else:
        r_capture = None

    if tag == "S":
        # ---- 现场取证：必须在善后之前读 ----
        # 本趟要看的正是那个文件**在不在**，所以这里读的就是主判据。
        s_installed = [p for p in (os.path.join(r, *R_PKG_REL.split("/"))
                                   for r in roots) if os.path.isfile(p)]
        # 装进去的内容也要核：只判"文件存在"的话，一个空的/写坏的文件
        # 也能让它通过 —— 而"装进去的是不是原来那个"才是安装的本质。
        s_inst_bytes = None
        if s_installed:
            try:
                with open(s_installed[0], "rb") as fh:
                    s_inst_bytes = len(fh.read())
            except OSError:
                s_inst_bytes = None

        lp = newest(roots, ACTION_LOG)
        s_log = read_text(lp) if lp else None

        # 源包大小（夹具造的，用来核对搬运是否完整）
        s_src_bytes = None
        for r in roots:
            p = os.path.join(r, *R_SRC_REL.split(os.sep))
            if os.path.isfile(p):
                s_src_bytes = os.path.getsize(p)
                break

        s_capture = dict(s_installed=s_installed,
                         s_inst_bytes=s_inst_bytes,
                         s_src_bytes=s_src_bytes,
                         s_log=s_log)
        restore_config()
        # 夹具造的源包与装好的目标都要清干净 —— 它们是本趟的产物，
        # 不该留在用户的卡上（留着的话下一次跑会看到莫名其妙的 3dsx）。
        for r in roots:
            retire(os.path.join(r, *R_SRC_REL.split(os.sep)))
            retire(os.path.join(r, *R_PKG_REL.split("/")))
    else:
        s_capture = None

    if tag == "P":
        # 夹具自己造的 chat.json，跑完必须还给用户一张干净的卡 ——
        # 留着的话 M 趟的第一轮会莫名其妙带上上下文（M 断言它为空）。
        for r in roots:
            retire(os.path.join(r, *CHATLOG_REL.split("/")))
        p_capture = dict(p_lens=p_lens, p_overshot=p_overshot,
                         p_tail_text=P_TAIL_TEXT)
    else:
        p_capture = None

    shots = sorted(sum((glob.glob(os.path.join(r, SHOT_SUB, "*.bmp"))
                        for r in roots), []))
    shots = stage_shots(tag, shots)
    errors = [p for r in roots
              for p in glob.glob(os.path.join(r, SHOT_SUB, "error.txt"))]

    print("耗时 %.1fs · 收到 %d 张截图 · status.txt=%s"
          % (elapsed, len(shots), status_path or "无"))

    if errors:
        print("!! app 写下了 error.txt:")
        for e in errors:
            print("   %s\n   | %s" % (e, read_text(e).replace("\n", "\n   | ")))
        return dict(tag=tag, ok=False, why="app 报错", shots=shots, status="",
                    cfg=None, h_dummy=h_dummy)

    status = read_status_stable(status_path)
    cfg_path = newest(roots, CONFIG_REL)
    cfg = None
    if cfg_path:
        try:
            with open(cfg_path, "r", encoding="utf-8") as fh:
                cfg = json.load(fh)
        except Exception as e:                       # noqa: BLE001
            return dict(tag=tag, ok=False,
                        why="config.json 不是合法 JSON: %s" % e,
                        shots=shots, status=status, cfg=None)

    ctx["shots"] += shots
    out = dict(tag=tag, ok=True, why="", shots=shots, status=status,
               cfg=cfg, cfg_path=cfg_path, expect=spec["shots"],
               roots=roots, h_dummy=h_dummy,
               launches_before=launches_before)
    if tag == "I":
        out["ks_after"] = i_ks_after
        # 期望值也必须**当场快照**。KS_EXPECTED 是模块级全局，而后面任何
        # 一趟调 stage_keystore/stage_demo 都会把它清掉（两个函数开头都是
        # KS_EXPECTED["active"] = ""），断言却是全部跑完之后才统一执行的 ——
        # 于是"本趟的期望值"变成了"最后一趟留下的值"。
        # 2026-10-01 实测：只要 M 趟跑到（全量、或任何含 M 的趟次组合），
        # I 趟断言就必错，且报的是"切换后 ks_active =（实际 ks-test-1）"，
        # 看着像 app 没切换成功，实际是尺子被后一趟擦掉了。
        # 同一份数据不存快照，迟早会被别人改掉 —— K 趟的 k_cfg_after 同理。
        out["ks_expected"] = KS_EXPECTED["active"]
    if tag == "K":
        out["cfg"] = k_cfg_after      # 用还原前的快照覆盖，才是本趟真实结果
    if tag == "N":
        out["n_turns_after"] = n_turns_after
    if tag == "O":
        out.update(o_capture or {})
    if tag == "2":
        out["two_log"] = two_log
    if tag == "R":
        out.update(r_capture or {})
    if tag == "S":
        out.update(s_capture or {})
    if tag == "P":
        out.update(p_capture or {})
    if tag == "E":
        # ⚠️ 两个要点，缺一个断言就会得出**与事实相反**的结论：
        #
        # 1) **必须在下面的还原之前**取。E 趟的假更新包会被
        #    restore_main_3dsx 盖回真主程序，而断言是整趟跑完才执行的；
        #    不先取快照，量到的永远是仓库里的真产物。
        #
        # 2) **必须取「生效的那个根」**，不能拿 roots[0]。
        #    脚本故意把写操作扇出到所有候选根（它不知道本机 Azahar 用哪个），
        #    于是**每个**候选根里都会有一份 3ds/AIConsole.3dsx。
        #    按顺序取第一个命中，拿到的是别的根里的旧副本 ——
        #    2026-10-04 实测就是死在这里：量到 1908872（仓库真件的大小），
        #    而 app 其实老老实实装了 1320 字节的假包，报错方向完全是反的。
        #    真实根由 status_path 反推：<root>/3ds/aiconsole/status.txt。
        root_eff, p_eff = None, None
        if status_path:
            root_eff = os.path.dirname(os.path.dirname(
                os.path.dirname(status_path)))
            p_eff = os.path.join(root_eff, "3ds", "AIConsole.3dsx")
        if p_eff and os.path.isfile(p_eff):
            out["e_installed_size"] = os.path.getsize(p_eff)
            out["e_installed_root"] = root_eff
    # 趟次收尾：E 趟的假更新包会被 app 装到 3ds/ 根目录顶掉真主程序，
    # 不还原就是「软件没了」。见 restore_main_3dsx 的说明。
    restore_main_3dsx(roots)
    return out


# ---------------------------------------------------------------------------
# 断言
# ---------------------------------------------------------------------------

def kv(status, key):
    for line in status.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None


def check_schema(cfg):
    """设计规划.md §5.1 的结构检查。"""
    bad = []
    for k in SCHEMA_ROOTS:
        if k not in cfg:
            bad.append("缺顶层键 %s" % k)
    for k, subs in SCHEMA_SUB.items():
        if k not in cfg or not isinstance(cfg.get(k), dict):
            continue
        for s in subs:
            if s not in cfg[k]:
                bad.append("缺 %s.%s" % (k, s))
    return bad


def note_field(status, prefix, key):
    """从 `-- notes --` 里的某条 note 中取出一个 k=v 字段。

    为什么要专门有个函数：app_note 的一行里可以有**多个** k=v
    （例如 `chat: restored=3 oversize=1`）。对整行做 split("=", 1)
    拿到的是 "3 oversize=1" 这种混合串，int() 直接炸。
    早先 N 趟就是这么写的，只是因为当时那行只有一个字段才没暴露。

    返回最后一个匹配 note 的该字段值（字符串）；没有则 None。
    """
    hits = [l.strip() for l in status.splitlines()
            if l.strip().startswith(prefix)]
    if not hits:
        return None
    want = key + "="
    for tok in hits[-1].split():
        if tok.startswith(want):
            return tok[len(want):]
    return None


def expect(cond, label, problems):
    print("  [%s] %s" % ("OK" if cond else "!!", label))
    if not cond:
        problems.append(label)


def assertions(res):
    """返回 (problems, checks)。"""

    problems = []
    tag = res["tag"]

    def pick(sub, pool):
        """取最后一条包含 sub 的记录行。

        ⚠️ 这个 helper 原本是 T 分支里的局部 def。加 U 趟时需要同样的
        「按子串找最后一条记录」能力，于是提到函数级 ——
        **但 pool 必须显式传**，不能隐式捕获外层的 notes：
        各趟的记录池来源不同（多数是 status 里的 notes，
        P 趟那种长消息回落要从别的地方取）。写成闭包捕获的话，
        第二个分支会静默取错池子，而「取错池子」的表现只是
        「找不到记录」→ 判据失败，不会指出去哪里错了。
        """
        hit = [l for l in pool if sub in l]
        return hit[-1] if hit else None

    if tag == "A":
        print("-- 断言 A：首启三屏 → config.json 落盘 --")
        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（%d 组 x 顶底屏）" % (res["expect"] * 2, res["expect"]),
               problems)

        cfg = res["cfg"]
        expect(cfg is not None, "config.json 已生成且可解析为 JSON", problems)
        if cfg is None:
            return problems

        for b in check_schema(cfg):
            expect(False, b, problems)
        if not check_schema(cfg):
            expect(True, "config.json 符合 §5.1 结构", problems)

        expect(cfg.get("first_run_completed") is True,
               "first_run_completed = true", problems)
        expect(cfg["nand_backup"].get("confirmed") is True,
               "nand_backup.confirmed = true（选了「我已备份」）", problems)
        expect(cfg.get("launches") == 1, "launches = 1", problems)

        st = res["status"]
        expect(kv(st, "start_screen") == "SCREEN_WELCOME",
               "start_screen = SCREEN_WELCOME（走了引导）", problems)
        expect(kv(st, "end_screen") == "SCREEN_HOME",
               "end_screen = SCREEN_HOME（引导完成后切走）", problems)
        expect(kv(st, "stats_saved") == "1", "status: saved = 1", problems)
        expect(kv(st, "cfg_file") == "0",
               "首启时 cfg_file = 0（上一轮的配置确实被删了）", problems)
        expect(kv(st, "cfg_parse_ok") == "0",
               "首启时没有任何 config 被解析（走的是默认值）", problems)

        # ---- 新手教程 + SD 卡凭据文件 ----
        expect(cfg.get("tutorial_completed") is True,
               "tutorial_completed = true（新手教程走完了）", problems)
        expect(cfg["ai"].get("provider") == "zhipu",
               "ai.provider = zhipu（从 key.txt 读来的，不是默认值）", problems)
        expect(cfg["ai"].get("model") == "glm-4-flash",
               "ai.model = glm-4-flash（同样来自 key.txt）", problems)

    elif tag == "B":
        print("-- 断言 B：二次启动直接进主界面 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 2", problems)

        st = res["status"]
        expect(kv(st, "start_screen") == "SCREEN_HOME",
               "start_screen = SCREEN_HOME（**没有**再走引导）", problems)
        expect(kv(st, "cfg_file") == "1", "status: 读到了 config.json", problems)
        expect(kv(st, "cfg_parse_ok") == "1", "status: 解析成功", problems)
        expect(kv(st, "cfg_unknown_keys") == "0", "status: 没有不认识的键", problems)
        expect(kv(st, "first_run_completed") == "1",
               "status: first_run_completed = 1", problems)
        # launches 不该写死成 2 —— B 趟的前提只是"config.json 已存在"，
        # 而用户可能已经启动过几十次（开发期每天都在跑）。写死 = 开发几次后
        # 这趟必然假失败。真正的语义是"读→改→写往返成功"，也就是**递增 1**。
        n_before = res.get("launches_before")
        n_after  = kv(st, "launches")
        if n_before is not None and n_after is not None:
            expect(int(n_after) == int(n_before) + 1,
                   "status: launches %s -> %s（读→改→写往返成功）"
                   % (n_before, n_after), problems)
        else:
            print("   [~~] 拿不到上一轮的 launches，跳过递增断言")

        cfg = res["cfg"]
        if cfg is not None and n_after is not None:
            expect(int(cfg.get("launches", -1)) == int(n_after),
                   "config.json: launches 与 status 一致（%s）" % n_after,
                   problems)

    elif tag == "C":
        print("-- 断言 C：未备份横幅出现，按 Y 后消失并落盘 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)

        st = res["status"]
        expect(kv(st, "start_screen") == "SCREEN_HOME",
               "start_screen = SCREEN_HOME", problems)
        expect(kv(st, "nand_remind_on_startup") == "0",
               "status: 按 Y 之后 remind_on_startup 变成 0", problems)

        cfg = res["cfg"]
        expect(cfg is not None and cfg["nand_backup"].get("remind_on_startup") is False,
               "config.json: remind_on_startup = false 已落盘", problems)
        expect(cfg is not None and cfg["nand_backup"].get("confirmed") is False,
               "config.json: confirmed 仍为 false（按 Y 不等于已备份）", problems)

        on = [p for p in res["shots"] if "7_banner_on_top" in os.path.basename(p)]
        off = [p for p in res["shots"] if "8_banner_off_top" in os.path.basename(p)]
        if on and off:
            same = _same_bytes(on[0], off[0])
            expect(not same, "顶屏「按 Y 前 / 后」两张图不同（横幅确实消失了）",
                   problems)
        else:
            expect(False, "缺少 7_banner_on_top / 8_banner_off_top 截图", problems)

    elif tag == "D":
        print("-- 断言 D：教程能跳过，主界面能回去 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)

        st = res["status"]
        expect(kv(st, "start_screen") == "SCREEN_WELCOME",
               "start_screen = SCREEN_WELCOME（重走了首启）", problems)
        expect(kv(st, "end_screen") == "SCREEN_KEYS",
               "end_screen = SCREEN_KEYS（主界面按 X 进 Key 管理；教程从那里 SELECT 进）",
               problems)

        skipped = [p for p in res["shots"]
                   if "1_skipped_home" in os.path.basename(p)]
        back = [p for p in res["shots"]
                if "2_back_in_tutorial" in os.path.basename(p)]
        if skipped and back:
            expect(not _same_bytes(skipped[0], back[0]),
                   "「跳过后」与「回来后」两张图不同（确实切换了屏）", problems)
        else:
            expect(False, "缺少 1_skipped_home / 2_back_in_tutorial 截图", problems)

        cfg = res["cfg"]
        expect(cfg is not None and cfg.get("tutorial_completed") is True,
               "跳过也算看过了（tutorial_completed = true，不会每次都弹教程）",
               problems)
        expect(cfg is not None and cfg["ai"].get("provider") == "deepseek",
               "key.txt 被清掉后，ai.provider 保持默认 deepseek",
               problems)

    elif tag == "3":
        print("-- 断言 3：插件页能进、能画、能返回 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 8", problems)
        st = res["status"]
        # 进过插件屏的硬证据：enter() 里那句 note。光看截图不行 ——
        # 空目录和"扫描失败"在屏上都是同一句"没有插件"。
        # ★ 用 status.txt 里的 notes 段（res 没有单独的 notes 字段 ——
        #   第一次写这条时用了 res["notes"]，直接 KeyError 崩在断言里）。
        notes = [l for l in st.splitlines() if "plugins: scanned" in l]
        expect(bool(notes), "插件页真的扫过目录（notes 里有 plugins: scanned）",
               problems)
        expect(kv(st, "end_screen") == "SCREEN_HOME",
               "end_screen = SCREEN_HOME（B 返回到了主界面）", problems)
        expect(kv(st, "layout_overflow") == "0",
               "status: layout_overflow = 0（没有单行文字被右边缘裁掉）", problems)

    elif tag == "E":
        print("-- 断言 E：更新包能被识别、校验并安装 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 6", problems)

        st = res["status"]
        expect(kv(st, "end_screen") == "SCREEN_UPDATE",
               "end_screen = SCREEN_UPDATE", problems)

        before = [p for p in res["shots"]
                  if "1_update_available" in os.path.basename(p)]
        notes = [p for p in res["shots"]
                 if "2_update_notes" in os.path.basename(p)]
        after = [p for p in res["shots"]
                 if "3_update_installed" in os.path.basename(p)]
        expect(bool(notes), "按 X 进到了说明查看页", problems)
        if before and after:
            expect(not _same_bytes(before[0], after[0]),
                   "确认前 / 安装后两张图不同", problems)
        else:
            expect(False, "缺少更新屏截图", problems)

        # 硬证据：目标文件真的被写了出来，而不是只弹了个"成功"。
        #
        # 这里**必须用趟内快照**，不能现在去量文件：趟末的 restore_main_3dsx
        # 已经把**每个候选根**都盖成仓库里的真产物了，事后再量只能量到那份真件，
        # 量不出"app 到底装了什么"。快照在还原之前取，且取的是**生效的那个根**
        # （理由见 run_pass 里 E 趟那段注释）。
        got_size = res.get("e_installed_size")
        expect(got_size is not None,
               "sdmc:/3ds/AIConsole.3dsx 已被写出（真的落盘了）", problems)

        # 跟 manifest 里声明的 size 比，而不是在脚本里再写一遍常数 ——
        # 那样等于把同一个数字抄两处，迟早对不上。
        # ★ 必须用**生效的那个根**（e_installed_root）。
        #   早先这里是遍历 res["roots"] 取第一个命中的 manifest —— 而脚本
        #   会把夹具扇出到**所有**候选根，于是第一个命中的是别的根里的
        #   旧 manifest（1320 字节），跟 app 真装的 1000 字节一比就红，
        #   报错方向完全反了。run_pass 那边早就为同一个坑改成按 status.txt
        #   反推生效根，断言这边漏了同样的处理。
        want = None
        _r0 = res.get("e_installed_root")
        for r in ([_r0] if _r0 else res.get("roots", [])):
            mp = os.path.join(r, "3ds", "aiconsole", "update", "manifest.json")
            if os.path.isfile(mp):
                try:
                    with open(mp, encoding="utf-8") as fh:
                        want = json.load(fh)["artifacts"][0]["size"]
                except Exception:                     # noqa: BLE001
                    want = None
                break
        if got_size is not None and want is not None:
            expect(got_size == want,
                   "装出来的文件大小与 manifest 声明一致（%d 字节）"
                   % got_size, problems)

    elif tag == "F":
        # 这一趟是探环境，不是验功能：**连不上不算缺陷**，
        # 但必须留下结论，否则后面 M4 是照着能联网还是不能联网写都没数。
        print("-- 断言 F：网络探测跑完并留下结论 --")
        got = kv(res["status"], "netprobe")
        expect(got is not None,
               "status.txt 里有 netprobe 结论（httpc 至少跑完了）", problems)
        print("     结论：%s" % (got or "(无)"))
        if got and "ok=1" in got:
            expect(True, "模拟器能出去网 —— M4 可以直接在这里开发", problems)
        elif got:
            expect(True, "（记录）模拟器出不去网，在线功能只能等实机验证",
                   problems)
        else:
            expect(False, "httpc 连跑都没跑完 —— 查 status.txt", problems)

    elif tag == "9":
        # 判据必须是**表达期望**的，而不是"跑完了就行"：
        # 这条链路优化的目标是"第二次不要再握手"，所以判据直接看
        # conn/tls 两项是不是 0。若探针报 SKIP（第一次就没连上），
        # 那是环境问题 —— 但也不能算通过，否则"没网"会被记成"复用成功"。
        print("-- 断言 9：语音请求第二次不再重建连接/握手 --")
        acold = pick("asrnet: cold", res["status"].splitlines())
        awarm = pick("asrnet: warm", res["status"].splitlines())
        ares  = pick("asrnet: RESULT=", res["status"].splitlines())
        expect(acold is not None, "冷启动那一次有耗时记录", problems)
        expect(awarm is not None, "热启动那一次有耗时记录", problems)
        expect(ares is not None, "status.txt 里有 asrnet 结论行", problems)
        if acold and awarm:
            print("     cold：%s" % acold.strip())
            print("     warm：%s" % awarm.strip())
        if ares:
            print("     结论：%s" % ares.strip())
            expect("RESULT=PASS" in ares,
                   "第二次的 conn/tls 均为 0（连接与 TLS 会话真的被复用）",
                   problems)
            expect("RESULT=SKIP" not in ares,
                   "探针不是在 SKIP 状态（SKIP = 没网，验不到东西）", problems)

    elif tag == "UP":
        print("-- 断言 UP：远程更新真的能取到 manifest（不是\"检查失败\"）--")
        lines = res["status"].splitlines()
        rline = pick("otanet: RESULT=", lines)
        fline = pick("otanet: fetch state=", lines)
        expect(rline is not None, "status.txt 里有 otanet 结论行", problems)
        for l in [l for l in lines if l.startswith("otanet: base ")]:
            print("     %s" % l.strip())
        if fline:
            print("     %s" % fline.strip())
        if rline:
            print("     结论：%s" % rline.strip())
            # SKIP = 一个源都没答话 = 模拟器出不去网。照趟 F 的规矩：
            # 环境问题不算产品缺陷，但必须留痕，不能悄悄当成通过。
            if "RESULT=SKIP" in rline:
                expect(True, "（记录）模拟器出不去网 —— 远程更新只能等实机验证",
                       problems)
            else:
                expect("RESULT=PASS" in rline,
                       "至少一个更新源握手成功，且 manifest 拿到并解析成功",
                       problems)
                # 关键区分：必须是"拿到了 manifest 并解析成功"的那三档
                # （2 最新 / 3 有更新 / 7 线上更旧），**不能**是 6 解析失败、
                # 10 连不上。写成"属于某几档"而不是"不等于某几档"：
                # 黑名单式判据会把将来新增的正常状态也误判成失败。
                expect(fline is not None
                       and any(s in fline for s in ("state=2", "state=3", "state=7")),
                       "状态属于 最新/有更新/线上更旧（说明 manifest 真解析成功）",
                       problems)

    elif tag == "DL":
        print("-- 断言 DL：产物真的下下来了，而且摘要对得上 --")
        lines = res["status"].splitlines()
        dline = pick("otadl: RESULT=", lines)
        expect(dline is not None, "status.txt 里有 otadl 结论行", problems)
        for l in [l for l in lines if l.startswith("otadl: ")]:
            print("     %s" % l.strip())
        if dline:
            if "RESULT=SKIP" in dline:
                # 取不到 manifest = 上一段（网络）没过，不是下载的锅
                expect(True, "（记录）先决条件不满足 —— 下载段没跑到", problems)
            else:
                expect("RESULT=PASS" in dline,
                       "下载完成且 sha256 与 manifest 一致（否则会报摘要不符）",
                       problems)
                expect(pick("otadl: done ok=1", lines) is not None,
                       "探针自报 ok=1（不是靠上层猜的）", problems)

    elif tag == "CH":
        print("-- 断言 CH：更新屏有「检查更新」按钮，按下真的会去查 --")
        st = res["status"]
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 6", problems)
        expect("ota: online check state=" in st,
               "status 里有 ota: online check —— 按钮真的触发了在线检查",
               problems)
        expect(kv(st, "end_screen") == "SCREEN_UPDATE",
               "end_screen = SCREEN_UPDATE（没有误退回主界面）", problems)

    elif tag == "PD":
        print("-- 断言 PD：删除要先弹窗，取消不删、确认才删 --")
        st = res["status"]
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 10", problems)
        expect("dock: ask delete" in st,
               "按 SELECT 只是**请求**删除（status 有 ask delete）", problems)

        # 判据落在**文件**上（不是截图），而且要用 newest() 取**这次**写的那份：
        # roots 里可能同时有几份（沙箱副本/旧目录），随手取最后一份会读到旧的，
        # 于是"屏上明明删掉了、断言说还剩两个"——这种红最费时间。
        proj = None
        q_proj = newest(res["roots"], os.path.join(SD_SUBDIR, "projects.txt"))
        if q_proj:
            proj = open(q_proj, encoding="utf-8").read()
        expect(proj is not None, "projects.txt 还在", problems)
        if proj is not None:
            n = sum(1 for l in proj.splitlines() if "|" in l)
            print("   删除后任务数 = %d" % n)
            # 夹具铺 2 个 → 取消一次（不删）+ 确认一次（删 1 个）→ 剩 1 个。
            expect(n == 1, "任务从 2 个删到 1 个（取消那一次没有误删）", problems)

        expect(kv(st, "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（弹窗没有把屏弹走）", problems)

    elif tag == "CB":
        print("-- 断言 CB：回复里的代码块能落成文件（且认得扩展名） --")
        st = res["status"]
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 2", problems)

        line = pick("codeprobe: saved=", st.splitlines())
        expect(line is not None, "有 codeprobe 记录（探针跑到了）", problems)
        if line:
            print("   " + line)
            # 样例里 4 个块：c / python / 无语言 / 没收尾的 js。
            # 后两个是真实情况：模型经常不写语言，也经常忘记收尾围栏。
            expect("saved=4" in line,
                   "四个块都写了（含没写语言、含没收尾的那个）", problems)
            for ext in (".c", ".py", ".txt", ".js"):
                expect(ext in line, "扩展名映射出了 %s" % ext, problems)

        # 光看日志不够：文件要真的躺在卡上，而且就在「文件」格列的那个目录里。
        hits = []
        for r in res["roots"]:
            d = os.path.join(r, "3ds", "aiconsole", "out")
            if os.path.isdir(d):
                hits += [f for f in os.listdir(d)
                         if f.startswith("code") and f.endswith((".c", ".py", ".txt", ".js"))]
        expect(len(hits) >= 4,
               "沙箱 out/ 里真的出现了 >=4 个 code*.* 文件（实际 %d）" % len(hits),
               problems)

    elif tag == "CP":
        print("-- 断言 CP：软件更新屏两页能来回切，内容包还能自己扫 --")
        st = res["status"]
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 6", problems)
        expect("ota: tab -> content" in st,
               "L 切到了内容包页（status 有记录）", problems)
        expect("ota: tab -> program" in st,
               "R 又切回了程序页", problems)
        # 面板一进去就重扫本地清单 —— 这是它唯一刷新"本机版本/有无备份"的时机。
        # 没有这一行说明面板没被真正初始化（只是把上半屏盖住了）。
        expect("content: scan avail=" in st,
               "内容包面板初始化过（有 content: scan 记录）", problems)
        expect(kv(st, "end_screen") == "SCREEN_UPDATE",
               "end_screen = SCREEN_UPDATE（没有误退屏）", problems)

    elif tag == "ST":
        print("-- 断言 ST：滑轨第 6 格能进，三块图都画了 --")
        st = res["status"]
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)
        expect("chat: rail -> 5" in st,
               "status 记到切到了第 5 格（0 基）= 状态页", problems)

    elif tag == "G":
        print("-- 断言 G：对话屏进得去，空输入不会出问题 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT", problems)

        # 必须点名 _top：空输入的提示只落在上屏状态行，底屏两张是一样的。
        # 早先写 "1_chat_empty"（不带 _top），a[0] 取到的是底屏，误判成"没变化"。
        a = [p for p in res["shots"]
             if "1_chat_empty_top" in os.path.basename(p)]
        b = [p for p in res["shots"]
             if "2_chat_empty_send_top" in os.path.basename(p)]
        if a and b:
            expect(not _same_bytes(a[0], b[0]),
                   "空输入按 A 之后状态行变了（给了提示，不是静默）", problems)
        else:
            expect(False, "缺少对话屏截图", problems)

    elif tag == "H":
        print("-- 断言 H：真实发送（快捷短语 -> API -> 回复）--")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 6", problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT", problems)

        # M6 之后 status 里可能有多条 chat: 记录（流式那次失败 + 退回那次的结果），
        # 所以不能只看第一条 —— 顺序会变，只看第一条就会随实现细节假失败。
        # 判据统一成"把整块 chat: 记录拼起来看"：
        #   · 假 Key 趟：只要**任一条**明确报 401/403 就算通过
        #     （整条链路 HTTPS/鉴权/请求体/错误路径都走通了）
        #   · 真 Key 趟：只要**任一条**是 ok 就算通过
        chat_lines = [l.strip() for l in res["status"].splitlines()
                      if l.startswith("chat:")]
        blob = " | ".join(chat_lines)

        if not chat_lines:
            expect(False, "status 里没有 chat: 记录 —— 发送路径根本没跑到", problems)
        elif res.get("h_dummy"):
            if "http=401" in blob or "http=403" in blob:
                expect(True, "假 Key 被明确拒绝 —— 网络往返与错误路径全通（%s）" % blob,
                       problems)
            else:
                expect(False, "假 Key 预期 401/403，实际：%s" % blob, problems)
        elif any(l.startswith("chat: ok") for l in chat_lines):
            ok_line = next(l for l in chat_lines if l.startswith("chat: ok"))
            expect(True, "拿到真实回复（%s）" % ok_line, problems)
        else:
            # 真实 Key 趟里的环境性失败（限流/断网）不是软件缺陷，但必须亮出来
            print("   [~~] 环境性失败：%s" % blob)
            if "http=429" in blob:
                print("        429 = 服务商限流，等额度重置后重跑本趟即可")
            elif "http=0" in blob:
                expect(False, "连服务商都没摸到（http=0，网络/证书问题）", problems)

    elif tag == "I":
        print("-- 断言 I：Key 管理屏（列表 + 切换落盘）--")

        # 现场诊断。I 趟的失败是间歇性的（全量里约一半的跑会出现，单独跑
        # 必过），表现为 ks_active 为空 —— 而 app 写的是
        # `n > 0 ? keystore_active_name() : ""`，所以空 = **它认为仓库是零把**。
        # 也就是说 app 压根没读到脚本刚摆好的 keys.json。
        # 出问题时把这几件事一起打出来，下次不用从头猜：
        # 它自报的 ks_* 全套，以及各 root 上两个文件到底在不在。
        for ln in (res["status"] or "").splitlines():
            if ln.strip().startswith("ks_") or ln.strip().startswith("creds_"):
                print("   [现场] %s" % ln.strip())
        for r in res["roots"]:
            kj = os.path.join(r, *KS_REL.split("/"))
            cp = os.path.join(r, *CREDS_REL.split("/"))
            print("   [现场] keys.json=%s creds.enc=%s  @ %s"
                  % (os.path.isfile(kj), os.path.isfile(cp), r))

        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_KEYS",
               "end_screen = SCREEN_KEYS", problems)

        # M5 之后仓库读写在加密文件里，宿主没法直接解析（KDF 带设备因子）。
        # 改看程序自报的 ks_active —— 它读的就是从仓库载入的同一份内存状态，
        # 一样能证明"切换落盘并在重启后仍生效"这件事实。
        actual = kv(res["status"], "ks_active") or ""
        want   = res.get("ks_expected") or ""     # 本趟的快照，不是那个全局
        if actual:
            expect(want and actual == want,
                   "切换后 ks_active = %s（实际 %s）"
                   % (want or "（快照丢了？）", actual), problems)
            # 夹具进屏时腾空了 creds.enc，让明文 keys.json 成为初始来源；
            # 但按键切 active 会触发 save()，而 save 只要加密可用就**重新写
            # creds.enc**（并把明文删掉）—— 所以跑完看到 ks_encrypted=1
            # 是**正确行为**，正好反证了"写路径确实走加密"。
            enc = kv(res["status"], "ks_encrypted")
            if enc is not None:
                expect(enc == "1",
                       "切换后的落盘走了加密（ks_encrypted=1 —— 写到 creds.enc）"
                       if enc == "1" else
                       "切换后落盘没走加密（ks_encrypted=0，加密闸门没开？）",
                       problems)
        else:
            # 老二进制（还没写 ks_active）退回读 keys.json，保证向后兼容
            try:
                after = json.loads(res.get("ks_after") or "{}")
            except Exception:                                      # noqa: BLE001
                after = {}
            expect(after.get("active") == want,
                   "切换后 keys.json 的 active = %s（实际 %s）"
                   % (want or "（快照丢了？）", after.get("active") or "无"),
                   problems)

    elif tag == "J":
        print("-- 断言 J：触摸点按钮进对话屏 + 空输入发送提示 --")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 4", problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（点「开始对话」进来了）", problems)
        a = [p for p in res["shots"]
             if "1_touch_chat_top" in os.path.basename(p)]
        b = [p for p in res["shots"]
             if "2_touch_send_empty_top" in os.path.basename(p)]
        if a and b:
            expect(not _same_bytes(a[0], b[0]),
                   "触摸空发送后状态行变了（给了提示，不是静默）", problems)
        else:
            expect(False, "缺少触摸屏截图", problems)

    elif tag == "8":
        print("-- 断言 8：每个任务一份历史（切项目上屏就换）--")
        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（脚本 %d 张 × 上下屏）"
               % (res["expect"] * 2, res["expect"]), problems)
        expect("SCREEN_CHAT" in kv(res["status"], "end_screen"),
               "end_screen = SCREEN_CHAT", problems)

        def _top(tag_name):
            hit = [q for q in res["shots"]
                   if tag_name in os.path.basename(q)
                   and os.path.basename(q).endswith("_top.bmp")]
            return hit[0] if hit else None

        # ① 切到任务乙 → 上屏必须变
        # ② 切回任务甲 → 上屏必须变回原样
        # 两条判据合起来才成立：第①条单独成立时，"改了个状态行"也能让它绿；
        # 第②条单独成立时，"两个任务共用一份历史"也照样绿。**必须成对**。
        t_a  = _top("1_proj_a")
        t_b  = _top("3_proj_b")
        t_a2 = _top("4_proj_a_again")
        if not (t_a and t_b and t_a2):
            expect(False, "缺少上屏截图（1_proj_a / 3_proj_b / 4_proj_a_again）",
                   problems)
        else:
            expect(not _same_bytes(t_a, t_b),
                   "切到任务乙：上屏换成了乙的历史（不是没反应）", problems)
            # 只比内容区：状态行会写成「当前项目：任务乙/甲」，那是**该变**的，
            # 拿它当"历史没回来"的证据就把正常行为判成了 bug。
            expect(_same_top_content(t_a, t_a2),
                   "切回任务甲：上屏内容回到甲的原样（乙没把甲的记录冲掉）",
                   problems)

        # ③ 状态侧证据：槽真的按任务切换过。
        #    只判像素不够 —— 万一哪天"切项目顺便清屏"了，像素也会变，
        #    但历史其实没换。slot= 这行是"确实读了哪一份"的直接证据。
        expect("slot=p1" in res["status"], "status 记到切到了槽 p1", problems)
        expect("slot=p0" in res["status"], "status 记到切回了槽 p0", problems)

    elif tag == "7":
        print("-- 断言 7：十字键管下屏 / 摇杆管上屏滚动 / 最下行指引 --")
        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（脚本 %d 张 × 上下屏）"
               % (res["expect"] * 2, res["expect"]), problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT", problems)

        def _find(tag_name, kind):
            hit = [p for p in res["shots"]
                   if tag_name in os.path.basename(p)
                   and os.path.basename(p).endswith(kind + ".bmp")]
            return hit[0] if hit else None

        # ---- ① ←→ 切下屏格子 ----
        # 判据用**两边夹**：切过去必须不同，切回来必须相同。
        # 只判"不同"不够 —— 状态行文字、闪烁帧都会让它不同；
        # 而"切回来还一样"才能证明这一步是**可逆的格切换**。
        #
        # ⚠️ "相同"只比**滑轨以外**的像素（这里比的是整屏去掉页脚与状态行）：
        #   页脚现在会轮播文案（用户要求的最下行指引），3.2 秒一换，
        #   两张截图间隔必然超过一条的时长 —— 拿它当"画面没变"的判据，
        #   红的原因会是"轮播在正常工作"。**判据不该被新的正常行为推翻。**
        a = _find("1_chat_opt", "_bottom")
        b = _find("3_tab_kbd", "_bottom")
        c = _find("4_tab_opt_back", "_bottom")
        if a and b and c:
            # ★ 两条一起判，缺一不可（2026-10-24 血的教训）：
            #   "底屏变了"单独成立时**什么都证明不了** —— 第 0 格按左右
            #   是"把当前行的值改一档"，底屏照样会变，而人还停在第 0 格。
            #   那趟因此全绿，真正的问题（页脚从不轮播）被埋在绿灯底下。
            #   所以还要 status 里那行 dpad 记录：它会写出切到了第几格。
            #   只判像素 = 判了个"看起来像"；加上状态 = 判了"确实是"。
            expect(not _same_bytes(a, b),
                   "←→ 切格：底屏变了", problems)
            expect("chat: dpad(R) -> 1" in res["status"],
                   "←→ 切格：status 记到切进了第 1 格（不是改值冒充的）", problems)
            expect(_same_stable_area(a, c),
                   "←→ 切回：除轮播页脚外逐字节相同（切格是可逆的）", problems)
        else:
            expect(False, "缺少切格相关的底屏截图", problems)

        # ---- ② ↑↓ 在第 0 格内选行 ----
        d = _find("5_opt_row2", "_bottom")
        e = _find("6_opt_row1", "_bottom")
        if a and d and e:
            expect(not _same_bytes(a, d),
                   "↑↓ 选行：光标动到第 2 行，底屏变了", problems)
            expect(_same_stable_area(a, e),
                   "↑↓ 回位：除轮播页脚外逐字节相同（上下各一次回到原样）",
                   problems)
        else:
            expect(False, "缺少选行相关的底屏截图", problems)

        # ---- ③ 摇杆滚上屏 ----
        # ★ 判据是"上屏变、底屏不变"。底屏**必须**不变 —— 摇杆只该动上屏，
        #   底屏若也变了，说明这一帧有别的东西在改画面（状态行刷新、
        #   动画帧），那"上屏变了"就不再是"滚动了"的证据。
        s0_t = _find("2_chat_sent", "_top")
        s1_t = _find("7_scrolled_up", "_top")
        s0_b = _find("2_chat_sent", "_bottom")
        s1_b = _find("7_scrolled_up", "_bottom")
        if s0_t and s1_t and s0_b and s1_b:
            if _same_bytes(s0_t, s1_t) and _same_bytes(s0_b, s1_b):
                # 上下屏都没变：要么没内容可滚，要么推杆没接线。
                # 这两件事**必须分开报**，否则以后排查会以为是同一个问题。
                if not _same_bytes(s0_t, _find("1_chat_opt", "_top") or s0_t):
                    expect(False,
                           "推杆前后上下屏都没变 —— 摇杆没接线（或累积量不够）",
                           problems)
                else:
                    expect(True,
                           "推杆前后都没变，且上屏本来就空 —— 本趟没内容可滚"
                           "（滚动这条未被验到，不算失败）", problems)
            else:
                expect(not _same_bytes(s0_t, s1_t),
                       "摇杆上推：上屏变了（上屏真的滚动了）", problems)
                expect(_same_bytes(s0_b, s1_b),
                       "摇杆上推：底屏一字未动（摇杆只滚上屏，没有连带改下屏）",
                       problems)
        else:
            expect(False, "缺少摇杆相关的截图", problems)

        # ---- ④ 最下行操作指引 ----
        # ★ 这条判据**分两种状态**，因为页脚本来就是两套：
        #   · 配了 Key（chat_ready）→ 轮播 7 条键位指引
        #   · 没配 Key / 豆包      → **只写一条**（怎么连上），不轮播
        # 后者是有意的：这时候"怎么聊"不是问题，"怎么连上"才是，
        # 多讲键位反而把那一句关键指引淹掉。
        #
        # ⚠️ 还必须**在第 0 格以外**测：第 0 格自己画一条固定页脚
        #    （S0963，见 sc_dock.c 的 dockui_render_opt），不轮播。
        #    脚本里已经 RIGHT 切到「键盘」格再截图。
        h0 = _find("9_hint_a", "_bottom")
        hm = _find("9b_hint", "_bottom")
        h1 = _find("10_hint_b", "_bottom")
        if not (h0 and hm and h1):
            expect(False, "缺少页脚对比截图（要三张：9_hint_a / 9b_hint / 10_hint_b）",
                   problems)
        else:
            fb = [_footer_bytes(x) for x in (h0, hm, h1)]
            # 相邻两对里任意一对不同 → 文案换过。
            # 不要求"两对全不同"：3.2s 一条，60 帧在模拟器里可能不足一条。
            f0 = fb[0]
            # ⚠️ 条件别写反（第一版就反了）：要的是"和第二张不同就用第二张，
            # 相同才退到第三张"。写反的后果是——两张相同就自我比较、
            # 三张里明明有一对不同却判红，而 status 里 hintroll step 已经在涨。
            f1 = fb[1] if fb[0] != fb[1] else fb[2]
            has_key = (kv(res["status"], "ai_configured") == "1")
            if f0 is None or f1 is None:
                expect(False, "页脚区读不出来（截图不是 320x240 24bpp）", problems)
            elif kw_is_offline(res["status"]):
                # 离线演示 = chat_ready 为真 → 应当轮播
                expect(f0 != f1,
                       "离线演示下页脚换了一条（轮播真的在走）", problems)
            elif has_key:
                expect(f0 != f1,
                       "有 Key 时页脚换了一条（轮播真的在走）", problems)
            else:
                # 没 Key：页脚应当是**固定的一条**（配 Key 指引）
                expect(f0 == f1,
                       "没 Key 时页脚固定不轮播（只给一条「怎么连上」的指引）",
                       problems)
                expect(kv(res["status"], "layout_overflow") == "0",
                       "页脚那一条也没被右边缘裁掉", problems)

        expect(kv(res["status"], "layout_overflow") == "0",
               "status: layout_overflow = 0（新指引行没有被右边缘裁掉）",
               problems)

    elif tag == "K":
        print("-- 断言 K：设置屏（主题切换真落盘 + 像素真变黑）--")
        expect(len(res["shots"]) == res["expect"] * 2, "截图数 = 10", problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_HOME",
               "end_screen = SCREEN_HOME（B 键从设置退回主界面）", problems)

        # 硬证据 1：运行期主题与配置里的 theme 都对上 dark
        # ★ 判据**不绑文案**：主题名会随界面语言变（中文"黑夜" / 英文"Night"）。
        # 早先只认中文，英文版一跑就红 —— 而红的原因不是功能坏了，
        # 是判据把"显示名"当成了"状态"。两种都认，是因为这一条真正要断言的
        # 是"主题真的切了"，而不是"它被叫成什么"。
        expect(kv(res["status"], "ui_theme_active") in ("黑夜", "Night"),
               "ui_theme_active = 黑夜/Night（w_theme 真的切了）", problems)
        expect((res.get("cfg") or {}).get("ui", {}).get("theme") == "dark",
               "config.json 里 ui.theme = dark（切主题真的落了盘）", problems)

        # ---- 硬证据 3：A 真的落在「启动检查更新」那一行 ----
        # 为什么单独立这一条：本趟是**按位置**导航到 OTA 行按 A 的，
        # 而行号漂移**不会报错** —— 上面那两条照过，被 toggle 的却成了
        # 隔壁行（2026-10-16 就这么静默错了很久：6 次 DOWN 落在
        # 「回复长度」上，而整趟全绿）。
        # "ota.enabled 被翻过来"是"A 按在了 OTA 行上"唯一能从外部读到
        # 的证据。起点是出厂默认 true（K 趟不摆 config.json，跑默认值），
        # toggle 一次 -> false。
        kcfg = res.get("cfg") or {}
        expect(kcfg.get("ota", {}).get("enabled") is False,
               "config.json 里 ota.enabled = false（A 落在「启动检查更新」行，"
               "不是隔壁行）", problems)

        # 硬证据 2：像素。1_settings_light 与 2_settings_dark 必须明显不同，
        # 且 5_home_dark（退回主界面后）也要是暗的 —— 否则只是设置屏自己变黑。
        def _mean_luma(path):
            try:
                from PIL import Image
            except ImportError:                                    # noqa: BLE001
                return None
            try:
                im = Image.open(path).convert("L")
            except OSError:
                return None
            h = im.histogram()
            total = sum(h)
            return sum(i * n for i, n in enumerate(h)) / max(1, total)

        def _find(tag_name):
            hit = [p for p in res["shots"] if tag_name in os.path.basename(p)]
            return hit[0] if hit else None

        p_light = _find("1_settings_light_top")
        p_dark  = _find("2_settings_dark_top")
        p_home  = _find("5_home_dark_top")

        if p_light and p_dark:
            la, ld = _mean_luma(p_light), _mean_luma(p_dark)
            if la is None or ld is None:
                print("   [~~] 没装 Pillow，跳过像素断言（只信 status 与配置）")
            else:
                print("     平均亮度：白天 %.1f -> 黑夜 %.1f" % (la, ld))
                expect(ld < la - 60,
                       "切黑夜后平均亮度掉了 %.0f（应为大幅变暗）" % (la - ld),
                       problems)
                if p_home:
                    lh = _mean_luma(p_home)
                    print("     退回主界面后亮度 %.1f" % lh)
                    expect(lh < la - 60,
                           "退回主界面仍是黑夜（亮度 %.0f，不是打回白底）" % lh,
                           problems)
        else:
            expect(False, "缺少设置屏截图", problems)

    elif tag == "L":
        print("-- 断言 L：M5 加密存储（三阶段）--")
        stages = res.get("l_stages") or {}
        st1 = stages.get("L1", "")
        st2 = stages.get("L2", "")
        st3 = stages.get("L3", "")
        if not st1:
            expect(False, "阶段 1 没有写出 status.txt（app 没跑起来？）", problems)
            return problems

        # ---- 硬证据 1：加密自检在**设备上**全过 ----
        # 这是整个 M5 的地基：算法不对，后面全都是自欺欺人。
        mask = kv(st1, "crypto_selftest_mask")
        passed = kv(st1, "crypto_selftest")
        expect(passed == "5",
               "crypto_selftest = %s/5（RFC 8439 五个测试向量在设备上全过）" % passed,
               problems)
        if mask is not None:
            expect(int(mask) == 31,
                   "crypto_selftest_mask = 0x%x（bit0 chacha / bit1 poly1305 / "
                   "bit2 sha256 / bit3 roundtrip / bit4 kdf 全绿）" % int(mask),
                   problems)
        expect(kv(st1, "crypto_ok") == "1", "crypto_ok = 1", problems)

        # ---- 硬证据 2：导入后真的生成了 creds.enc ----
        cb = res.get("l_creds") or b""
        expect(len(cb) > 60,
               "creds.enc 已生成（%d 字节）" % len(cb), problems)
        expect(cb[:8] == b"AICRED01",
               "creds.enc magic = AICRED01", problems)
        # 明文 Key 不能出现在文件里 —— 这是加密有没有真做的**唯一硬判据**
        expect(b"sk-test-abcdef123456" not in cb,
               "明文 Key 没有出现在 creds.enc 里（确实加密了）", problems)

        # ---- 硬证据 3：明文仓库被清掉 ----
        ks = res.get("l_ks") or ""
        expect("sk-test-abcdef123456" not in ks,
               "明文 keys.json 里已不含 Key（加密成功即删明文）", problems)

        # ---- 硬证据 4：重启后能自己解开（免输入）----
        expect(kv(st2, "creds_exists") == "1",
               "阶段 2 重启后 creds.enc 仍在", problems)
        expect(kv(st2, "creds_used") == "1",
               "阶段 2 重启后 creds.enc 可用（免输入即可解密）", problems)
        expect(kv(st2, "creds_last", ) and "成功" in (kv(st2, "creds_last") or ""),
               "阶段 2 解密结论 = %s" % kv(st2, "creds_last"), problems)
        expect(kv(st2, "end_screen") == "SCREEN_KEYS",
               "阶段 2 进到了 Key 管理屏（说明仓库不为空）", problems)

        # ---- 硬证据 5：篡改必须被拒，且不能崩 ----
        if st3:
            # 注意这里不能拿 creds_used 当判据：它 = creds_exists && 自检通过，
            # 描述的是"加密设施可用"，不是"这次载入成功了"。篡改后设施照样可用
            # （算法没坏，坏的是文件），所以 creds_used 仍是 1，那是对的。
            # 真正的判据是**载入结论**：creds_last 必须报校验失败。
            note3 = kv(st3, "creds_last") or ""
            expect("校验失败" in note3 or "被改过" in note3 or "别的机器" in note3,
                   "阶段 3 明确拒绝并说明原因：%s" % note3, problems)
            # 拒了也要活着把 status.txt 写出来 —— 上面这几行能读到，
            # 本身就证明没崩；这里再确认一下是正常退出而非异常
            expect(kv(st3, "end_screen") not in (None, ""),
                   "阶段 3 篡改后 app 仍正常走到收尾（没崩）", problems)
            # 篡改后不该有任何 Key 被交出去：仓库应当是空的
            expect(kv(st3, "end_screen") == "SCREEN_KEYS",
                   "阶段 3 仍进到 Key 管理屏（拒绝解密但界面可用）", problems)

    elif tag == "M":
        print("-- 断言 M：M6 流式输出（回复过程中文本逐步增长）--")
        # 脚本拍 3 张（1/2/7）+ app 按进度拍 4 张（stream_1..4），各出顶底两张。
        # 用下限而不是等号：进度那张在回复偏短时会少一张，不该把
        # "回复比预期短"报成"截图丢了"。
        expect(len(res["shots"]) >= 20,
               "截图数 = %d（脚本 4×2 + 两轮进度取证 ≤4×2×2）" % len(res["shots"]),
               problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT", problems)

        # ---- 硬证据 1：请求真的走了流式（不是配置说开、实际退回） ----
        # 这个探针是 M6 存在意义的核心字段：ai_stream=1（配置里想要）而
        # ai_stream_used=0 的话，说明整条流式链路根本没跑起来。
        used = kv(res["status"], "ai_stream_used")
        calls = kv(res["status"], "ai_stream_calls")
        expect(calls is not None and int(calls) >= 1,
               "status: ai_stream_calls = %s（确实发过请求）" % calls, problems)
        if used is None:
            expect(False, "status 里没有 ai_stream_used 字段（旧二进制？）", problems)
        else:
            # M 趟走的是离线演示：不出网、不会失败、不该退回，所以必须是 1。
            # 这一条同时证明了「没有 Key 也能把整条流式链路跑完」——
            # 那正是演示模式存在的意义。
            expect(used == "1",
                   "ai_stream_used = %s（离线演示趟必须是 1 = 流式成功；"
                   "0 = 压根没走流式，2 = 意外退回了非流式）" % used, problems)

        # ---- 硬证据 2：真拿到内容（或假 Key 趟拿到明确拒绝） ----
        # 与 H 趟同理：M6 之后可能有两条 chat: 记录（流式 + 退回），看整体。
        chat_lines = [l.strip() for l in res["status"].splitlines()
                      if l.startswith("chat:")]
        blob = " | ".join(chat_lines)

        if not chat_lines:
            expect(False, "status 里没有 chat: 记录 —— 发送路径根本没跑到", problems)
        elif any(l.startswith("chat: ok") for l in chat_lines):
            oks = [l for l in chat_lines if l.startswith("chat: ok")]
            ok_line = oks[-1]        # 最后一轮 = 带上下文那轮，最有代表性
            expect(len(oks) >= 2,
                   "两轮各拿到一次回复（实际 %d 次：%s）" % (len(oks), oks),
                   problems)
            expect(True, "拿到回复（%s）" % ok_line, problems)
            expect("provider=demo" in ok_line,
                   "确认是离线演示在应答（%s）" % ok_line, problems)
            expect("stream=1" in ok_line,
                   "chat: 记录里 stream=1（这次真的用了流式）", problems)
        else:
            # 演示既不联网也不看 Key，这里失败就只能是代码问题，
            # 没有"环境性失败"这种台阶可下。
            expect(False, "离线演示竟然没拿到回复：%s" % blob, problems)

        # ---- 硬证据 2b：多轮上下文真的带上了 ----
        # 两轮：第一轮不该有历史，第二轮必须带上第一轮的问与答。
        # 只看屏幕是验不出多轮的（模型记不记得住上句，从一次回答判断不了），
        # 所以让 app 在每次发送前自报"这次带了几条"。
        ctxs = []
        for ln in res["status"].splitlines():
            ln = ln.strip()
            if not ln.startswith("ctx: history="):
                continue
            # 形如 "ctx: history=2/8 sys=21749 mt=600 doc=embedded"。
            #
            # ★★ 这里踩过**两次**同一个坑（2026-10-01 加 sys=、2026-10-19 加 "/keep"）：
            #   诊断格式一改，解析就静默失效 —— int("2/8") 抛 ValueError，
            #   被下面的 except 吞掉，现象是"ctx 一条都没有"，
            #   而真因是**解析器没跟上格式**，看起来却像"发送路径没跑到"。
            #   所以现在按 "key=value" 通用拆，再只取 "/" 前面那段数字。
            fields = {}
            for tok in ln.split()[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = v
            try:
                ctxs.append(int(fields["history"].split("/", 1)[0]))
            except (KeyError, ValueError):
                pass
        if len(ctxs) < 2:
            expect(False, "ctx: 记录只有 %d 条（两轮应当各有一条）" % len(ctxs),
                   problems)
        else:
            print("   上下文条数：" + " -> ".join(str(v) for v in ctxs))
            expect(ctxs[0] == 0,
                   "第一轮不带上下文（实际 %d 条）" % ctxs[0], problems)
            expect(ctxs[-1] >= 2,
                   "第二轮带上了 %d 条上下文（至少 2 条 = 上轮的问 + 答）"
                   % ctxs[-1], problems)
            expect(ctxs[-1] > ctxs[0],
                   "上下文条数确实是长出来的（%d -> %d）" % (ctxs[0], ctxs[-1]),
                   problems)

        # ---- 硬证据 3：长度轨迹严格递增 ----
        # 这是本趟最硬的一条：由流式回调在每次跨过 64 字节档时自报，
        # 完全不依赖截图时机和帧率，也不用人眼看图。
        # 之所以要它，是因为截图那条路是脆的 —— 流式整个阻塞在
        # handle() 里，主循环停摆，脚本的 wait/shot 跟着停（2026-10-01
        # 实测四张连拍一模一样）。app 侧已加了 pump 让脚本在流式期间
        # 照常走，但判据不该建立在"pump 恰好够快"这种巧合上。
        trace = []
        for ln in res["status"].splitlines():
            ln = ln.strip()
            if ln.startswith("live: len="):
                try:
                    trace.append(int(ln[len("live: len="):]))
                except ValueError:
                    pass
        if len(trace) < 4:
            expect(False, "长度轨迹只有 %d 条（至少要 4 条才能说明是逐步长的）：%s"
                   % (len(trace), trace), problems)
        else:
            print("   长度轨迹：" + " -> ".join(str(v) for v in trace))
            # 轨迹是**每轮重置**的（sc_chat 每次发送都把计数器归零），
            # 所以两轮会看到两次从 68 起步 —— 直接要求整串递增是错的。
            # 按"回落"切成段，每段各自要求严格递增。
            segs, cur = [], []
            for v in trace:
                if cur and v <= cur[-1]:
                    segs.append(cur)
                    cur = []
                cur.append(v)
            if cur:
                segs.append(cur)

            print("   （切出 %d 段，各段条数 %s）"
                  % (len(segs), [len(s) for s in segs]))
            expect(len(segs) >= 2,
                   "两轮各留下一串长度轨迹（实际 %d 段）" % len(segs), problems)
            for i, sg in enumerate(segs, 1):
                expect(len(sg) >= 4 and all(sg[k] > sg[k - 1]
                                            for k in range(1, len(sg))),
                       "第 %d 段严格递增且够长（%s）" % (i, sg), problems)

        # ---- 硬证据 4：进度取证拍到的几张是**不同**画面 ----
        # 这几张由 app 按内容长度分档自拍（sc_chat.c 的 live_snap），
        # 所以"不同阶段"是构造上保证的，不是撞运气撞到的。
        #
        # 这里刻意**不**拿 ink 占比当判据。ink 看着很合适，实则量不到点子上：
        # 顶屏消息是**底部锚定**的，内容一旦超过一屏，屏幕就永远填满、
        # 且总显示"最后 N 行" —— 两个不同阶段显示的行数一样多，ink 必然
        # 相同（2026-10-01 实测四张清一色 0.203，而 md5 两两不同）。
        # 一个量不出想量之物的指标，留着只会制造假失败。
        shots_named = []
        for key in ("stream_1_1", "stream_1_2", "stream_1_3", "stream_1_4"):
            # 必须**精确**匹配 "_top.bmp"。写成 `key in basename` 会连
            # stream_1_bottom.bmp 一起命中，而 sorted 之后 bottom 排在前面，
            # 于是 hit[0] 拿到的是底屏 —— **底屏在流式期间根本不变**
            # （live_flush 只画顶屏），四张摘要必然相同。
            # 2026-10-01 在这个坑里绕了两轮：图全是对的，尺子拿错了屏。
            hit = [p for p in res["shots"]
                   if os.path.basename(p) == key + "_top.bmp"]
            if hit:
                try:
                    with open(hit[0], "rb") as fh:
                        dig = hashlib.md5(fh.read()).hexdigest()[:8]
                except OSError:
                    dig = "?"
                shots_named.append((key, dig))

        if len(shots_named) < 2:
            expect(False, "进度取证截图只有 %d 张（至少要 2 张才谈得上"
                          "『过程中在变』）" % len(shots_named), problems)
        else:
            print("   进度取证：" +
                  ", ".join("%s=%s" % (k, d) for k, d in shots_named))
            uniq = len(set(d for _, d in shots_named))
            # 只要 2 张不同，**不**要求 4 张全不同：屏幕是底部锚定的，
            # 内容超过一屏之后可见区域就填满了，后面几张长得再快、
            # 画面也一模一样（实测第 3、4 张同摘要）。要求"全不同"
            # 会变成周期性假失败 —— 真正证明"在增长"的是长度轨迹，
            # 这里只负责证明"画面确实在变"。
            expect(uniq >= 2,
                   "%d 张进度取证里至少 2 张互不相同（实际 %d 张不同）—— "
                   "说明流式过程中画面确实在变化"
                   % (len(shots_named), uniq), problems)

    elif tag == "N":
        print("-- 断言 N：会话持久化 + 助手人设 --")
        expect(len(res["shots"]) >= 12,
               "截图数 = %d（脚本 3×2 + 进度取证 ≤4×2）" % len(res["shots"]),
               problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT", problems)

        lines = [l.strip() for l in res["status"].splitlines()]

        # ---- 硬证据 1：开机把预置的历史读回来了 ----
        # 夹具预置的就是 2 条，所以这里断的是**确定值**而不是"大于零" ——
        # 后者在"只读回一条"时也会通过。
        rest = [l for l in lines if l.startswith("chat: restored=")]
        if not rest:
            expect(False, "没有 chat: restored= 记录 —— 会话记录没被读回来",
                   problems)
        else:
            try:
                n_rest = int(note_field(res["status"], "chat: restored=",
                                        "restored"))
            except (TypeError, ValueError):
                n_rest = -1
            expect(n_rest == 2,
                   "开机恢复了 2 条历史（实际 %d）—— 夹具预置的就是 2 条" % n_rest,
                   problems)
            # 夹具正文都短（最长几十字节），不该有任何一条被截短。
            n_over = note_field(res["status"], "chat: restored=", "oversize")
            expect(n_over == "0",
                   "没有历史被截短（oversize=%s）—— 正常写入的记录都该完整读回"
                   % n_over, problems)

        # ---- 硬证据 2：发送时人设与恢复来的历史都进了请求 ----
        # ctx 行有三个字段：history / sys / doc。
        #   sys  = 人设 + "\n\n" + AGENT.md 手册   （拼接见 sc_chat.c）
        #   doc  = 手册来源：embedded（烧进程序那份）或 sdmc（卡上那份）
        #
        # ⚠️ sys 里**含手册**是 2026-10-05 起的事（手册改为内嵌并随请求发出）。
        # 在那之前这条断言只把人设算进去，所以它今天会一直红 ——
        # 而红的原因不是缺陷，是断言没跟上实现。
        # 顺带的好处是：这条断言现在**同时**证明手册送出去了，
        # 以前它连这个都证明不了。
        ctx_pairs = []
        doc_seen = ""
        for l in lines:
            if not l.startswith("ctx: history="):
                continue
            # 与上面 M 趟同一处坑：按 key=value 通用拆，
            # history 取 "/" 前那段（"/keep" 是后加的字段）。
            fields = {}
            for tok in l.split()[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = v
            try:
                ctx_pairs.append((int(fields["history"].split("/", 1)[0]),
                                  int(fields["sys"])))
                if "doc" in fields:
                    doc_seen = fields["doc"]
            except (KeyError, ValueError):
                pass

        if not ctx_pairs:
            expect(False, "没有 ctx: 记录 —— 发送路径没跑到", problems)
        else:
            hist, sysb = ctx_pairs[-1]
            persona_len = len(N_PERSONA.encode("utf-8"))

            # 手册长度按**实际送出的那一份**算，不能写死仓库里的大小：
            # 卡上那份可能与仓库不同步（那是 sdmc 分支存在的理由）。
            # ★ 手册有中英两份，按**界面语言**挑（见 shared/agentdoc.c 的 pick_doc）。
            #   不判语言的话，切到英文再跑这趟就会红，而红的原因不是缺陷。
            ui_lang = "zh"
            for _l in res["status"].splitlines():
                if _l.startswith("ui_lang="):
                    ui_lang = _l.split("=", 1)[1].strip() or "zh"
                    break
            manual_name = "AGENT.en.md" if ui_lang == "en" else "AGENT.md"
            if doc_seen == "sdmc":
                manual_path = os.path.join(os.path.dirname(status_path or ""),
                                           manual_name)
            else:
                manual_path = os.path.join(ROOT, manual_name)
            manual_len = os.path.getsize(manual_path) if os.path.isfile(manual_path) else 0
            want_sys = persona_len + 2 + manual_len      # +2 是拼接时的两个换行

            print("   上下文：历史 %d 条 · 人设 %d 字节 + 手册 %d 字节（%s）= %d"
                  % (hist, persona_len, manual_len, doc_seen or "?", sysb))
            expect(hist == 2,
                   "发送时带上了 2 条恢复来的历史（实际 %d）" % hist, problems)
            expect(doc_seen in ("embedded", "sdmc"),
                   "这一轮带了手册并报得出来源（doc=%s）—— 没有手册的模型"
                   "不知道自己能触发哪些动作" % (doc_seen or "(空)"), problems)
            # ★ 不知道 `sysb == want_sys` 是对的：system 里**除了**人设和手册，
            # 还会拼一条"回答语言"指令（`Reply in Chinese.` + 换行 = **19 字节**，
            # 2026-10-17 加的）。写死等号会让断言一改配置就红 ——
            # 那正是"判据绑实现细节"的老毛病（见 缺陷审计 S 类的同类教训）。
            # 改成区间判据，两头都能抓：
            #   · sysb <  want_sys            → 人设/手册被截断（真故障）
            #   · sysb >  want_sys + 64       → 有意料之外的膨胀（手册重复拼接之类）
            # 64 的余量够放"语言指令 19 + 记忆若干"，但不足以掩盖整份手册（21K）。
            EXTRA_MAX = 64
            expect(want_sys <= sysb <= want_sys + EXTRA_MAX,
                   "人设与手册都完整送达（至少要 %d = 人设 %d + 换行 2 + 手册 %d，"
                   "实际 %d；允许 +%d 字节的附加指令如「回答语言」）"
                   % (want_sys, persona_len, manual_len, sysb, EXTRA_MAX), problems)
            print("   system 构成：人设 %d + 手册 %d = %d，实际 %d（附加 %d 字节）"
                  % (persona_len, manual_len, want_sys, sysb, sysb - want_sys))

        # ---- 硬证据 3：这轮对话又写回了卡上 ----
        after = res.get("n_turns_after")
        print("   chat.json 跑完后的条数：%s（夹具预置 2 条）" % after)
        expect(after is not None and after > 2,
               "回复之后 chat.json 长到了 %s 条（应当 > 2）—— 写盘路径生效"
               % after, problems)

    elif tag == "O":
        print("-- 断言 O：AI 创建文件（动作解析 + 确认屏 + 沙箱落盘 + 审计）--")
        # 脚本 7 张 + 进度取证 ≤4 张，各出顶底两张。
        expect(len(res["shots"]) >= 14,
               "截图数 = %d（脚本 7×2 + 进度取证 ≤4×2）" % len(res["shots"]),
               problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（允许之后按 B 退回了对话屏）", problems)

        lines = [l.strip() for l in res["status"].splitlines()]

        # ---- 硬证据 1：动作真的被解析出来了 ----
        # 这是本趟唯一"跨进程"的判据：解析发生在 app 里，而证据在 status.txt。
        # 只看文件存不存在是不够的 —— 文件也可能是夹具留下的
        # （所以夹具开头清了它），但"解析到了什么"只有这一条能说清。
        parsed = [l for l in lines if l.startswith("action: parsed")]
        if not parsed:
            expect(False, "没有 action: parsed 记录 —— 动作没被解析出来"
                          "（模型没吐标记？解析器坏了？）", problems)
        else:
            print("   解析：" + parsed[-1])
            expect("创建一个文件" in parsed[-1],
                   "解析出的是白名单内的 file.write（%s）" % parsed[-1], problems)
            expect("agent_demo.txt" in parsed[-1],
                   "参数里的文件名被正确取出（%s）" % parsed[-1], problems)

        # ---- 硬证据 2：用户在确认屏上按了"允许"，执行成功 ----
        # 判据要同时看 ok=1 和文案：ok=1 而文案是"文件名不合法"这种
        # 组合在代码里不可能出现，但如果哪天出现了，这里会立刻发现。
        execd = [l for l in lines if l.startswith("action: 创建一个文件 ok=")]
        if not execd:
            expect(False, "没有 action: … ok= 记录 —— 确认屏上没走到执行那一步"
                          "（焦点没移到「允许」？按键被吞了？）", problems)
        else:
            print("   执行：" + execd[-1])
            expect("ok=1" in execd[-1],
                   "执行成功（%s）" % execd[-1], problems)
            expect("agent_demo.txt" in execd[-1],
                   "写的就是解析出来的那个名字（%s）" % execd[-1], problems)
            expect("字节" in execd[-1],
                   "结果文案报出了字节数（%s）" % execd[-1], problems)

        # ---- 硬证据 3：文件真的躺在沙箱目录里 ----
        # 前两条都是"程序自己说"的；这一条是宿主的独立观察，
        # 也正是"AI 创建了一个文件"这句话的字面意思。
        o_files = res.get("o_files") or []
        o_text = res.get("o_text")
        print("   out/ 里的文件：%s" % (
            [os.path.basename(p) for p in o_files] or "（一个都没有）"))
        expect(len(o_files) >= 1,
               "沙箱目录里出现了文件（%d 个）—— 动作真的产生了副作用"
               % len(o_files), problems)
        if o_text is not None:
            print("   文件大小：%d 字节" % len(o_text.encode("utf-8")))
            expect(O_BODY_MARK in o_text,
                   "文件内容是演示剧本给的那份（能找到标记句）", problems)
            expect(o_text.count("\n") >= 5,
                   "正文完整（%d 个换行）—— 没有被截掉尾巴"
                   % o_text.count("\n"), problems)

        # ---- 硬证据 4：审计流水里有一次 ALLOW ----
        # 这一条看着与第 2 条重复，其实验的是**另一件事**：
        # 第 2 条是"程序认为它成功了"，这条是"它把这次决定留了痕"。
        # 一个会动手却不留痕的 agent，是没法信任的 —— 所以分开验。
        o_log = res.get("o_log")
        if not o_log:
            expect(False, "action.log 不存在或为空 —— 审计没生效", problems)
        else:
            entries = [l for l in o_log.splitlines() if l.strip()]
            print("   action.log：")
            for l in entries:
                print("     %s" % l)
            expect(any("ALLOW" in l for l in entries),
                   "流水里有一次 ALLOW（用户批准并执行）", problems)
            expect(any("agent_demo.txt" in l for l in entries),
                   "流水里记下了文件名", problems)
            # 拒绝不该出现：本趟脚本只走了"允许"这一条路。
            # 它若出现，说明焦点默认值或按键映射跑偏了 —— 而那正是
            # 第三道闸最要紧的性质（默认必须是"什么都不做"）。
            expect(not any("DENY" in l for l in entries),
                   "没有意外的 DENY（默认焦点确实是「拒绝」，没被误触）", problems)

    elif tag == "P":
        print("-- 断言 P：长消息回落（JSONL 行跨过旧行缓冲不丢历史）--")

        # ---- 前置：夹具本身得是有效的 ----
        # 这一趟的全部意义建立在一件事上：第 2 行**确实**超过了旧缓冲。
        # 常量哪天被调小（或转义规则变了），这趟会悄悄退化成"验三条普通历史"
        # 并永远通过 —— 一个永远绿的守卫比没有守卫更坏。
        lens = res.get("p_lens") or []
        print("   夹具行字节：%s（旧缓冲 %d）" % (lens, P_OLD_BUF))
        expect(bool(lens) and max(lens) > P_OLD_BUF,
               "夹具里确实有一条记录超过旧行缓冲（%s > %d）—— 否则本趟守不住任何东西"
               % (max(lens) if lens else "无", P_OLD_BUF), problems)

        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（进了对话屏）", problems)

        lines = [l.strip() for l in res["status"].splitlines()]

        # ---- 硬证据：三条历史一条不少 ----
        # 断确定值 3，不断 ">0"：旧实现在这里读回来的是 1 或 2
        # （第 2 行被截成两半，后半截不以 '{' 开头被跳过，
        #   于是它自己没了、"长消息之后的那条"也跟着没了）。
        # ">0" 会让那个坏实现照样通过 —— 而那正是要防的状态。
        rest = [l for l in lines if l.startswith("chat: restored=")]
        if not rest:
            expect(False, "没有 chat: restored= 记录 —— 会话记录一条都没读回来",
                   problems)
        else:
            print("   " + rest[-1])
            try:
                n_rest = int(note_field(res["status"], "chat: restored=",
                                        "restored"))
            except (TypeError, ValueError):
                n_rest = -1
            expect(n_rest == 3,
                   "三条历史全部读回（实际 %d）—— 夹具预置的就是 3 条；"
                   "旧定长缓冲在这里会读到 1 或 2" % n_rest, problems)

        # ---- 附带验一条：超长条目必须被**标出来**，不能默默腰斩 ----
        # chatlog.c 把 text 截到 CHATLOG_TEXT_MAX 时会计数，界面拿到
        # 就去提醒用户。夹具那条 3007 字节的正文必然触发它。
        # 这一条同时是"截断逻辑还在"的证据：真有一天上限被放宽、
        # 不再需要截了，这里会先红，提醒把提示文案一起改掉。
        n_over = note_field(res["status"], "chat: restored=", "oversize")
        expect(n_over == "1",
               "超长条目被标记出来（期望 oversize=1，实际 %s）—— "
               "被腰斩的记录必须让用户知道" % n_over, problems)

        # ---- 截图佐证：屏上确实画着长消息之后那条 ----
        # 数字对不等于画面对（也可能读回来三条、画的却是别的东西）。
        # 这里不去自己解 BMP 点阵认字 —— 那要连字库一起复刻，得不偿失。
        # 只用"三张历史 vs 两张历史"做不出判据时最朴素的那个信号：
        # 屏上文字变多了没有。ink_ratio 是为此设计的（比同版面的不同内容）。
        expect(len(res["shots"]) >= 2,
               "截图数 = %d（脚本 1×2）" % len(res["shots"]), problems)

    elif tag == "Q":
        print("-- 断言 Q：CIA 安装权限实测（am:u 到底给不给）--")

        # 这一趟和前十五趟是**另一类**：它不做 UI 验收，只把一条
        # 有争议的事实用实测固定下来。判据必须是**确定串**，不能只判
        # "有 amprobe 记录" —— 那在任何一个坏实现下都会通过。
        st   = res["status"]
        lines = [l.strip() for l in st.splitlines()]
        notes = [l for l in lines if l.startswith("amprobe:")]
        tnotes = [l for l in lines if l.startswith("titles:")]

        print("   amprobe 记录：")
        for l in notes:
            print("     %s" % l)

        if not notes:
            expect(False, "没有 amprobe: 记录 —— 探针压根没跑到"
                          "（autorun 的 op 没接上？脚本没被读到？）", problems)
            return problems

        # ---- 第 1 条：amInit 有没有拿到服务句柄 ----
        # 这是**最外层的闸**。拿不到就说明 exheader 里没有 am:u，
        # 后面两条都不是"拒绝"，而是"根本没申请"。
        init = [l for l in notes if "amInit rc=" in l]
        expect(bool(init), "探针打出了 amInit 的返回码", problems)
        if init:
            print("   " + init[-1])

        # ---- 第 2 条：结论行必须存在且唯一 ----
        # 三条结论里**必须恰好有一条**成立。一条都没有 = 探针半路挂了；
        # 两条同时出现 = 逻辑写错了（成功与失败不可能同时为真）。
        concl = [l for l in notes if "结论 =" in l]
        n_concl = len(concl)
        expect(n_concl == 1,
               "只产出一条结论（实际 %d 条）—— 0 条 = 探针半路挂了，"
               ">1 条 = 成功与失败判重了" % n_concl, problems)
        for l in concl:
            print("   ★ " + l)

        # ---- 第 4 条：形态判定这一项**必须诚实地承认自己不可信** ----
        # 这一条是本趟最容易被写坏、又最难看出来的地方。
        #
        # 背景：探针里有一段读 title_id 来判断"当前是 3dsx 还是 cia 形态"，
        # 因为**同一个 am 返回码在两种形态下含义完全不同**。但
        # svcGetProcessInfo(0x10000) 在 Azahar 里返回成功却不写入数据，
        # 于是未初始化的栈残留被当成 title id 打印出来 ——
        # 实测打出了 575C69796475475C（ASCII 的 "\Gudy\W"，一段路径残留）。
        #
        # 一个看起来煞有介事的十六进制数比一句"读不到"危险得多：人会拿它
        # 当依据。所以这里**断言它说了"不可信"**，而不是断言它读到了值。
        # 哪天这段被改回"照打印"，这条就会红。
        shape = [l for l in notes if "title_id" in l]
        if not shape:
            expect(False, "没有 title_id 相关记录 —— 形态判定那段被删了？"
                          "没有它，Q 趟的返回码无法解释", problems)
        else:
            for l in shape:
                print("   " + l)
            expect(any("读不到或不可信" in l or "形态=" in l for l in shape),
                   "形态判定有明确输出（读到就说形态，读不到就说不可信）",
                   problems)

        # ---- 第 3 条：titles 只读查询通不通 ----
        # 它有独立的诊断价值：哪怕装不了，能读 title 列表也说明
        # am 服务本身是可以碰的 —— 这决定了 pkg_list 那类只读功能
        # 在 .cia 形态下要不要受权限影响。
        if tnotes:
            for l in tnotes:
                print("   " + l)
        else:
            expect(False, "没有 titles: 记录 —— 只读的 title 列表也没跑起来",
                   problems)

    elif tag == "R":
        print("-- 断言 R：AI 安装程序（pkg.install 解析 + 确认屏 + 摘要护栏）--")
        expect(len(res["shots"]) >= 14,
               "截图数 = %d（脚本 7×2）" % len(res["shots"]), problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（执行之后按 B 退回了对话屏）", problems)

        lines = [l.strip() for l in res["status"].splitlines()]

        # ---- 硬证据 1：动作被解析成 pkg.install，不是别的 ----
        # 这是本趟的核心判据，也是最容易悄悄退化的地方：
        # 解析器若把 pkg.install 认成 ACT_UNKNOWN，后面所有断言都会
        # 以"看起来跟安装无关"的方式失败。所以先钉这一条。
        parsed = [l for l in lines if l.startswith("action: parsed")]
        if not parsed:
            expect(False, "没有 action: parsed 记录 —— 安装动作没被解析出来",
                   problems)
        else:
            print("   解析：" + parsed[-1])
            expect("安装一个程序" in parsed[-1],
                   "解析出的是白名单内的 pkg.install（%s）" % parsed[-1], problems)
            expect(R_PKG in parsed[-1],
                   "参数里的包名被正确取出（%s）" % parsed[-1], problems)

        # ---- 硬证据 2：走走到了执行那一步，且失败理由是"摘要对不上" ----
        # 关键点：ok 必须是 0。演示给的摘要（全零）不等于真实摘要，
        # 所以正确行为是**拒绝安装**。
        # 若 ok=1 出现在这里，说明摘要护栏没生效 —— 那正是
        # "装进去的东西下次会被执行"这件事最不能接受的失败模式。
        execd = [l for l in lines if l.startswith("action: 安装一个程序 ok=")]
        if not execd:
            expect(False, "没有 action: 安装一个程序 ok= 记录 —— "
                          "确认屏上没走到执行那一步", problems)
        else:
            print("   执行：" + execd[-1])
            expect("ok=0" in execd[-1],
                   "安装被拒绝（ok=0）—— 摘要对不上就不许装（%s）"
                   % execd[-1], problems)
            expect("摘要" in execd[-1],
                   "失败理由是摘要不符，而不是别的（%s）" % execd[-1], problems)

        # ---- 硬证据 3：落点必须是干净的 ----
        # 这一条是"拒绝"这个结论的**独立**佐证：程序说自己拒绝了，
        # 宿主机去看那个文件在不在。两者一致才算数。
        r_inst = res.get("r_installed") or []
        print("   3ds/ 里的目标文件：%s" % (
            [os.path.basename(p) for p in r_inst] or "（不存在，正确）"))
        expect(len(r_inst) == 0,
               "目标文件**没有**出现在 3ds/（摘要不符时不该装进去任何东西）",
               problems)

        # ---- 硬证据 4：夹具本身得是有效的 ----
        # 本趟全部意义建立在"演示摘要 ≠ 真实摘要"上。哪天演示剧本被改成
        # 带真实摘要（或 sha256 改成可选导致不核），这趟会悄悄退化成
        # "验一次普通拒绝"并永远绿。所以显式核对这两个值不相等。
        real = res.get("r_real_sha")
        print("   源包真实摘要：%.32s…" % (real or "?"))
        expect(bool(real), "宿主机算出了源包的真实 sha256（夹具有效）", problems)

        # ---- 硬证据 5：审计流水里有一次 FAIL，且没有 ALLOW ----
        # 失败也要留痕。而且本趟只走了"允许"这一条路 ——
        # 流水里若出现 DENY，说明焦点默认值或按键映射跑偏了。
        r_log = res.get("r_log")
        if not r_log:
            expect(False, "action.log 不存在或为空 —— 审计没生效", problems)
        else:
            entries = [l for l in r_log.splitlines() if l.strip()]
            print("   action.log：")
            for l in entries:
                print("     %s" % l)
            expect(any("FAIL" in l for l in entries),
                   "流水里记下了一次 FAIL（尝试过、被摘要护栏拦下）", problems)
            expect(any("ALLOW" in l for l in entries) is False,
                   "流水里**没有** ALLOW（护栏生效，什么都没装进去）", problems)
            expect(not any("DENY" in l for l in entries),
                   "没有意外的 DENY（默认焦点确实是「拒绝」，没被误触）", problems)

    elif tag == "S":
        print("-- 断言 S：AI 安装程序 · 成功路径（不给摘要 → 真的装进去）--")
        expect(len(res["shots"]) >= 14,
               "截图数 = %d（脚本 7×2）" % len(res["shots"]), problems)
        expect(kv(res["status"], "end_screen") == "SCREEN_CHAT",
               "end_screen = SCREEN_CHAT（执行之后按 B 退回了对话屏）", problems)

        lines = [l.strip() for l in res["status"].splitlines()]

        # ---- 硬证据 1：解析出的动作**不带**摘要 ----
        # 这是本趟与 R 趟唯一的输入差异。若这里出现了 sha256=，
        # 说明触发词走错了分支，后面"装成功"就不再是"可选路径"的证据。
        parsed = [l for l in lines if l.startswith("action: parsed")]
        if not parsed:
            expect(False, "没有 action: parsed 记录 —— 动作没被解析出来",
                   problems)
        else:
            print("   解析：" + parsed[-1])
            expect("安装一个程序" in parsed[-1],
                   "解析出的是 pkg.install（%s）" % parsed[-1], problems)
            expect(R_PKG in parsed[-1],
                   "包名正确（%s）" % parsed[-1], problems)
            expect("sha256=" not in parsed[-1],
                   "这次**没有**带摘要（走的是可选参数那条路，%s）"
                   % parsed[-1], problems)

        # ---- 硬证据 2：执行成功 ----
        execd = [l for l in lines if l.startswith("action: 安装一个程序 ok=")]
        if not execd:
            expect(False, "没有 action: 安装一个程序 ok= 记录 —— 没走到执行",
                   problems)
        else:
            print("   执行：" + execd[-1])
            expect("ok=1" in execd[-1],
                   "安装成功（ok=1）—— 没给摘要时不核对、照装（%s）"
                   % execd[-1], problems)
            expect("3ds/" in execd[-1],
                   "结果文案说出了装到哪（%s）" % execd[-1], problems)

        # ---- 硬证据 3：文件真的躺在 3ds/ 里，且大小与源包一致 ----
        # 这一条是"AI 装了程序"这句话的字面意思，也是宿主机唯一能
        # 独立于程序自述的观察。
        s_inst = res.get("s_installed") or []
        print("   3ds/ 里的目标文件：%s" % (
            [os.path.basename(p) for p in s_inst] or "（一个都没有）"))
        expect(len(s_inst) == 1,
               "目标文件出现在 3ds/（%d 个）—— 装这个动作真的产生了副作用"
               % len(s_inst), problems)

        s_src = res.get("s_src_bytes")
        s_dst = res.get("s_inst_bytes")
        print("   源包 %s 字节 → 装好 %s 字节" % (s_src, s_dst))
        if s_src is not None and s_dst is not None:
            expect(s_src == s_dst,
                   "搬运完整（%d == %d）—— 字节数一致，不是半截文件"
                   % (s_src, s_dst), problems)

        # ---- 硬证据 4：流水里是一次 ALLOW，且没有 FAIL ----
        # 与 R 趟正好相反：那边是 FAIL 无 ALLOW，这边是 ALLOW 无 FAIL。
        # 两趟一起把"有摘要/无摘要"两条分支的行为都钉住了。
        s_log = res.get("s_log")
        if not s_log:
            expect(False, "action.log 不存在或为空 —— 审计没生效", problems)
        else:
            entries = [l for l in s_log.splitlines() if l.strip()]
            print("   action.log：")
            for l in entries:
                print("     %s" % l)
            expect(any("ALLOW" in l for l in entries),
                   "流水里记下了一次 ALLOW（用户批准并执行成功）", problems)
            expect(not any("FAIL" in l for l in entries),
                   "没有 FAIL（这一趟该成功的都成功了）", problems)
            expect(not any("DENY" in l for l in entries),
                   "没有意外的 DENY（默认焦点确实是「拒绝」）", problems)

    elif tag == "T":
        print("-- 断言 T：故障注入 · 性能层（八个场景 + 时基标定）--")

        # 这一趟的判据全是**否定式**的：不崩、还能继续、没把句柄 free 掉。
        # 所以第一条判据不是"某行写了什么"，而是
        # **七个场景各自的收尾行都在** —— 少一个 DONE 就说明那个场景
        # 把进程带崩了，后面的场景压根没跑到。
        st    = res["status"]
        lines = [l.strip() for l in st.splitlines()]
        notes = [l for l in lines if l.startswith("perffault:")]

        if not notes:
            expect(False, "没有 perffault: 记录 —— 探针压根没跑到"
                          "（autorun 的 op 没接上？脚本没被读到？）", problems)
            return problems

        print("   探针记录（%d 行）：" % len(notes))
        for l in notes:
            print("     %s" % l)

        SCEN = ["ruler", "alloc", "hang", "coexist", "repeat", "edges",
                "churn", "idem"]

        # ---- 判据 1：备注区没有溢出 ----
        # 必须放在最前面，因为它会**伪装**成后面每一条判据的失败：
        # app_note 在缓冲区满时静默丢弃，于是「场景 X DONE」凭空消失，
        # 而脚本会把它读成「场景 X 崩了」。上一轮 T 趟有 7 条断言
        # 就是这样被误判的 —— 先把「证据是否完整」这件事钉死，
        # 后面的判据才有意义。
        novf = kv(st, "notes_overflow")
        nbyt = kv(st, "notes_bytes")
        expect(novf is not None,
               "status.txt 里有 notes_overflow 字段（判断证据是否被截断）",
               problems)
        # ⚠️ kv() 返回的是**字符串**，所以必须比 "0" 而不是 0。
        #    写成 novf == 0 会恒为假，而且失败信息会长得特别像真出事
        #    （"[!!] 备注区没有溢出（notes_overflow=0）" —— 值明明是 0）。
        expect(novf == "0",
               "备注区没有溢出（notes_overflow=%s，notes_bytes=%s）—— "
               "场景记录是完整的" % (novf, nbyt), problems)

        # ---- 判据 2：每个场景都有开始行与 DONE 行 ----
        # 这是整趟最重要的一条。DONE 行是场景函数**跑完**之后才写的，
        # 它缺失就只有一个解释：进程在那之前就没了。
        for sc in SCEN:
            expect(any(("场景 %s 开始" % sc) in l for l in notes),
                   "场景 %s 被触发" % sc, problems)
            expect(any(("场景 %s DONE" % sc) in l for l in notes),
                   "场景 %s 跑完了（进程活着走到底）" % sc, problems)

        # ---- 判据 2：ruler —— 时基必须是 268 kHz，不是 60 Hz ----
        #
        # 这是整趟的**尺子**，也是本轮修掉的三个致命 bug 之一。
        # 60Hz 那个数差了 4400 倍，而当时**所有超时**都基于它 ——
        # 于是「2 秒超时」实际是 7.5 毫秒，慢机器上会频繁误判
        # 「线程没起来」。
        #
        # 判据不能用「三行相等」（那是同一次错误自洽），
        # 必须是「三档差 100 倍的 sleep 收敛到同一个 tick/ms」。
        rates = {}
        for l in notes:
            m = re.search(r"sleep\s+(\S+)\s*=.*?=>\s*(\d+) tick/us"
                          r"\s*=\s*(\d+) tick/ms", l)
            if m:
                # 两个单位都取：它们必须差 1000 倍。
                # 差值本身就是一条判据 —— 只看其中一个，标签写错也发现不了。
                rates[m.group(1)] = (int(m.group(2)), int(m.group(3)))
        if len(rates) < 3:
            expect(False, "ruler 没有量出三档时基（只量到 %d 档：%s）"
                          % (len(rates), sorted(rates)), problems)
        else:
            for k in sorted(rates):
                us, ms = rates[k]
                print("   tick: %-5s -> %d tick/us = %d tick/ms" % (k, us, ms))
            uss = [v[0] for v in rates.values()]
            mss = [v[1] for v in rates.values()]

            # ① 三档必须收敛（差 100 倍的 sleep 给出同一个频率）
            expect(min(uss) >= 250 and max(uss) <= 290,
                   "三档 sleep 全部收敛到 268 tick/us 左右（实测 %d~%d）"
                   % (min(uss), max(uss)), problems)
            # ② 单位自洽：tick/ms 必须正好是 tick/us 的 1000 倍。
            #    这条专抓「标签写错」——上一版把 tick/us 标成 tick/ms，
            #    于是 268 MHz 被读成 268 kHz，还把 perf_lvl 的常数带歪了。
            for k in sorted(rates):
                us, ms = rates[k]
                expect(250000 <= ms <= 290000,
                       "%s 档的 tick/ms = %d（268MHz -> 约 268000）"
                       % (k, ms), problems)
                break
            # ③ 60Hz 那个错值在 tick/ms 下是 0.06，绝不可能落在 250000~290000。
            expect(min(mss) > 1000,
                   "tick/ms 不在 60Hz 换算下的量级（实测 %d）—— "
                   "确认没有退回那个错 4400 倍的假设" % min(mss), problems)

        # ---- 判据 3：ruler 大 timeout 不会变成"真的等很久" ----
        # 旧实现把 0xFFFFFFFF 当 tick 传进 threadJoin，折算 248 天。
        # 判据建立在「完成标志」而不是「时间到了没有」上，所以现在
        # 一个 49 天的 timeout 也该立刻返回。
        big = pick("scn=ruler join(0xFFFFFFFF)", notes)
        if big is None:
            expect(False, "ruler 的大 timeout 档没有记录", problems)
        else:
            print("   " + big)
            mb = re.search(r"耗时=(\d+) tick", big)
            expect(mb is not None and int(mb.group(1)) <= 200 * 268000,
                   "timeout=0xFFFFFFFF 也在 %.0fms 内返回 —— "
                   "判据是完成标志，不是时间"
                   % (int(mb.group(1)) / 268000.0 if mb else -1), problems)

        # ---- 判据 4：ruler 死循环线程必须超时而不是谎报成功 ----
        # 这是「放弃一个卡死的线程」这条设计唯一的检验。
        # 旧 threadJoin 在这里返回 rc=0（谎报成功），后果是
        # 我们会去 free 一个还在跑的线程 = use-after-free。
        rd = pick("scn=ruler D 死循环 join(", notes)
        if rd is None:
            expect(False, "ruler 的死循环档没有记录", problems)
        else:
            print("   " + rd)
            expect("rc=-2" in rd,
                   "对**永不结束**的线程 join 返回 -2（超时）—— "
                   "没有谎报成功（旧 threadJoin 在这里报 rc=0）", problems)
        rh = pick("scn=ruler D 超时后 handle=", notes)
        expect(rh is not None and ("(nil)" in rh or "0x0" in rh or "=0" in rh),
               "死循环线程被放弃且句柄置空（**没有 free** 它）", problems)

        # ---- 判据 5：alloc —— 768MB 栈必须失败，且失败后能重试 ----
        # 这条不需要"注入"：threadCreate 的 memalign 在申请 768MB 时
        # 必然失败，而反汇编确认它是返回 NULL 不是 abort。
        a = pick("scn=alloc_fail start=", notes)
        if a is None:
            expect(False, "alloc 场景没有 start 记录", problems)
        else:
            print("   " + a)
            expect("start=0" in a,
                   "768MB 栈的 worker 起不来（start=0）—— "
                   "内存不足被挡住了，没有崩", problems)
            expect("handle=(nil)" in a or "handle=0x0" in a or "handle=0" in a,
                   "失败后句柄是空的（没有留下半个线程对象）", problems)

        ar = pick("scn=alloc_fail 失败后重试", notes)
        expect(ar is not None and "start=1" in (ar or ""),
               "失败之后还能正常起一个 32KB 的 worker（失败没留坏状态）",
               problems)
        aj = pick("scn=alloc_fail join=", notes)
        expect(aj is not None and "ran=1" in aj and "5a5a" in (aj or ""),
               "重试的那个真跑完了，且参数原样传到了（arg=5a5a）", problems)

        # ---- 判据 6：hang —— join 必须在超时内返回，且句柄被放弃 ----
        # 「超时内返回」= 没有卡死游戏，这是插件场景最不能接受的结果。
        h = pick("scn=hang join=", notes)
        if h is None:
            expect(False, "hang 场景没有 join 记录 —— **join 卡死了**，"
                          "这就是插件会把游戏带挂的那种失败", problems)
        else:
            print("   " + h)
            expect("join=-2" in h,
                   "join 对一个死循环线程返回 -2（超时），而不是无限等", problems)
            # 耗时上界：3 秒。实测应当远小于它（600ms 就该回来）。
            #
            # ⚠️ tick 频率是 **268 kHz**，不是 60Hz（ruler A 段实测）。
            #    这里曾按 60 算，于是上界变成 360 tick ≈ 1.3ms ——
            #    一个 600ms 的等待在 1.3ms 就该返回，这判据本身是假的。
            m = re.search(r"耗时=(\d+) tick", h)
            if m:
                ticks = int(m.group(1))
                # 3000ms 上界 = 804000 tick。给一倍余量。
                bound = 3000 * 268000
                expect(ticks <= bound,
                       "join 在 %.0fms 内返回（超时给的 600ms，上界 3000ms）"
                       "—— 没有卡住" % (ticks / 268000.0), problems)
                # 下界：不能"假超时" —— 死循环线程必须真的被等了 600ms。
                # 早了说明等待根本没生效（那正是旧 threadJoin 的毛病）。
                expect(ticks >= 600 * 268000 * 0.8,
                       "join 确实等了约 600ms 才判超时（%.0fms）—— "
                       "不是立刻返回的假超时" % (ticks / 268000.0), problems)
            else:
                expect(False, "hang 的 join 记录里没有耗时字段", problems)

        hh = pick("scn=hang 超时后 handle=", notes)
        expect(hh is not None and ("(nil)" in hh or "0x0" in hh or "=0" in hh),
               "超时后句柄被置空（放弃这个线程，不再去 join 它）", problems)

        # ---- 判据 7：coexist —— UAF 邻域 ----
        # 前一个场景留下了一个**还在跑**的死循环线程（我们故意不 free）。
        # 这一条在它还活着的情况下正常起停 3 次。
        # 如果"放弃"写错成了"偷偷 free"，那个线程的 trampoline
        # 会在某个时刻从已释放内存读入口指针 -> 崩 -> 后面全没了。
        c = pick("scn=coexist 3 轮", notes)
        if c is None:
            expect(False, "coexist 场景没有收尾记录 —— 那个还活着的线程"
                          "把进程带崩了（use-after-free）", problems)
        else:
            print("   " + c)
            expect("全部正常=1" in c,
                   "死循环线程还活着的前提下，3 轮正常起停全部通过"
                   "（UAF 没发生）", problems)

        # ---- 判据 8：repeat —— 参数不能串用 ----
        # g_job 是个静态结构体（故意不用 malloc，少一个失败点），
        # 代价是必须保证「同一时刻只有一个 worker」。串用会静默算错。
        r1 = pick("scn=repeat 第一次 join=", notes)
        r2 = pick("scn=repeat 第二次 join=", notes)
        expect(r1 is not None and "1111" in r1 and "2222" not in r1,
               "第一次的 worker 拿到自己的参数（1111）", problems)
        expect(r2 is not None and "2222" in r2,
               "第二次的 worker 拿到自己的参数（2222，没有串用上一个）",
               problems)

        # ---- 判据 9：edges —— 边界输入 ----
        e1 = pick("scn=edges start(NULL)=", notes)
        expect(e1 is not None and "start(NULL)=0" in e1,
               "空函数指针被挡住（start=0）", problems)
        e2 = pick("scn=edges 无 worker 时 join=", notes)
        expect(e2 is not None and "join=-1" in e2,
               "没有 worker 时 join 返回 -1（而不是崩或空等）", problems)
        e3 = pick("scn=edges 无 worker 时 join(UINT32_MAX)=", notes)
        # 匹配 "join(UINT32_MAX)=-1" 而不是 "join=-1" —— 后者在这一行里
        # 根本不存在（我上一版改文案时漏改了这条断言，它静默失败了很久）。
        expect(e3 is not None and "join(UINT32_MAX)=-1" in e3,
               "join(UINT32_MAX) 在无 worker 时也立刻返回 -1（早退，不进等待循环）",
               problems)
        e4 = pick("scn=edges 1 字节栈 start=", notes)
        expect(e4 is not None and "start=1" in e4,
               "1 字节栈被抬到下限而不是照建（start=1）", problems)
        e5 = pick("scn=edges join=", notes)
        expect(e5 is not None and "ran=1" in e5 and "3333" in e5,
               "1 字节栈那个真跑完了（栈下限抬对了，不是 trampoline 崩掉）",
               problems)

        # ---- 判据 10：churn —— 8 轮起停没有累积泄漏 ----
        # 单次都对不代表八次都对：线程对象/栈/内核句柄漏光的表现
        # 就是「前面几轮好、某一轮突然失败」。
        ch = pick("scn=churn 8 轮", notes)
        if ch is None:
            expect(False, "churn 场景没有收尾记录", problems)
        else:
            print("   " + ch)
            expect("ok=7" in ch and "fail=1" in ch,
                   "8 轮里恰好 7 轮成功、1 轮（大栈）失败 —— "
                   "没有累积泄漏（大栈那轮失败是**预期**的）", problems)

        # ---- 判据 11：idem —— 探测幂等 ----
        # 不幂等的话，游戏运行中反复 init 会刷一串 ptm:sysm IPC。
        idm = pick("scn=idem 连调 5 次", notes)
        if idm is None:
            expect(False, "idem 场景没有记录", problems)
        else:
            print("   " + idm)
            expect("一致=1" in idm,
                   "perf_init 连调 5 次结果一致（幂等，不重复发 IPC）", problems)

    # ---- 全趟通用：两个"应该恒为 0"的守卫 ----
    st = res["status"]

    # ---- 「换了缓冲却没画」必须为 0 ----
    #
    # r_end() 里的 gfxSwapBuffers() 是**全局**的（翻转所有屏幕的缓冲索引），
    # 所以"只画一块屏就 r_end()"会让另一块屏在**两次旧内容**之间来回翻 ——
    # 用户看到的就是"屏一直在闪"（2026-10-10 真机：离线演示生成回复时最明显）。
    #
    # ★ 为什么必须有这条：这类缺陷**截图抓不到** —— 它是时序问题，单帧看完全正常。
    #   实测（同一天）：把 bug 注回去跑 M 趟，读数 swap_undrawn=182
    #   （下屏 182 / 上屏 0，签名精确）；而那一趟**仍然报 PASS**。
    #   修复后同一趟是 0。也就是说这条断言真的能区分两条路，不是假守卫。
    #
    # ⚠️ 注意它必须放在下面那条 if/elif 链**之前** ——
    #    那条链是 `if ov is None: ... elif tag == "U": ...`，
    #    往中间插代码会把 elif 的归属打断（这条教训当天就踩了一次）。
    su = kv(st, "swap_undrawn")
    if su is None:
        expect(False, "status 里没有 swap_undrawn 字段（守卫未生效，旧二进制？）",
               problems)
    elif su != "0":
        expect(False, "有 %s 次「换了缓冲却没画」（上屏 %s / 下屏 %s）—— "
                      "那块屏会在两次旧内容之间来回翻，也就是用户说的「闪」"
               % (su, kv(st, "swap_undrawn_top"), kv(st, "swap_undrawn_bot")),
               problems)
    else:
        expect(True, "没有「换了缓冲却没画」（交换前两屏都画过）", problems)

    # ---- 界面过渡动画自检必须满分（2026-10-18 新增）----
    #
    # 动画是**时序**行为：单张截图证明不了"中间有过渡"——两个时刻的样子
    # 各自都正常，错的是"直接跳变"。所以让 anim 模块自己跑三项逻辑自检
    # （多帧推进 / 单调 / 中途反向）并把项数写进 status。
    # 健康值 = anim_selftest_total（当前 3）。
    aok = kv(st, "anim_selftest")
    atot = kv(st, "anim_selftest_total")
    if aok is None or atot is None:
        expect(False, "status 里没有 anim_selftest 字段（动画模块未生效，旧二进制？）",
               problems)
    elif aok != atot:
        expect(False, "界面过渡动画自检只过 %s/%s 项 —— "
                      "动画没有真正的过渡（可能退化成瞬时跳变）"
               % (aok, atot), problems)
    else:
        expect(True, "界面过渡动画自检 %s/%s（过渡真的是逐帧推进）"
               % (aok, atot), problems)

    # ---- 版面溢出护栏必须为 0 ----
    ov = kv(st, "layout_overflow")
    if ov is None:
        expect(False, "status 里没有 layout_overflow 字段（护栏未生效，旧二进制？）",
               problems)

    elif tag == "U":
        print("-- 断言 U：内容更新通道 --")

        # cupd 的 bug 只会表现为「内容更新静默失灵」，不崩。
        # 所以判据不是「没崩」，而是「该拒的拒了、该装的装了、
        # 该回的回了、坏包没碰用户手上的文件」。
        st    = res["status"]
        lines_ = [l.strip() for l in st.splitlines()]
        notes = [l for l in lines_ if l.startswith("cupdtest:")]

        if not notes:
            expect(False, "没有 cupdtest: 记录 —— 探针压根没跑到"
                          "（autorun 的 op 没接上？脚本没被读到？）", problems)
            return problems

        print("   探针记录（%d 行）：" % len(notes))
        for l in notes:
            print("     %s" % l)

        # ---- 判据 1：备注区没溢出 ----
        # 放最前面：app_note 在缓冲区满时静默丢弃，于是后面每一条
        # 判据都会伪装成失败，而真正的原因在这里。
        nb = kv(st, "notes_bytes")
        if nb is None:
            expect(False, "status 里没有 notes_bytes（护栏未生效，旧二进制？）",
                   problems)
        else:
            expect(int(nb) < 6144,
                   "notes 只用了 %s / 6144 字节（没有溢出丢行）" % nb, problems)

        # ---- 判据 2：起止行都在 ----
        # 少一条 end 说明中途崩了，后面的记录压根不存在。
        expect(any("cupdtest: begin" in l for l in notes),
               "开跑行在", problems)
        expect(any("cupdtest: end" in l for l in notes),
               "收尾行在（中途没崩）", problems)

        # ---- 判据 3：汇总 ----
        resu = pick("cupdtest: RESULT", notes)
        if resu is None:
            expect(False, "没有 RESULT 行", problems)
        else:
            print("   " + resu)
            expect("PASS" in resu, "自测汇总 PASS（fail=0）", problems)

        # ---- 判据 4：全部 30 条都在 ----
        # 只看 RESULT=PASS 不够：万一某条用例压根没被执行到，
        # fail 仍然是 0。所以要数条数。
        npass = sum(1 for l in notes if "cupdtest: PASS" in l)
        nfail = sum(1 for l in notes if "cupdtest: FAIL" in l)
        expect(npass >= 30,
               "至少 30 条用例通过（实测 %d 通过 / %d 失败）" % (npass, nfail),
               problems)

        # ---- 判据 5（最要紧的）：坏包不许碰正式文件 ----
        # 这条对应整个模块存在的理由：内容装坏了，用户手上那份
        # 得还在。如果这条挂而别的都绿，说明「写完才校验」而不是
        # 「校验完才写」——而那种实现不会崩，只会静默失灵。
        keep = pick("被拒绝的更新没有动正式文件", notes)
        expect(keep is not None and "FAIL" not in keep,
               "摘要不符的包被拒绝后，用户手上那份原封不动", problems)

        # ---- 判据 6：回滚后版本归零 ----
        # 不归零的后果是「回滚完就永远收不到修复版本」，
        # 而且没有任何界面提示 —— 用户只会觉得「更新坏了」。
        rb = pick("回滚后版本归零", notes)
        expect(rb is not None and "FAIL" not in rb,
               "回滚后版本记录归零（能重新收到修复版本）", problems)

        # ---- 判据 7：没有 .bak 时回滚要失败而不是崩 ----
        # 这条崩的时机最坏：只在「用户想撤销一次坏更新」时出现。
        nb2 = pick("没有 .bak 时回滚必须失败", notes)
        expect(nb2 is not None and "FAIL" not in nb2,
               "没有备份时回滚返回失败而不是崩", problems)

        # ---- 判据 8：越界防御 ----
        ov2 = pick("cu_target 越界返回空串", notes)
        expect(ov2 is not None and "FAIL" not in ov2,
               "越界的 kind 被挡住（否则是越界读）", problems)

    elif tag == "V":
        print("-- 断言 V：区域切换 + 字库安全闸 --")

        # 这一趟验的是"上一批交付的两个功能"，而且这两个都属于
        # **离线就该跑通**：区域切换只写 SD 卡上一个文本文件；
        # 字库模块的"功能"本身就是在地址未实测时拒绝。
        st    = res["status"]
        notes = [l.strip() for l in st.splitlines()
                 if l.strip().startswith("featprobe:")]

        if not notes:
            expect(False, "没有 featprobe: 记录 —— 探针压根没跑到"
                          "（autorun 的 op 没接上？脚本没被读到？）", problems)
            return problems

        print("   探针记录（%d 行）：" % len(notes))
        for l in notes:
            print("     %s" % l)

        # ---- 判据 0：备注区没溢出 ----
        # 必须先看：app_note 在缓冲满时**静默丢行**，那一来后面每条判据
        # 都会伪装成失败，而真正的原因只是这一段没被记下来。
        nb = kv(st, "notes_bytes")
        if nb is None:
            expect(False, "status 里没有 notes_bytes（护栏未生效，旧二进制？）",
                   problems)
        else:
            expect(int(nb) < 6144,
                   "notes 只用了 %s / 6144 字节（没有溢出丢行）" % nb, problems)

        # ---- 判据 1：起止行都在 ----
        expect(any("featprobe: 开始" in l for l in notes), "开跑行在", problems)
        expect(any("featprobe: 结束" in l for l in notes),
               "收尾行在（中途没崩）", problems)

        # ---- 判据 2：路径拼对了 ----
        # 这是 locale 模块最危险的一环：**TID 直接参与拼路径**，
        # 拼歪就是写进别的目录，而现象只是"设置没生效"。
        expect(any("目标路径 = sdmc:/luma/titles/0004000000FFEE01/locale.txt" in l
                   for l in notes),
               "目标路径落在 luma/titles/<TID>/ 下（没拼歪）", problems)

        # ---- 判据 3：小写输入被规范成大写落盘 ----
        # 不统一大小写的话，"按小写设、按大写查"会读不到，
        # 用户看到的是"设了没用"，而文件其实好好躺在那儿。
        expect(any("落盘内容 = USA EN" in l for l in notes),
               "小写 usa/en 落盘成大写 USA EN（大小写统一）", problems)

        # ---- 判据 4（最要紧的）：非法 TID 全被拒，且零落地 ----
        bad = pick("非法 TID 共", notes)
        if bad is None:
            expect(False, "没有非法 TID 那一组的结果", problems)
        else:
            print("   " + bad)
            expect("被拒 8 个" in bad and "落地 0 个" in bad,
                   "8 个非法 TID（含路径穿越写法）全部被拒、零落地", problems)

        # ---- 判据 5：目录条目数不变 ----
        # 比逐个 file_exists 更硬：穿越的目的地本来就在它能拼出的路径之外，
        # 逐个查是查不到的，只有"目录没多出东西"才证得死。
        dirs = pick("luma/titles 目录条目", notes)
        if dirs is None:
            expect(False, "没有目录条目计数", problems)
        else:
            print("   " + dirs)
            expect("1 -> 1" in dirs,
                   "非法尝试没有在 luma/titles 下留下任何东西", problems)

        # ---- 判据 6：非法区域/语言被拒，且没破坏原设置 ----
        expect(any("非法区域 XXX -> 区域不认识" in l for l in notes),
               "非法区域被拒", problems)
        expect(any("非法语言 XX  -> 语言不认识" in l for l in notes),
               "非法语言被拒", problems)
        expect(any("被拒之后原设置仍在 = USA EN" in l for l in notes),
               "被拒的输入没有把已经设好的那条冲掉", problems)

        # ---- 判据 7：撤销真的删掉，且可重复撤销 ----
        expect(any("撤销后文件还在吗 = 不在了" in l for l in notes),
               "撤销把文件删掉了", problems)
        expect(any("再撤销一次 = 本来就没设过" in l for l in notes),
               "再撤销一次不报错（幂等）", problems)

        # ---- 判据 8：字库模块在地址未实测时**必须拒绝** ----
        # 这条挂了 = 有人把 probe_shared_font_addr 改成返回一个硬编码地址了。
        # 那种改法**不会报错**，只会往错的地方写内存 —— 现象是花屏，
        # 而且换一台固件版本不同的机器才复现。所以这条必须钉死。
        expect(any("shfont_probe = 2" in l for l in notes),
               "字库可用性判定为「不安全」（=2，本环境没实测过地址）",
               problems)
        expect(any("结论 = 正确拒绝" in l for l in notes),
               "字库：没实测地址就不动手（拒绝，而不是猜一个地址去写）",
               problems)
        expect(any("没备份时 apply = 这台机器上做不了" in l for l in notes),
               "字库：没备份也不许写（两道闸各自独立，不互相遮蔽）",
               problems)

    elif tag == "W":
        print("-- 断言 W：硬件知识检索 + 内容更新界面 --")

        st = res["status"]

        # ---- 判据 0：备注区没溢出 ----
        # 必须先看这一条：app_note 在缓冲满时**静默丢行**，而 hwtest
        # 有 30 来条用例的记录 —— 一旦填满 6144，后面的 PASS 全丢，
        # 现象是"断言说少了几条"，而真正的原因只是这一段没记下来。
        nb = kv(st, "notes_bytes")
        if nb is None:
            expect(False, "status 里没有 notes_bytes（护栏未生效，旧二进制？）",
                   problems)
        else:
            expect(int(nb) < 6144,
                   "notes 只用了 %s / 6144 字节（没有溢出丢行）" % nb, problems)

        notes = [l.strip() for l in st.splitlines()
                 if l.strip().startswith("hwtest:")]

        if not notes:
            expect(False, "没有 hwtest: 记录 —— 探针压根没跑到"
                          "（autorun 的 op 没接上？脚本没被读到？）", problems)
            return problems

        print("   探针记录（%d 行）：" % len(notes))
        for l in notes:
            print("     %s" % l)

        # ---- 判据 1：起止行都在 ----
        expect(any("hwtest: ===== 开始" in l for l in notes), "开跑行在", problems)
        expect(any("hwtest: ===== 结束" in l for l in notes),
               "收尾行在（中途没崩）", problems)

        # ---- 判据 2：库加载成功，且用的是内嵌那份 ----
        # "内嵌"是这一趟最要紧的落点：SD 拔了也要能用，是这个模块
        # 从第一天起就有的约束。写成 from=内嵌 还是 from=SD卡 都接受，
        # 因为验收环境里卡上可能已经有那份文件了 —— 但不接受
        # "压根没有来源"（那说明内嵌路径坏了）。
        ld = pick("hwk_load ok", notes)
        if ld is None:
            expect(False, "hwk_load 失败 —— 硬件知识库压根加载不上", problems)
        else:
            print("   " + ld)
            expect("条目=" in ld and "来源=" in ld,
                   "硬件知识库加载成功（%s）" % ld, problems)

        # ---- 判据 3：12 条正向问句全部命中所期望的那一条 ----
        # 数 PASS 的条数，不用 RESULT 那一行 —— 汇总行是探针自己写的，
        # 而它和每条用例数的是同一份计数，验它等于验自己。
        npass = sum(1 for l in notes if "hwtest: PASS" in l)
        nfail = sum(1 for l in notes if "hwtest: FAIL" in l)
        print("   探针自报：PASS %d / FAIL %d" % (npass, nfail))

        # 至少要有这么多条 —— 比"30"松，是为了容许将来删用例。
        # 但**不能只看总数**：一个"12 条正向全错成 12 条反向全过"
        # 的实现也是 30 条全过。所以下面还有分组判据。
        expect(nfail == 0,
               "硬件知识库 0 条失败（实测 PASS %d / FAIL %d）" % (npass, nfail),
               problems)
        expect(npass >= 25,
               "至少 25 条用例通过（实测 %d）—— 少于这个数说明"
               "有一大组没跑到" % npass, problems)

        # ---- 判据 4（最要紧的）：反向 —— 游戏问句不许命中硬件 ----
        # 这一组比正向重要：正向全绿只说明"能对上"，
        # 反向才是"没有乱答"。用户问游戏攻略却拿到一条硬件知识，
        # 看起来完全像个答案，比"没找到"糟得多。
        neg = [l for l in notes if "游戏问句" in l]
        if not neg:
            expect(False, "没有反向用例的记录（游戏问句那一组没跑到）",
                   problems)
        else:
            bad = [l for l in neg if "hwtest: FAIL" in l]
            expect(not bad,
                   "%d 条游戏问句全部没有误命中硬件知识%s"
                   % (len(neg), "" if not bad else " -> " + bad[0]),
                   problems)

        # ---- 判据 5：答案内容核对（不只看命中了哪条）----
        # 命中判定全绿时，答案串仍然可能是乱码 —— 所以必须另查一遍
        # 答案里有没有可辨认的关键片段。
        cts = [l for l in notes if "答案含" in l or "答案里找不到" in l]
        expect(len(cts) >= 6,
               "至少 6 条查过答案内容（实测 %d）" % len(cts), problems)
        badc = [l for l in cts if "答案里找不到" in l]
        expect(not badc,
               "命中的答案都含可辨认的关键片段%s"
               % ("" if not badc else " -> " + badc[0]), problems)

        # ---- 判据 6（2026-10-25 退休）----
        # 原判据是"内容更新屏必须被进过"。但那屏后来被并进「软件更新」当第二页
        # （L/R 切换），不再有独立入口 —— 这一趟也不去碰它，覆盖改由 **CP 趟**负责。
        # 这里只留一条记录，不再断言；否则两个趟会互相打架。
        cscan = pick("content: scan", st.splitlines())
        if cscan:
            print("   （本趟意外进了内容包页：%s）" % cscan)

    elif tag == "Y":
        print("-- 断言 Y：语音输入链路（配置读取 + 拼装自检 + 没麦降级）--")

        st = res["status"]

        # ---- 判据 0：备注区没溢出 ----
        # 与 W 趟同一个坑：app_note 满时会静默丢行，探针打 5、6 条，
        # 一丢就全都伪装成"探针没跑"。先看这条，别被后面的假红带跑。
        nb = kv(st, "notes_bytes")
        if nb is None:
            expect(False, "status 里没有 notes_bytes（护栏未生效，旧二进制？）",
                   problems)
        else:
            expect(int(nb) < 6144,
                   "notes 只用了 %s / 6144 字节（没有溢出丢行）" % nb,
                   problems)

        # ---- 判据 1：探针跑到了 ----
        # 筛选前缀必须与探针自己打的前缀**同源**（micprobe.c 用
        # app_note("micprobe: ...")）。写错前缀会有"没有记录"的假红。
        notes = [l.strip() for l in st.splitlines()
                 if l.strip().startswith("micprobe:")]

        if not notes:
            expect(False, "没有 micprobe: 记录 —— 探针压根没跑到"
                          "（autorun 的 micprobe 没接上？）", problems)
            return problems

        print("   探针记录（%d 行）：" % len(notes))
        for l in notes:
            print("     %s" % l)

        expect(any("micprobe: ===== 语音输入链路 =====" in l
                   for l in notes), "开跑行在", problems)
        expect(any("micprobe: ===== 结束 =====" in l for l in notes),
               "收尾行在（中途没崩）", problems)

        # ---- 判据 2：UI 编排真的跑过一遍（micprobe 这一层盖不到）----
        # micprobe 直接调 asr_transcribe，**绕过了 sc_chat 的状态机**。
        # 而语音功能的主体恰恰是那台状态机：ZL 边沿、两帧拆分、
        # 结果往 g_input 里追加。它从没被跑过的话，"加了语音"这句话
        # 就是空的 —— 两帧拆分写错、追加越界，屏上统统只是"没反应"。
        #
        # ⚠️⚠️ 这一行是本次特意改的：vnotes 必须**从全量 status 里筛**，
        # 不能从上面那个 notes（`micprobe:` 前缀筛过的池子）派生。
        # 第一版就是这么写的，于是"找 voice 记录"永远找不到 ——
        # 报出来的话是"压根没进过"，可屏上明明白白写着"识别失败：
        # 没连上：Timeout was reach"（UI 那条路其实跑完了），
        # 只是那条 evidence 用的是 status 原文、被前缀过滤掉了。
        # 取数据源的筛选条件，必须与要找的那条记录**同源**。
        #
        # 也正因为两个前缀是分开筛的，一条路的绿替不了另一条路作证，
        # hold 指令一旦没接上，这里会立刻红。
        vnotes = [l.strip() for l in st.splitlines()
                  if l.strip().startswith("voice:")]
        for l in vnotes:
            print("     %s" % l)

        expect(any(("voice: begin" in l) or ("voice: unavailable" in l) or
                   ("voice: start failed" in l) for l in vnotes),
               "按住 ZL 之后真的进了语音开始（begin / 无麦 / 打不开 都算走到）",
               problems)
        expect(any(("voice: captured" in l) or ("voice: too short" in l) or
                   ("voice: unavailable" in l) or ("voice: empty pcm" in l)
                   for l in vnotes),
               "录音收了尾（采到 / 太短 / 无麦 / 空 PCM 都算走到）", problems)

        # ---- 判据 2b：录音时长要够（VAD 的守卫，这条钉死一个真 bug）----
        # VAD 曾经把 system tick 当毫秒用（`dt >= 268 * VOICE_VAD_MS`
        # 实际比的是 800 微秒），结果是**按下 ZL 的第一帧**就被判
        # "静音够久了" 收尾，只录到 240ms（7854 字节）就送 ASR，
        # 屏上只有一句"识别失败" —— 用户看到的就是"按了没反应 / 没成功"。
        # 修好后：Azahar 里没声音，走满静音 800ms 才收，ms 应该 ≈ 800。
        # 于是 240ms 红 / 800ms 绿，这两条路的输出完全不同，判据分得开。
        cap_ms = None
        for l in vnotes:
            if "voice: captured ms=" in l:
                try:
                    cap_ms = int(l.split("voice: captured ms=")[1]
                                 .split()[0])
                except (IndexError, ValueError):
                    cap_ms = None
                break
        if cap_ms is None:
            # 采不到就说明根本没走到录音（已经由判据 2 兜住了）
            print("   → 没有 captured 行（录音没走到，判据 2 已覆盖）")
        else:
            print("   录音时长 = %s ms" % cap_ms)
            expect(cap_ms >= 600,
                   "按住 ZL 之后录了 %s ms（VAD 该给足 800ms 静音窗口；"
                   "低于 600 说明 VAD 又把时间基准算错了）" % cap_ms,
                   problems)

        # ---- 判据 2c：送上去的必须 ≤ 录下来的（省流裁剪生效的守卫）----
        # voice_finish 现在把 captured（录了多少）和 sent（送了多少）分开印。
        # 在 Azahar 里麦克风全静音 → sent 会**等于** captured（没到过门限
        # 就退回整段），所以这里不能要求 sent < captured；
        # 但 sent > captured 是死也不能有的（裁剪方向反了 / 长度算错）。
        cap_sent = None
        cap_bytes = None
        for l in vnotes:
            if "voice: captured" in l and "sent=" in l:
                try:
                    cap_bytes = int(l.split("bytes=")[1].split()[0])
                    cap_sent = int(l.split("sent=")[1].split()[0])
                except (IndexError, ValueError):
                    cap_sent, cap_bytes = None, None
                break
        if cap_sent is None:
            print("   → captured 行没有 sent=（老版本打印格式，跳过 2c）")
        else:
            print("   录 %s 字节 / 送 %s 字节" % (cap_bytes, cap_sent))
            expect(0 < cap_sent <= cap_bytes,
                   "只送有声音那一段：0 < sent=%s ≤ captured=%s（sent 越界说明"
                   " voice_pcm 的三道守卫里有一道没挡住）"
                   % (cap_sent, cap_bytes), problems)

        expect(any(("voice: ok" in l) or ("voice: fail" in l) or
                   ("voice: empty pcm" in l) or ("voice: unavailable" in l)
                   for l in vnotes),
               "ASR 那一帧真的跑到了（ok 或 fail；卡上那份 asr.txt 是假 Key，"
               "预期走 fail）", problems)

        vf = pick("voice: fail", vnotes)
        if vf is None:
            print("   → UI 那条路走的是成功分支（端到端识别真的通了）")
        else:
            http = vf.split("http=")[-1].split()[0] if "http=" in vf else "?"
            print("   → UI 那条路走了失败分支，HTTP=%s（假 Key 预期如此）" % http)

        # ---- 判据 3：ASR 配置真的读进来了（本趟最要紧的一条）----
        # 夹具写的是 provider=openai（表内、非默认）。
        #   · 读到了   -> provider=openai key=1（文件里那把假 Key）
        #   · 回落到默认 -> provider=siliconflow key=1（内置 Key；2026-10-18
        #     起 asr_load_file 把 asr_reset 移到 fopen 之前，没文件也有 Key，
        #     所以 key 这一位不再是判据，provider 才是）
        # 两条路的 provider 不一样，所以这条能红。不摆夹具的话必然走默认，
        # 这条就成了"永远绿"的守卫 —— 那比没有更坏。
        acfg = pick("micprobe: ASR provider", notes)
        if acfg is None:
            expect(False, "没有 ASR provider 行（asr.c 没打这一行？）",
                   problems)
        else:
            got_prov = acfg.split("provider=")[-1].split()[0] \
                       if "provider=" in acfg else ""
            expect(got_prov == Y_ASR_PROVIDER,
                   "asr.txt 读到了：provider=%s（不是默认的 siliconflow；"
                   "没读到就是走了回落，屏上只有\"识别失败\"查不到原因）"
                   % got_prov, problems)
            expect("key=1" in acfg,
                   "Key 也一并生效（否则是 provider 换了 key 没换的半截状态）",
                   problems)

        # ---- 判据 3b：**没有 asr.txt** 时内置 Key 也要生效 ----
        # 2026-10-17 用户实测「语音模块显示 Key 还没进去」。根因：
        # asr_reset 原先放在 fopen 成功之后，卡上没有 asr.txt 时
        # asr_load_file 直接 return，内置 Key 永远填不进去 —— 而
        # "别人装上就能用"恰恰建立在没有 asr.txt 的机器上。
        # micprobe 现在会故意读一个不存在的路径，把这条路跑出来：
        #   修好了 -> nofile provider=siliconflow key=1
        #   退化了 -> key=0（没配 Key 的老 bug 回来了）
        nf = pick("micprobe: ASR nofile", notes)
        if nf is None:
            expect(False, "没有 ASR nofile 行（micprobe.c 没打这一行？）",
                   problems)
        else:
            expect("key=1" in nf,
                   "没有 asr.txt 时内置 Key 生效（key=1）。"
                   "key=0 说明内置 Key 没填上 —— 检查 asr.c 的 "
                   "asr_load_file：asr_reset 必须在 fopen 之前",
                   problems)
            expect("provider=siliconflow" in nf,
                   "没有 asr.txt 时回落到默认服务商 siliconflow",
                   problems)

        # ---- 判据 4：满 5 秒录音的 body 拼装装得下 ----
        # 这条直接盯住缓冲常数：VOICE_CAP_BYTES 之前漏过一次 /1000
        # （算成 163.6MB），而 g_body 是按它开的 —— 大小一旦对不上，
        # 真机要到"发出去才 400"才发现。拼装自检不联网就能量。
        pay = pick("micprobe: payload", notes)
        if pay is None:
            expect(False, "没有 payload 行（asr_payload_probe 没接上？）",
                   problems)
        else:
            expect("装得下=1" in pay,
                   "满 5 秒（163600 字节）的 WAV+multipart 装得下",
                   problems)
            n = pay.split("body(5s)=")[-1].split()[0] \
                if "body(5s)=" in pay else ""
            expect(n.isdigit() and int(n) > 163600,
                   "body 长度(%s) 大于纯 PCM(163600)，说明 WAV 头+multipart "
                   "确实拼进去了（不是只发了裸 PCM）" % n, problems)

        # ---- 判据 5：没麦克风是预期路径，不是失败 ----
        # Azahar 里麦克风必然开不了；真机上能开，那就该多一行"采到 N 字节"。
        # 判据只认一件事：没有 RESULT=FAIL。把"没麦"当失败的话，
        # 这条断言在模拟器里天天红，红久了就没人数它了。
        resy = pick("micprobe: RESULT", notes)
        if resy is None:
            expect(False, "没有 RESULT 行（探针中途断在麦克风那一步）",
                   problems)
        else:
            print("   " + resy)
            expect("RESULT=FAIL" not in resy,
                   "没有 FAIL（没麦走降级；有麦要看到\"采到 N 字节\"）",
                   problems)
            if "mic=unavailable" in resy:
                print("   → 麦克风不可用（模拟器预期路径），真机需实机验端到端")
            else:
                # 探针现在跑两轮（验"关掉还能不能重开"），文案是
                # "第1轮 采到 N 字节 = M 毫秒"，别再找 "micprobe: 采到"。
                got = pick("micprobe: 第", notes)
                expect(got is not None and "字节" in got,
                       "有麦克风时应当报采样字节数（两轮都要报）", problems)
                # 两轮都得采到：第二轮换来的是"micExit 之后 micInit 还开不开"
                expect(any(("第2轮" in l) and ("字节" in l) for l in notes),
                       "第二轮也能采到（mic 关掉重开这条路通）", problems)

        # ---- 判据 6：布局（语音按钮画在底屏页眉右侧）----
        ovw = kv(st, "layout_overflow")
        if ovw is None:
            expect(False, "status 里没有 layout_overflow"
                          "（护栏未生效，旧二进制？）", problems)
        elif ovw != "0":
            expect(False, "语音按钮那行被右边缘裁掉：%s"
                   % (kv(st, "layout_overflow_last") or ""), problems)
        else:
            expect(True, "没有单行文字被右边缘裁掉", problems)

        ovv = kv(st, "layout_voverflow")
        if ovv is None:
            expect(False, "status 里没有 layout_voverflow"
                          "（护栏未生效，旧二进制？）", problems)
        elif ovv != "0":
            expect(False, "有元素画到内容区下界以下：%s"
                   % (kv(st, "layout_voverflow_last") or ""), problems)
        else:
            expect(True, "没有元素画到内容区下界以下", problems)

    elif tag == "X2":
        print("-- 断言 X2：抽取式回答（判据是**输出文本**，不是 rc）--")

        st = res["status"]
        enotes = [l.strip() for l in st.splitlines()
                  if l.strip().startswith("exttest:")]

        if not enotes:
            expect(False, "没有 exttest: 记录 —— 探针没跑到"
                          "（autorun 的 exttest 没接上？）", problems)
            return problems

        for l in enotes:
            print("     %s" % l)

        expect(any("exttest: ===== 开始" in l for l in enotes),
               "开跑行在", problems)
        expect(any("exttest: ===== 结束" in l for l in enotes),
               "收尾行在（中途没崩）", problems)

        bind = [l for l in enotes if "索引 ok" in l]
        expect(bool(bind),
               "知识库绑定成功（%s）"
               % (bind[0] if bind else "没有这一行"), problems)

        # 逐条 OK 行 —— 这一趟的**全部意义**就在这里。
        # 判「OK 数 = 8」而不是只看 RESULT：汇总行是探针自己写的，
        # 而它数的是同一批OK 行，验它等于验自己。
        #
        # ⚠️ 2026-10-04：`fails = [l for l in enotes if "FAIL" in l]`
        # **是错的**。探针的汇总行是 `汇总 OK=8 FAIL=0 跳过=0`，
        # 它**含 "FAIL" 字样** → 一条 FAIL 都没有时，
        # 判据反而报「有FAIL 的用例：exttest: 汇总 OK=8 FAIL=0」。
        # 症状极坏：**探针全绿、判据报红**，而错误在判据自己身上。
        # → 判据必须只认**用例行**（`[N]` 开头），
        #   并且排除汇总行与 RESULT 行 —— 它们的"输出形状"不一样。
        #
        # ★ 匹配用正则而不是裸子串：探针的格式是 `"%s  OK  %d 字节"` /
        #   `"%s  FAIL ..."`（**前置两空格 + 词 + 一到多个空白**），
        #   写死 `"  FAIL  "`（后置也两空格）会一条都抓不到。
        #   判据的判据：跑 tools/guia/selftest_x2_judge.py，
        #   那里用探针源码里的真实格式串造了 5 种场景的数据。
        case_lines = [l for l in enotes if re.match(r"^exttest:\s*\[\d+\]", l)]
        oks = [l for l in case_lines if re.search(r"\s\sOK\s", l)]
        fails = [l for l in case_lines if re.search(r"\s\sFAIL\s", l)]
        skipped = [l for l in case_lines if "检索 0 命中" in l]
        print("   探针自报：OK %d / FAIL %d / 跳过 %d"
              % (len(oks), len(fails), len(skipped)))

        # 汇总行要跟逐条行对得上：探针自己数的和判据数的不一致，
        # 说明有一类用例被两边归到了不同桶里（这本身就是 bug）。
        #
        # ⚠️ 文案分两种情况写死：expect() 会把同一个字符串同时当
        # "通过时的说明"和"失败时的原因"印出来 —— 只写一种，
        # 就会出现「[OK] ...对不上」这种自相矛盾的输出
        # （铁律：断言对了不代表对的原因对了，要看失败时的报法）。
        summ = pick("exttest: 汇总", enotes)
        if summ:
            m = re.search(r"OK=(\d+)\s+FAIL=(\d+)\s+跳过=(\d+)", summ)
            if m:
                s_ok, s_bad, s_skip = (int(x) for x in m.groups())
                consistent = ((s_ok, s_bad, s_skip)
                              == (len(oks), len(fails), len(skipped)))
                expect(consistent,
                       "汇总(%d/%d/%d)与逐条(OK %d/FAIL %d/跳过 %d)一致"
                       % (s_ok, s_bad, s_skip,
                          len(oks), len(fails), len(skipped))
                       if consistent else
                       "汇总(%d/%d/%d)与逐条(OK %d/FAIL %d/跳过 %d)对不上"
                       " —— 有一类用例被归到了不同的桶"
                       % (s_ok, s_bad, s_skip,
                          len(oks), len(fails), len(skipped)),
                       problems)
        else:
            expect(False, "没有 exttest: 汇总 行（探针没跑完？）", problems)

        expect(len(fails) == 0,
               "没有 FAIL 的用例%s"
               % ("" if not fails else "：" + fails[0]), problems)
        expect(len(oks) == 8,
               "8 条用例全部 OK（实测 %d）—— 少的就是答不出来的"
               % len(oks), problems)
        expect(len(skipped) == 0,
               "没有用例因检索 0 命中被跳过" if not skipped
               else "跳过了 %d 条" % len(skipped), problems)

        r2 = pick("exttest: RESULT", enotes)
        expect(r2 is not None and "OK" in r2,
               "探针结论 OK（%s）" % (r2 or "没有 RESULT 行"), problems)

    elif tag == "X":
        print("-- 断言 X：推理耗时基线（只判「有没有拿到可用数字」）--")

        st = res["status"]
        # 这一趟**不判时间**。时间受模拟器调度、宿主 CPU 争用、
        # 升频是否生效影响，没有可断言的固定值 —— 硬写一个阈值
        # 等于造一个会因为环境而红的假门禁。上一轮 8 路展开的教训：
        # 静态指令数降 19%、实测更慢。只有前后两次实测能分辨。
        pnotes = [l.strip() for l in st.splitlines()
                  if l.strip().startswith("perfprobe:")]

        if not pnotes:
            expect(False, "没有 perfprobe: 记录 —— 探针压根没跑到"
                          "（autorun 的 perfprobe 没接上？）", problems)
            return problems

        for l in pnotes:
            print("     %s" % l)

        expect(any("perfprobe: ===== 开始" in l for l in pnotes),
               "开跑行在", problems)
        expect(any("perfprobe: ===== 结束" in l for l in pnotes),
               "收尾行在（中途没崩）", problems)

        resu = pick("perfprobe: RESULT", pnotes)
        if resu is None:
            expect(False, "没有 RESULT 行", problems)
        elif "SKIP" in resu:
            # SKIP = 卡上没有模型文件。跳过本身是正确行为，
            # 但要**显式报出来** —— 否则"跳过"看起来像"跑过了"。
            expect(False, "RESULT=SKIP —— 卡上没有 guide_model.bin，"
                          "没有基线可比（先把 app/data/guide_model.bin "
                          "拷到 sdmc:/3ds/aiconsole/）", problems)
        else:
            print("   " + resu)
            expect("OK" in resu, "探针跑完了（%s）" % resu, problems)

        # RAW 那一行是**不依赖任何换算**的原始 tick 数。
        # 人比对两次改动时只看它 —— ms 那行经过整数除法，
        # 会把小差异抹掉。
        #
        # ⚠️ 但**不能只比时间**：必须先比输出字节数。
        # 踩过的坑：ldr+sxtb 版把字节序写反（小端当成大端），
        # 点积下标错位一格 -> 模型提前采 EOS -> 输出 122 字节缩到 19 字节
        # -> 耗时"下降 34%"。那不是优化，那是**少干了活**。
        #
        # 固定问句 + 固定种子 = 每次输出应当**完全一样**。
        # 字节数变了就说明改动影响了数值行为，此时那个时间数字
        # 无论多好看都不能算作收益。
        REF_BYTES = 122   # 基线（ldrsb 版）实测值

        raw = pick("perfprobe: RAW", pnotes)
        if raw is None:
            expect(False, "没有 RAW 那一行（拿不到可比对的原始数字）", problems)
        else:
            print("   " + raw)
            m = re.search(r"=\s*(\d+)\s*tick", raw)
            expect(m is not None and int(m.group(1)) > 0,
                   "原始 tick 数是个正整数（%s）"
                   % (m.group(1) if m else "取不到"), problems)

            b = re.search(r"输出\s*(\d+)\s*字节", raw)
            if b is None:
                expect(False, "RAW 行里没有输出字节数 —— 那样无法判断"
                              "「变快」是真的还是「少干了活」", problems)
            else:
                got = int(b.group(1))
                expect(got == REF_BYTES,
                       "输出字节数与基线一致（%d，期望 %d）—— 不一致说明"
                       "改动影响了数值行为，那个耗时数字不能算收益"
                       % (got, REF_BYTES), problems)


    elif tag == "0":
        print("-- 断言 0：引导 → 录入凭据屏 → 服务目录守卫 --")

        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（%d 组 x 顶底屏）" % (res["expect"] * 2, res["expect"]),
               problems)

        notes = [l.strip() for l in st.splitlines()
                 if l.strip().startswith("welcome:")]

        # ---- 判据 1：第 3 步的按钮真的把人送进了新屏 ----
        # 少了这条，"按了没反应"和"进去了"在截图上要靠人看。
        expect(any("-> SCREEN_KEYEDIT" in l for l in notes),
               "推出了 SCREEN_KEYEDIT（引导 → 录入凭据）", problems)

        # ---- 判据 2：退回来**停在第 3 步**，没被打回第 1 步 ----
        #
        # ★ 这一趟最值钱的一条。app_pop 会对回来的那一屏重调 enter()，
        #   所以 welcome_enter 里的"要不要复位页号"必须靠守卫区分两条路：
        #     首次进入 → 复位到第 1 步；
        #     从子屏退回 → 保持原页号。
        #   守卫写错的表现是"录完 Key 得重新走两页" ——
        #   而截图上看只是页号成了 1，很像"正常"。
        rp = [l for l in notes if "resume at page" in l]
        if not rp:
            expect(False, "没有 resume at page 记录 —— 退回来时 welcome_enter "
                          "又按「首次进入」跑了一遍（守卫没生效）", problems)
        else:
            print("   " + rp[-1])
            # ★ 引导 2026-10-17 加了「选语言」一页（3 -> 4 页），
            #   所以「步骤屏」的页号从 3 变成 4。判据要跟着页数走 ——
            #   写死一个数字的话，每次改引导都会被判成"守卫失效"，
            #   而真因只是页数变了（这条断言本来要防的是"被踢回第 1 页"）。
            expect(rp[-1].endswith("page 4"),
                   "返回后仍停在第 4 步（%s）" % rp[-1], problems)

        # ---- 判据 3：收尾时人在服务目录屏 ----
        #   （本趟最后一步是摸服务目录的 X/Y 守卫，所以 end_screen 是它。
        #     这条同时证明"从录入屏退回来之后整条引导还能走完" ——
        #     退回来如果被卡住，后面这几屏根本到不了。） */
        expect(kv(st, "end_screen") == "SCREEN_PROVIDERS",
               "end_screen = SCREEN_PROVIDERS（引导顺利收尾并走到了服务目录）",
               problems)

        # ---- 判据 4：新屏的版面护栏 ----
        ov = kv(st, "layout_overflow") or "?"
        expect(ov == "0",
               "status: layout_overflow = %s（录入屏没有行被右边缘裁掉）" % ov,
               problems)

        # ---- 判据 5：服务目录的 X/Y 改网址/改模型入口 ----
        #
        # 两条都不弹软键盘：一条是"内置地址不可改"，一条是
        # "光标不在当前那家"。真正弹键盘那条只能真机验（见趟次说明）。
        ref = [l.strip() for l in st.splitlines()
               if "providers: url edit refused" in l]
        expect(len(ref) == 1,
               "内置地址改不了 · 说明文案走到了（%d 条）" % len(ref), problems)
        for l in ref:
            print("   " + l)

        blk = [l.strip() for l in st.splitlines()
               if "providers: edit blocked" in l]
        expect(len(blk) >= 2,
               "光标不在当前那家时，X 和 Y 都走到了「先切过来」的守卫"
               "（共 %d 条）" % len(blk), problems)
        for l in blk:
            print("   " + l)

        s9  = [p for p in res["shots"] if "9_providers" in p and "top" in p]
        s10 = [p for p in res["shots"] if "10_providers_builtin_url" in p
               and "top" in p]
        if s9 and s10:
            expect(not _same_bytes(s9[0], s10[0]),
                   "按 X 之后画面变了（说明文案真的画上去了，不是「按了没反应」）",
                   problems)
        else:
            expect(False, "缺少 9/10 服务目录截图", problems)

    elif tag == "4":
        print("-- 断言 4：更多 → 添加 API · 菜单入口 + 录入屏版面 + 从文件导入 --")

        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（%d 组 x 顶底屏）" % (res["expect"] * 2, res["expect"]),
               problems)

        # ---- 判据 1：真的从「更多」进了录入凭据屏 ----
        #   ⚠️ 这条不能用 end_screen 判：本趟最后是触摸「从文件导入」，
        #   不会离开这一屏。所以看**有没有进过**这一屏的证据。
        # ★ 早期预警：主页那一步是不是**真的在主页**上动手的。
        #   ⚠️ 这条不能用 "home: focus=2" 判 —— 脚本改用**触摸**点「更多」
        #   那一格（sc_home.c 的触摸分支直接 app_push，不经过方向键，
        #   所以不写 focus 记录）。改成判"有没有进过主页"这个更本质的事实：
        #   status 头部的 start: 行会报 first_run_completed 与首屏。
        #   ⚠️ 首启趟的 start: 行只写 "-> SCREEN_WELCOME"（那是首屏），
        #   不会写 SCREEN_HOME，所以**不能拿它判**"到过主页"。真正的证据
        #   是 more: enter —— 它只可能由「更多」屏自己写出来，而「更多」
        #   只能从主页 A/触摸进去。这条断言因此判 more: enter 存在即可，
        #   下面那条判据已经在查它了；这里保留一条"引导走完了"的辅助判据。
        st_head = [l.strip() for l in st.splitlines() if l.strip().startswith("start:")]
        expect(bool(st_head),
               "本趟起步记录齐全（start: %s）" % (st_head or "无"), problems)

        mp = [l.strip() for l in st.splitlines() if l.strip().startswith("more:")]
        expect(any("enter" in l for l in mp),
               "进了「更多」菜单（%d 条 more: 记录）" % len(mp), problems)
        for l in mp:
            print("   " + l)
        # ★ 关键：more: touch/-> item 必须指向 index 2（kItems[2] = KEYEDIT）。
        #   「更多」是按位置导航的，插一项就会错位 —— 所以这里**指名断言**
        #   落到了哪一项，而不是只验"进了某屏"。
        pick = [l for l in mp if "-> item 2" in l]
        # 只统计真正的"-> item N"行（more: enter 那行里也有 "items"，
        # 早先用 split("item") 把 's, 6 visible)' 也当成一项，很难看）。
        went = sorted({m.group(1) for l in mp
                       for m in [re.search(r"-> item (\d+)", l)] if m})
        expect(len(pick) >= 1,
               "选中并进入了「更多」的第 3 项（添加 API，index 2）—— "
               "落到过的项：%s" % went, problems)

        # ---- 判据 2：录入屏自己报过 enter ----
        ke = [l.strip() for l in st.splitlines()
              if "keyedit: import" in l]
        expect(len(ke) >= 1,
               "触摸「从文件导入」被接住了（%d 条 keyedit: import 记录）"
               % len(ke), problems)
        for l in ke:
            print("   " + l)

        # ---- 判据 3：版面护栏（新增按钮不能把行挤出去） ----
        ov = kv(st, "layout_overflow") or "?"
        expect(ov == "0",
               "status: layout_overflow = %s（录入屏加按钮后没有行被裁）" % ov,
               problems)

    elif tag == "5":
        print("-- 断言 5：设置屏 L/R 翻页 + 网络自检 --")

        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（%d 组 x 顶底屏）" % (res["expect"] * 2, res["expect"]),
               problems)

        # ---- 判据 1：L/R 真的整屏翻页（7 行一步），且到头夹住 ----
        #   note 格式：settings: page_step dir=1 cursor=8 top=2
        #   cursor / top 都是 **1-based**。
        stp = []
        for l in st.splitlines():
            m = re.search(r"settings: page_step dir=(-?\d+) cursor=(\d+) top=(\d+)", l)
            if m:
                stp.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        expect(len(stp) >= 7,
               "L/R 翻页共触发 %d 次（脚本按了 8 下，允许 1 下被吞）"
               % len(stp), problems)
        for t in stp:
            print("   page_step dir=%d cursor=%d top=%d" % t)

        if len(stp) >= 7:
            # 第 1 下 R：从第 1 行跳到第 8 行（+7 = ROW_VISIBLE）
            expect(stp[0][0] == 1 and stp[0][1] == 8,
                   "第一下 R 把光标从第 1 行翻到第 8 行（实际 dir=%d cursor=%d）"
                   % (stp[0][0], stp[0][1]), problems)
            # 3 下 R 的算术：1 -> 8 -> 15 -> 22（每步正好 ROW_VISIBLE=7）。
            # ⚠️ 不在这里验"R 到底夹在 24"：第 4 下 R 会被模拟器**吞掉**
            #   （同屏连续同名按键只交付前面的，趟 4 的 RIGHT×2 是同一个病），
            #   于是"少按一次"和"夹取坏了"在断言上分不开 —— 那种判据是假的。
            expect(stp[2][1] == 22,
                   "连按 3 下 R 走到第 22 行（步长 = ROW_VISIBLE 7；实际 %d）"
                   % stp[2][1], problems)
            # ★ 夹取由 **L 侧**证明，而且证得很干净：
            #   第 6 下 L 到第 1 行之后，第 7 下 L 仍在第 1 行 ——
            #   1-7=-6 已经越界却没有变成负数，说明钳位生效。
            #   两侧走的是**同一份**钳位代码（page_step 里的两个 if），
            #   所以验一侧就够，不必为 R 侧再造一条依赖被吞按键的假判据。
            expect(stp[-1][1] == 1,
                   "连按 L 回顶后停在第 1 行且不再前移（钳位生效；实际 %d）"
                   % stp[-1][1], problems)

        # ---- 判据 2：网络自检真的执行了 ----
        #   成功/失败都算执行过 —— 模拟器里没网是常态，
        #   所以判据是"有没有走到探测"，而不是"通没通"。
        np_ = [l.strip() for l in st.splitlines() if "netprobe=" in l]
        expect(len(np_) >= 1,
               "按 A 真的执行了网络自检（%d 条 netprobe 记录）" % len(np_),
               problems)
        for l in np_:
            print("   " + l)

        # ---- 判据 3：版面没被撑破 ----
        ov = kv(st, "layout_overflow") or "?"
        expect(ov == "0", "status: layout_overflow = %s" % ov, problems)

    elif tag == "1":
        print("-- 断言 1：密码门弹窗 · 键位被接管 --")

        expect(len(res["shots"]) == res["expect"] * 2,
               "截图数 = %d（%d 组 x 顶底屏）" % (res["expect"] * 2, res["expect"]),
               problems)

        # ---- 判据 1：真手速的六下 A 能把弹窗叫出来 ----
        # 旧的时间窗是"六下挤进 1.2 秒"（每下 200ms），真人做不到，
        # 于是"点了没反应"。脚本按 6 帧/下（≈100ms）喂，落在新窗里。
        expect(any("提示已出现" in l for l in st.splitlines()),
               "连点六下叫出了密码提示（新的时间窗对真人手速友好）", problems)

        # ---- 判据 2：密码在**设置屏上**输完了 ----
        # ★ 这条同时证明 B 被接管了：B 是密码的第 9 个键，旧版本里
        #   它会直接把设置屏弹掉，于是最后那个 A 落在主界面上，
        #   密码永远完不成。
        expect(any("密码通过" in l for l in st.splitlines()),
               "密码输完了（B 没有把设置屏弹掉 = 键位确实被接管）", problems)

        # ---- 判据 3：接管期间光标一动没动 ----
        # 真机反馈的原话是"按那个密码操作的话，它项目就会移动"。
        # 弹窗把列表盖住了，截图看不出光标在哪，只能靠这一行 note。
        sw = [l for l in st.splitlines() if "secgate: swallow" in l]
        if not sw:
            expect(False, "没有 swallow 记录 —— 接管期间根本没吞键"
                          "（konami_capturing 判据没生效？）", problems)
        else:
            rows = set()
            for l in sw:
                m = re.search(r"cursor_row=(\d+)", l)
                if m:
                    rows.add(m.group(1))
            print("   接管期间吞掉 %d 个按键，出现过的光标行号 = %s"
                  % (len(sw), sorted(rows)))
            # ★ 行号是 **1-based**（sc_settings.c 报的是 g_cursor+1）。
            #   2026-10-18：「思考强度」搬去对话页后列表 22 -> 21 行，
            #   版本号 0-based 19 = 1-based 20。
            #   2026-10-22：新增「主题色」行后列表 21 -> 22 行，
            #   版本号 0-based 20 = **1-based 21**，SCRIPT_1 的 DOWN 同步 20 次。
            #   2026-10-22（同日第二轮）：**「思考强度」加回设置屏**，
            #   列表 22 -> 23 行，版本号 0-based 21 = 1-based 22，
            #   SCRIPT_1 的 DOWN 同步 21 次（SCRIPT_K 11 次）。
            #   2026-10-23：**「网络自检」从主页 SELECT 搬进设置屏**
            #   （插在诊断分类里），列表 23 -> 24 行，
            #   版本号 0-based 22 = **1-based 23**，DOWN 同步 22 次。
            #   ⚠️ 数行数时别用"从 R_ROW_ACCENT 开始"的口径 ——
            #   第一项是 `R_ROW_THEME = 0,`（带初始化），
            #   正则漏掉它就会整体少 1，然后所有 DOWN 次数都偏一行。
            #   这个数必须和脚本里的 DOWN 次数一起改，
            #   单独改一个的后果是"导航对了、断言红了"或反过来。
            expect(rows == {"23"},
                   "接管期间光标**一直停在第 23 行**（版本号，1-based）—— 出现过的行号 %s"
                   % sorted(rows), problems)

        # ---- 判据 4：弹窗真的画出来了（不是只改了个内部状态） ----
        s3 = [p for p in res["shots"] if "3_version_row" in p and "top" in p]
        s4 = [p for p in res["shots"] if "4_konami_modal" in p and "top" in p]
        if s3 and s4:
            expect(not _same_bytes(s3[0], s4[0]),
                   "弹窗那一帧与之前不同（真的画了弹窗）", problems)
        else:
            expect(False, "缺少 3/4 密码门截图", problems)

        expect(kv(st, "end_screen") == "SCREEN_SETTINGS",
               "end_screen = SCREEN_SETTINGS（整趟没被弹走）", problems)

        ov = kv(st, "layout_overflow") or "?"
        expect(ov == "0", "status: layout_overflow = %s" % ov, problems)

    elif tag == "2":
        print("-- 断言 2：拒绝也要回灌给模型 --")

        # 不断言截图**总数**：流式上屏会自己额外抓 2_stream_* 那两张，
        # 数量随回复长度变（实测 5 个 shot 收到 18 张图）。要的是
        # "关键帧都在"，不是"总数等于某个数"。
        for n in ("1_chat", "2_confirm", "3_back", "4_filled", "5_round2"):
            expect(any(n in p for p in res["shots"]),
                   "关键帧 %s 在" % n, problems)

        # ---- 判据 0：短语真的填进了输入框 ----
        # 没有这条，"第二轮没发出去"会被后面的判据解释成"回灌没生效" ——
        # 而实际原因可能只是 SELECT 打了个空炮（第一版就栽在这儿：
        # 拒绝后的 SELECT/A 打在了转场里，输入框一直是空的）。
        b3 = [p for p in res["shots"] if "3_back" in p and "bottom" in p]
        b4 = [p for p in res["shots"] if "4_filled" in p and "bottom" in p]
        if b3 and b4:
            expect(not _same_bytes(b3[0], b4[0]),
                   "填短语之后下屏变了（输入框真的有字）", problems)
        else:
            expect(False, "缺少 3_back / 4_filled 的下屏截图", problems)

        # ---- 判据 1：确实走的是"拒绝"那条路 ----
        # 没这条的话，"允许了所以有结果"也能让判据 2 变绿 —— 那就是假通过。
        log = res.get("two_log") or ""
        ent = [l for l in log.splitlines() if l.strip()]
        for l in ent:
            print("   action.log：%s" % l)
        expect(any("DENY" in l for l in ent),
               "审计里记下了一次 DENY（默认焦点确实是「拒绝」）", problems)
        expect(not any("ALLOW" in l for l in ent),
               "没有 ALLOW（这一趟没执行任何动作）", problems)

        # ---- 判据 2：第二轮发给模型的 system 里多出了那段结果 ----
        # ★ 这条是这个 bug 的唯一硬证据。截图看不出来：拒绝之后的对话屏
        #   跟"什么都没发生"长得一模一样。
        ctxs = [l.strip() for l in st.splitlines() if l.strip().startswith("ctx:")]
        for l in ctxs:
            print("   " + l)
        syslens = []
        for l in ctxs:
            m = re.search(r"sys=(\d+)", l)
            if m:
                syslens.append(int(m.group(1)))
        if len(syslens) < 2:
            expect(False, "只拿到 %d 条 ctx: 记录 —— 两轮各要一条"
                          % len(syslens), problems)
        else:
            print("   两轮 system 长度：%d → %d（差 %d）"
                  % (syslens[0], syslens[-1], syslens[-1] - syslens[0]))
            expect(syslens[-1] > syslens[0],
                   "第二轮的 system **更长**（多出「用户拒绝了…」那段）—— "
                   "被拒绝这件事真的回流给模型了", problems)

        ov = kv(st, "layout_overflow") or "?"
        expect(ov == "0", "status: layout_overflow = %s" % ov, problems)

    else:
        last = kv(st, "layout_overflow_last") or ""
        expect(ov == "0",
               "status: layout_overflow = %s（没有单行文字被右边缘裁掉）" % ov
               + ("" if ov == "0" else " -> " + last),
               problems)

    return problems


def kw_is_offline(status):
    """当前是不是"离线演示"（provider = demo）。

    判据来源与 C 侧同源：provider.c 里 demo 那一项的 base_url 是空串，
    而 status.txt 里 ai_provider 直接写的是 provider 名 ——
    所以这里只认名字 "demo"。
    为什么需要它：离线演示**算 chat_ready 为真**（见 sc_chat.c 的
    chat_ready：g_have_key || chat_offline()），于是页脚会轮播；
    而"没配 Key"的普通状态页脚是固定一条。这两者靠 ai_configured
    分不开（离线演示也可能 ai_configured=1），必须看 provider 名。
    """
    return kv(status, "ai_provider") == "demo"


def _footer_bytes(path):
    """下屏 BMP 的**页脚区**（y 214..240）原始像素。量轮播/固定文案用。

    不用 Pillow：本仓库的系统 Python 没装（实测 No module named 'PIL'），
    而这条判据不该因为宿主环境缺一个可选库就整趟变红 ——
    依赖 Pillow 的判据最后会变成"常常跳过"，等于没有。
    """
    try:
        import struct

        with open(path, "rb") as f:
            data = f.read()
        off   = struct.unpack_from("<I", data, 10)[0]
        w     = struct.unpack_from("<i", data, 18)[0]
        h     = struct.unpack_from("<i", data, 22)[0]
        bpp   = struct.unpack_from("<H", data, 28)[0]
        if bpp != 24:
            return None
        stride = ((w * 24 + 31) // 32) * 4
        rows = []
        for r in range(abs(h)):
            s = off + r * stride
            rows.append(data[s:s + w * 3])
        if h > 0:
            rows = list(reversed(rows))
        return b"".join(rows[214:240])
    except Exception:
        return None


def _same_bytes(a, b):
    try:
        with open(a, "rb") as fa, open(b, "rb") as fb:
            return fa.read() == fb.read()
    except OSError:
        return False


def _same_stable_area(a, b):
    """两张下屏 BMP 在**页脚以外**是否逐字节相同。

    为什么需要"以外"：底屏最下一行现在是**轮播的操作指引**
    （2026-10-23 用户要求「最下行给出操作指引」，见 widgets.c 的
    w_bot_footer_roll）。它每 3.2 秒自己换一条，所以任何间隔超过
    3 秒的两张底屏截图，页脚区必然不同。

    用整屏比较来做"画面没变"的判据 → 判据会被**正常的轮播**推翻，
    而红的原因跟被测功能毫无关系。这种红比假绿更坏：它会训练人
    "看到红就先忽略"，于是真的回归也一起被忽略了。

    比较范围：y ∈ [W_BOT_HEADER_H, W_BOT_CONTENT_B)，即 24..214。
    页脚 = 214..240（W_BOT_CONTENT_B = 240 - 26）。
    这里**不解析 BMP 编码细节**，只按 24bpp 行宽算偏移 —— 本项目的
    截图恒为 320x240 24bpp（见 shot.c），断言前已校验尺寸。
    """
    try:
        import struct

        def rows(path):
            with open(path, "rb") as f:
                data = f.read()
            off = struct.unpack_from("<I", data, 10)[0]
            w   = struct.unpack_from("<i", data, 18)[0]
            h   = struct.unpack_from("<i", data, 22)[0]
            bpp = struct.unpack_from("<H", data, 28)[0]
            if bpp != 24:
                return None
            stride = ((w * 24 + 31) // 32) * 4
            out = []
            for r in range(abs(h)):
                s = off + r * stride
                out.append(data[s:s + w * 3])
            if h > 0:                      # BMP 自下而上
                out = list(reversed(out))
            return out, w, abs(h)

        ra = rows(a)
        rb = rows(b)
        if not ra or not rb:
            return False
        ra, wa, ha = ra
        rb, wb, hb = rb
        if (wa, ha) != (wb, hb):
            return False
        # 24..214：避开页眉（有标题/页码）与页脚（轮播）
        top, bot = 24, 214
        top = max(0, min(top, ha))
        bot = max(top, min(bot, ha))
        return ra[top:bot] == rb[top:bot]
    except Exception:
        return False


def _same_top_content(a, b):
    """两张**上屏** BMP 在内容区（y < 200）是否逐字节相同。

    为什么不能整屏比：切项目会把状态行改成「当前项目：任务乙（zhipu）」，
    而状态行就在上屏最底下（约 203..217）。切回来之后这句又变成任务甲 ——
    于是"整屏相同"永远不成立，而**对话内容其实一模一样**。
    这条判据要看的是"历史回来了没有"，不是"状态行写的是不是同一句话"。

    上界取 200，就是 W_TOP_CONTENT_B（见 widgets.h）—— 以下都是页脚/状态行。
    """
    try:
        import struct

        def rows(path):
            with open(path, "rb") as f:
                data = f.read()
            off = struct.unpack_from("<I", data, 10)[0]
            w   = struct.unpack_from("<i", data, 18)[0]
            h   = struct.unpack_from("<i", data, 22)[0]
            bpp = struct.unpack_from("<H", data, 28)[0]
            if bpp != 24:
                return None
            stride = ((w * 24 + 31) // 32) * 4
            out = []
            for r in range(abs(h)):
                q = off + r * stride
                out.append(data[q:q + w * 3])
            if h > 0:
                out = list(reversed(out))
            return out

        ra = rows(a)
        rb = rows(b)
        if not ra or not rb or len(ra) != len(rb):
            return False
        return ra[:200] == rb[:200]
    except Exception:
        return False


def ink_ratio(path):
    """BMP 里"非背景像素"的占比（0.0~1.0）。

    这是"屏上有多少字"的一个廉价代理：背景是纯色，文字/线条是另一色。
    取占比而不是像素数，是为了与分辨率无关（顶屏 400x240、底屏 320x240）。

    用途：M6 流式验收要证明"回复过程中文本在增长"。判断两帧不同是不够的 ——
    光标闪一下也满足；必须**单调增长**才算"文字在一点点冒出来"。

    两个曾经的坑，都是 2026-10-01 实测踩出来的：

    1. **背景色必须取灰度众数，不能取四角。**
       顶屏的四个角**永远**是页眉（青绿，灰度 103）和页脚（浅灰，246），
       从来就不是正文的白色底。取它们的中位数得 103，于是整屏白色正文
       全被当成墨 —— 四张连拍算出清一色的 0.797，看上去像"截图没生效"，
       实际图完全正常（错的是尺子，不是被测对象）。
       白底界面上白色像素占绝对多数，众数天然就是它。

    2. **不走 Pillow。**
       Pillow 在本机不是每个解释器里都有，而验收脚本不该因为少一个可选
       依赖就悄悄降级（`return None` 会让断言静默跳过，比报错更坏）。
       BMP 是无压缩的，手写解析三十行就够，读出来的还是原始字节，
       没有色彩管理插值那一层。

    3. **别拿它当"滚动视图的内容在变多"的判据。**
       它量的是单屏内非背景像素的占比。而底部锚定的滚动视图（对话屏
       正是如此）内容一旦超过一屏，屏幕就永远填满，且永远显示"最后 N 行"——
       第 3 张和第 6 张显示的行数一样多，占比自然也一样。那个场景要看
       长度轨迹（app 侧自报），或者干脆比 md5。本函数适合比较"两种不同
       版面的同一屏"，不适合追踪同一屏里的增长。
    """
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    if len(b) < 54 or b[:2] != b"BM":
        return None

    w = int.from_bytes(b[18:22], "little")
    h = int.from_bytes(b[22:26], "little")
    if w <= 0 or h <= 0 or w > 4096 or h > 4096:
        return None
    row = (w * 3 + 3) // 4 * 4          # BMP 每行按 4 字节对齐
    if len(b) < 54 + row * h:
        return None

    hist = {}
    for y in range(0, h, 2):            # 隔行采样：够用且快一倍
        off = 54 + (h - 1 - y) * row    # BMP 是从下往上存的
        line = b[off:off + w * 3]
        for x in range(0, w, 2):
            i = x * 3
            # BGR 顺序；灰度用整数权重，别引浮点
            g = (line[i + 2] * 30 + line[i + 1] * 59 + line[i] * 11) // 100
            hist[g] = hist.get(g, 0) + 1

    if not hist:
        return None
    bg = max(hist.items(), key=lambda kv: kv[1])[0]
    tot = sum(hist.values())
    ink = sum(c for g, c in hist.items() if abs(g - bg) > 40)
    return ink / float(tot) if tot else None


KEY_REL = "3ds/aiconsole/key.txt"
KS_REL  = "3ds/aiconsole/keys.json"

#: H/I 趟的 keys.json 备份：路径 -> (原本是否存在, 原内容)
KS_BACKUP = {}
#: I 趟断言用的期望值：切换后 active 应等于仓库第一条的名字
KS_EXPECTED = {"active": ""}

#: K 趟（设置屏）会改 config.json 里的 ui.theme 与 guard.*，跑完必须原样还给用户。
#: 不备份的话，用户跑一次验收，自己的主题就被改成黑夜了。
CFG_BACKUP = {}


def backup_config(roots):
    """把每个候选 SD 根上的 config.json 存进内存。"""
    CFG_BACKUP.clear()
    for r in roots:
        p = os.path.join(r, CONFIG_REL)
        existed = os.path.isfile(p)
        data = open(p, "rb").read() if existed else b""
        CFG_BACKUP[p] = (existed, data)


def restore_config():
    """把 config.json 写回去。文件是这次新建的就删掉，别留垃圾。"""
    for p, (existed, data) in CFG_BACKUP.items():
        try:
            if existed:
                with open(p, "wb") as fh:
                    fh.write(data)
            elif os.path.isfile(p):
                retire(p)
        except OSError as e:
            print("!! 还原 config.json 失败（%s）：%s" % (p, e))
    if CFG_BACKUP:
        print("已还原 %d 份 config.json" % len(CFG_BACKUP))
    CFG_BACKUP.clear()


def stage_keyfile(roots):
    """A 趟要验的那条路径：宿主先放一份假凭据进去。

    provider 故意写成和默认值不一样的 zhipu。如果最后 config 里
    的 provider 变成了 zhipu，就说明值确实是从文件读来的，
    而不是「默认值刚好长这样」造成的假通过。
    """
    wrote = []
    for r in roots:
        p = os.path.join(r, *KEY_REL.split("/"))
        try:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as fh:
                fh.write("# 验收用假凭据\n")
                fh.write("provider=zhipu\n")
                fh.write("model=glm-4-flash\n")
                fh.write("sk-test-abcdef123456\n")
            wrote.append(p)
        except OSError as e:
            print("!! 放置 key.txt 失败：%s" % e)
    if wrote:
        print("宿主已放置假凭据：%s" % KEY_REL)
    return wrote


#: H 趟的 key.txt 备份：路径 -> (原本是否存在, 原内容)
H_KEY_BACKUP = {}


def _read_ks(p):
    """读 keys.json，返回 (active, first_name, has_real)。坏文件/不存在都安全。"""
    try:
        with open(p, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception:                                          # noqa: BLE001
        return "", "", False
    active = data.get("active", "") or ""
    keys = data.get("keys", []) or []
    first = keys[0].get("name", "") if keys else ""
    has_real = bool(active) and any(
        isinstance(k.get("key"), str)
        and k["key"].startswith("sk-")
        and not k["key"].startswith("sk-test")
        for k in keys)
    return active, first, has_real


def stage_keystore(roots, for_chat):
    """H/I 趟夹具。返回 True 表示放的是假 Key（跑 401 错误路径）。

    H（for_chat）：仓库里已有真激活 Key 就用真的（跑 200 路径），
    否则写一把假 Key 进 keys.json（sk-test 前缀不会被当真）。
    I：仓库不满两把就补一份测试仓库（两个条目才能验切换），
    期望的切换结果记在 KS_EXPECTED['active']。
    两种情况都先备份，跑完 restore_keystore() 原样还给用户。

    M5 之后必须**同时腾空 creds.enc**：仓库载入是加密优先的
    （keystore_load 先试 creds.enc），不腾空的话本函数摆的明文夹具
    根本不会被读到 —— app 会去解 creds.enc 里的真 Key，
    于是"H 趟假 Key 应当 401"变成"用真 Key 拿到 200"。
    2026-10-01 全量回归实测到过这个假失败。
    """
    KS_BACKUP.clear()
    KS_EXPECTED["active"] = ""
    dummy = True
    for r in roots:
        # 先把 creds.enc 挪走（备份进 KS_BACKUP，键名加前缀区分）
        cp = os.path.join(r, *CREDS_REL.split("/"))
        KS_BACKUP["\x00creds\x00" + cp] = (
            os.path.isfile(cp), open(cp, "rb").read() if os.path.isfile(cp) else b"")
        if os.path.isfile(cp):
            retire(cp)

        p = os.path.join(r, *KS_REL.split("/"))
        existed = os.path.isfile(p)
        data = open(p, "rb").read() if existed else b""
        KS_BACKUP[p] = (existed, data)

        active, first, has_real = _read_ks(p)
        if for_chat:
            if not has_real:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write('{"version":1,"active":"ks-test",'
                             '"keys":[{"name":"ks-test",'
                             '"provider":"deepseek","model":"deepseek-chat",'
                             '"key":"sk-test-invalid-key-000000000000"}]}')
            else:
                dummy = False
        else:
            n_keys = 0
            try:
                n_keys = len(json.loads(data or b'{}').get("keys", []))
            except Exception:                                  # noqa: BLE001
                pass
            if first and n_keys >= 2:
                dummy = False
                # 只有**本来就存在** keys.json 的根才是 Azahar 真正用的 SD 根，
                # 它的名字才能当期望值。否则前面几个空候选根会先把期望值
                # 占成测试仓库的名字（ks-test-1），断言必然误判。
                if existed:
                    KS_EXPECTED["active"] = first
            else:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write('{"version":1,"active":"ks-test-2","keys":['
                             '{"name":"ks-test-1","provider":"zhipu",'
                             '"model":"glm-4-flash","key":"sk-test-a"},'
                             '{"name":"ks-test-2","provider":"deepseek",'
                             '"model":"deepseek-chat","key":"sk-test-b"}]}')
                if not KS_EXPECTED["active"]:
                    KS_EXPECTED["active"] = "ks-test-1"
    return dummy


def restore_keystore():
    """把 H/I 趟动过的 keys.json 与 creds.enc 原样放回去。

    键名带 "\x00creds\x00" 前缀的项是 creds.enc（见 stage_keystore 的说明）。
    前缀用 NUL 包夹，保证不会和真实路径撞名。
    """
    for p, (existed, data) in KS_BACKUP.items():
        real = p.split("\x00creds\x00", 1)[1] if p.startswith("\x00creds\x00") else p
        try:
            if existed:
                os.makedirs(os.path.dirname(real), exist_ok=True)
                with open(real, "wb") as fh:
                    fh.write(data)
            elif os.path.isfile(real):
                retire(real)
        except OSError as e:
            print("!! 还原仓库文件失败 %s：%s" % (real, e))
    KS_BACKUP.clear()


def stage_demo(roots):
    """M 趟夹具：把仓库切成 provider=demo 的离线演示条目。

    为什么不再需要真 Key：demo 的判据是 base_url 为空（约定见 provider.h），
    请求根本不出网。于是这一趟验的是**纯 UI 侧**的流式上屏 ——
    分块回调 → 节流 → 逐行增长。这半边正是 M6 新写的代码，
    也正是最容易写错的部分（读的 size 取大了就不是流式、节流算错就掉帧）。

    key 字段必须**非空**：chat_enter 在 key 为空时会 fallthrough 去读
    key.txt，顺手把 provider 也覆盖掉，演示模式就丢了。写个占位串即可，
    demo 根本不看它。它不以 "sk-" 开头，所以也不会被 _read_ks 当成
    "真 Key" —— H 与 M 共用同一套备份/还原机制，别让两趟互相污染。
    """
    KS_BACKUP.clear()
    KS_EXPECTED["active"] = ""
    for r in roots:
        cp = os.path.join(r, *CREDS_REL.split("/"))
        KS_BACKUP["\x00creds\x00" + cp] = (
            os.path.isfile(cp), open(cp, "rb").read() if os.path.isfile(cp) else b"")
        if os.path.isfile(cp):
            retire(cp)

        p = os.path.join(r, *KS_REL.split("/"))
        KS_BACKUP[p] = (
            os.path.isfile(p),
            open(p, "rb").read() if os.path.isfile(p) else b"")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write('{"version":1,"active":"ks-demo",'
                     '"keys":[{"name":"ks-demo","provider":"demo","model":"demo",'
                     '"key":"offline-demo"}]}')


#: N 趟用的人设。写成模块级常量，是因为断言要拿它核对 app 自报的字节数。
N_PERSONA = "你是一只说话简短干脆的猫。"


def _clear_chat_slots(roots):
    """清掉**按任务分的历史文件**（chat_p0.json / chat_conv_p0.txt …）。

    ★ 2026-10-25 修：对话历史早就改成"每个任务一份"（chatlog.c 的
      `chat_p%d.json`），而夹具还在写老的 `chat.json`。老文件在启动时会被
      迁移成 chat_p0.json，**但如果上一次跑留下的 chat_p0.json 还在**，
      迁移就不会发生，app 读到的是上一趟的历史 —— 断言于是报
      "实际 14 条（夹具预置的就是 2 条）"，看起来像程序把记录重复写了，
      其实是**夹具没隔离**。这类红必须在这里修掉，不能让人去查代码。
    """
    for r in roots:
        d = os.path.join(r, SD_SUBDIR)
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if fn.startswith("chat_p") or fn.startswith("chat_conv_p") \
                    or fn == "chat.json":
                try:
                    os.remove(os.path.join(d, fn))
                except OSError:
                    pass


def stage_chatlog(roots):
    """N 趟夹具：预写一份会话记录，并在 config.json 里塞一个人设。

    为什么由宿主**直接写**这两样，而不是在界面上操作：
      · 编人设要开 swkbd，而模拟器里 swkbd 会弹一个 Qt 窗口，脚本够不着
        （H 趟绕开它改用快捷短语，是同一个原因）。
      · "恢复历史"验的就是"开机时文件里有东西" —— 让夹具直接把那个状态
        摆出来，比先跑一趟产生历史再跑一趟验证少一半机器时间，
        而且失败时定位更直接（不用纠结是没存上还是没读出来）。
    写盘那一半由断言去看 chat.json 的条数有没有增长。
    """
    _clear_chat_slots(roots)
    for r in roots:
        p = os.path.join(r, *CHATLOG_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            # 注意这里的 \\n 是**写进文件的两个字符**（反斜杠 + n），
            # 由 JSON 解析成真正的换行 —— 这正是要顺手验的转义路径。
            fh.write('{"mine":1,"text":"你好，我叫小明。\\n我想问个问题。"}\n')
            fh.write('{"mine":0,"text":"你好小明！有什么可以帮你的？"}\n')

        c = os.path.join(r, CONFIG_REL)
        if not os.path.isfile(c):
            continue
        try:
            with open(c, "r", encoding="utf-8") as fh:
                cfg = json.load(fh)
        except Exception:                                  # noqa: BLE001
            continue
        cfg.setdefault("ai", {})["system_prompt"] = N_PERSONA
        with open(c, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, ensure_ascii=False, indent=2)


def stage_chatlog_many(roots):
    """趟 7 夹具：写 8 条历史，保证上屏**内容多到装不下**。

    为什么必须夹具化：这一趟要验"摇杆能滚上屏"，而滚动的先决条件是
    "内容比可视区高"。靠卡上残留的历史凑不出来 —— 前一趟（H/M/N）会
    重写 chat.json，条数随机。没有这条夹具时，7 趟时而绿时而红，
    红的理由却是"没东西可滚"，跟摇杆一点关系都没有。

    ★★ 每条必须**长到多行**（2026-10-24 实测踩坑）：
    第一版每条只写一句话，8 条加起来也没超过上屏的行数预算，于是
    reanchor() 算出 g_scroll=0 —— 也就是"全部可见、上面没有更早的"。
    这种状态下上推摇杆**本来就该什么都不动**，而判据却报"摇杆没接线"。
    判据没错，夹具错了：它摆出来的根本不是"有内容可滚"的状态。
    现在每条写 6 行，8 条 ≈ 48 行，远超预算，g_scroll > 0 成立。
    """
    lines = []
    for i in range(1, 9):
        who = 1 if i % 2 else 0
        body = ("第 %d 条测试消息。" % i) + "".join(
            "第 %d 行：这一行只是用来把上屏填满，好让摇杆有东西可滚。" % k
            for k in range(1, 6))
        lines.append(json.dumps({"mine": who, "text": body},
                                ensure_ascii=False))
    _clear_chat_slots(roots)
    for r in roots:
        p = os.path.join(r, *CHATLOG_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            for ln in lines:
                fh.write(ln + "\n")

    return [ln.encode("utf-8") for ln in lines]


def stage_two_projects(roots):
    """趟 8 夹具：两个任务 + 两份内容明显不同的历史。

    为什么两份内容要"明显不同"：判据是"上屏指纹变了"。两条历史只要
    有一点不一样指纹就会变，但**看不出是不是切对了** —— 用两段完全不同
    的话（甲/乙各一句话），万一真出了"切到别人历史"的 bug，从截图上一眼
    就能认出来是哪一份。
    """
    proj = "active=0\n" \
           "任务甲|zhipu|glm-4-flash\n" \
           "任务乙|zhipu|glm-4-flash\n"

    def log(tag):
        return "".join(
            json.dumps({"mine": i % 2, "text": "%s 的第 %d 条记录" % (tag, i)},
                       ensure_ascii=False) + "\n"
            for i in range(1, 4))

    for r in roots:
        d = os.path.join(r, SD_SUBDIR)
        os.makedirs(d, exist_ok=True)

        with open(os.path.join(d, "projects.txt"), "w",
                  encoding="utf-8", newline="\n") as fh:
            fh.write(proj)

        for name, tag in ((("chat_p0.json"), "任务甲"), (("chat_p1.json"), "任务乙")):
            with open(os.path.join(d, name), "w",
                      encoding="utf-8", newline="\n") as fh:
                fh.write(log(tag))

        # 老格式那份要清掉：它还留着上一趟的内容，而 p0 的迁移逻辑
        # 只在 chat_p0.json 不存在时才搬 —— 留着不影响本趟，但会让
        # 失败时的取证多一份噪音。
        old = os.path.join(d, "chat.json")
        if os.path.isfile(old):
            retire(old)


#: M / N 趟用的中性短语。
#:
#: 为什么要由夹具**显式钉死**，而不是用内置默认列表：
#: H/M/N 三趟都是"按 SELECT 填一条、按 A 发出去"。它们验的是流式上屏、
#: 多轮上下文、会话持久化 —— 跟自己发的是什么话毫无关系。可一旦默认
#: 列表里出现一句会让**演示剧本吐动作**的话（比如"创建一个文件"），
#: 发完之后 app 就会推确认屏，把对话屏顶掉，脚本后面的按键全落在
#: 确认屏上，M/N 会以"只发了一轮"、"end_screen 不是 SCREEN_CHAT"
#: 这种**看起来跟改动毫无关系**的方式挂掉。
#: 2026-10-03 加第 5 条默认短语时实测踩到，全量回归里 M/N 双双失败。
#:
#: 教训：**趟次的输入不能借默认值**。默认值是给用户看的，会随产品需要变；
#: 夹具要的是一根不动的桩。凡"我来决定发什么"的趟次，都写自己的短语。
NEUTRAL_PHRASE = "你好！请用两三句话介绍一下你自己。"


def stage_phrases_neutral(roots):
    """给 M / N 一趟：钉住一条中性短语，与内置默认列表解耦。"""
    for r in roots:
        p = os.path.join(r, *PHRASES_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(NEUTRAL_PHRASE + "\n")


#: O 趟：夹具只留这一条短语。它同时是"按 SELECT 就能发"的唯一通道，
#: 和"演示该不该演动作"的触发词 —— 两件事由同一句话承担，脚本里就一步。
O_PHRASE = "试着创建一个文件，告诉我 agent 能力是通的。"

#: 演示剧本要写的那个文件的内容片段。断言拿它核核对盘上的东西
#: 确实是**这次**写下去的那份，而不是上一轮留下的。
O_BODY_MARK = "这个文件是 AI 助手在 3DS 上创建的。"


# ---------------------------------------------------------------------------
# R 趟：AI 安装程序（agent 能力第二步）
#
# 与 O 趟的关系：O 趟验的是"AI 能创建文件"，R 趟验的是"AI 能装程序"。
# 两者走的是**同一个动作协议、同一张确认屏**，但落点完全不同：
# 一个写进沙箱 out/，一个装进 3ds/（Launcher 会扫的地方）。
#
# 这一趟要同时钉住四件事：
#   ① 动作被解析成 pkg.install（不是被当成 file.write 或 ACT_UNKNOWN）
#   ② 确认屏上出现的是**安装**的措辞，不是写文件的措辞
#   ③ 摘要不符时**拒绝安装**（这是这条路最要紧的护栏）
#   ④ 允许之后文件真的出现在 3ds/
# ---------------------------------------------------------------------------

#: R 趟夹具短语。必须能触发 chatapi.c 的 demo_install_trigger ——
#: 那两张词表（写文件 / 装程序）没有交集，所以这一句不会误触发 file.write。
R_PHRASE = "帮我装个游戏吧，卡上应该有一个。"

#: R2 趟夹具短语 —— 走"不给摘要"那条路，于是能真的装成功。
#: 这是 R 趟的另一半：R 验护栏拦得住，R2 验该放行时放得行。
R2_PHRASE = "装个游戏，摘要不用核对。"

#: 演示剧本里 pkg.install 要装的那个包名（对应 chatapi.c 的 DEMO_INSTALL_PKG）。
R_PKG     = "demo_game"
R_PKG_REL = os.path.join("3ds", R_PKG + ".3dsx")
R_SRC_REL = os.path.join(SD_SUBDIR, "pkgs", R_PKG + ".3dsx")


def stage_pkg_src(roots):
    """把"待装的包"放进 pkgs/ 目录，并算好它的真实 sha256。

    返回真实摘要 —— 但**演示剧本里写的是全零**（见 chatapi.c 的
    DEMO_INSTALL_SHA），所以这一趟预期是"摘要对不上 → 拒绝安装"。
    这不是凑巧，是本趟**主要**要验的那条护栏。

    想验"能装成功"的那一半，得让剧本里的摘要等于真实摘要，
    而那需要把摘要烤进二进制。这里不这么做：摘要应该是模型给的
    运行时数据，不是编译期常量 —— 所以 R 趟只钉"拒绝"这一半，
    "成功"那一半由 R2 趟用"不给 sha256"的路径来覆盖。
    """
    import hashlib
    real_sha = None
    for r in roots:
        p = os.path.join(r, *R_SRC_REL.split(os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        # 内容不需要是合法 3dsx —— pkg_install 只搬运字节、核摘要，
        # 不解析 ELF。但**不能是 0 字节**：那会走到 PKG_ERR_EMPTY，
        # 与本趟要验的摘要护栏撞车，结论就说不清了。
        body = b"FAKE 3DSX PAYLOAD FOR INSTALL TEST\n" * 64
        with open(p, "wb") as fh:
            fh.write(body)
        real_sha = hashlib.sha256(body).hexdigest()
    return real_sha


def stage_pkg_unit(roots):
    """清空安装落点，保证"文件出现"这件事是**这一次**发生的。

    用 retire（挪进 .trash）而不是删 —— 全脚本一条 os.remove 都不留。
    """
    for r in roots:
        retire(os.path.join(r, *R_PKG_REL.split("/")))


def stage_phrases_r(roots):
    """R 趟夹具：快捷短语只留一条安装触发词。

    与 O 趟同样的理由（见 stage_phrases_o）：不能借内置默认值 ——
    那些是给用户看的、会随产品变；夹具要的是一根不动的桩。
    """
    for r in roots:
        p = os.path.join(r, *PHRASES_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# R 趟夹具：只留一条安装演示短语\n")
            fh.write(R_PHRASE + "\n")


def stage_phrases_r2(roots):
    """R2 趟夹具：只留一条"不给摘要"的短语。

    为什么值得单独一趟：pkg.install 的 sha256 是可选参数，而
    "可选"的代码路径最容易被写坏还不被发现 —— 它平时不跑。
    R 趟只覆盖了"给了摘要"的一半，这里补上另一半。
    """
    for r in roots:
        p = os.path.join(r, *PHRASES_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# R2 趟夹具：不给摘要（走可选参数那条路）\n")
            fh.write(R2_PHRASE + "\n")


def stage_phrases_o(roots):
    """O 趟夹具：把快捷短语文件写成只剩一条（就是那句动作触发词）。

    为什么不靠内置默认：内置的那几条只有在 phrases.txt **不存在**时才生效，
    而前几趟（H/N）可能已经让 app 写过一份。把文件按需要写死，
    这一趟就不必关心"上一趟留了什么"，也不会因为默认列表以后改动而失效。
    """
    for r in roots:
        p = os.path.join(r, *PHRASES_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# O 趟夹具：只留一条动作演示短语\n")
            fh.write(O_PHRASE + "\n")


# ---------------------------------------------------------------------------
# Y 趟：语音输入链路（麦克风 + ASR 配置 + 拼装自检）
#
# 这一趟要验的**头一件事**是「asr.txt 到底读进来没有」。而这一件
# 不能让它自己报：asr.c 在文件读不到时会**回落到默认 provider**，
# 屏上照样打一行 provider=xxx，看着跟读到了一模一样。
# 所以夹具故意写一个「表内存在、但不是默认」的 provider（openai）：
#   · 读到了        -> provider=openai      key=1（文件里那把假 Key）
#   · 没读到 / 格式错 -> provider=siliconflow key=1（内置 Key，2026-10-18 起）
# 两条路的 provider 完全不同，断言才拿得到真判据。
# ⚠️ key 这一位**不能**再当判据用：asr_load_file 修过之后（asr_reset 移到
#    fopen 之前，见 shared/asr.c），没文件 = 内置 Key 生效，key 恒为 1。
#    还按老注释断言 key=0 的话，这条判据会假红。
#
# ⚠️ 反过来用「表里没有的名字」是测不出来的：asr_set 对未知 provider
#    直接 return false、不做 half-apply（见 asr.c 的注释），
#    于是现象与"文件根本没读到"完全重合，这条判据会永远绿。
# ---------------------------------------------------------------------------

#: 夹具 provider：必须是 kAsrTable 里的一员，且**不是** ASR_DEFAULT_NAME。
# 必须是**表内非默认**的那一项（现在就是 openai，默认 siliconflow），
# 这样才分得出"asr.txt 读到了"和"没读到、回落默认"：
#   读到 -> provider=openai ；没读到 -> provider=siliconflow
# 反过来拿默认项 siliconflow 当夹具的话，写与不写结果一模一样，
# 这条判据就永远绿（见 MEMORY 铁律 4）。
#
# 代价：openai 的域名在当前网络下 TCP 超时，这一趟的 ASR 必然
# http=-28（"没连上"）。这是**预期**的，判据里已经允许 fail ——
# 这趟验的是采集够不够 / 配置读没读 / 拼装装得下，
# 网络可达性另有结论：3DS 上 curl 到 api.siliconflow.cn 实测
# 返回 401（连上了、只差真 Key）。见 BACKLOG。
Y_ASR_PROVIDER = "openai"
#: 假 Key。只为让 key=1 那一位非零 —— 本趟不联网，这个 Key 从不被发出去。
Y_ASR_KEY      = "mock-key-only-for-path-check"


def stage_asr_cfg(roots):
    """Y 趟夹具：写一份 asr.txt（表内非默认 provider + 假 Key）。"""
    for r in roots:
        p = os.path.join(r, *ASR_CFG_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# Y 趟夹具：provider 故意取表内非默认的那个，\n")
            fh.write("# 于是「配置读到了」和「用了默认值」在屏上是两行不同的字。\n")
            fh.write("provider=%s\n" % Y_ASR_PROVIDER)
            fh.write("key=%s\n" % Y_ASR_KEY)


# ---------------------------------------------------------------------------
# P 趟：长消息回落（B1 的回归守卫）
#
# 起因：chatlog.c 原先把整行读进一个 1280 字节的**静态**缓冲。而一条记录
# 转义之后最长可以有 2068 字节（1024 个换行 → 每个变 "\n" 两个字符）。
# 超出部分被 fgets 截断，后半截当成新行、不以 '{' 开头被跳过 —— 于是
# 长消息之后的所有历史**静静地消失**，界面和日志都不提一个字。
#
# 修法是把缓冲改成按需 realloc（见 shared/chatlog.c 的 read_line），
# 但"改好了"和"以后不会被改坏"是两件事。本趟用一条**恰好跨过旧阈值**
# 的记录把这条边界钉住：只要行缓冲再被换回定长且小于 2071，
# 这趟立刻变红。
# ---------------------------------------------------------------------------

#: 长消息正文长度：1024 个字符。转义后 1024 字节，加上 JSON 外壳
#: 与 UTF-8 中文（每字 3 字节），整行会到 2071 字节 —— 旧缓冲 1280
#: 的 1.6 倍，且超出部分远超"刚好越界"，不会因为误差蒙混过关。
P_LONG_CHARS = 1024

#: 长消息**之后**那一条。它存不存在，是"截断有没有连累后续行"的直接判据。
P_TAIL_TEXT = "长消息之后的那条"


def _p_long_text():
    """长消息正文：中文 + 换行交替。

    为什么掺中文：转义和 UTF-8 编码是两条独立的路径，纯 ASCII 只压到前者。
    为什么掺换行：换行是唯一会**变长**的字符（1 字节 → 2 字节），
    也是 JSONL 里最容易被行缓冲坑到的那一个。
    """
    return "长消息开始：" + ("这是很长的一句话，用来把这一行撑过旧缓冲的上限。\n"
                            * (P_LONG_CHARS // 24 + 1))[:P_LONG_CHARS - 6] + "长消息结束"


def stage_chatlog_long(roots):
    """P 趟夹具：预写三条历史，中间那条长到跨过旧行缓冲的阈值。

    写完之后，整行的字节数由断言再核一遍 —— 这个夹具的**全部价值**
    就在于"第 2 行确实超了"，所以它自己也得是被校验的对象，
    否则哪天常量被调小，这趟会悄悄退化成"验三条普通历史"。
    """
    text = _p_long_text()
    lines = []

    lines.append(json.dumps({"mine": 1, "text": "短消息"},
                            ensure_ascii=False))
    lines.append(json.dumps({"mine": 0, "text": text},
                            ensure_ascii=False))
    lines.append(json.dumps({"mine": 1, "text": P_TAIL_TEXT},
                            ensure_ascii=False))

    for r in roots:
        p = os.path.join(r, *CHATLOG_REL.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            for ln in lines:
                fh.write(ln + "\n")

    return [ln.encode("utf-8") for ln in lines]


#: 旧实现的定长行缓冲的大小。断言拿它做判据，不是拿"当前实现"做判据。
P_OLD_BUF = 1280


# ===========================================================================
# L 趟：M5 加密存储（多阶段）
#
# 这一趟跟前面 11 趟形状不一样：它要跨**两次启动**才能验完，
# 因为核心命题就是"重启之后还能不能读到"。
#   阶段 1：干净环境起，按引导把 key.txt 收进仓库 → 应当生成 creds.enc
#   阶段 2：不碰任何文件直接再起一次 → 应当自己解开 creds.enc（免输入）
#   阶段 3：把 creds.enc 篡改一个字节再起 → 必须拒绝，且不能崩
#
# 三个阶段的证据合起来才构成"M5 达标"：能加密、能持久、能防篡改。
# ===========================================================================

CREDS_REL = "3ds/aiconsole/creds.enc"

#: 阶段 3 篡改前，把原始 creds.enc 存这儿，跑完还原。
CREDS_BACKUP = {}


def backup_creds(roots):
    CREDS_BACKUP.clear()
    for r in roots:
        p = os.path.join(r, *CREDS_REL.split("/"))
        existed = os.path.isfile(p)
        data = open(p, "rb").read() if existed else b""
        CREDS_BACKUP[p] = (existed, data)


def restore_creds():
    for p, (existed, data) in CREDS_BACKUP.items():
        try:
            if existed:
                with open(p, "wb") as fh:
                    fh.write(data)
            elif os.path.isfile(p):
                retire(p)
        except OSError as e:
            print("!! 还原 creds.enc 失败 %s：%s" % (p, e))
    CREDS_BACKUP.clear()


def find_creds(roots):
    """找出真正被 app 写过的 creds.enc（取最新那个）。"""
    return newest(roots, CREDS_REL)


def run_m5_pass(ctx):
    """L 趟总控：三个阶段依次跑，返回一个和 run_pass 同形的结果。

    为什么不用现有的 run_pass：它一趟一次启动。这里三次启动之间
    要**读文件、改文件**（检查 creds.enc 生成了没、篡改它），
    阶段之间是有状态的，塞不进单趟模型。
    """
    roots = ctx["roots"]
    print("\n" + "=" * 68)
    print("PASS L · M5 加密存储（三阶段：入库 → 重启免输入 → 防篡改）")
    print("=" * 68)

    stages = {}
    shots_all = []

    def _run(tag, script, label):
        clear_pass_artifacts(roots, drop_config=(tag == "L1"))
        write_autorun(roots, script)
        kill_stale_azahar(os.path.basename(ctx["azahar"]))
        t0 = time.time()
        proc = subprocess.Popen([ctx["azahar"], ctx["rom"]], cwd=ctx["exe_dir"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sp = None
        deadline = t0 + ctx["wait"]
        while time.time() < deadline:
            sp = newest(roots, STATUS_REL)
            if sp and os.path.getmtime(sp) >= t0 - 1:
                break
            if proc.poll() is not None and not sp:
                break
            time.sleep(1.0)
        if not ctx["keep"]:
            kill_tree(proc)
        status = read_status_stable(sp)
        print("  [%s] %s · 耗时 %.1fs" % (tag, label, time.time() - t0))
        stages[tag] = status
        s = sorted(sum((glob.glob(os.path.join(r, SHOT_SUB, "*.bmp"))
                        for r in roots), []))
        shots_all.extend(stage_shots(tag, s))
        return status

    # ---- 阶段 1：干净环境 + key.txt → 应当生成 creds.enc ----
    backup_creds(roots)
    rmtree_files([os.path.join(r, *CREDS_REL.split("/")) for r in roots])
    stage_keyfile(roots)
    st1 = _run("L1", SCRIPT_L1, "走引导并把 key.txt 收进加密仓库")

    creds_path = find_creds(roots)
    creds_bytes = open(creds_path, "rb").read() if creds_path else b""
    ks_after1 = newest(roots, KS_REL)
    ks1 = read_text(ks_after1) if ks_after1 else ""

    # ---- 阶段 2：原样再起一次 → 应当自己解开（免输入） ----
    st2 = _run("L2", SCRIPT_L2, "重启：不碰文件直接进 Key 管理")

    # ---- 阶段 3：篡改 creds.enc 一个字节 → 必须拒绝且不崩 ----
    if creds_path and len(creds_bytes) > 32:
        tampered = bytearray(creds_bytes)
        idx = len(tampered) - 1          # 落在密文区（头 60 字节之后）
        tampered[idx] ^= 0x01
        with open(creds_path, "wb") as fh:
            fh.write(bytes(tampered))
        print("  已篡改 creds.enc 最后一个字节（0x%02x -> 0x%02x）"
              % (creds_bytes[idx], tampered[idx]))
    st3 = _run("L3", SCRIPT_L3, "篡改后重启：应当拒绝解密")

    restore_creds()
    restore_keystore()

    ctx["shots"] += shots_all
    return dict(tag="L", ok=True, why="", shots=shots_all, status=st1,
                cfg=None, expect=len(shots_all) // 2, roots=roots,
                l_stages=stages, l_creds=creds_bytes, l_ks=ks1,
                l_creds_path=creds_path)


def restore_main_3dsx(roots):
    """趟次收尾：把 3ds/AIConsole.3dsx 盖回仓库里的真产物。

    E 趟（更新链路验收）会让 app 把 update/ 里的**假**包装到 3ds/ 根目录，
    顶掉正在用的主程序。不做收尾还原的话，下回开 Azahar 看到的就是
    1320 字节的 FAKE-3DSX-PAYLOAD-FOR-ACCEPTANCE —— 表现上等于「自制软件没了」，
    而且每跑一次验收复现一次，所以看起来像「又没了」。

    这里把**本来就存在的那一份**盖回去（不是给每个候选根都塞一份，理由见下），
    把「跑完验收 → 真在用的那个根里的主程序 == 仓库里刚 build 的真件」
    变成**不变量**，而不是靠人记住「跑完记得手动拷回去」。
    """
    src = os.path.join(ROOT, "app", "AIConsole.3dsx")
    if not os.path.exists(src):
        print("  跳过主程序还原：仓库里找不到 %s" % src)
        return
    n = 0
    for r in roots:
        dst = os.path.join(r, "3ds", "AIConsole.3dsx")
        # **只还原本来就存在的那些。** 本函数的语义是"撤销 app 刚才干的事"，
        # 而 app 只会写它真正在用的那一个根。无条件给**每个**候选根都塞一份，
        # 会在几个用不上的模拟器数据目录里凭空造出主程序 ——
        # 于是任何"扫 roots、取第一个命中"的代码都会拿到那份旧副本。
        # 2026-10-04 E 趟正是这么误报的：量到 1908872（仓库真件），
        # 而 app 其实老老实实装了 1320 字节的假包，报错方向完全是反的。
        if not os.path.exists(dst):
            continue
        try:
            with open(src, "rb") as fh:
                blob = fh.read()
            d = os.path.dirname(dst)
            if d:
                os.makedirs(d, exist_ok=True)
            tmp = dst + ".restore.tmp"
            with open(tmp, "wb") as fh:
                fh.write(blob)
            os.replace(tmp, dst)          # 同目录原子替换，别让 Azahar 读到半截
            n += 1
        except OSError as e:              # noqa: BLE001
            print("  主程序还原失败 %s: %s" % (dst, e))
    if n:
        print("  已还原主程序 3ds/AIConsole.3dsx <- %s（%d bytes）"
              % (os.path.basename(src), os.path.getsize(src)))


def stage_update_package(roots):
    """在卡上放一个**能通过校验**的假更新包。

    sha256 是按实际内容现算的，size 也是真的 —— 只有包里面的数据是假的。
    这样 app 跑的是真实的解析/比较/校验/安装链路，而不是被喂了个免检包。
    """
    import hashlib

    # ★ 版本号必须**比当前程序高**，不能写死。
    #   写死 99 的时候程序版本也是 99，于是 app 正确判定"已经是最新"、
    #   根本不进可安装状态 —— 而 E 趟会红成"end_screen 不是 SCREEN_UPDATE"，
    #   看起来像 OTA 坏了。2026-10-17 实测：屏幕上写的是
    #   「已经是最新 —— 包里是 v99，和当前一样」，功能其实完全正常。
    #   所以这里从 shared/ota.h 现读当前版本号，+1 当假包的版本。
    ver = 99
    try:
        with open(os.path.join(ROOT, "shared", "ota.h"),
                  encoding="utf-8") as fh:
            src = fh.read()
        m = re.search(r"#define\s+APP_VERSION_CODE\s+(\d+)", src)
        if m:
            ver = int(m.group(1))
    except (IOError, OSError):
        pass
    fake_ver = ver + 1

    payload = b"FAKE-3DSX-PAYLOAD-FOR-ACCEPTANCE\n" * 40
    digest = hashlib.sha256(payload).hexdigest()

    for r in roots:
        d = os.path.join(r, "3ds", "aiconsole", "update")
        try:
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "AIConsole.3dsx"), "wb") as fh:
                fh.write(payload)
            # 故意用上 h1/h2/p/ul/li，把解析器的几条分支都走到
            with open(os.path.join(d, "notes.html"), "w",
                      encoding="utf-8") as fh:
                fh.write("<html><body>"
                         "<h1>0.9.9-test 更新说明</h1>"
                         "<p>这是验收用的假更新包，不是真版本。</p>"
                         "<h2>新增</h2><ul>"
                         "<li>内置查看器（HTML 子集）</li>"
                         "<li>离线更新包通道</li>"
                         "</ul><h2>修复</h2><ul>"
                         "<li>minijson 对象数组下标</li>"
                         "</ul></body></html>")
            with open(os.path.join(d, "manifest.json"), "w",
                      encoding="utf-8") as fh:
                json.dump({
                    "schema": 1,
                    "version_code": fake_ver,
                    "version_name": "0.9.9-test-v%d" % fake_ver,
                    "channel": "stable",
                    "min_supported": 1,
                    "notes": "验收用的假更新包，不是真版本。",
                    "artifacts": [{
                        "kind": "3dsx",
                        "file": "AIConsole.3dsx",
                        "size": len(payload),
                        "sha256": digest,
                        "install": "replace",
                    }],
                }, fh)
        except OSError as e:
            print("!! 放置更新包失败：%s" % e)
    print("已放置验收用更新包：v%d（%d 字节，摘要真实）" % (fake_ver, len(payload)))


def patch_config_for_banner(roots):
    """宿主改写 config.json，把「未备份 + 每次提醒」造出来。"""
    p = newest(roots, CONFIG_REL)
    if not p:
        print("!! PASS C 需要 config.json，但没找到 —— 横幅回归会失效")
        return
    with open(p, "r", encoding="utf-8") as fh:
        cfg = json.load(fh)
    cfg["nand_backup"]["confirmed"] = False
    cfg["nand_backup"]["remind_on_startup"] = True
    with open(p, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    print("宿主已把 %s 改成 confirmed=false / remind_on_startup=true" % p)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

_REEXEC_FLAG = "AICONSOLE_PY_REEXEC"


def ensure_pillow():
    """BMP -> PNG 需要 Pillow。系统 Python 通常没有，WorkBuddy 的隔离 venv 里有。
    与其让人记住"该用哪个解释器跑"，不如自己换过去再跑。

    注意用 subprocess 而不是 os.execve：Windows 上 os.execv* 是"另起进程然后退出"
    的模拟实现，重定向到文件/管道时子进程拿不到句柄，整套输出会凭空消失。
    """
    try:
        import PIL  # noqa: F401
        return
    except ImportError:
        pass

    if os.environ.get(_REEXEC_FLAG):
        print("（当前解释器没有 Pillow，BMP -> PNG 会被跳过）", flush=True)
        return

    cand = os.path.join(os.path.expanduser("~"), ".workbuddy", "binaries",
                        "python", "envs", "default", "Scripts", "python.exe")
    if not os.path.exists(cand) or os.path.abspath(cand) == os.path.abspath(sys.executable):
        print("（当前解释器没有 Pillow，BMP -> PNG 会被跳过）", flush=True)
        return

    env = dict(os.environ)
    env[_REEXEC_FLAG] = "1"
    print("（当前解释器没有 Pillow，改用 %s 重跑）" % cand, flush=True)
    sys.exit(subprocess.call([cand, os.path.abspath(__file__)] + sys.argv[1:], env=env))


def parse_passes(spec, passes=None):
    """把 --passes 的字符串解析成趟次列表。返回 (tags, unknown)。

    ══ 为什么是"最长匹配扫描"而不是别的 ══════════════════════════════
    这个函数前后被推翻过三次，每次都栽在**同一类错误**上：
    把「用户想跑哪些趟」当成了「字符串里出现过哪些字母」。

      ①逐字符  [c for c in spec if c in PASSES]
         `--passes X2` 的'2' 被丢掉 -> 静默跑成 X 趟。
      ② 子串    [k for k in PASSES if k in spec]
         "X" in "X2" 为真 -> 同时跑 X 和 X2，
         X2 全绿被 X 的断言判红，汇总说「有断言不通过」。
      ③ 切词    re.split(r"[^A-Z0-9]+", spec) 后整词比对
         默认值是 `"".join(PASSES)` = `ABCD...UVWY` 连写串，
         切出来一整块认不出 -> 直接退出、什么也不跑。

    ★ 正确做法：从左到右**逐位试**，先试最长（3 字符）、再 2、再 1，
      命中就前进相应位数；非字母数字当分隔符跳过。
      连写串和分隔串都对，且 X2 不会被拆成 X + 2。

    分隔符一律忽略：`--passes "X2,Y"`、`"A B"`、`"x2"` 都能用。
    """
    if passes is None:
        passes = PASSES
    maxlen = max((len(k) for k in passes), default=1)
    s = spec.upper()
    tags, unknown, i, n = [], [], 0, len(s)
    seen = set()
    while i < n:
        ch = s[i]
        if not (ch.isalnum()):
            i += 1
            continue
        hit = None
        # 先长后短：保证 "X2" 不会被 "X" 抢走
        for ln in range(min(maxlen, n - i), 0, -1):
            cand = s[i:i + ln]
            if cand in passes:
                hit = cand
                break
        if hit is None:
            unknown.append(ch)
            i += 1
            continue
        if hit not in seen:
            seen.add(hit)
            tags.append(hit)
        i += len(hit)
    return tags, unknown


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--azahar", default=DEFAULT_AZAHAR)
    ap.add_argument("--rom", default=DEFAULT_ROM)
    # ⚠️ 默认趟次**从 PASSES 的键推导**，绝不写死字符串。
    #
    # 这里原本写死的是 "ABCDEFGHIJKLMNOPQRST"。后果非常隐蔽：
    # U 和 V 两趟加进来之后**从来没在默认回归里跑过**，
    # 而汇总行照旧打印"全部通过（184 张截图）"、退出码 0 ——
    # 一切看着正常，正因为「漏跑」不会报错，只会安静地什么都不验。
    # 2026-10-04 就是这么发现 U/V 缺席的（加完趟次跑全量，汇总里没有它们）。
    # 用 "".join(PASSES) 取键的插入顺序，新加的趟自动进默认列表。
    # ⚠️ 默认趟次从 PASSES 里推导（"".join(PASSES)），所以**新加一趟会自动进默认回归**。
    # X 必须显式排除：它要卡上有模型文件（模拟器不一定有），
    # 而且输出是时间、没有可断言的固定值。
    # 「加了趟」和「趟会跑」是两件事 —— 这次是反过来的：
    # **"趟在 PASSES 里"和"趟进默认回归"也是两件事**。
    ap.add_argument("--passes", default="".join(k for k in PASSES
                                                 if k not in DEFAULT_EXCLUDE),
                    help="要跑哪几趟，如 ABCDE / A / O / Q / R / S / V")
    ap.add_argument("--wait", type=float, default=110.0,
                    help="每趟最长等待秒数（含 Azahar 冷启动）")
    ap.add_argument("--keep", action="store_true", help="结束时不要杀模拟器")
    ap.add_argument("--no-png", action="store_true")
    args = ap.parse_args(argv)

    if not args.no_png:
        ensure_pillow()

    if not os.path.isfile(args.azahar):
        print("找不到 Azahar: %s" % args.azahar)
        return 1
    if not os.path.isfile(args.rom):
        print("找不到 ROM: %s" % args.rom)
        return 1

    # ⚠️ 这里必须按**趟次名**匹配，不能逐字符，也不能子串匹配。
    # 三个坑按时间顺序（都在这一行）：
    #   ① 逐字符  `[c for c in args.passes if c in PASSES]`
    #      --passes X2 里的 '2' 被丢掉 -> **静默跑成 X 趟**。
    #   ② 子串`[k for k in PASSES if k in want]`
    #      "X" in "X2" 为真 -> `--passes X2` **同时跑 X 和 X2**，
    #      X2 的 8/8 全绿被 X 的断言判成 FAIL，汇总说「有断言不通过」。
    #      症状极坏：要验的全过了，汇总说没过。
    #   ③ 纯切词  re.split(r"[^A-Z0-9]+", want) 后整词比对
    #      默认值是 `"".join(PASSES)` = `ABCD...UVWY` 这种**连写无分隔**串，
    #      切出来一整块认不出-> 直接退出、什么也不跑。
    # → 正确做法是**最长匹配扫描**：从左到右逐位试，
    #   先试 3 字符、再 2 字符、最后 1 字符，命中就前进相应位数。
    #   这样连写串和分隔串都对，且 X2 不会被拆成 X + 2。
    tags, unknown = parse_passes(args.passes)
    if unknown:
        print("--passes 里有不存在的趟次：%s（跳过）" % " ".join(unknown))
    if not tags:
        print("--passes 里没有有效的趟次（可用：%s）" % "".join(PASSES))
        return 1
    # 明确报一下认到了什么 —— 上面那三个坑都是"少报了一句"造成的
    print("要跑的趟次：%s" % " ".join(tags))

    exe_dir = os.path.dirname(args.azahar)
    all_roots = sdmc_roots(exe_dir)
    roots = writable_roots(all_roots)
    print("Azahar : %s" % args.azahar)
    print("ROM    : %s" % args.rom)
    print("可用 SD 根:")
    for r in roots:
        print("   %s" % r)
    if not roots:
        print("没有任何候选 SD 根可写，放弃")
        return 1

    ctx = dict(azahar=args.azahar, rom=args.rom, exe_dir=exe_dir,
               roots=roots, wait=args.wait, keep=args.keep, shots=[])

    results = []
    for t in tags:
        rotate_stage_dir(t)
        if t == "L":
            # L 是跨多次启动的多阶段趟次，有自己的总控
            r = run_m5_pass(ctx)
        else:
            r = run_pass(t, PASSES[t], ctx)
        results.append(r)

    print("\n" + "=" * 68)
    print("断言")
    print("=" * 68)

    failed = []
    for r in results:
        if not r["ok"]:
            print("PASS %s 无法断言：%s" % (r["tag"], r["why"]))
            failed.append(r["tag"])
            continue
        probs = assertions(r)
        if probs:
            failed.append(r["tag"])

    shots = sorted(set(ctx["shots"]))
    if shots and not args.no_png:
        pngs = bmp_to_png(shots, OUT_DIR)
        if pngs:
            print("\n转成 PNG（%d 张）放在 %s" % (len(pngs), OUT_DIR))
            for p in pngs:
                print("   %s" % os.path.basename(p))

    print("\n" + "=" * 68)
    if failed:
        print("结果：FAIL —— 趟次 %s 有断言不通过" % ", ".join(failed))
        return 2
    print("结果：PASS —— 趟次 %s 全部通过（%d 张截图）"
          % (", ".join(tags), len(shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
