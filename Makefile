KSRC ?= /lib/modules/$(shell uname -r)/build
ASCEND_HAL_INCLUDE ?= /usr/local/Ascend/driver/kernel/dev_inc/inc

ccflags-y += -I$(ASCEND_HAL_INCLUDE)

ifneq ($(KERNELRELEASE),)
P2P_BLOCK_HEADERS := $(srctree)/include/linux/blk_types.h \
	$(srctree)/include/linux/blk-mq.h \
	$(srctree)/include/linux/blkdev.h

ifneq ($(shell grep -s -l "enum rq_end_io_ret" $(P2P_BLOCK_HEADERS) 2>/dev/null),)
ccflags-y += -DP2P_HAVE_RQ_END_IO_RET
endif
endif

.PHONY: all mod lib clean test

all: mod lib

mod:
	$(MAKE) M=$(shell pwd) -C $(KSRC) modules

lib:
	$(MAKE) -C file_p2p

obj-m := stub.o
obj-m += p2p_dev.o

p2p_dev-objs := dev.o topo.o debugfs.o mem.o

test:
	$(MAKE) -C test

clean:
	rm -rf *.o *.ko *.mod.c *.mod.o *.mod modules.* Module.* .*.ko.cmd .*.mod.o.cmd .*.o.cmd
	rm -rf .*.mod.cmd .tmp_versions/
	$(MAKE) -C file_p2p clean
	$(MAKE) -C test clean
