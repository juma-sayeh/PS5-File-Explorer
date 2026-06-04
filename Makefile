# File Explorer - minimal PS5 browser file manager payload.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

VERSION_TAG := file-explorer-v0.2.0
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
PYTHON ?= python3
HOST_LLVM_BINDIR ?= $(shell dirname "$$(command -v llvm-strip 2>/dev/null || command -v llvm-strip.exe 2>/dev/null || echo llvm-strip)" 2>/dev/null)
LLVM_STRIP ?= llvm-strip
UNIX_CURDIR := $(shell cygpath -u '$(CURDIR)' 2>/dev/null || pwd)

export LLVM_BINDIR ?= $(HOST_LLVM_BINDIR)
export LLVM_CONFIG ?= $(UNIX_CURDIR)/build-tools/llvm-config
BUILD_PATH := $(UNIX_CURDIR)/build-tools:$(HOST_LLVM_BINDIR):/mingw64/bin:/usr/local/bin:/usr/bin:/bin:/c/Users/Blurf/scoop/shims
BUILD_ENV := cd "$(UNIX_CURDIR)"; export LLVM_CONFIG="$(LLVM_CONFIG)"; export LLVM_BINDIR="$(LLVM_BINDIR)"; export PATH="$(BUILD_PATH):$$PATH"

BIN := file-explorer.elf

C_SRCS := src/lite_main.c
C_SRCS += src/websrv_lite.c
C_SRCS += src/asset.c
C_SRCS += src/fs.c
C_SRCS += src/mime.c
C_SRCS += src/notify.c
C_SRCS += src/transfer.c
C_SRCS += src/archive_common.c
C_SRCS += src/zip_archive.c
C_SRCS += src/rar_transfer.c
C_SRCS += src/app_installer.c
C_SRCS += src/miniz_tinfl.c

CXX_SRCS := src/rar_extract.cpp

UNRAR_SRCS := src/unrar/strlist.cpp
UNRAR_SRCS += src/unrar/strfn.cpp
UNRAR_SRCS += src/unrar/pathfn.cpp
UNRAR_SRCS += src/unrar/smallfn.cpp
UNRAR_SRCS += src/unrar/global.cpp
UNRAR_SRCS += src/unrar/file.cpp
UNRAR_SRCS += src/unrar/filefn.cpp
UNRAR_SRCS += src/unrar/filcreat.cpp
UNRAR_SRCS += src/unrar/archive.cpp
UNRAR_SRCS += src/unrar/arcread.cpp
UNRAR_SRCS += src/unrar/unicode.cpp
UNRAR_SRCS += src/unrar/system.cpp
UNRAR_SRCS += src/unrar/crypt.cpp
UNRAR_SRCS += src/unrar/crc.cpp
UNRAR_SRCS += src/unrar/rawread.cpp
UNRAR_SRCS += src/unrar/encname.cpp
UNRAR_SRCS += src/unrar/resource.cpp
UNRAR_SRCS += src/unrar/match.cpp
UNRAR_SRCS += src/unrar/timefn.cpp
UNRAR_SRCS += src/unrar/rdwrfn.cpp
UNRAR_SRCS += src/unrar/consio.cpp
UNRAR_SRCS += src/unrar/options.cpp
UNRAR_SRCS += src/unrar/errhnd.cpp
UNRAR_SRCS += src/unrar/rarvm.cpp
UNRAR_SRCS += src/unrar/secpassword.cpp
UNRAR_SRCS += src/unrar/rijndael.cpp
UNRAR_SRCS += src/unrar/getbits.cpp
UNRAR_SRCS += src/unrar/sha1.cpp
UNRAR_SRCS += src/unrar/sha256.cpp
UNRAR_SRCS += src/unrar/blake2s.cpp
UNRAR_SRCS += src/unrar/hash.cpp
UNRAR_SRCS += src/unrar/extinfo.cpp
UNRAR_SRCS += src/unrar/extract.cpp
UNRAR_SRCS += src/unrar/volume.cpp
UNRAR_SRCS += src/unrar/list.cpp
UNRAR_SRCS += src/unrar/find.cpp
UNRAR_SRCS += src/unrar/unpack.cpp
UNRAR_SRCS += src/unrar/headers.cpp
UNRAR_SRCS += src/unrar/threadpool.cpp
UNRAR_SRCS += src/unrar/rs16.cpp
UNRAR_SRCS += src/unrar/cmddata.cpp
UNRAR_SRCS += src/unrar/ui.cpp
UNRAR_SRCS += src/unrar/largepage.cpp
UNRAR_SRCS += src/unrar/filestr.cpp
UNRAR_SRCS += src/unrar/recvol.cpp
UNRAR_SRCS += src/unrar/rs.cpp
UNRAR_SRCS += src/unrar/scantree.cpp
UNRAR_SRCS += src/unrar/qopen.cpp

CFLAGS := -Os -Wall -Werror -Isrc
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -flto
CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"

CXXFLAGS := $(CFLAGS) -std=c++11
FAST_CFLAGS := $(filter-out -Os,$(CFLAGS)) -O2
FAST_CXXFLAGS := $(filter-out -Os,$(CXXFLAGS)) -O2
UNRAR_CXXFLAGS := -O2 -std=c++11 -Isrc/unrar
UNRAR_CXXFLAGS += -ffunction-sections -fdata-sections -flto
UNRAR_CXXFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
UNRAR_CXXFLAGS += -DSILENT -DBFPILOT_UNRAR_STREAM
UNRAR_CXXFLAGS += -Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else
UNRAR_CXXFLAGS += -Wno-unused-variable -Wno-unused-function

LDFLAGS := -Wl,--gc-sections -flto
LDFLAGS += -B$(PS5_PAYLOAD_SDK)/win

LDADD := -lkernel_sys -lSceNotification
LDADD += -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService
LDADD += -lSceNetCtl

ASSETS := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/assets/%,$(ASSETS:=.c))
APP_ASSETS := assets-app/param.json assets-app/icon0.png
OBJS := $(C_SRCS:.c=.o)
OBJS += $(CXX_SRCS:.cpp=.o)
OBJS += $(UNRAR_SRCS:.cpp=.o)
OBJS += $(GEN_SRCS:.c=.o)

all: $(BIN)

gen/assets:
	mkdir -p gen/assets

gen/assets/%.c: assets/% | gen/assets
	$(PYTHON) gen-asset-module.py --path $* $< > $@

src/transfer.o: src/transfer.c
	bash -lc '$(BUILD_ENV); $(CC) $(FAST_CFLAGS) -c -o $@ $<'

src/archive_common.o: src/archive_common.c
	bash -lc '$(BUILD_ENV); $(CC) $(FAST_CFLAGS) -c -o $@ $<'

src/zip_archive.o: src/zip_archive.c
	bash -lc '$(BUILD_ENV); $(CC) $(FAST_CFLAGS) -c -o $@ $<'

src/rar_transfer.o: src/rar_transfer.c
	bash -lc '$(BUILD_ENV); $(CC) $(FAST_CFLAGS) -c -o $@ $<'

src/miniz_tinfl.o: src/miniz_tinfl.c
	bash -lc '$(BUILD_ENV); $(CC) $(FAST_CFLAGS) -c -o $@ $<'

src/rar_extract.o: src/rar_extract.cpp
	bash -lc '$(BUILD_ENV); $(CXX) $(FAST_CXXFLAGS) -c -o $@ $<'

%.o: %.c
	bash -lc '$(BUILD_ENV); $(CC) $(CFLAGS) -c -o $@ $<'

src/unrar/%.o: src/unrar/%.cpp
	bash -lc '$(BUILD_ENV); $(CXX) $(UNRAR_CXXFLAGS) -c -o $@ $<'

%.o: %.cpp
	bash -lc '$(BUILD_ENV); $(CXX) $(CXXFLAGS) -c -o $@ $<'

$(BIN): $(OBJS) $(APP_ASSETS)
	bash -lc '$(BUILD_ENV); $(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDADD)'
	bash -lc 'cd "$(UNIX_CURDIR)"; test -f $@'
	bash -lc '$(BUILD_ENV); $(LLVM_STRIP) --strip-all $@'

deploy: $(BIN)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -rf $(BIN) gen $(OBJS)

.PHONY: all clean deploy
