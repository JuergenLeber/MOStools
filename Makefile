# MOStools - tools for alphatronic P2 MOS floppy disk images

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PROGS   = mosls moscp mosrecover
OBJS    = mosfs.o mosimd.o

all: $(PROGS)

mosls: mosls.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mosls.o $(OBJS)

moscp: moscp.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ moscp.o $(OBJS)

mosrecover: mosrecover.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mosrecover.o $(OBJS)

mosfs.o mosls.o moscp.o mosrecover.o: mosfs.h
mosfs.o mosimd.o: mosimd.h

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROGS) $(DESTDIR)$(BINDIR)

check: all
	./mosls -v -l alphatronic-system30.img

clean:
	rm -f $(PROGS) *.o

.PHONY: all install check clean
