CXX ?= g++
TARGET := hyprsplitrow.so
SRC := src/main.cpp

PKGS := pixman-1 libdrm hyprland pangocairo libinput wayland-server xkbcommon xcb xcb-icccm xcb-composite xcb-xfixes xcb-render xcb-res xcb-errors

CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++26 -Wall -Wextra -g -DWLR_USE_UNSTABLE
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
