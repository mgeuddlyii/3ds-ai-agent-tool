# 局域网语音识别（给 3DS 用）

3DS 录 5 秒音频 → POST 到这台电脑 → 电脑识别 → 返回文字。
不花钱、不走外网、音频不出门。

## 为什么要有这个东西

云端那几家免费 ASR 已经全挂了（2026-10-24 实测）：

| 服务 | 结果 |
|---|---|
| 硅基流动 6 个 STT 模型 | HTTP 402「余额不足」 |
| 模力方舟（含 Fun-ASR / SenseVoice / GLM-ASR） | 400「没有可用的计费资源」 |
| Groq（免费额度最大） | `console.groq.com` 注册被 Cloudflare 403 |

再往服务商表里加一家，只是换个地方失效。**能自己掌控的只有这台电脑。**

## 跑起来

```bash
# 先验证链路（不装任何模型，只回显收到了多少字节）
python stt_server.py --echo

# 真识别（第一次会下模型，几百 MB）
pip install faster-whisper
python stt_server.py --model small
```

启动后会打印：

```
听在 http://0.0.0.0:8000  模式=small
3DS 的 asr.txt 写：provider=custom / base_url=http://<本机IP>:8000
```

查本机 IP：`ipconfig`，找「IPv4 地址」（形如 192.168.1.5）。

## 3DS 这边

`sdmc:/3ds/aiconsole/asr.txt`：

```
provider=custom
base_url=http://192.168.1.5:8000
```

不需要 key（本地服务不验）。想加也支持：`key=随便什么`，服务端不看。

## 两个坑

**1. Windows 防火墙**：第一次启动会弹窗，要选「允许」。
没弹窗就去「Windows 安全中心 → 防火墙 → 允许应用通过防火墙」放行 python.exe。
不放行的表现是 3DS 那边一直「没连上」，而电脑上一条日志都没有。

**2. 电脑和 3DS 要在同一个 Wi-Fi**。
3DS 连的是 2.4G（它不支持 5G），电脑如果只连 5G，两边可能不同网段 ——
那就把电脑也连 2.4G，或者确认路由器没开「AP 隔离」。

## 怎么算成功

3DS 上按住 ZL 说一句，电脑窗口里应该出现：

```
19:23:41  ← 192.168.1.9  a.wav  32044 字节  16000 Hz / 1 ch / 16 bit
          → '帮我把这个装上去'（840 ms）
```

电脑上看到这行、3DS 上出现文字 = 通了。

## 实测记录

- 回声模式：POST 一个 1 秒 16kHz 的合成 wav → `HTTP 200 {"text": "收到 32044 字节音频（16000 Hz）"}` ✅
- 真识别：需要装 faster-whisper，本机未装（模型下载要几百 MB，且 hf 不通，得用 modelscope）
