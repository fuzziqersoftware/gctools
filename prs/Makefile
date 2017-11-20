CC=gcc
OBJECTS=main.o data_log.o errors.o prs.o util.o yay0.o yaz0.o
CFLAGS=-g -O0 -Wall
LDFLAGS=-g
EXECUTABLES=prs

all: prs

prs: $(OBJECTS)
	gcc $(LDFLAGS) -o prs $^

install: prs
	cp prs /usr/bin/prs

clean:
	-rm -rf *.o $(EXECUTABLES) *.dSYM

.PHONY: clean
