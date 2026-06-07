# --- LilBrimstone Generic LV2 Makefile ---
# Plugin: Compressor

PLUGIN_NAME := compressor
URI         := https://github.com/lilbrimstone/$(PLUGIN_NAME)
BUNDLE      := $(PLUGIN_NAME).lv2
BUILD_DIR   := build

SRC         := compressor.c
OBJ         := $(BUILD_DIR)/compressor.o

CC       ?= gcc
CFLAGS   += -fPIC -O3 -Wall -Wno-unused-parameter
LDFLAGS  += -shared -lm

UNAME_S := $(shell uname -s)

ifeq ($(OS),Windows_NT)
    TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).dll
    LDFLAGS += -static -static-libgcc -Wl,-Bstatic -lpthread -Wl,-Bdynamic
    CFLAGS += -D_WIN32
else
    ifeq ($(UNAME_S),Darwin)
        TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).dylib
    else
        TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).so
        LDFLAGS += -Wl,-Bsymbolic
    endif
endif

.PHONY: all clean install

all: $(BUNDLE)

$(BUNDLE): $(TARGET)
	@echo "Creating LV2 bundle: $(BUNDLE)"
	@mkdir -p $(BUNDLE)
	@cp $(TARGET) $(BUNDLE)/
	@cp $(PLUGIN_NAME).ttl $(BUNDLE)/
	@echo "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> ." > $(BUNDLE)/manifest.ttl
	@echo "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> ." >> $(BUNDLE)/manifest.ttl
	@echo "" >> $(BUNDLE)/manifest.ttl
	@echo "<$(URI)>" >> $(BUNDLE)/manifest.ttl
	@echo "    a lv2:Plugin ;" >> $(BUNDLE)/manifest.ttl
	@echo "    lv2:binary <$(notdir $(TARGET))> ;" >> $(BUNDLE)/manifest.ttl
	@echo "    rdfs:seeAlso <$(PLUGIN_NAME).ttl> ." >> $(BUNDLE)/manifest.ttl

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) -o $@

install: $(BUNDLE)
ifeq ($(OS),Windows_NT)
	@echo "Install target is for Linux/macOS. Manually copy $(BUNDLE) to your VST/LV2 directory."
else
	@mkdir -p ~/.lv2
	@rm -rf ~/.lv2/$(BUNDLE)
	@cp -r $(BUNDLE) ~/.lv2/
	@echo "Installed to ~/.lv2/$(BUNDLE)"
endif

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(BUNDLE)