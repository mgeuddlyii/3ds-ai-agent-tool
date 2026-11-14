# Makefile —— 把 translate 插件打成 .3gx
#
# ── 和主程序（build.py）的关系 ──
# 完全独立：插件是**另一个可执行体**，跑在游戏进程里，不共享主程序的任何
# 代码或内存。所以这里不用 build.py，也不链接主程序的 .o。
# 唯一共用的是"同一个人在维护"这件事。
#
# ── 为什么 ARCH 里没有 -march=native 之类 ──
# 插件跑在**游戏进程**里，必须和游戏同架构约束（armv6k + hard float）。
# 用 -O2 而不是 -Os：插件体积上限宽松，性能比体积重要
# （翻译时要解码 JSON / 拼 HTTP）。
#
# 用法：
#   make                # 产出 translate.3gx
#   make clean

ifeq ($(strip $(DEVKITPRO)),)
  export DEVKITPRO := /opt/devkitpro
endif
ifeq ($(strip $(DEVKITARM)),)
  export DEVKITARM := $(DEVKITPRO)/devkitARM
endif

PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
CC      := $(PREFIX)gcc
LD      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
G3XTOOL := $(DEVKITPRO)/tools/bin/3gxtool.exe

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := $(ARCH) -O2 -Wall -Wextra -std=gnu11 \
           -ffunction-sections -fdata-sections -fno-strict-aliasing \
           -I$(DEVKITPRO)/libctru/include \
           -I$(DEVKITPRO)/portlibs/3ds/include \
           -I../../shared
ASFLAGS := $(ARCH) -I$(DEVKITPRO)/libctru/include
# ★ 不用 `-specs=3dsx.specs`：那套 specs 里带 `-T 3dsx.ld`，会和我们的
#   `-T translate.ld` 同时生效 —— 结果是链接出**两套 PT_LOAD**
#   （3gxtool 直接报 "Too many segments!"），而且基址被顶到 0x00100000。
#   插件要自己完全掌控段布局，所以这里手给参数，只要 `-nostartfiles`
#   （不链 crt0，入口由 boot.s 当）和 libctru 的库路径。
LDFLAGS := -nostartfiles \
           -L$(DEVKITPRO)/libctru/lib \
           -L$(DEVKITPRO)/portlibs/3ds/lib \
           -T translate.ld $(ARCH) -O2 \
           -Wl,--gc-sections -Wl,-Map,translate.map
# 顺序有讲究：curl 依赖 mbedtls 与 zlib，静态链接时被依赖的放后面
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

TARGET  := translate
BUILD   := build

# font16.c 是烤出来的点阵字库（tools/mkfont16.py 生成），别手改。
# 它 1.4MB 源码 / 235KB 二进制 —— 插件体积上限宽松，换来中文能直接显示。
SRCS_C  := main.c cfg.c hud.c help.c png.c net.c font16.c lcd.c
SRCS_S  := boot.s
OBJS    := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o)) $(addprefix $(BUILD)/,$(SRCS_S:.s=.o))

# ---------------------------------------------------------------------------
# 复用主程序的 Key 仓库与服务商表（2026-11-14）
#
# ★ 为什么必须复用、不能在插件里再写一套：
#   用户要求"只能选已经添加的 Key"，而"已添加的 Key"就住在
#   sdmc:/3ds/aiconsole/keys.json 里（shared/keystore.c 在管）。
#   插件原来自己解析 key.txt，两套判断迟早不一致，且**不会报错**——
#   只会静默用错 Key 或走错地址。
# ★ 为什么还要 provider.c：KeyEntry 里**没有网址字段**，
#   base_url 只存在于服务商表里。只拿 Key 不拿表，就会出现
#   "用 A 家的 Key 发到 B 家的地址"这种最难查的错。
# ---------------------------------------------------------------------------
SHARED_SRC := ../../shared/keystore.c ../../shared/creds.c ../../shared/crypto.c \
              ../../shared/minijson.c ../../shared/sha256.c ../../shared/provider.c \
              ../../shared/uistr.c ../../shared/uistr_table.c ../../shared/locale.c
SHARED_OBJ := $(addprefix $(BUILD)/sh_,$(notdir $(SHARED_SRC:.c=.o)))

$(BUILD)/sh_%.o: ../../shared/%.c $(HDRS) | $(BUILD)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean

all: $(TARGET).3gx

$(BUILD):
	@mkdir -p $(BUILD)

# ★ 头文件必须挂成依赖（2026-11-14 踩过，代价是一整趟真机测试）：
#   原来只有 `%.o: %.c`，我改了 lcd.h 之后 make 认为 lcd.o 还是新的，**不重编译**，
#   打出来的 .3gx 里跑的还是旧代码 —— 真机上看到的现象跟"改动没生效"一模一样。
#   这种"构建不可信"比 bug 本身更难查，所以宁可多编几次。
HDRS := $(wildcard *.h)

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.s $(HDRS) | $(BUILD)
	@echo "  AS   $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

$(TARGET).elf: $(OBJS) $(SHARED_OBJ) translate.ld
	@echo "  LD   $@"
	@$(LD) $(OBJS) $(SHARED_OBJ) $(LDFLAGS) $(LIBS) -o $@

# ★ 3gxtool 吃的是 **ELF 本体**（v2 格式），不是 objcopy 出来的裸 bin。
#   老教程里那句 `objcopy -O binary` 是 v1（3GX$0001）的写法，
#   照做会被现行 Luma 判成 "Outdated plugin file"。
$(TARGET).3gx: $(TARGET).elf translate.plgInfo
	@echo "  3GX  $@"
	@$(G3XTOOL) -s $< translate.plgInfo $@
	@ls -la $@

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3gx $(TARGET).map

# ---------------------------------------------------------------------------
# 分步探针：`make diag` 出 diag.3gx
#
# 它只链 boot.s + probe2.c（不碰界面、字库、网络），目的是把
# 「加载 → srv/fs → TLS 嫁接 → newlib → gsp → gfx」拆成 D0~D7 八个路标，
# 每走一步覆盖写一次 sdcard:/3ds/aiconsole/plugin_boot.txt。
# 读出来那一行 = 最后活着走到的步骤。
#
# ★ 体积故意做小（约 88KB），这样"插件太大加载不进去"这个变量就被排除掉了。
# ---------------------------------------------------------------------------
diag: $(BUILD)/boot.o $(BUILD)/probe2.o
	@echo "  LD   probe2.elf"
	@$(LD) $(BUILD)/boot.o $(BUILD)/probe2.o $(LDFLAGS) -lctru -lm -o probe2.elf
	@echo "  3GX  diag.3gx"
	@$(G3XTOOL) -s probe2.elf translate.plgInfo diag.3gx
	@ls -la diag.3gx

# ---------------------------------------------------------------------------
# 根目录版探针：`make root` 出 rootdiag.3gx → 放到 /luma/plugins/default.3gx
#
# 和 diag 的区别：**全程不碰 gfx**，所以加载进主菜单也不会有副作用。
# 多带了 APT_GetProgramID，会把当前 title 的低 32 位 ID 写进痕迹文件，
# 用来分「主菜单那次跑」还是「游戏那次跑」。
# ---------------------------------------------------------------------------
root: $(BUILD)/boot.o $(BUILD)/probe3.o
	@echo "  LD   probe3.elf"
	@$(LD) $(BUILD)/boot.o $(BUILD)/probe3.o $(LDFLAGS) -lctru -lm -o probe3.elf
	@echo "  3GX  rootdiag.3gx"
	@$(G3XTOOL) -s probe3.elf translate.plgInfo rootdiag.3gx
	@ls -la rootdiag.3gx

# ---------------------------------------------------------------------------
# 游戏目录版探针（跳过 gspInit）：`make titlediag`
#
# 22:10 真机实测 gspInit() 不返回（gsp::Gpu 单会话独占，游戏已握着）。
# 这个目标用 -DPROBE_SKIP_GSP 把它摘掉，用来验证两件事：
#   ①正确的 TID 目录（0004000000186700）下加载生不生效
#   ②插件不碰 gsp 时，游戏会不会被拖死（即插件是否占着主线程）
# ---------------------------------------------------------------------------
titlediag:
	@echo "  CC   probe3.c (-DPROBE_SKIP_GSP)"
	@$(CC) $(CFLAGS) -DPROBE_SKIP_GSP -c probe3.c -o $(BUILD)/probe4.o
	@echo "  LD   probe4.elf"
	@$(LD) $(BUILD)/boot.o $(BUILD)/probe4.o $(LDFLAGS) -lctru -lm -o probe4.elf
	@echo "  3GX  titlediag.3gx"
	@$(G3XTOOL) -s probe4.elf translate.plgInfo titlediag.3gx
	@ls -la titlediag.3gx

# ---------------------------------------------------------------------------
# 架构验证版：`make stage1`
#
# 只验两件事：①main() 能不能安全返回 ②工作线程补 TLS 后能不能跑 newlib。
# 完全不碰显示，所以不会因为寄存器未映射把游戏打崩。
# ---------------------------------------------------------------------------
stage1: $(BUILD)/boot.o $(BUILD)/stage1.o
	@echo "  LD   stage1.elf"
	@$(LD) $(BUILD)/boot.o $(BUILD)/stage1.o $(LDFLAGS) -lctru -lm -o stage1.elf
	@echo "  3GX  stage1.3gx"
	@$(G3XTOOL) -s stage1.elf translate.plgInfo stage1.3gx
	@ls -la stage1.3gx

# ---------------------------------------------------------------------------
# 架构+绘制 一起验：`make stage2`
#
# 正确入口（照抄 CTRComposer）+ 工作线程 + 直接读 LCD 寄存器画像素。
# 跑通的话：游戏正常启动，下屏底部出现闪烁的洋红色带，痕迹停在 `B4 drawing`。
# ---------------------------------------------------------------------------
stage2: $(BUILD)/boot.o $(BUILD)/stage2.o $(BUILD)/lcd.o
	@echo "  LD   stage2.elf"
	@$(LD) $(BUILD)/boot.o $(BUILD)/stage2.o $(BUILD)/lcd.o \
	      $(LDFLAGS) -lctru -lm -o stage2.elf
	@echo "  3GX  stage2.3gx"
	@$(G3XTOOL) -s stage2.elf translate.plgInfo stage2.3gx
	@ls -la stage2.3gx


