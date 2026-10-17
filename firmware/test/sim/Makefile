# DeskDial firmware simulator. Builds the firmware sources for Linux against
# the stubs in stubs/, once per feature set, and runs the tests:
#
#   make            build and run full, media and hardware
#   make render     also turn the screen dumps into PNG (needs Python 3)
#   make clean
#
# LVGL 8.4.0 is cloned into .lvgl on first use; point LVGL_DIR at an existing
# copy to skip that.
# SPDX-License-Identifier: MIT

LVGL_VERSION ?= v8.4.0
LVGL_DIR ?= .lvgl
BUILD ?= build
FW := ../..
CC ?= cc
CXX ?= c++

CPPFLAGS := -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DDESKDIAL_SIM \
            -DARDUINO_USB_MODE=1 -DBOARD_DRIVER='"sim_board.h"' \
            -Istubs -I$(FW)/include -I$(FW)/src -I$(LVGL_DIR)
CFLAGS := -O1 -g -w
CXXFLAGS := -O1 -g -Wall -Wno-unused-function -std=c++17

FW_SRC := $(wildcard $(FW)/src/*.cpp)
FONTS := $(wildcard $(FW)/src/fonts/*.c)
CONFIGS := full media hardware
FLAGS_full := -DDESKDIAL_MEDIA=1 -DDESKDIAL_HARDWARE=1
FLAGS_media := -DDESKDIAL_MEDIA=1 -DDESKDIAL_HARDWARE=0
FLAGS_hardware := -DDESKDIAL_MEDIA=0 -DDESKDIAL_HARDWARE=1

.PHONY: all test render clean
all: test

$(LVGL_DIR)/lvgl.h:
	git clone --quiet --depth 1 --branch $(LVGL_VERSION) https://github.com/lvgl/lvgl.git $(LVGL_DIR)

# LVGL and the fonts don't depend on the feature set: built once.
LVGL_SRC = $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
LVGL_OBJ = $(patsubst $(LVGL_DIR)/%.c,$(BUILD)/lvgl/%.o,$(LVGL_SRC))
FONT_OBJ := $(patsubst $(FW)/src/fonts/%.c,$(BUILD)/fonts/%.o,$(FONTS))

$(BUILD)/lvgl/%.o: $(LVGL_DIR)/%.c $(FW)/include/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/fonts/%.o: $(FW)/src/fonts/%.c | $(LVGL_DIR)/lvgl.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/liblvgl.a: $(LVGL_DIR)/lvgl.h $(FW)/include/lv_conf.h
	@$(MAKE) --no-print-directory lvgl-objs
	@rm -f $@
	ar rcs $@ $(LVGL_OBJ)

.PHONY: lvgl-objs
lvgl-objs: $(LVGL_OBJ)

# One simulator binary per feature set
define CONFIG_RULES
$(BUILD)/sim-$(1): harness.cpp $(FW_SRC) $(FONT_OBJ) $(BUILD)/liblvgl.a $(wildcard $(FW)/include/*.h $(FW)/src/*.h stubs/*.h stubs/*/*.h)
	$(CXX) $(CPPFLAGS) $(FLAGS_$(1)) $(CXXFLAGS) harness.cpp $(FW_SRC) $(FONT_OBJ) $(BUILD)/liblvgl.a -lm -o $$@
endef
$(foreach c,$(CONFIGS),$(eval $(call CONFIG_RULES,$(c))))

test: $(foreach c,$(CONFIGS),$(BUILD)/sim-$(c))
	@for c in $(CONFIGS); do \
	  mkdir -p $(BUILD)/screens/$$c; \
	  ./$(BUILD)/sim-$$c $(BUILD)/screens/$$c fixtures || exit 1; \
	done

render: test
	python3 render.py $(BUILD)/screens

clean:
	rm -rf $(BUILD)
