# --no-gnu-unique is a GCC-only flag; Hyprland plugins should be built with GCC.
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-c++11-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon`
LIBS =

SRC = main.cpp TwoPanePersistent.cpp
TARGET = twopanepersistent.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

clean:
	rm -f ./$(TARGET)

.PHONY: all clean
