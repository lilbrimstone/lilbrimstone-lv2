# ==============================================================================
# LilBrimstone Stereo Chorus - LV2 Plugin Makefile
# ==============================================================================

PLUGIN_NAME := stereo_chorus
SRC         := stereo_chorus.c
BUNDLE      := $(PLUGIN_NAME).lv2
URI         := https://github.com/lilbrimstone/$(PLUGIN_NAME)

# --- Environment & OS Detection ---
UNAME_S := $(shell uname -s)
CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wno-unused-function

# Use pkg-config to find LV2 SDK headers (standard for open source LV2s)
LV2_CFLAGS := $(shell pkg-config --cflags lv2 2>/dev/null || echo "-I/usr/include/lv2 -I/usr/local/include")

# Default Base Flags
BASE_FLAGS := -std=c11 -fPIC -fvisibility=hidden $(LV2_CFLAGS) $(CFLAGS)

# --- Platform Specific Flags ---
ifeq ($(UNAME_S),Linux)
    # Native Linux
    TARGET     := $(PLUGIN_NAME).so
    LDFLAGS    := -shared -Wl,-Bsymbolic -lm
    LV2_DIR    ?= ~/.lv2
else ifeq ($(UNAME_S),Darwin)
    # Native macOS
    TARGET     := $(PLUGIN_NAME).dylib
    LDFLAGS    := -dynamiclib -lm
    LV2_DIR    ?= ~/Library/Audio/Plug-Ins/LV2
else
    # Windows (MinGW or MSYS2)
    TARGET     := $(PLUGIN_NAME).dll
    BASE_FLAGS += -D_WIN32
    LDFLAGS    := -shared -static -static-libgcc -Wl,-Bstatic -lpthread -Wl,-Bdynamic -lm
    LV2_DIR    ?= $(APPDATA)/LV2
endif

# ==============================================================================
# Targets
# ==============================================================================
.PHONY: all clean install

all: bundle

# Creates the binary
$(TARGET): $(SRC)
	$(CC) $(BASE_FLAGS) -o $@ $^ $(LDFLAGS)

# Generates the manifest and packages the .lv2 bundle directory
bundle: $(TARGET)
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
	@echo "Build successful. Run 'make install' to deploy to $(LV2_DIR)"

# Installs the bundle to the user's native plugin directory
install: bundle
	@mkdir -p $(LV2_DIR)
	@rm -rf $(LV2_DIR)/$(BUNDLE)
	@cp -R $(BUNDLE) $(LV2_DIR)/
	@echo "Installed to $(LV2_DIR)/$(BUNDLE)"

clean:
	@rm -rf $(BUNDLE) $(TARGET) *.o