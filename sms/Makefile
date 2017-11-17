CC=gcc
CXX=g++
OBJECTS=aaf.o afc.o instrument.o ../wav.o
CFLAGS=-g -O0 -Wall -Werror
CXXFLAGS=-std=c++14 -g -O0 -Wall -Werror
LDFLAGS=-g -lphosg
EXECUTABLES=smsgetseqs smsdumpbanks smsrenderbms

all: $(EXECUTABLES)

smsgetseqs: $(OBJECTS) smsgetseqs.o
	g++ $(LDFLAGS) -o smsgetseqs $^

smsdumpbanks: $(OBJECTS) smsdumpbanks.o
	g++ $(LDFLAGS) -o smsdumpbanks $^

smsrenderbms: $(OBJECTS) smsrenderbms.o
	g++ $(LDFLAGS) -o smsrenderbms $^

clean:
	-rm -f *.o $(EXECUTABLES)

.PHONY: clean
