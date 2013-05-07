all: lxc-snap

lxc-snap: lxc-snap.c
	$(CC) -o lxc-snap lxc-snap.c /usr/lib/x86_64-linux-gnu/liblxc.so.0

install:
	install lxc-snap $(DESTDIR)/bin/

clean:
	rm -f lxc-snap lxc-snap.o
