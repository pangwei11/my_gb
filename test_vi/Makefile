# GB28181 + VENC Makefile (Adapted for vi_test.c)

# 保留原有的交叉编译工具链配置
export PATH := /usr/local/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin:$(PATH)

CROSS_COMPILE ?= aarch64-none-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip

DEBUG ?= 0
DIAG ?= 0
USE_LOCAL_VENC ?= 1

# 保留原有的依赖路径配置
TOP_DIR := /home/spepc/RV1126_IPC/02_rv1126b_ipc_firmware/rv1126b_ipc
BUILDROOT_DIR := $(TOP_DIR)/buildroot
OSIP_SYSROOT := $(BUILDROOT_DIR)/output/rockchip_rv1126b_ipc/host/aarch64-buildroot-linux-gnu/sysroot
FFMPEG_INC_DIR := $(BUILDROOT_DIR)/output/rockchip_rv1126b_ipc/build/ffmpeg-4.4.4
ROOTFS_LIB := $(TOP_DIR)/output/rockchip_rv1126b_ipc/target/usr/lib
RK_ALGO_INC := /home/spepc/RV1126_IPC/02_fireware/rv1126b_ipc/external/common_algorithm/misc/include
ROCKIT_LIB_PATH := /home/spepc/RV1126_IPC/02_rv1126b_ipc_firmware/rv1126b_ipc/external/rockit/lib/arm64/rv1126b/linux
RK_ALGO_LIB_PATH := /home/spepc/RV1126_IPC/02_fireware/rv1126b_ipc/external/common_algorithm/misc/lib/aarch64-rockchip1031-linux-gnu

TARGET := vi_test

# 修改点1：去除旧工程的 GB_SRCS，仅指定当前目录的 vi_test.c
SRCS := vi_test.c

OBJS := $(patsubst %.c, build/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

# 保留原有的 CFLAGS 和 sysroot 设置
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

# 保留原有的 LDFLAGS，确保链接器能找到目标板的动态库和 sysroot
LDFLAGS := --sysroot=$(OSIP_SYSROOT)
LDFLAGS += -L$(ROOTFS_LIB) -L$(OSIP_SYSROOT)/lib -L$(OSIP_SYSROOT)/usr/lib
LDFLAGS += -Wl,-rpath-link=$(ROOTFS_LIB) -Wl,-rpath=/usr/lib -Wl,--no-as-needed

# 保留原有的依赖库
LIBS := -leXosip2 -losip2 -losipparser2
LIBS += -lavformat -lavcodec -lavutil
LIBS += -lcrypto -lssl
LIBS += -lpthread -lm -ldl -lrt -lc

# 修改点2：保留 USE_LOCAL_VENC 的宏定义和库依赖，但不再追加旧的 src/venc_local.c 源文件
ifeq ($(USE_LOCAL_VENC),1)
# 删除了 SRCS += src/venc_local.c，避免 make 去寻找不存在的 src 目录
CFLAGS += -DUSE_LOCAL_VENC -DOS_LINUX -I$(RK_ALGO_INC)
LDFLAGS += -L$(ROCKIT_LIB_PATH) -L$(RK_ALGO_LIB_PATH)
LIBS += -lrockit -lstdc++
endif

all: $(TARGET)
	@echo "== Compile OK: $(TARGET) VENC=$(USE_LOCAL_VENC) =="
	@ls -lh $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# 适配当前目录下 .c 文件的编译规则
build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f build/*.o build/*.d
	rm -rf build/
	rm -f $(TARGET)
	@echo "== Clean OK =="

.PHONY: all clean
