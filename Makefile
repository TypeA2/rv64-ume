#
# Based on rv64-emu
# 

CXX = g++

# Give us some how of compiling fast enough on this JH7110
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -g -O0 \
	-Wno-unused-parameter -Wno-unused-function

OBJECTS = \
	main.o \
	elf_file.o \
	helpers.o

HEADERS = \
	arch.h \
	elf_file.h

all: rv64-ume

rv64-ume: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $<

%.o: %.s
	$(CXX) -c $<

clean:
	rm -f rv64-ume
	rm -f $(OBJECTS)
