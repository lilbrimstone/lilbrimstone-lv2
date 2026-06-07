# --- LilBrimstone Public Makefile (Braids) ---

PLUGIN_NAME := braids
BUNDLE      := $(PLUGIN_NAME).lv2
URI         := https://github.com/lilbrimstone/$(PLUGIN_NAME)

# --- SOURCES ---
SRCS_C      := braids.c
SRCS_CPP    := dsp/braids_96_dsp.cc \
               dsp/braids/macro_oscillator.cc \
               dsp/braids/analog_oscillator.cc \
               dsp/braids/digital_oscillator.cc \
               dsp/braids/quantizer.cc \
               dsp/braids/resources.cc \
               dsp/stmlib/dsp/units.cc \
               dsp/stmlib/utils/random.cc

OBJS        := $(SRCS_C:.c=.o) $(SRCS_CPP:.cc=.o)

# --- COMPILER SETUP ---
CC          ?= gcc
CXX         ?= g++
UNAME_S     := $(shell uname -s)

LV2_CFLAGS  := $(shell pkg-config --cflags lv2 2>/dev/null || echo "-I/usr/include/lv2 -I/usr/local/include")
COMMON_FLAGS:= -O3 -fPIC -fvisibility=hidden -Wall -Wno-unused-parameter \
               -D_USE_MATH_DEFINES -I. -Idsp -Idsp/braids $(LV2_CFLAGS)

CFLAGS      := -std=c11 $(COMMON_FLAGS)
CXXFLAGS    := -std=c++11 $(COMMON_FLAGS)

# --- OS SPECIFIC LINKING ---
ifeq ($(UNAME_S),Linux)
    TARGET  := $(PLUGIN_NAME).so
    LDFLAGS := -shared -Wl,-Bsymbolic -lm
    LV2_DIR ?= ~/.lv2
else ifeq ($(UNAME_S),Darwin)
    TARGET  := $(PLUGIN_NAME).dylib
    LDFLAGS := -dynamiclib -lm
    LV2_DIR ?= ~/Library/Audio/Plug-Ins/LV2
else
    TARGET  := $(PLUGIN_NAME).dll
    CFLAGS  += -D_WIN32
    CXXFLAGS+= -D_WIN32
    LDFLAGS := -shared -static -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -lm
    LV2_DIR ?= $(APPDATA)/LV2
endif

# --- TARGETS ---
.PHONY: all clean install bundle

all: bundle

# Rule to compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Rule to compile C++ files
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Link everything together (Requires CXX because of the MI backend)
$(TARGET): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

bundle: $(TARGET)
	@echo "📦 Bundling..."
	@mkdir -p $(BUNDLE)
	@cp $(TARGET) $(BUNDLE)/
	@cp $(PLUGIN_NAME).ttl $(BUNDLE)/
	@echo "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> ." > $(BUNDLE)/manifest.ttl
	@echo "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> ." >> $(BUNDLE)/manifest.ttl
	@echo "" >> $(BUNDLE)/manifest.ttl
	@echo "<$(URI)>" >> $(BUNDLE)/manifest.ttl
	@echo "    a lv2:Plugin ;" >> $(BUNDLE)/manifest.ttl
	@echo "    lv2:binary <$(TARGET)> ;" >> $(BUNDLE)/manifest.ttl
	@echo "    rdfs:seeAlso <$(PLUGIN_NAME).ttl> ." >> $(BUNDLE)/manifest.ttl

install: bundle
	@echo "🚀 Installing to $(LV2_DIR)..."
	@mkdir -p $(LV2_DIR)
	@rm -rf $(LV2_DIR)/$(BUNDLE)
	@cp -R $(BUNDLE) $(LV2_DIR)/

clean:
	@echo "🧹 Cleaning up..."
	@rm -rf $(BUNDLE) $(TARGET) $(OBJS)