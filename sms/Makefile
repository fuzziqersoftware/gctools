CC=gcc
CXX=g++
OBJECTS=aaf.o afc.o instrument.o audio.o ../wav.o
CFLAGS=-g -O0 -Wall -Werror
CXXFLAGS=-std=c++14 -g -O0 -DMACOSX -Wall -Werror -I/usr/local/include
LDFLAGS=-g -lphosg -lsamplerate -framework OpenAL -framework Cocoa -framework IOKit -L/usr/local/lib
EXECUTABLES=smsdumpbanks smsrenderbms

all: $(EXECUTABLES)

smsdumpbanks: $(OBJECTS) smsdumpbanks.o
	g++ $(LDFLAGS) -o smsdumpbanks $^

smsrenderbms: $(OBJECTS) smsrenderbms.o
	g++ $(LDFLAGS) -o smsrenderbms $^

clean:
	-rm -rf *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
