# SPDX-License-Identifier: GPL-2.0-only

obj-m := jbd3.o
jbd3-objs := transaction.o commit.o recovery.o checkpoint.o revoke.o journal.o

CUR_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
src ?= $(CUR_DIR)

ccflags-y := -I$(src)/../include -I$(src)/../include/linux
ccflags-y += -D__KERNEL__ -DCONFIG_JBD3_DEBUG_STATS
ccflags-y += -Wno-unused-function -Wno-unused-variable
ccflags-y += -Wno-declaration-after-statement

KDIR ?= /lib/modules/$(shell uname -r)/build
CC := gcc

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CUR_DIR) CC=$(CC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CUR_DIR) CC=$(CC) clean
