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
           -I$(DEVKITPRO)/libctru/include
ASFLAGS := $(ARCH) -I$(DEVKITPRO)/libctru/include
# ★ 不用 `-specs=3dsx.specs`：那套 specs 里带 `-T 3dsx.ld`，会和我们的
#   `-T translate.ld` 同时生效 —— 结果是链接出**两套 PT_LOAD**
#   （3gxtool 直接报 "Too many segments!"），而且基址被顶到 0x00100000。
#   插件要自己完全掌控段布局，所以这里手给参数，只要 `-nostartfiles`
#   （不链 crt0，入口由 boot.s 当）和 libctru 的库路径。
LDFLAGS := -nostartfiles \
           -L$(DEVKITPRO)/libctru/lib \
           -T translate.ld $(ARCH) -O2 \
           -Wl,--gc-sections -Wl,-Map,translate.map
LIBS    := -lctru -lm

TARGET  := translate
BUILD   := build

SRCS_C  := main.c
SRCS_S  := boot.s
OBJS    := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o)) $(addprefix $(BUILD)/,$(SRCS_S:.s=.o))

.PHONY: all clean

all: $(TARGET).3gx

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	@echo "  AS   $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) translate.ld
	@echo "  LD   $@"
	@$(LD) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

# ★ 3gxtool 吃的是 **ELF 本体**（v2 格式），不是 objcopy 出来的裸 bin。
#   老教程里那句 `objcopy -O binary` 是 v1（3GX$0001）的写法，
#   照做会被现行 Luma 判成 "Outdated plugin file"。
$(TARGET).3gx: $(TARGET).elf translate.plgInfo
	@echo "  3GX  $@"
	@$(G3XTOOL) -s $< translate.plgInfo $@
	@ls -la $@

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3gx $(TARGET).map
