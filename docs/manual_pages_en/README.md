# manual_pages_en — Route B manual page pack (English)

> Status: **resource ready / not embedded** (2026-10-17).
> These 16 `.html` files are the screen-split English edition of
> `docs/manual_pages/` (Chinese), written against the HTML subset that
> `app/source/gfx/html.c` actually supports.

## Page list (order = suggested manual TOC order)

| File | Content | Chinese source | Master section |
|---|---|---|---|
| `00_about.html` | what it is / forms / models / first launch | `00_关于.html` | §1 |
| `01_quickstart.html` | three prep items + first chat | `01_快速上手.html` | §2 |
| `02_home.html` | core tiles + Settings | `02_主界面.html` | §3 |
| `03_chat.html` | controls + action confirm + translate + states | `03_对话.html` | §4 |
| `04_credentials.html` | three credential ways + priority | `04_配凭据.html` | §5 |
| `05_offline_guides.html` | 211 games + BM25 + usage | `05_离线攻略.html` | §6 |
| `06_more_menu.html` | 6 entries | `06_更多菜单.html` | §7 |
| `07_settings.html` | five groups + safety level entry | `07_设置.html` | §8 |
| `08_software_update.html` | manifest / install / confirm | `08_软件更新.html` | §9 |
| `09_security_gates.html` | four gates + action allowlist | `09_安全_四道闸.html` | §10 |
| `10_security_levels.html` | level table + audit/diagnostic files | `10_安全_等级.html` | §10 |
| `11_buttons.html` | button table | `11_按键.html` | §11 |
| `12_FAQ_1.html` | FAQ (1): 6 questions | `12_FAQ_1.html` | §12 |
| `12_FAQ_2.html` | FAQ (2): 5 questions | `12_FAQ_2.html` | §12 |
| `13_troubleshooting.html` | symptom table | `13_故障排查.html` | §13 |
| `14_credits.html` | acrac / DS Ojousama / artwork note | `14_署名.html` | credits |

## On-screen strings: provenance

Button names, level names and status lines in this pack are the
**official English strings from `shared/uistr_table.c`** (verified
2026-10-17), not free translation. Terms follow
`docs/英文版术语表.md` (e.g. Allowlist, never "whitelist"; Thinking
level; Gate; Audit):

- Buttons: Chat (S0367) / Providers (S0368) / Settings (S0369) /
  Guides (S0370) / Region set (S0371) / PC files (S0372) /
  Plugins (S0881) / More (S0888) / Translate (S0893) / Update (S0277)
- Levels: L0 Chat (S0819) / L1 Files (S0820) / L2 System (S0821) /
  L3 Low-level (S0822) / L4 Full system (S0823) / L5 Highest (S0824).
  Note the English table has six levels; the Chinese pack page lumps
  L3-L5 as "higher levels" - this pack names all three, per the table.
- Status lines: S0104 "Microphone unavailable - see status.txt" /
  S0106 "Recording under 0.5 s - hold ZL, then speak" /
  S0108 "No audio captured - check the mic is not in use" /
  S0111 "Nothing recognised - try again, slower" /
  S0112 "Connection failed - reason on the top screen" /
  S0113 "Key rejected (%ld) - reason on the top screen" /
  S0304 "'%s' is already the latest; nothing to install."
- **One exception:** the guide-screen failure message
  "vocabulary load failed - search unavailable" is a **descriptive
  translation only**. The source string
  `app/source/ui/sc_guide.c:957` is a hardcoded Chinese literal
  ("词表加载失败 · 检索不可用") with no entry in the uistr table, so
  no official English exists yet. Quoted on `05_offline_guides.html`
  and `12_FAQ_1.html`; flagged here so a future i18n pass can add a
  real S-id for it.
- `14_credits.html`: the Chinese original of the credit line is
  「由 DS 大小姐构建，制作」; rendered here as "Built and made by
  DS Ojousama".

## Writing constraints (html.c capability boundary)

- Only `h1` / `h2` / `h3` / `p` / `div` / `li` / `ul` / `ol` /
  `blockquote`.
- **No** inline styles (b/i/strong/em do not render), **no** `img`
  (the viewer has no texture mechanism), **no** JS/CSS/links.
- Keep each page around 13 lines (one 3DS screen of capacity);
  split when over budget.
- Keep on-screen wording identical to the uistr strings above.

## Guards (copies, same version as the Chinese pack)

- `check_pages.py` — allowed-tag set + h1 first + line budget (<=20).
- `render_preview.py` — PC-side replica of the html.c + text.c
  rendering chain; flags any page over 3 screens (27 lines).

Run from inside this directory:

    python check_pages.py --list
    python render_preview.py

Both scripts are unmodified copies of the Chinese pack's guards
(2026-10-17), so each pack is self-contained. If a guard is updated
in one pack, re-sync the other.

## Relation to the master

The Chinese master `docs/说明书内容草稿.md` (v1.0) remains the single
source of content truth; the Chinese pack `docs/manual_pages/` is its
screen-split form; this pack is the English edition of that pack.
Change content in the master first, then sync both packs.
Artwork: `docs/deepseek_whale_girl.png` (repo docs only - the viewer
cannot show images yet). The master's Appendix B line "English version
follows the long-text schedule" refers to uistr integration of long
texts, which is still pending; this pack is the ready resource for it.
