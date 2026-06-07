# --- LilBrimstone Generic LV2 Makefile ---
# Plugin: Dexed (16-Voice MSFA FM Engine)

PLUGIN_NAME := dexed
URI         := https://github.com/lilbrimstone/$(PLUGIN_NAME)
BUNDLE      := $(PLUGIN_NAME).lv2
BUILD_DIR   := build

# Sources
SRC_CPP := dexed.cpp msfa/porta.cpp
SRC_CC  := msfa/dx7note.cc msfa/env.cc msfa/exp2.cc msfa/fm_core.cc \
           msfa/fm_op_kernel.cc msfa/freqlut.cc msfa/lfo.cc \
           msfa/pitchenv.cc msfa/sin.cc msfa/tuning.cc

# Object Files (Mirrors folder structure inside build/)
OBJ := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(SRC_CPP)) \
       $(patsubst %.cc, $(BUILD_DIR)/%.o, $(SRC_CC))

# Compiler and Flags
CXX      ?= g++
CXXFLAGS += -fPIC -std=c++17 -O3 -Wall -Wno-unused-parameter -Imsfa
LDFLAGS  += -shared -lm

# OS Detection
UNAME_S := $(shell uname -s)

ifeq ($(OS),Windows_NT)
    TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).dll
    LDFLAGS += -static -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic
    CXXFLAGS += -D_WIN32
else
    ifeq ($(UNAME_S),Darwin)
        TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).dylib
    else
        TARGET = $(BUILD_DIR)/$(PLUGIN_NAME).so
        LDFLAGS += -Wl,-Bsymbolic
    endif
endif

# Default target
.PHONY: all clean install

all: $(BUNDLE)

# Create the LV2 bundle structure
$(BUNDLE): $(TARGET)
	@echo "Creating LV2 bundle: $(BUNDLE)"
	@mkdir -p $(BUNDLE)
	@cp $(TARGET) $(BUNDLE)/
	@cp patches/$(PLUGIN_NAME).ttl $(BUNDLE)/
	@echo "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> ." > $(BUNDLE)/manifest.ttl
	@echo "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> ." >> $(BUNDLE)/manifest.ttl
	@echo "" >> $(BUNDLE)/manifest.ttl
	@echo "<$(URI)>" >> $(BUNDLE)/manifest.ttl
	@echo "    a lv2:Plugin ;" >> $(BUNDLE)/manifest.ttl
	@echo "    lv2:binary <$(notdir $(TARGET))> ;" >> $(BUNDLE)/manifest.ttl
	@echo "    rdfs:seeAlso <$(PLUGIN_NAME).ttl> ." >> $(BUNDLE)/manifest.ttl

# Compile .cpp sources
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile .cc sources
$(BUILD_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link target
$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) -o $@

# System-wide install (Linux/macOS)
install: $(BUNDLE)
ifeq ($(OS),Windows_NT)
	@echo "Install target is for Linux/macOS. Manually copy the .lv2 folder to your VST/LV2 directory."
else
	@mkdir -p ~/.lv2
	@rm -rf ~/.lv2/$(BUNDLE)
	@cp -r $(BUNDLE) ~/.lv2/
	@echo "Installed to ~/.lv2/$(BUNDLE)"
endif

clean:
	@echo "Cleaning build artifacts..."
	@rm
