CC=gcc
CXX=g++
OBJECTS=aaf.o afc.o instrument.o audio.o wav.o
CFLAGS=-g -O0 -Wall -Werror
CXXFLAGS=-std=c++14 -g -O0 -Wall -Werror -I/usr/local/include
LDFLAGS=-g -lphosg -L/usr/local/lib
EXECUTABLES=smsdumpbanks smsrenderbms
LIBS=-lphosg -lsamplerate

UNAME = $(shell uname -s)
ifeq ($(UNAME),Darwin)
	CXXFLAGS += -DMACOSX -I/opt/local/include
	LDFLAGS += -framework OpenAL -framework Cocoa -framework IOKit
endif
ifeq ($(UNAME),Linux)
	CXXFLAGS += -DLINUX
	LDFLAGS += -L/usr/lib/x86_64-linux-gnu
	LIBS += -lopenal -lpthread
endif


all: $(EXECUTABLES)

smsdumpbanks: $(OBJECTS) smsdumpbanks.o
	g++ $(LDFLAGS) -o smsdumpbanks $^ $(LIBS)

smsrenderbms: $(OBJECTS) smsrenderbms.o
	g++ $(LDFLAGS) -o smsrenderbms $^ $(LIBS)

clean:
	-rm -rf *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
