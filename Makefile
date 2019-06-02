CFLAGS=-g -Wall -Werror
CXXFLAGS=-g -Wall -Werror -I/opt/local/include -L/opt/local/lib -std=c++14
CXXLIBS=-lphosg -lpthread

ifeq ($(OS),Windows_NT)
	CXXFLAGS +=  -DWINDOWS -D__USE_MINGW_ANSI_STDIO=1 -IX:\\ -LX:\\phosg
	RM=del /S
	EXE_EXTENSION=.exe
else
	ifeq ($(shell uname -s),Darwin)
		CXXFLAGS +=  -DMACOSX -mmacosx-version-min=10.11
	else
		CXXFLAGS +=  -DLINUX
	endif
	RM=rm -rf
	EXE_EXTENSION=
endif

EXECUTABLES=afsdump$(EXE_EXTENSION) gcmdump$(EXE_EXTENSION) gsldump$(EXE_EXTENSION) gvmdump$(EXE_EXTENSION) rcfdump$(EXE_EXTENSION) pae2gvm$(EXE_EXTENSION)




all: $(EXECUTABLES) prs sms

afsdump$(EXE_EXTENSION): afsdump.cc
	g++ $(CXXFLAGS) -o $@ afsdump.cc $(CXXLIBS)

gcmdump$(EXE_EXTENSION): gcmdump.cc
	g++ $(CXXFLAGS) -o $@ gcmdump.cc $(CXXLIBS)

gsldump$(EXE_EXTENSION): gsldump.c
	gcc $(CFLAGS) -o $@ gsldump.c

gvmdump$(EXE_EXTENSION): gvmdump.c
	gcc $(CFLAGS) -o $@ gvmdump.c

rcfdump$(EXE_EXTENSION): rcfdump.cc
	g++ $(CXXFLAGS) -o $@ rcfdump.cc $(CXXLIBS)

pae2gvm$(EXE_EXTENSION): pae2gvm.c
	gcc $(CFLAGS) -o $@ pae2gvm.c prs/prs.c prs/data_log.c

sms:
	cd sms && make && cd ..

prs:
	cd prs && make && cd ..

install: all
	cd prs && make install && cd ..
	cp $(EXECUTABLES) /usr/bin/

clean:
	$(RM) *.o $(EXECUTABLES) *.dSYM
	cd prs && make clean && cd ..
	cd sms && make clean && cd ..

.PHONY: clean prs sms
