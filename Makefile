$(info KERNEL_DIR:    $(KERNEL_DIR))
$(info PLATFORM:      $(PLATFORM))
$(info CHIPSET:       $(CHIPSET))

export BASE_DIR := $(PWD)
export CORE_DIR := $(BASE_DIR)/core
export OBJDIR := $(BASE_DIR)/.objs

ifeq ($(PLATFORM),virt)
SUBDIRS = stdlib core tinycrypt platform/$(PLATFORM)
else
SUBDIRS = platform/$(PLATFORM) stdlib core tinycrypt platform/$(PLATFORM)/common
endif
include core/tools.mk
include core/makevars.mk
include core/makeflags.mk

all: check dirs
check:
	@[ "${KERNEL_DIR}" ] && echo -n "" || ( echo "KERNEL_DIR is not set"; exit 1 )
	@[ "${PLATFORM}" ] && echo -n "" || ( echo "PLATFORM is not set"; exit 1 )
	@[ "${TOOLS}" ] && echo -n "" || ( echo "TOOLS is not set"; exit 1 )
	@[ "${PLATFORM}" = "virt" ] || [ "${CHIPSET}" ] && echo -n "" || ( echo "CHIPSET is not set"; exit 1 )

dirs: $(SUBDIRS) | $(OBJDIR)
	@for DIR in $(SUBDIRS); do \
		$(MAKE) $(SUBMAKEFLAGS) ENABLE_TESTS=false -C$${DIR}; \
	done

clean:
	@for DIR in $(SUBDIRS); do \
		$(MAKE) $(SUBUMAKEFLAGS) -C$${DIR} clean; \
	done
	@rm -rf $(OBJDIR)

submodule-update:
	@echo "Fetching sources.."
	@git submodule update --init --recursive

$(TOOLS_GCC): | submodule-update
	@mkdir -p $(TOOLDIR)
	@echo "Extracting tools.."
	@pbzip2 -dc $(TOOLS) | tar x -C $(TOOLDIR)

$(OBJDIR): | $(TOOLS_GCC)
	@mkdir -p $(OBJDIR)/$(PLATFORM)

gdb:
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) KERNEL_DIR=$(KERNEL_DIR) -Cplatform/$(PLATFORM) gdb

run:
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) KERNEL_DIR=$(KERNEL_DIR) -Cplatform/$(PLATFORM) run

tools: | $(TOOLS_GCC)

test: | module-test

module-test:
	python scripts/module-test.py $(MODULE)

package:
	$(MAKE) -C platform/$(PLATFORM)/tools/sign

.PHONY: all check submodule-update tools clean gdb qemu package run $(SUBDIRS)