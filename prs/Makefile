CC=gcc
OBJECTS=main.o data_log.o errors.o prs.o util.o yay0.o yaz0.o
CFLAGS=-O3 -Wall
EXECUTABLES=prs

all: prs

prs: $(OBJECTS)
	gcc $(LDFLAGS) -o prs $^

install: prs
	cp prs /usr/bin/prs

clean:
	-rm -f *.o $(EXECUTABLES)

.PHONY: clean
