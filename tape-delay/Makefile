PLUGIN_NAME := tape-delay
SRC         := tape-delay.c
BUNDLE      := $(PLUGIN_NAME).lv2
URI         := https://github.com/lilbrimstone/$(PLUGIN_NAME)

UNAME_S := $(shell uname -s)
CC      ?= gcc
CFLAGS  ?= -std=c11 -O3 -Wall -Wno-unused-function
LV2_CFLAGS := $(shell pkg-config --cflags lv2 2>/dev/null || echo "-I/usr/include/lv2 -I/usr/local/include")

BASE_FLAGS := -fPIC -fvisibility=hidden $(LV2_CFLAGS) $(CFLAGS)

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
    BASE_FLAGS += -D_WIN32
    LDFLAGS := -shared -static -static-libgcc -Wl,-Bstatic -lpthread -Wl,-Bdynamic -lm
    LV2_DIR ?= $(APPDATA)/LV2
endif

.PHONY: all clean install

all: bundle

$(TARGET): $(SRC)
	$(CC) $(BASE_FLAGS) -o $@ $^ $(LDFLAGS)

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

install: bundle
	@mkdir -p $(LV2_DIR)
	@rm -rf $(LV2_DIR)/$(BUNDLE)
	@cp -R $(BUNDLE) $(LV2_DIR)/

clean:
	@rm -rf $(BUNDLE) $(TARGET) *.o