#
# Based on rv64-emu
# 

CXX = g++

# Give us some how of compiling fast enough on this JH7110
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -g -O0 \
	-Wno-unused-parameter

OBJECTS = \
	main.o

HEADERS = \


all: rv64-ume

rv64-ume: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f rv64-ume
	rm -f $(OBJECTS)
