#
# Based on rv64-emu
# 

CXX = g++

# Give us some how of compiling fast enough on this JH7110
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -g -O0 \
	-Wno-unused-parameter -Wno-unused-function
LDFLAGS = 

OBJECTS = main.o elf_file.o helpers.o util.o
HEADERS = elf_file.h util.h

ifdef ENABLE_FRAMEBUFFER
OBJECTS += framebuffer.o
HEADERS += framebuffer.h

CXXFLAGS += -DENABLE_FRAMEBUFFER `pkg-config --cflags sdl2`
LDFLAGS += `pkg-config --libs sdl2`
endif

all: rv64-ume

rv64-ume: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $<

%.o: %.s
	$(CXX) -c $<

clean:
	rm -f rv64-ume
	rm -f $(OBJECTS)
	rm -f framebuffer.o
