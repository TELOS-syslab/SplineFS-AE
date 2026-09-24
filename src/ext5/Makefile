# SPDX-License-Identifier: GPL-2.0

obj-m := ext5.o

ext5-objs := balloc.o bitmap.o block_validity.o dir.o dir_stable.o \
	ext5_jbd3.o \
	extents.o extents_status.o file.o fsmap.o fsync.o hash.o ialloc.o \
	indirect.o inline.o inode.o ioctl.o li_stats.o li_radix_sort.o mballoc.o migrate.o mmp.o \
	move_extent.o namei.o page-io.o readpage.o resize.o super.o symlink.o \
	sysfs.o xattr.o xattr_hurd.o xattr_trusted.o xattr_user.o \
	fast_commit.o orphan.o acl.o xattr_security.o verity.o crypto.o \
	promote.o radix_spline.o


CUR_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
src ?= $(CUR_DIR)

ccflags-y := -I$(src)/../include -I$(src)/../include/linux
ccflags-y += -D__KERNEL__
ccflags-y += -DCONFIG_EXT5_FS=1 -DCONFIG_EXT5_FS_XATTR=1
ccflags-y += -DCONFIG_EXT5_FS_POSIX_ACL=1 -DCONFIG_EXT5_FS_SECURITY=1
ccflags-y += -DCONFIG_JBD3_DEBUG_STATS
ccflags-y += -Wno-unused-function -Wno-unused-variable
ccflags-y += -Wno-declaration-after-statement

KDIR ?= /lib/modules/$(shell uname -r)/build
JBD3_DIR := $(CUR_DIR)/../jbd3
CC := gcc

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CUR_DIR) CC=$(CC) \
		KBUILD_EXTRA_SYMBOLS=$(JBD3_DIR)/Module.symvers modules

clean:
	$(MAKE) -C $(KDIR) M=$(CUR_DIR) CC=$(CC) clean
