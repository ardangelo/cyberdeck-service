CXX ?= g++
CC  ?= gcc

CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++20 -fno-rtti
CFLAGS   ?= -O2 -g -Wall -Wextra
CPPFLAGS := -Isrc/nanopb -Isrc
LDFLAGS  ?=
LDLIBS   ?=

# nanopb sources
NANOPB_SRCS = src/nanopb/pb_encode.c src/nanopb/pb_decode.c src/nanopb/pb_common.c
NANOPB_OBJS = $(NANOPB_SRCS:.c=.o)

# pebble protocol sources
PEBBLE_SRCS = src/pebble/cobs.c src/pebble/crc8.c src/pebble/k230_protocol.pb.c
PEBBLE_OBJS = $(PEBBLE_SRCS:.c=.o)

PROTO_OBJS = $(NANOPB_OBJS) $(PEBBLE_OBJS)

.PHONY: all clean proto

all: cyberdeck_daemon fake_uart fake_uinput ppouart_test

# Generate nanopb files from proto
proto:
	python3 src/nanopb/generator/nanopb_generator.py -I src/pebble -D src/pebble src/pebble/k230_protocol.proto

cyberdeck_daemon: src/main.o src/uart.o $(PROTO_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fake_uart: src/fake_uart.o src/uart.o $(PROTO_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fake_uinput: src/fake_uinput.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

ppouart_test: src/ppouart_test.o src/pebble/cobs.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Compile C++ sources
src/%.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# Compile C sources in pebble/
src/pebble/%.o: src/pebble/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Compile nanopb C sources
src/nanopb/%.o: src/nanopb/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f cyberdeck_daemon fake_uart fake_uinput ppouart_test src/*.o src/pebble/*.o src/nanopb/*.o
