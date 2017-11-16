CFLAGS=-g -Wall -Werror
CXXFLAGS=-g -Wall -Werror -std=c++14
EXECUTABLES=afsdump gcmdump gsldump gvmdump pae2gvm smsgetseqs smsrenderbms smsdumpbanks

all: $(EXECUTABLES) prs

afsdump: afsdump.c
	gcc $(CFLAGS) -o afsdump afsdump.c

gcmdump: gcmdump.cc
	g++ $(CXXFLAGS) -o gcmdump gcmdump.cc

gsldump: gsldump.c
	gcc $(CFLAGS) -o gsldump gsldump.c

gvmdump: gvmdump.c
	gcc $(CFLAGS) -o gvmdump gvmdump.c

pae2gvm: pae2gvm.c
	gcc $(CFLAGS) -o pae2gvm pae2gvm.c prs/prs.c prs/data_log.c

smsgetseqs: smsgetseqs.cc
	g++ $(CXXFLAGS) -o smsgetseqs smsgetseqs.cc -lphosg

smsdumpbanks: smsdumpbanks.cc
	g++ $(CXXFLAGS) -o smsdumpbanks wav.cc smsdumpbanks.cc -lphosg

smsrenderbms: smsrenderbms.cc
	g++ $(CXXFLAGS) -o smsrenderbms wav.cc smsrenderbms.cc -lphosg

prs:
	cd prs && make && cd ..

install: all
	cd prs && make install && cd ..
	cp $(EXECUTABLES) /usr/bin/

clean:
	-rm -f *.o $(EXECUTABLES)
	cd prs && make clean && cd ..

.PHONY: clean prs
