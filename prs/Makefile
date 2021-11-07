CXX=g++
CXXFLAGS=-g -Wall -Werror -I/opt/local/include -std=c++20
CXXLIBS=-lphosg
OBJECTS=main.o PRSDataLog.o PRS.o Yay0.o Yaz0.o
LDFLAGS=-g -L/opt/local/lib -std=c++20

ifeq ($(OS),Windows_NT)
	CXXFLAGS +=  -DWINDOWS -D__USE_MINGW_ANSI_STDIO=1

	RM=del /S
	EXE_EXTENSION=.exe
else
	RM=rm -rf
	EXE_EXTENSION=

	ifeq ($(shell uname -s),Darwin)
		CXXFLAGS +=  -DMACOSX -mmacosx-version-min=10.11
	else
		CXXFLAGS +=  -DLINUX
	endif
endif

prs$(EXE_EXTENSION): $(OBJECTS)
	g++ $(LDFLAGS) -o $@ $^ -lphosg

install: prs
	cp prs /usr/local/bin/prs

clean:
	$(RM) *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
