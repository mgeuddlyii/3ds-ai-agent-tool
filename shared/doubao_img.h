/*
 * doubao_img.h -- 把一张 PNG 传到豆包 imagex，产出 attachments 里的那个对象
 *
 * 为什么单独一个模块：这一段是"纯协议"——AWS4 签名、四步 HTTP、md5，
 * 与对话逻辑无关。塞进 doubao.c 会让那个文件更难读。
 *
 * ── 依据（2026-11-14 实测通过，不是猜的）──
 *   豆包通道技术交接.md §2.4 五步，tools/doubao_imgprobe.py 是参考实现。
 *   当天用**卡上那份 cookie**（sdmc:/3ds/aiconsole/doubao/cookie.txt，
 *   10-17 存的）实测：上传 320×240 红底白方块，豆包答
 *   "红色的四个矩形，组成十字形的白色长条"，服务端 SSE 里
 *   intention=vlm / seed_agent_name=Agent-Vlm。
 *   ⚠️ 之前那次"看不到图"是 **session 过期走了访客通道**，不是结构问题。
 *
 * 前置条件：调用方必须先 db_load_cookie()（把 cookie 灌进 httpx 的 jar）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 上传一张 PNG。成功时把 attachment 对象（JSON 文本，不含外层数组）写进
 * att_json，可直接塞进 messages[].attachments。
 *
 * png/n 是 PNG 字节（**不是裸 RGB**，服务端按扩展名 .png 解）；
 * w/h 是原始像素尺寸，会写进 option.width/height。
 *
 * 失败返回 false，msg 里是一句人话（哪一步、服务端原话片段）。 */
bool dbimg_upload(const unsigned char *png, size_t n, int w, int h,
                  char *att_json, size_t att_cap,
                  char *msg, size_t msg_cap);
