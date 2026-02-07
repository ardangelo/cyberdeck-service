CXX ?= g++

CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17 -fno-rtti
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS   ?=

.PHONY: all clean

all: cyberdeck_daemon fake_uart fake_uinput

cyberdeck_daemon: src/main.o src/uart.cpp src/uinput.cpp
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fake_uart: src/fake_uart.cpp
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fake_uinput: src/fake_uinput.cpp
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f cyberdeck_daemon fake_uart fake_uinput src/*.o
