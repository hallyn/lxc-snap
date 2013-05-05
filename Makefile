all: lxc-snap

lxc-snap: lxc-snap.c lxccontainer.h lxclock.h
	$(CC) -o lxc-snap lxc-snap.c /usr/lib/x86_64-linux-gnu/liblxc.so.0

install:
	install lxc-snap $(DESTDIR)/

clean:
	rm -f lxc-snap lxc-snap.o
