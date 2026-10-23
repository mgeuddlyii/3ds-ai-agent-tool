#---------------------------------------------------------------------------------
# doubao probe — 3dsx that logs into Doubao with no relay server
#
# Purpose: prove (or disprove) that a 3DS can complete a TLS 1.2 handshake.
# Links libcurl + 3ds-mbedtls because the stock httpc/sslc stops at TLS 1.1.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := doubao
BUILD       := build
SOURCES     := source
INCLUDES    := source

APP_TITLE       := doubao probe
APP_DESCRIPTION := Doubao direct-access probe
APP_AUTHOR      := WorkBuddy
ICON            := $(DEVKITPRO)/libctru/default_icon.png

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS      := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS      += $(INCLUDE) -D__3DS__
ASFLAGS     := -g $(ARCH)
LDFLAGS      = -specs=3dsx.specs -g $(ARCH) -Wl,-z,noexecstack -Wl,-Map,$(notdir $*.map)

# portlibs: 3ds-curl is built against 3ds-mbedtls, which devkitPro patches to
# draw entropy from sslcGenerateRandomData() (MBEDTLS_ENTROPY_HARDWARE_ALT).
PORTLIBS    := $(DEVKITPRO)/portlibs/3ds
LIBS        := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS     := $(CTRULIB) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD   := $(CC)
export OFILES_SRC := $(CFILES:.c=.o)
export OFILES := $(OFILES_SRC)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh $(TARGET).map

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).3dsx

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh

$(OUTPUT).smdh: $(TOPDIR)/Makefile
	@echo building smdh ...
	$(DEVKITPRO)/tools/bin/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(ICON) $@

$(OUTPUT).elf: $(OFILES)
	@echo linking $(notdir $@)
	$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

%.o: %.c
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

-include $(DEPENDS)

endif
