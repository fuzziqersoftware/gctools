CC=gcc
OBJECTS=main.o data_log.o errors.o prs.o util.o yay0.o yaz0.o
CFLAGS=-g -Wall
LDFLAGS=-g

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
	gcc $(LDFLAGS) -o $@ $^

install: prs
	cp prs /usr/bin/prs

clean:
	$(RM) *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
