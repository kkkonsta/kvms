# SPDX-License-Identifier: GPL-2.0-only
PVAR := $(shell echo $(PLATFORM) | tr a-z A-Z)
export DEFINES := -D$(PVAR) -D_GNU_SOURCE -D__OPTIMIZE__ -include "config.h"
export WARNINGS := -Wall -Werror -Wno-pointer-arith -Wno-variadic-macros
export INCLUDES := -I. -I$(KERNEL_DIR) -I$(CORE_DIR) -I$(BASE_DIR)/stdlib \
		-I$(BASE_DIR)/tinycrypt/lib/include/tinycrypt \
		-I$(BASE_DIR)/platform/common \
		-I$(BASE_DIR)/platform/$(PLATFORM)/common \
		-I$(BASE_DIR)/platform/$(PLATFORM) \
		-I$(BASE_DIR)/platform/$(PLATFORM)/$(CHIPSET) \
		-I$(BASE_DIR)/platform/$(PLATFORM)/$(CHIPSET)/$(PRODUCT) \
		-I$(BASE_DIR)/stdlib/sys \
		-I$(OBJDIR)/$(PLATFORM)/$(CHIPSET)/$(PRODUCT)
export CFLAGS := -march=armv8-a+nofp --sysroot=$(TOOLDIR) --no-sysroot-suffix \
		-fstack-protector-strong -mstrict-align -static -ffreestanding \
		-fno-hosted -std=c99 -mgeneral-regs-only -mno-omit-leaf-frame-pointer \
		-Wstack-protector $(DEFINES) $(OPTS) $(INCLUDES) $(WARNINGS)
export ASFLAGS := -D__ASSEMBLY__ $(CFLAGS)
export LDFLAGS := -O1 --gc-sections -nostdlib \
		-L$(BASE_DIR)/tinycrypt/lib \
		-L$(BASE_DIR)/.objs
export EXT_CFLAGS := '--sysroot=$(TOOLDIR) --no-sysroot-suffix'
export SUBMAKEFLAGS := CROSS_COMPILE=$(CROSS_COMPILE) CC=$(CC) LD=$(LD) \
	AR=$(AR) OBJCOPY=$(OBJCOPY) OUTPUT_OPTION=$(EXT_CFLAGS) \
	KERNEL_DIR=$(KERNEL_DIR)
