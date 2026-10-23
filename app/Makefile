#---------------------------------------------------------------------------------
# 3DS AI Console - Application (.cia + .3dsx)
#
# Standard devkitPro 3DS Makefile. Differences from stock:
#   * `make cia` additionally packages the .cia with makerom
#   * `make clean` does NOT delete AIConsole.rsf (it is a source file)
#
# Expects DEVKITARM / DEVKITPRO in the environment (see tools/env.cmd).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
# DATA is a list of directories containing binary data
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile
#---------------------------------------------------------------------------------
TARGET      := AIConsole
BUILD       := build
SOURCES     := source source/gfx source/ui source/test ../shared ../shared/nnet
DATA        := data
INCLUDES    := source source/gfx source/ui source/test ../shared ../shared/nnet

# 注意：devkitPro 的 Makefile 用$(notdir) 把对象名拍平到 build/ 下，
# 所以**不同目录里不能有同名的 .c**。新增子目录时先确认没有重名。
# （shared/nnet/ 已确认与 source/* 无重名：guide_kb / nnet / nnet_text / nnet_kb_data）

# ⚠️ 推理内核已从 EXCLUDE_SRC 移除（2026-10-04）。
# 移除前先修掉了三个静默错误，它们 gcc 一个字都没报：
#   1. RoPE 配对方式错（交错 vs 训练端的前后半分半）+ 漏了多头循环
#      → 只有 head 0 有位置编码，3/4 的头从来没被转过
#   2. 架构不匹配：nnet.c 是 post-LN + 无残差，而模型按 pre-LN + 残差训练
#      → 数学上完全不同，输出是垃圾但程序跑得好好的
#   3. nnet_state_bytes 把 cache_k_t 按一份算，实际访问 L 份
#      → 第 1 层写完就踩后面的堆，差 122880 字节
# 详见 shared/nnet/nnet.c 里的注释。
# 口径与 tools/build.py 的 EXCLUDE_SRC 保持一致，两条构建路径不能分叉。
EXCLUDE_SRC  :=

# 点阵字库走 bin2o（app/data/font.bin -> font_bin.o），不走 romfs。
# 原因：makerom 0.19 的 -romfs 不接受 mkromfs3ds 产出的标准 RomFS 镜像，
# 带 / 不带目录哈希表两种布局都被判为 "Invalid RomFS Binary"，
# 于是 .cia 里装不进 romfs。字库本来就要整份载入内存，bin2o 直接绕开。
ROMFS       :=

# 与 tools/build.py 的 APP_TITLE 保持一致（两条构建路径不能分叉）。
# 中文标题可用：smdhtool 会正确写成 UTF-16LE，见 tools/build.py 里的实测记录，
# 验证用 tools/smdhdump.py（别只看 smdhtool 的退出码，它写坏了也返回 0）。
APP_TITLE       := 测试 0.99
APP_DESCRIPTION := AI console for 3DS homebrew
APP_AUTHOR      := WorkBuddy

# HOME menu icon: app/icon.png if present, else the libctru one.
ifeq ($(wildcard $(TOPDIR)/icon.png),)
ICON := $(DEVKITPRO)/libctru/default_icon.png
else
ICON := $(TOPDIR)/icon.png
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS      := -g -Wall -O2 -mword-relocations \
               -ffunction-sections \
               $(ARCH)

CFLAGS      += $(INCLUDE) -D__3DS__

# ---- 推理内核的可切换实现（与 tools/build.py 的同名开关对齐）--------
#
# AIC_DOT_VIA_LDR=1 → nnet.c 的 dot_i8_i8 走「ldr + sxtb」：
# 每 4 个 MAC 里 w 侧访存从 4 次降到 1 次。
# 不设 → 原来的逐字节 ldrsb，4 路展开。
#
# ⚠️ 这里**必须**与 tools/build.py 用同一套判断式。
# 本项目已经因为两条构建路径口径分叉咬过一次（EXCLUDE_SRC 那次：
# 改了 Makefile 忘了改 build.py，于是"改了代码没进构建"，
# 而 gcc 一个字都不报）。性能 A/B 更怕这个 ——
# 两条路径编出不同的 dot，实现却只有一份，测出来的数字毫无意义。
ifneq ($(strip $(AIC_DOT_VIA_LDR)),)
CFLAGS      += -DNNET_DOT_VIA_LDR
endif

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
# -Wl,-z,noexecstack：devkitARM 自带的 crtn.o 缺 .note.GNU-stack，
# 不显式声明会打一条 "implies executable stack" 的弃用告警。
LDFLAGS      = -specs=3dsx.specs -g $(ARCH) -Wl,-z,noexecstack -Wl,-Map,$(notdir $*.map)

LIBS        := -lcitro3d -lctru -lm -lz

# portlibs 3ds：zlib 的头和库。只为了 guide_kb.c 里的 uncompress()
# —— 知识库是压缩内嵌的（tools/guia/export_kb_zlib.py，408KB -> 165KB）。
# libz 本来是 3ds-curl 的依赖，但那个功能没编进这个 CIA，所以自己加。
# 3DS 的 portlibs 是静态库，链进去不占 SD 卡。
LIBDIRS     := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT       := $(CURDIR)/$(TARGET)
export TOPDIR       := $(CURDIR)
export VPATH        := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                       $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR      := $(CURDIR)/$(BUILD)

CFILES      := $(filter-out $(EXCLUDE_SRC),\
                 $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES      := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN  := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ROMFS)),)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
else
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Package the .cia. Requires the .elf/.smdh built by the step above and the
# hand-written AIConsole.rsf.
#---------------------------------------------------------------------------------
cia: $(BUILD)
	@echo "building $(TARGET).cia ..."
	$(DEVKITPRO)/tools/bin/makerom -f cia -target t \
		-o $(CURDIR)/$(TARGET).cia \
		-elf $(CURDIR)/$(TARGET).elf \
		-rsf $(CURDIR)/$(TARGET).rsf \
		-icon $(CURDIR)/$(TARGET).smdh \
		-exefslogo
	@[ -f $(CURDIR)/$(TARGET).cia ] && echo "CIA: $(CURDIR)/$(TARGET).cia"

#---------------------------------------------------------------------------------
# Clean. NOTE: AIConsole.rsf is a *source* file - never delete it here.
#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh $(TARGET).cia
	@rm -fr $(TARGET).map

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).3dsx

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh

$(OFILES_SRC): $(HFILES_BIN)

#---------------------------------------------------------------------------------
# 覆盖 devkitPro 的 bin2o 规则：bin2s 的输出没有 .note.GNU-stack 段，
# 链接器会为此报一条 "executable stack" 弃用告警。这里补上再汇编。
# 与 tools/build.py 的步骤 1 保持一致。
#---------------------------------------------------------------------------------
%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	$(SILENTCMD)bin2s -a 4 -H $*_bin.h $< > $*.bin.s
	$(SILENTCMD)printf '\n.section .note.GNU-stack,"",%%progbits\n' >> $*.bin.s
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(ASFLAGS) -c $*.bin.s -o $@

#---------------------------------------------------------------------------------
# SMDH: title / description / author / icon for the HOME menu entry.
#---------------------------------------------------------------------------------
$(OUTPUT).smdh: $(TOPDIR)/Makefile
	@echo building smdh ...
	$(DEVKITPRO)/tools/bin/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(ICON) $@

#---------------------------------------------------------------------------------
$(OUTPUT).elf: $(OFILES)
	@echo linking $(notdir $@)
	$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

#---------------------------------------------------------------------------------
%.o: %.c
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

%.o: %.cpp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) -c $< -o $@

%.o: %.s
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(ASFLAGS) -c $< -o $@

-include $(DEPENDS)

endif
