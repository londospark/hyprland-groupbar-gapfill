CXX      ?= g++
PKG      := hyprland
CXXFLAGS := -shared -fPIC -O2 -std=c++2b -Wno-c++11-narrowing
TARGET   := groupbar-gapfill.so
DEST     := $(HOME)/.local/lib/hyprland

all: $(TARGET)

$(TARGET): gapfill.cpp
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags $(PKG)) $< -o $@ $(shell pkg-config --libs $(PKG))

install: $(TARGET)
	install -Dm755 $(TARGET) $(DEST)/$(TARGET)

# Releases are named after the Hyprland release they are built against.
HYPRLAND_VERSION := $(shell pkg-config --modversion $(PKG))

tag:
	git tag -a "v$(HYPRLAND_VERSION)" -m "Built against Hyprland $(HYPRLAND_VERSION)"

clean:
	rm -f $(TARGET)

.PHONY: all install tag clean
