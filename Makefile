# n_tty_info: FreeBSD-style ^T/SIGINFO line discipline for Linux
#
#   make module    build the kernel module (needs linux-headers for your kernel)
#   make helper    build setinfold + infotest (userspace)
#   make all       both
#   make clean

obj-m := n_tty_info.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= cc

.PHONY: all module helper clean

all: module helper

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

helper: setinfold infotest

setinfold: setinfold.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

infotest: infotest.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f setinfold infotest
