/*
 * input.h -- 系统软键盘的一层薄封装
 *
 * 为什么要有这一层：项目里已经有**两处**各自写了一遍 swkbd 的调用序列
 * （sc_chat.c 的 open_keyboard、sc_guide.c 的提问框）。2026-10-16 又多了
 * "在机器上直接敲 Key / 网址 / 模型名"这一路需求，如果再来第三份，
 * 那三份的差异就只会是**各自漏掉的那一步**（漏设 hint、漏判按钮、
 * 缓冲按字节算成字符）—— 而这类差异的表现是"某处输入框行为怪怪的"，
 * 极难定位。
 *
 * ⚠️ 已验证的**边界**（不是推测）：
 *   模拟器（Azahar）里 swkbd 会弹一个 Qt 窗口，**验收脚本够不着** ——
 *   所以这条路走的是真机验证，自动化趟次只能覆盖"按下 A 之前的界面"。
 *   见 tools/azahar_test.py 里 H 趟那句注释。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 调系统软键盘输入一行文本。
 *
 * 返回 true  = 用户点了「确定」，out 里是**去掉首尾空白**后的结果
 *              （没有粘贴板的设备上，尾部多一个空格是常事，
 *               而 "https://x/v1 " 这种地址会直接拼出畸形 URL）。
 * 返回 false = 用户取消。**此时 out 一个字节都不动** —— 取消的语义是
 *              "当没发生过"，让调用方自己决定保留原值还是清空，
 *              比在这里写一半更安全。
 *
 * max_chars：swkbd 的**字符数**上限（不是字节数）。
 * cap      ：out 的字节容量。中文一个字 3 字节，两者不是一个口径，
 *            内部按 3 字节/字符换算，装不下就自动收紧 max_chars。
 */
bool ui_input_text(const char *hint, const char *initial, int max_chars,
                   char *out, size_t cap);
