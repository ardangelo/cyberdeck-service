CXX ?= g++
CC  ?= gcc

CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++20 -fno-rtti
CFLAGS   ?= -O2 -g -Wall -Wextra
CPPFLAGS := -Isrc/nanopb -Isrc
LDFLAGS  ?=

# JNI
JAVA_HOME ?= /usr/lib/jvm/java-21-openjdk-riscv64/
JNI_CPPFLAGS := $(CPPFLAGS) -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
JNI_CXXFLAGS := $(CXXFLAGS)
JNI_LDFLAGS := -ldl

# nanopb sources
PROTO_SRCS = src/nanopb/pb_encode.c src/nanopb/pb_decode.c src/nanopb/pb_common.c src/pebble/k230_protocol.pb.c
PROTO_OBJS = $(PROTO_SRCS:.c=.o)

.PHONY: all clean deb fatjar

all: cyberdeck_daemon fake_uinput jni_test_daemon

fatjar:
	cd mobileapp && ./gradlew :libpebble3:jvmFatJar

fake_uinput: src/fake_uinput.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

src/jni/uart_jni_bridge.o: src/jni/uart_jni_bridge.cpp src/jni/uart_jni_bridge.hpp
	$(CXX) $(CPPFLAGS) $(JNI_CPPFLAGS) $(JNI_CXXFLAGS) -fPIC -c -o $@ $<

src/main.o: src/main.cpp src/jni/uart_jni_bridge.hpp src/pebble/k230_protocol.pb.h
	$(CXX) $(CPPFLAGS) $(JNI_CPPFLAGS) $(JNI_CXXFLAGS) -c -o $@ $<

cyberdeck_daemon: src/main.o src/jni/uart_jni_bridge.o $(PROTO_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(JNI_LDFLAGS)

src/jni_test_main.o: src/jni_test_main.cpp src/jni/uart_jni_bridge.hpp
	$(CXX) $(CPPFLAGS) $(JNI_CPPFLAGS) $(JNI_CXXFLAGS) -c -o $@ $<

jni_test_daemon: src/jni_test_main.o src/jni/uart_jni_bridge.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(JNI_LDFLAGS)

# Generate nanopb files from proto
src/pebble/k230_protocol.pb.h src/pebble/k230_protocol.pb.c: src/pebble/k230_protocol.proto
	python3 src/nanopb/generator/nanopb_generator.py -I src/pebble -D src/pebble src/pebble/k230_protocol.proto

# Compile nanopb C sources
src/nanopb/%.o: src/nanopb/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
src/pebble/%.o: src/pebble/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f cyberdeck_daemon fake_uinput jni_test_daemon src/*.o src/jni/*.o \
		src/nanopb/*.o src/nanopb/*.pb.c src/nanopb/*.pb.h
deb:
	dpkg-buildpackage -us -uc --host-arch riscv64
