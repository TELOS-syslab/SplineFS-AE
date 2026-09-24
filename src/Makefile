# SPDX-License-Identifier: GPL-2.0

obj-m := jbd3/ ext5/

KDIR ?= /lib/modules/$(shell uname -r)/build
MODDIR := $(shell pwd)
CC := gcc

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(MODDIR) CC=$(CC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MODDIR) CC=$(CC) clean
