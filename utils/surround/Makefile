# neko-surround: the nekosurround GStreamer element
#
#   make plugin                  builds libgstnekosurround.so (needs GStreamer dev packages)
#   make test                    DSP tests and element tests through GStreamer
#   make test DLL=DolbyHph.dll   also checks Dolby Headphone against the lakehp engine
#   make install                 installs into $(DESTDIR)

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Wno-unused-parameter
INC = -Isrc -Ilakehp

GST_PKGS = gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0
GST_PLUGIN_DIR ?= $(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)

BUILD = build

DSP_SRC = src/surround_dsp.c
LHP_SRC = lakehp/lakehp.c src/lhp_dll.c
HEADERS = src/*.h lakehp/lakehp.h
PLUGIN = $(BUILD)/libgstnekosurround.so

.PHONY: all plugin test install clean

all: plugin

plugin: $(PLUGIN)

$(PLUGIN): src/gstnekosurround.c $(DSP_SRC) $(LHP_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -fPIC -shared -o $@ src/gstnekosurround.c $(DSP_SRC) $(LHP_SRC) \
		$$($(PKG_CONFIG) --cflags --libs $(GST_PKGS)) -lm

$(BUILD)/test_dsp: tests/test_dsp.c $(DSP_SRC) src/surround_dsp.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ tests/test_dsp.c $(DSP_SRC) -lm

$(BUILD)/test_element: tests/test_element.c $(DSP_SRC) $(LHP_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ tests/test_element.c $(DSP_SRC) $(LHP_SRC) \
		$$($(PKG_CONFIG) --cflags --libs gstreamer-1.0 gstreamer-app-1.0) -lm

$(BUILD)/test_lakehp71: tests/test_lakehp71.c $(LHP_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ tests/test_lakehp71.c $(LHP_SRC) -lm

test: $(BUILD)/test_dsp $(BUILD)/test_element $(BUILD)/test_lakehp71 $(PLUGIN)
	$(BUILD)/test_dsp
	$(BUILD)/test_element $(BUILD) $(DLL)
	if [ -n "$(DLL)" ]; then $(BUILD)/test_lakehp71 $(DLL); fi

install: $(PLUGIN)
	install -D -m 0644 $(PLUGIN) $(DESTDIR)$(GST_PLUGIN_DIR)/libgstnekosurround.so

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
