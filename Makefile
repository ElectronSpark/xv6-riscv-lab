# xv6-os kernel sub-repo — thin wrapper around the CMake build.
#
# This Makefile exists so the kernel sub-repo can be invoked uniformly
# via `make ARCH=... CROSS_COMPILE=... O=... all`, including by the
# umbrella's BuildKernel.cmake driver.
#
# All real build logic lives in the standalone CMakeLists.txt next to
# this file (which itself is lifted from xv6-tmp's monolithic top
# CMakeLists, trimmed to kernel-only concerns).

ARCH          ?= riscv
PLATFORM      ?= qemu
OPT           ?= 0
LAB           ?= util
CROSS_COMPILE ?=
O             ?= $(CURDIR)/build

# Resolve O to absolute.
override O := $(abspath $(O))

# CMake passes TOOLPREFIX via env; honor CROSS_COMPILE if given.
ifneq ($(CROSS_COMPILE),)
TOOLPREFIX_ENV := TOOLPREFIX=$(CROSS_COMPILE)
endif

JOBS ?= $(shell nproc 2>/dev/null || echo 1)

.PHONY: all configure build clean help

all: build

$(O)/CMakeCache.txt: CMakeLists.txt
	@mkdir -p $(O)
	$(TOOLPREFIX_ENV) ARCH=$(ARCH) PLATFORM=$(PLATFORM) OPT=$(OPT) LAB=$(LAB) \
	    cmake -S $(CURDIR) -B $(O) \
	          -DARCH=$(ARCH) -DPLATFORM=$(PLATFORM) -DOPT_LEVEL=$(OPT)

configure: $(O)/CMakeCache.txt

build: configure
	cmake --build $(O) -j$(JOBS) --target kernel

clean:
	rm -rf $(O)

help:
	@echo "xv6-os kernel sub-repo — make wrapper"
	@echo "  make ARCH=<arch> PLATFORM=<plat> CROSS_COMPILE=<prefix> O=<dir> all"
	@echo "  make ARCH=$(ARCH) configure   # cmake configure only"
	@echo "  make ARCH=$(ARCH) build       # configure + build"
	@echo "  make ARCH=$(ARCH) clean"
	@echo ""
	@echo "Defaults: ARCH=$(ARCH) PLATFORM=$(PLATFORM) OPT=$(OPT) LAB=$(LAB)"
	@echo "Out dir : O=$(O)"
