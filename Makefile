# File Explorer - reproducible PS5 payload builds.

SHELL := bash

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
$(error PS5_PAYLOAD_SDK is required, e.g. export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

HOST_UNAME := $(shell uname -s 2>/dev/null || echo unknown)
HOST_IS_WINDOWS := 0
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(HOST_UNAME)))
HOST_IS_WINDOWS := 1
endif

PS5_HOST ?= ps5
PS5_PORT ?= 9021
PYTHON ?= python3

VERSION_TAG := file-explorer-v0.2.1
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

LLVM_BINDIR ?= $(shell dirname "$$(command -v clang 2>/dev/null || command -v clang.exe 2>/dev/null || command -v llvm-strip 2>/dev/null || command -v llvm-strip.exe 2>/dev/null || echo clang)" 2>/dev/null || echo .)
LLVM_CONFIG ?= $(CURDIR)/build-tools/llvm-config
export LLVM_BINDIR
export LLVM_CONFIG

CORE_BIN := file-explorer-core.elf
FULL_BIN := file-explorer.elf

COMMON_C_SRCS := src/lite_main.c
COMMON_C_SRCS += src/diag.c
COMMON_C_SRCS += src/websrv_lite.c
COMMON_C_SRCS += src/asset.c
COMMON_C_SRCS += src/fs.c
COMMON_C_SRCS += src/mime.c
COMMON_C_SRCS += src/notify.c
COMMON_C_SRCS += src/transfer.c
COMMON_C_SRCS += src/archive_common.c
COMMON_C_SRCS += src/zip_archive.c
COMMON_C_SRCS += src/rar_transfer.c
COMMON_C_SRCS += src/pfs_compress.c
COMMON_C_SRCS += src/pfs_decompress.c
COMMON_C_SRCS += src/pfs_block_pipeline.c
COMMON_C_SRCS += src/miniz_tdef.c
COMMON_C_SRCS += src/miniz_tinfl.c

FULL_C_SRCS := $(COMMON_C_SRCS)
FULL_C_SRCS += src/app_installer.c
FULL_C_SRCS += src/sce_resolve.c

COMMON_CXX_SRCS := src/rar_extract.cpp

UNRAR_CXX_SRCS := src/unrar/strlist.cpp
UNRAR_CXX_SRCS += src/unrar/strfn.cpp
UNRAR_CXX_SRCS += src/unrar/pathfn.cpp
UNRAR_CXX_SRCS += src/unrar/smallfn.cpp
UNRAR_CXX_SRCS += src/unrar/global.cpp
UNRAR_CXX_SRCS += src/unrar/file.cpp
UNRAR_CXX_SRCS += src/unrar/filefn.cpp
UNRAR_CXX_SRCS += src/unrar/filcreat.cpp
UNRAR_CXX_SRCS += src/unrar/archive.cpp
UNRAR_CXX_SRCS += src/unrar/arcread.cpp
UNRAR_CXX_SRCS += src/unrar/unicode.cpp
UNRAR_CXX_SRCS += src/unrar/system.cpp
UNRAR_CXX_SRCS += src/unrar/crypt.cpp
UNRAR_CXX_SRCS += src/unrar/crc.cpp
UNRAR_CXX_SRCS += src/unrar/rawread.cpp
UNRAR_CXX_SRCS += src/unrar/encname.cpp
UNRAR_CXX_SRCS += src/unrar/resource.cpp
UNRAR_CXX_SRCS += src/unrar/match.cpp
UNRAR_CXX_SRCS += src/unrar/timefn.cpp
UNRAR_CXX_SRCS += src/unrar/rdwrfn.cpp
UNRAR_CXX_SRCS += src/unrar/consio.cpp
UNRAR_CXX_SRCS += src/unrar/options.cpp
UNRAR_CXX_SRCS += src/unrar/errhnd.cpp
UNRAR_CXX_SRCS += src/unrar/rarvm.cpp
UNRAR_CXX_SRCS += src/unrar/secpassword.cpp
UNRAR_CXX_SRCS += src/unrar/rijndael.cpp
UNRAR_CXX_SRCS += src/unrar/getbits.cpp
UNRAR_CXX_SRCS += src/unrar/sha1.cpp
UNRAR_CXX_SRCS += src/unrar/sha256.cpp
UNRAR_CXX_SRCS += src/unrar/blake2s.cpp
UNRAR_CXX_SRCS += src/unrar/hash.cpp
UNRAR_CXX_SRCS += src/unrar/extinfo.cpp
UNRAR_CXX_SRCS += src/unrar/extract.cpp
UNRAR_CXX_SRCS += src/unrar/volume.cpp
UNRAR_CXX_SRCS += src/unrar/list.cpp
UNRAR_CXX_SRCS += src/unrar/find.cpp
UNRAR_CXX_SRCS += src/unrar/unpack.cpp
UNRAR_CXX_SRCS += src/unrar/headers.cpp
UNRAR_CXX_SRCS += src/unrar/threadpool.cpp
UNRAR_CXX_SRCS += src/unrar/rs16.cpp
UNRAR_CXX_SRCS += src/unrar/cmddata.cpp
UNRAR_CXX_SRCS += src/unrar/ui.cpp
UNRAR_CXX_SRCS += src/unrar/largepage.cpp
UNRAR_CXX_SRCS += src/unrar/filestr.cpp
UNRAR_CXX_SRCS += src/unrar/recvol.cpp
UNRAR_CXX_SRCS += src/unrar/rs.cpp
UNRAR_CXX_SRCS += src/unrar/scantree.cpp
UNRAR_CXX_SRCS += src/unrar/qopen.cpp

ARCHIVE_FAST_C_SRCS := src/transfer.c
ARCHIVE_FAST_C_SRCS += src/archive_common.c
ARCHIVE_FAST_C_SRCS += src/zip_archive.c
ARCHIVE_FAST_C_SRCS += src/rar_transfer.c
ARCHIVE_FAST_C_SRCS += src/pfs_compress.c
ARCHIVE_FAST_C_SRCS += src/pfs_decompress.c
ARCHIVE_FAST_C_SRCS += src/pfs_block_pipeline.c
ARCHIVE_FAST_C_SRCS += src/miniz_tdef.c
ARCHIVE_FAST_C_SRCS += src/miniz_tinfl.c

ASSETS := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/assets/%,$(ASSETS:=.c))
APP_ASSETS := assets-app/param.json assets-app/icon0.png

CORE_OBJS := $(patsubst %.c,build/core/%.o,$(COMMON_C_SRCS) $(GEN_SRCS))
CORE_OBJS += $(patsubst %.cpp,build/core/%.o,$(COMMON_CXX_SRCS) $(UNRAR_CXX_SRCS))
FULL_OBJS := $(patsubst %.c,build/full/%.o,$(FULL_C_SRCS) $(GEN_SRCS))
FULL_OBJS += $(patsubst %.cpp,build/full/%.o,$(COMMON_CXX_SRCS) $(UNRAR_CXX_SRCS))

CORE_FAST_OBJS := $(patsubst %.c,build/core/%.o,$(ARCHIVE_FAST_C_SRCS))
FULL_FAST_OBJS := $(patsubst %.c,build/full/%.o,$(ARCHIVE_FAST_C_SRCS))

COMMON_CFLAGS := -Os -Wall -Werror -Isrc
COMMON_CFLAGS += -ffunction-sections -fdata-sections -flto
COMMON_CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
COMMON_CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"
COMMON_CFLAGS += -DBFPILOT_SDK_PATH=\"$(PS5_PAYLOAD_SDK)\"

CORE_CFLAGS := $(COMMON_CFLAGS)
CORE_CFLAGS += -DBFPILOT_ENABLE_LAUNCHER=0
CORE_CFLAGS += -DBFPILOT_DISABLE_LAUNCHER=1
CORE_CFLAGS += -DBFPILOT_BUILD_MODE=\"core\"

FULL_CFLAGS := $(COMMON_CFLAGS)
FULL_CFLAGS += -DBFPILOT_ENABLE_LAUNCHER=1
FULL_CFLAGS += -DBFPILOT_BUILD_MODE=\"full\"

CORE_CXXFLAGS := $(CORE_CFLAGS) -std=c++11
FULL_CXXFLAGS := $(FULL_CFLAGS) -std=c++11
FAST_CORE_CFLAGS := $(filter-out -Os,$(CORE_CFLAGS)) -O2
FAST_FULL_CFLAGS := $(filter-out -Os,$(FULL_CFLAGS)) -O2
FAST_CORE_CXXFLAGS := $(filter-out -Os,$(CORE_CXXFLAGS)) -O2
FAST_FULL_CXXFLAGS := $(filter-out -Os,$(FULL_CXXFLAGS)) -O2

UNRAR_CXXFLAGS := -O2 -std=c++11 -Isrc/unrar
UNRAR_CXXFLAGS += -ffunction-sections -fdata-sections -flto
UNRAR_CXXFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
UNRAR_CXXFLAGS += -DSILENT -DBFPILOT_UNRAR_STREAM
UNRAR_CXXFLAGS += -Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else
UNRAR_CXXFLAGS += -Wno-unused-variable -Wno-unused-function

COMMON_LDFLAGS := -Wl,--gc-sections -flto
FULL_LDLIBS := -lkernel_sys -lSceNotification -lSceIpmi
FULL_LDLIBS += -lSceAppInstUtil -lSceUserService -lSceSystemService
FULL_LDLIBS += -lSceNetCtl

CC_CMD := "$(CC)"
CXX_CMD := "$(CXX)"
STRIP_CMD := "$(STRIP)"
DEPLOY_CMD := "$(PS5_DEPLOY)"

ifeq ($(HOST_IS_WINDOWS),1)
CURDIR_POSIX := $(shell pwd)
LLVM_CONFIG_POSIX := $(shell cygpath -u "$(LLVM_CONFIG)" 2>/dev/null || printf '%s' "$(LLVM_CONFIG)")
LLVM_BINDIR_POSIX := $(shell cygpath -u "$(LLVM_BINDIR)" 2>/dev/null || printf '%s' "$(LLVM_BINDIR)")
RUN_ENV := cd "$(CURDIR_POSIX)" && export LLVM_CONFIG="$(LLVM_CONFIG_POSIX)" && export LLVM_BINDIR="$(LLVM_BINDIR_POSIX)"
define run
bash -lc '$(RUN_ENV) && $(1)'
endef
else
define run
$(1)
endef
endif

all: core full

core: $(CORE_BIN)

full: $(FULL_BIN)

gen/assets:
	$(call run,mkdir -p $@)

gen/assets/%.c: assets/% | gen/assets
	$(call run,$(PYTHON) gen-asset-module.py --path $* $< > $@)

$(CORE_FAST_OBJS): build/core/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(FAST_CORE_CFLAGS) -c $< -o $@)

$(FULL_FAST_OBJS): build/full/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(FAST_FULL_CFLAGS) -c $< -o $@)

build/core/src/rar_extract.o: src/rar_extract.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(FAST_CORE_CXXFLAGS) -c $< -o $@)

build/full/src/rar_extract.o: src/rar_extract.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(FAST_FULL_CXXFLAGS) -c $< -o $@)

build/core/src/unrar/%.o: src/unrar/%.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(UNRAR_CXXFLAGS) -c $< -o $@)

build/full/src/unrar/%.o: src/unrar/%.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(UNRAR_CXXFLAGS) -c $< -o $@)

build/core/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(CORE_CFLAGS) -c $< -o $@)

build/full/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(FULL_CFLAGS) -c $< -o $@)

$(CORE_BIN): $(CORE_OBJS)
	$(call run,$(CXX_CMD) $(CORE_CXXFLAGS) $(COMMON_LDFLAGS) -o $@ $(CORE_OBJS))
	$(call run,$(STRIP_CMD) --strip-all $@)

$(FULL_BIN): $(FULL_OBJS) $(APP_ASSETS)
	$(call run,$(CXX_CMD) $(FULL_CXXFLAGS) $(COMMON_LDFLAGS) -o $@ $(FULL_OBJS) $(FULL_LDLIBS))
	$(call run,$(STRIP_CMD) --strip-all $@)

deploy-core: core
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(CORE_BIN))

deploy-full: full
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(FULL_BIN))

clean:
	$(call run,rm -rf $(CORE_BIN) $(FULL_BIN) file-explorer-full.elf build gen)

.SECONDARY: $(GEN_SRCS)
.PHONY: all core full clean deploy-core deploy-full
