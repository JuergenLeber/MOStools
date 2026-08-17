# MOStools - tools for alphatronic P2 MOS floppy disk images

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PROGS   = mosls moscp mosrecover mosbasic
OBJS    = mosfs.o mosimd.o mosdsk.o

all: $(PROGS)

mosls: mosls.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mosls.o $(OBJS)

moscp: moscp.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ moscp.o $(OBJS)

mosrecover: mosrecover.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mosrecover.o $(OBJS)

mosbasic: mosbasic.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mosbasic.o $(OBJS) -lm

mosfs.o mosls.o moscp.o mosrecover.o mosbasic.o: mosfs.h
mosfs.o mosimd.o: mosimd.h
mosfs.o mosdsk.o: mosdsk.h

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROGS) $(DESTDIR)$(BINDIR)

CHECK_IMAGES = alphatronic-system30.img DISK-BASIC-SYSTEM.dsk

check: all
	@for img in $(CHECK_IMAGES); do \
		[ -f "$$img" ] || { echo "skipping $$img (not here)"; continue; }; \
		echo "=== $$img ==="; \
		./mosls -v -l "$$img" || exit 1; \
		./mosbasic -u "$$img" | head -12 || exit 1; \
		./mosrecover "$$img" || exit 1; \
	done

clean:
	rm -f $(PROGS) *.o

.PHONY: all install check clean
