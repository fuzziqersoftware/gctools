CC=gcc
CXX=g++
OBJECTS=aaf.o afc.o instrument.o
CFLAGS=-g -Wall -Werror
CXXFLAGS=-std=c++14 -g -Wall -Werror -I/usr/local/include -I /opt/local/include
LDFLAGS=-g -lphosg -L/usr/local/lib -L/opt/local/lib
LIBS=-lphosg -lphosg-audio -lsamplerate

ifeq ($(OS),Windows_NT)
	CXXFLAGS +=  -DWINDOWS -D__USE_MINGW_ANSI_STDIO=1 -D_USE_MATH_DEFINES
	RM=del /S
	EXE_EXTENSION=.exe
else
	ifeq ($(shell uname -s),Darwin)
		CXXFLAGS += -DMACOSX -I/opt/local/include
		LDFLAGS += -framework OpenAL -framework Cocoa -framework IOKit
		LIBS += -lphosg-audio
	else
		CXXFLAGS += -DLINUX
		LDFLAGS += -L/usr/lib/x86_64-linux-gnu
		LIBS += -lopenal -lpthread
	endif
	RM=rm -rf
	EXE_EXTENSION=
endif

EXECUTABLES=smsdumpbanks$(EXE_EXTENSION) smssynth$(EXE_EXTENSION)

all: $(EXECUTABLES)

smsdumpbanks$(EXE_EXTENSION): $(OBJECTS) smsdumpbanks.o
	g++ $(LDFLAGS) -o $@ $^ $(LIBS)

smssynth$(EXE_EXTENSION): $(OBJECTS) smssynth.o
	g++ $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
