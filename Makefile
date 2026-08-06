SDK_ROOT ?= ..
TOOLCHAIN_BIN := $(SDK_ROOT)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
CROSS := $(TOOLCHAIN_BIN)/arm-rockchip830-linux-uclibcgnueabihf-
CC := $(CROSS)gcc
HOST_CC ?= gcc

MEDIA := $(SDK_ROOT)/media
MEDIA_OUT := $(MEDIA)/out
SAMPLE := $(MEDIA)/samples/example
COMMON := $(SAMPLE)/common
LVGL := $(SDK_ROOT)/project/app/component/lvgl/out
TARGETS := camera_capture camera_live
UI_SRC := ui/camera_ui.c
STREAM_SRC := media_callbacks.c
CONFIG_SRC := camera_config.c

CFLAGS := -Os -g -Wall -Wextra -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE \
	-D_FILE_OFFSET_BITS=64 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
	-DISP_HW_V30 -DRV1106 -DROCKIT_IVS -DROCKCOMBO -DUAPI2 -DRKAIQ_GRP -DHAVE_VO \
	-DLV_CONF_INCLUDE_SIMPLE \
	-I$(COMMON) -I$(COMMON)/isp3.x -I$(MEDIA_OUT)/include \
	-I$(MEDIA_OUT)/include/rkaiq -I$(MEDIA_OUT)/include/rkaiq/uAPI2 \
	-I$(MEDIA_OUT)/include/rkaiq/common -I$(MEDIA_OUT)/include/rkaiq/xcore \
	-I$(MEDIA_OUT)/include/rkaiq/algos -I$(MEDIA_OUT)/include/rkaiq/iq_parser \
	-I$(MEDIA_OUT)/include/rkaiq/iq_parser_v2 -I$(LVGL)/include \
	-I$(LVGL)/include/lvgl -Iui

LDFLAGS := -L$(COMMON)/lib -lsample_comm -L$(MEDIA_OUT)/lib \
	-Wl,-rpath-link,$(MEDIA_OUT)/lib:$(MEDIA_OUT)/root/usr/lib \
	-lrkaiq -lrockit_full -lrkmuxer -lrtsp -lpthread

LVGL_LDFLAGS := -L$(LVGL)/lib -llvgl -lm

.PHONY: all clean check simulator simulator-lvgl

all: check $(TARGETS)

check:
	@test -x $(CC) || (echo "Missing compiler: $(CC)"; exit 1)
	@test -f $(COMMON)/lib/libsample_comm.a || \
		(echo "Build SDK samples first: cd $(MEDIA) && make samples"; exit 1)

camera_capture: camera_capture.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

camera_live: camera_live.c $(UI_SRC) $(STREAM_SRC) $(CONFIG_SRC)
	@test -f $(LVGL)/lib/liblvgl.a || \
		(echo "Build LVGL first: make -C project/app/component/lvgl RK_ENABLE_LVGL=y"; exit 1)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LVGL_LDFLAGS)

SIM_BUILD := build-sim
SIM_LVGL := $(SIM_BUILD)/lvgl
SIM_CFLAGS := -O2 -g -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE \
	-Isim -Iui -I$(SDK_ROOT)/project/app/component/lvgl \
	$(shell pkg-config --cflags sdl2)
SIM_LDFLAGS := $(shell pkg-config --libs sdl2) -lm -lpthread

simulator-lvgl:
	@mkdir -p $(SIM_LVGL)
	@cd $(SIM_LVGL) && cmake $(abspath $(SDK_ROOT)/project/app/component/lvgl/lvgl) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-DLV_CONF_INCLUDE_SIMPLE -I$(abspath sim)" \
		-DBUILD_SHARED_LIBS=OFF >/dev/null
	@$(MAKE) -C $(SIM_LVGL) lvgl -j4

simulator: simulator-lvgl sim/camera_simulator.c $(UI_SRC)
	$(HOST_CC) $(SIM_CFLAGS) sim/camera_simulator.c $(UI_SRC) \
		-o camera_simulator $(SIM_LVGL)/lib/liblvgl.a $(SIM_LDFLAGS)

clean:
	rm -f $(TARGETS) camera_simulator
	rm -rf $(SIM_BUILD)
