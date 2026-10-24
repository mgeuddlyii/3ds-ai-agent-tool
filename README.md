# 3DS AI Agent Tool

**Language:** **English** · [简体中文](README.zh-CN.md)

An AI assistant that actually runs on a **New Nintendo 3DS** — chat, offline game
guides, and voice input, all on the console itself.

No custom firmware hacks, no self-hosted server. It's a single homebrew binary that
talks straight to the API providers. Chinese/English UI, dual-screen layout,
fully playable with buttons — touch only where it makes sense.

<!-- ─────────────────────────────────────────────────────────────
     Badges (shields.io). Hand-maintained: the version is written into
     the URL, so it must be bumped on every release (no CI yet).
     ───────────────────────────────────────────────────────────── -->
<p align="center">
  <img src="https://img.shields.io/badge/version-0.99.6%20beta-orange" alt="version">
  <img src="https://img.shields.io/badge/platform-New%203DS-red" alt="platform">
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="license">
  <img src="https://img.shields.io/badge/language-C-555" alt="language">
  <img src="https://img.shields.io/badge/UI-EN%2F%E4%B8%AD-2ea44f" alt="ui">
</p>

<p align="center">
  <b>Design by acrac</b> · <b>Built by DeepSeek V4.1-Flash</b>
</p>

<p align="center">
  <a href="https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/tag/v0.99.6">
    <img src="https://img.shields.io/badge/%E2%AC%87%EF%B8%8F_Download_v0.99.6-2ea44f?style=for-the-badge" alt="Download">
  </a>
</p>

> ### ⚠️ This is a beta (v0.99.6)
>
> It works, but it's still being finished: real-hardware edge cases are being
> chased down, the plugin and stereoscopic 3D backends aren't done.
> **A stable release is planned for late November 2026.**
>
> Betas may change the config format. Expect rough edges.
> Hit a problem? → [**open an issue**](https://github.com/mgeuddlyii/3ds-ai-agent-tool/issues)
> or email **dpsp2936@agent.qq.com** ([details](#feedback)).

> ### 🎯 Hardware requirements
>
> | | |
> |---|---|
> | ✅ **Supported** | **New 3DS** / **New 3DS XL** / **New 2DS XL** |
> | ❌ **Not supported** | Original 3DS / 3DS XL / 2DS |
>
> **Why New-series only** — three hard reasons, not a policy choice:
>
> 1. **RAM** — the original 3DS has 128 MB; New-series has 256 MB. The embedded
>    offline guide model takes a large chunk, and there simply isn't room left.
> 2. **Buttons** — the original 3DS **has no `ZL` / `ZR`**. Voice input is
>    "hold `ZL` to talk", and the offline guide is entered with `ZL` too.
>    On older hardware those two features have no entry point at all.
> 3. **CPU** — 134 MHz dual-core vs 268 MHz. This renderer is pure software,
>    so it's CPU-bound; on the older chip it would crawl.
>
> Firmware: **Luma3DS 13+** (needed to run homebrew).

<p align="center">
  <img src="docs/screenshots/home.png" alt="Home screen" width="720"><br>
  <sub>Screenshots show the Chinese UI. The English UI is complete (921 strings) —
  switch it in <b>Settings → Language</b>.</sub>
</p>

---

## What it does

| Feature | Details |
|---|---|
| **Chat** | Direct to DeepSeek / Zhipu / Volcengine Ark / Kimi / Qwen / Doubao / any custom endpoint. Streaming output. |
| **Offline guides** | 211 games, a 1.18M-parameter micro-model + BM25 retrieval embedded in the binary. **Works with no network, no API key, zero balance.** |
| **Voice input** | Hold `ZL`, speak, release — it transcribes and drops the text into the input box. |
| **API key vault** | Up to 8 keys in an encrypted store, switchable from the UI. Also imports from `key.txt`. |
| **Multiple profiles** | Each profile pins its own provider + model. Switching profile = switching model. |
| **AI actions** | The model can request actions (install a file, change a setting…). **Every single one requires you to press A.** |
| **Built-in viewer** | Renders an HTML subset (no JS/CSS) for guides and docs. |
| **Pull files from PC** | Built-in FTP client with segmented progress. |
| **Plugin manager** | Lists `.3gx` files under `luma/plugins/`, can enable/disable them. |

<p align="center">
  <img src="docs/screenshots/chat.png" alt="Chat screen" width="720"><br>
  <sub>Chat: conversation on top, input area + dock on the bottom screen</sub>
</p>

<p align="center">
  <img src="docs/screenshots/more.png" alt="More menu" width="720"><br>
  <sub>"More" submenu — everything infrequent lives here, 6 rows per page</sub>
</p>

---

## Is this for you?

Thirty seconds of honesty saves you a download.

### ✅ Probably yes

- You have a 3DS gathering dust and want it to do something new
- You have an API key, or don't mind topping up a few dollars
  (most providers give new accounts free credit)
- You want to ask something from bed or on the subway without reaching for a phone
- You find "wait, an old handheld can do *this*?" interesting on its own

### ❌ Probably not

- **You want a ChatGPT experience** — a 400×240 screen and a D-pad are fine for
  short exchanges, not long conversations
- **You refuse to set up an API key** — there's an offline demo to poke at,
  but the actual chat needs a key
- **You expect smooth animation** — this is software rendering and the frame rate
  is still being worked on (6.8 → 15.7 fps in the emulator; real hardware to be measured)
- **You want to read long articles** — the viewer only handles an HTML subset,
  and long text is rough on a small screen

> This isn't a sales pitch in reverse. The worst outcome for a beta isn't
> "nobody used it" — it's **the wrong people used it and were disappointed**.
> Knowing the boundaries tells you what to expect.

---

## 📥 Download

<p align="center">
  <a href="https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/tag/v0.99.6">
    <img src="https://img.shields.io/badge/%E2%AC%87%EF%B8%8F_Download_full_package-2ea44f?style=for-the-badge" alt="Download full package">
  </a>
  &nbsp;
  <a href="https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/download/v0.99.6/AIConsole-0.99.6.3dsx">
    <img src="https://img.shields.io/badge/just_the_.3dsx-0969da?style=for-the-badge" alt="Just the 3dsx">
  </a>
</p>

> **Short version:** hit the green button → download `AIConsole-0.99.6-pack.zip`
> → unzip → copy the `.3dsx` into `3ds/` on your SD card → put the card back →
> launch it from Homebrew Launcher.

### Pick one of three

| File | Size | Who it's for |
|---|---|---|
| 🟢 **`AIConsole-0.99.6-pack.zip`** | 6.6 MB | **Most people.** Contains both binaries + install notes + verification tools |
| **`AIConsole-0.99.6.3dsx`** | 5.1 MB | You just want the binary and already know homebrew |
| **`AIConsole-0.99.6.cia`** | 3.8 MB | You want it on the HOME menu (install via FBI; needs signature patches) |

### Direct links

| | |
|---|---|
| 📦 Full package (recommended) | [`AIConsole-0.99.6-pack.zip`](https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/download/v0.99.6/AIConsole-0.99.6-pack.zip) |
| 🔧 3dsx only | [`AIConsole-0.99.6.3dsx`](https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/download/v0.99.6/AIConsole-0.99.6.3dsx) |
| 🎮 cia only | [`AIConsole-0.99.6.cia`](https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases/download/v0.99.6/AIConsole-0.99.6.cia) |
| 📋 All versions | [Releases](https://github.com/mgeuddlyii/3ds-ai-agent-tool/releases) |

### What's inside the package

| File | Purpose |
|---|---|
| `装前必读.md` | Install steps and FAQ — **read this first** (Chinese; the steps are also below) |
| `MANIFEST.txt` | sha256 of every file, so you can verify the download |
| `tools/ciainfo.py` | Dump the CIA's Title ID to check it doesn't collide with anything |
| `tools/smdhdump.py` | Inspect the SMDH title (catches silent truncation) |
| `tools/ftp_push.py` | Push a new build over FTP instead of swapping the SD card |

> 💡 The `AIConsole` in filenames is a **legacy name** — the on-SD data path is
> `sdmc:/3ds/aiconsole/` too. Renaming the product deliberately left the internal
> paths alone, otherwise existing users would lose their config and keys.

> ⚠️ This points at the releases list rather than `releases/latest` on purpose:
> GitHub's `latest` only resolves to non-prerelease versions, and the only
> release right now is a prerelease — that endpoint would 404.

---

## Quick start

### 1. Install it

**`.3dsx` (simplest)**

```
SD:/3ds/AIConsole.3dsx
```

Put the card back → launch Homebrew Launcher → select it → A.

**`.cia` (HOME menu entry)**

Install `AIConsole.cia` with FBI. A new icon appears on the HOME menu.
The short name shows as **"AI Agent"** — SMDH short titles are capped at
8 code units, and the full name would be silently truncated.

### 2. Configure an API key

Three ways, pick one:

**① Type it on the console** (when you're away from a PC)

`Home → More → Add API` → fill in provider / model / base URL / key → save.

**② Import from a file** (saves typing)

Write `SD:/3ds/aiconsole/key.txt` on your PC:

```
sk-your-key-here
```

Or with full fields:

```
provider=deepseek
model=deepseek-chat
key=sk-your-key-here
```

Pointing at your own server:

```
provider=custom
base_url=https://your-server.example.com/v1
model=your-model-name
key=sk-your-key-here
```

Then `Home → More → Add API → Import from file`.

**③ Don't configure anything**

With no key it falls back to **offline demo** mode: replies come from a built-in
script, no network involved. Good for seeing what the UI does.

<p align="center">
  <img src="docs/screenshots/api.png" alt="Credential entry" width="720"><br>
  <sub>Credential screen — provider / model / URL / persona / context turns / key</sub>
</p>

### 3. Talk to it

On the chat screen, **hold `ZL`** and speak. Release and it transcribes.

Speech recognition defaults to **MoArk** ([moark.com](https://moark.com), Gitee's AI
platform) running **Qwen3-ASR-1.7B** — 52 languages plus 22 Chinese dialects, direct
connection from mainland China, **100 free calls per day**. Sign in with a Gitee
account, create a token, then write `SD:/3ds/aiconsole/asr.txt`:

```
key_moark=your_token_here
```

Alternatives, first configured wins: `key_moark=` (MoArk, recommended) →
`key_groq=` (Groq `whisper-large-v3`, 8 h/day free, but its console is blocked on
some networks) → the built-in SiliconFlow key (works with zero setup, stopgap only).

---

## Controls

### Home

| Button | Action |
|---|---|
| `←` `→` / touch | Select a tile |
| `A` | Open the selected tile |
| `L` | Settings |
| `X` | API keys |
| `R` | Software update |
| `ZL` | Offline guides |
| Gear icon, top right (touch) | Settings |

### Chat

| Button | Action |
|---|---|
| `A` | Send |
| `Y` | Keyboard (built-in Pinyin IME) |
| `SELECT` | Quick phrases |
| `R` | Edit phrases |
| `L` | Export chat log |
| `X` | Configure AI / Doubao QR login |
| `ZL` (hold) | Voice input |
| `←` `→` | Cycle reasoning effort |
| `↑` `↓` | Scroll history |
| Bottom-screen dock | Switch panel: chat / keyboard / profiles / files / browse |
| `B` | Back |

### Settings

| Button | Action |
|---|---|
| `↑` `↓` | Move one row |
| `L` `R` | **Page up/down** (7 rows at a time) |
| `A` / `→` | Change the value |
| `←` | Change it the other way |
| `B` | Back |

<p align="center">
  <img src="docs/screenshots/settings.png" alt="Settings" width="720"><br>
  <sub>Settings — 24 rows, colour-coded by category (appearance / AI / reminders / safety gates / diagnostics)</sub>
</p>

---

## Building from source

### Requirements

- [devkitPro](https://devkitpro.org/) with `devkitARM`, `libctru`, `citro3d`
- Python 3.9+ (for the build scripts)
- `DEVKITPRO` environment variable pointing at your devkitPro install

### One command

```bash
python tools/build.py
```

Output: `app/AIConsole.3dsx`, `app/AIConsole.elf`

Add `cia` to also produce the CIA:

```bash
python tools/build.py cia
# -> app/AIConsole.cia
```

> ⚠️ The build script only emits the `.3dsx` by default. If you package without
> passing `cia`, you'll ship a **stale** CIA from a previous build — check the
> file's mtime against the ELF.

### Packaging a release

```bash
cp app/AIConsole.3dsx release/AIConsole-0.99.6.3dsx
cp app/AIConsole.cia  release/AIConsole-0.99.6.cia
python tools/make_pack.py
# -> dist/AIConsole-0.99.6-pack.zip (re-verified by sha256 on unpack)
```

### Running the automated tests

```bash
python tools/azahar_test.py --passes "0,1,4,5,K" --wait 200 --no-png
```

Requires the [Azahar](https://azahar-emu.org/) emulator. Each "pass" is a key
script plus a set of assertions; it prints PASS/FAIL and leaves `status.txt` and
`boot.log` inside the emulator's virtual SD card.

---

## Project layout

```
app/
  source/
    main.c            Boot path + main loop (with a frame-time probe)
    gfx/              Software rasteriser (double buffering, clipping, glyphs)
    ui/               19 screens, one .c each
    test/             Test probes (bootlog / autorun / perfprobe)
  data/               Font, model weights, CA bundle (embedded at compile time)
shared/               Logic that doesn't touch the UI
  nnet/               Embedded micro-model + knowledge base (offline guides)
  chatapi.c           HTTP / streaming SSE / request assembly
  config.c            JSON config load/save
  keystore.c          Multi-key vault (encrypted at rest)
  credfile.c          key.txt inbox
  actions.c           AI action protocol + safety gates
plugin/translate/     Luma 3gx plugin (skeleton, translation not implemented)
tools/                Build, test, i18n and corpus-training scripts
docs/                 Design docs and screenshots
```

### Which doc to read

| File | Audience |
|---|---|
| **`CHANGELOG.md`** | Humans. What changed in each version, and which bug it fixed |
| **`使用手册.md`** | Humans, Chinese. Full project overview, screen reference, build/test, safety levels, constant cross-reference |
| `ARCHITECTURE.md` | Humans, Chinese. Layering, frame rendering, request path, server design |
| `SAFETY_DESIGN.md` | Humans, Chinese. The four gates (data-only model output / compile-time whitelist / physical confirmation / paths never from the AI) |
| `AGENT.md` | **The cloud model.** Injected into the system message on every request; teaches it the `[[ACT:]]` protocol and the red lines |

> ⚠️ `AGENT.md` is not a manual for people — it's the model's operating
> instructions. Editing it changes the model's behaviour.

---

## Design decisions

**No self-hosted server.** The 3DS talks to providers directly. The cost is that
every user brings their own key; the benefit is no middleman, no operating cost,
and no privacy exposure.

**Offline guides use a micro-model, not an API call.** 1.18M parameters running
on the ARM11, paired with BM25 retrieval, with 211 games' worth of guide content
stored on the SD card. Works offline, with no key, with zero balance.

**The AI must pass four gates before it can act.** The model can only emit
structured data (never code, never paths), action names go through a compile-time
whitelist, the user presses A to confirm, and paths never originate from the
model. See `SAFETY_DESIGN.md`.

---

## Known limitations

- **`.3dsx` builds have no system manual** — only `.cia` does (the 3DS manual is a
  separate content, and a 3DSX isn't a title)
- **The `.3gx` plugin is a skeleton** — translation isn't implemented
- **No stereoscopic 3D** — the current backend is 2D
- **Safety levels L1–L3 (filesystem/system domains) aren't fully wired up**;
  L0 chat works
- Real-hardware testing covers the main paths; edge cases are still being filled in
- Frame-rate numbers come from the Azahar emulator; real hardware hasn't been measured

---

## Feedback

This is a beta — things will break. **Telling me beats putting up with it.**

### Fastest: GitHub Issues

👉 **https://github.com/mgeuddlyii/3ds-ai-agent-tool/issues**

Public, searchable, and you can see what others already hit. Skim before filing —
someone may have reported it.

### No GitHub? Email works

📮 **dpsp2936@agent.qq.com**

Put the feature in the subject (e.g. "Chat · voice input"), and in the body say
**what you did → what you saw → what you expected**.

### Two files that make debugging ten times faster

| File | Where | What's in it |
|---|---|---|
| `status.txt` | `SD:/3ds/aiconsole/` | Version, config state, frame-time probe, result of the last action |
| `boot.log` | `SD:/3ds/aiconsole/` | Boot trace — where it stopped before it crashed |

Both are plain text; just read the SD card on a PC.

### One more thing

Bug reports **don't need to be polished**. "White screen after I tapped Add API"
beats "the software exhibits an anomaly" by a factor of a hundred —
**a reproducible description beats jargon.**

---

## License

[MIT](LICENSE) — use it, modify it, ship it commercially. No warranty.

### About the bundled speech key

`ASR_BUILTIN_KEY` in `shared/asr.c` is **deliberately public**: that account has a
zero balance and is only used for speech recognition, so abuse costs nothing.

**Don't "fix" it by blanking it** — that would drop "hold ZL to talk" straight
back to "no key configured", and ordinary users wouldn't know what to set up.

Paid chat keys never go into this repo (see `.gitignore`).

---

## Disclaimer

Unofficial homebrew. Not affiliated with Nintendo in any way.

Modifying your system and installing homebrew carries risk. Bricked consoles,
lost saves, account issues — that's on you. Back up your SD card first.

---

## Credits

| | |
|---|---|
| **Design** | acrac |
| **Implementation** | DeepSeek V4.1-Flash |
| **Current version** | v0.99.6 (beta) |
| **Stable release** | Planned for late November 2026 |
| **License** | MIT |

"Design" means deciding what to build, what it should look like, and which
trade-offs are acceptable. "Implementation" means the architecture, the rendering
layer, every screen, the built-in IME and offline retrieval, plus each round of
automated testing. Both live in this repo — file issues right here.
