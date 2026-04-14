# GB28181 + VENC Makefile
export PATH := /usr/local/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin:$(PATH)

CROSS_COMPILE ?= aarch64-none-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip

DEBUG ?= 0
DIAG ?= 0
USE_LOCAL_VENC ?= 1

TOP_DIR := /home/spepc/RV1126_IPC/02_rv1126b_ipc_firmware/rv1126b_ipc
BUILDROOT_DIR := $(TOP_DIR)/buildroot
OSIP_SYSROOT := $(BUILDROOT_DIR)/output/rockchip_rv1126b_ipc/host/aarch64-buildroot-linux-gnu/sysroot
FFMPEG_INC_DIR := $(BUILDROOT_DIR)/output/rockchip_rv1126b_ipc/build/ffmpeg-4.4.4
ROOTFS_LIB := $(TOP_DIR)/output/rockchip_rv1126b_ipc/target/usr/lib
RK_ALGO_INC := /home/spepc/RV1126_IPC/02_fireware/rv1126b_ipc/external/common_algorithm/misc/include
ROCKIT_LIB_PATH := /home/spepc/RV1126_IPC/02_rv1126b_ipc_firmware/rv1126b_ipc/external/rockit/lib/arm64/rv1126b/linux
RK_ALGO_LIB_PATH := /home/spepc/RV1126_IPC/02_fireware/rv1126b_ipc/external/common_algorithm/misc/lib/aarch64-rockchip1031-linux-gnu

TARGET := gb28181_streamer
GB_SRCS := src/app_config.c src/globals.c src/utils.c src/frame_queue.c src/ps_mux.c src/rtp.c src/rtsp.c src/sip.c src/main.c
SRCS := $(GB_SRCS)
OBJS := $(patsubst src/%.c, build/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

CFLAGS := --sysroot=$(OSIP_SYSROOT)
CFLAGS += -I./include -I.
CFLAGS += -I$(OSIP_SYSROOT)/usr/include
CFLAGS += -I$(OSIP_SYSROOT)/usr/include/osip2
CFLAGS += -I$(OSIP_SYSROOT)/usr/include/eXosip2
CFLAGS += -I$(FFMPEG_INC_DIR)/libavcodec
CFLAGS += -I$(FFMPEG_INC_DIR)/libavformat
CFLAGS += -I$(FFMPEG_INC_DIR)/libavutil
CFLAGS += -I$(OSIP_SYSROOT)/usr/include/openssl
CFLAGS += -Wall -O2 -g -std=gnu11
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_DEFAULT_SOURCE
CFLAGS += -DHAVE_OPENSSL=1
CFLAGS += -Wno-attributes -Wno-deprecated-declarations
CFLAGS += -Wno-implicit-function-declaration -Wno-stringop-truncation
CFLAGS += -fno-stack-protector -MMD -MP

LDFLAGS := --sysroot=$(OSIP_SYSROOT)
LDFLAGS += -L$(ROOTFS_LIB) -L$(OSIP_SYSROOT)/lib -L$(OSIP_SYSROOT)/usr/lib
LDFLAGS += -Wl,-rpath-link=$(ROOTFS_LIB) -Wl,-rpath=/usr/lib -Wl,--no-as-needed

LIBS := -leXosip2 -losip2 -losipparser2
LIBS += -lavformat -lavcodec -lavutil
LIBS += -lcrypto -lssl
LIBS += -lpthread -lm -ldl -lrt -lc

ifeq ($(USE_LOCAL_VENC),1)
SRCS += src/venc_local.c
CFLAGS += -DUSE_LOCAL_VENC -DOS_LINUX -I$(RK_ALGO_INC)
LDFLAGS += -L$(ROCKIT_LIB_PATH) -L$(RK_ALGO_LIB_PATH)
LIBS += -lrockit -lstdc++
endif

OBJS := $(patsubst src/%.c, build/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)
	@echo "== Compile OK: $(TARGET) VENC=$(USE_LOCAL_VENC) =="
	@ls -lh $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f build/*.o build/*.d
	rm -rf build/
	rm -f $(TARGET)
	@echo "== Clean OK =="

.PHONY: all clean
