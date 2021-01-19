# SPDX-License-Identifier: GPL-2.0-only
export CROSS_COMPILE := aarch64-linux-gnu-
export CC := $(CROSS_COMPILE)gcc
export CXX := $(CROSS_COMPILE)g++
export LD := $(CROSS_COMPILE)ld
export AS := $(CROSS_COMPILE)as
export AR := $(CROSS_COMPILE)ar
export AS := $(CROSS_COMPILE)gcc
export OBJCOPY := $(CROSS_COMPILE)objcopy
export RANLIB := $(CROSS_COMPILE)ranlib
export TOOLDIR := $(BASE_DIR)/buildtools
export PATH := $(BASE_DIR)/qemu/build/aarch64-softmmu:$(BASE_DIR)/buildtools/bin:$(PATH)
export TOOLS_GCC := $(TOOLDIR)/bin/$(CC)