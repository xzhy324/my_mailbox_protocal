obj-m := sw_mailbox.o
CURRENT_PATH := $(shell pwd)

LINUX_KERNEL_PATH := ../../linux/
build:
	make -C user_test build
	$(MAKE) -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH)  ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- modules
install:build
	cp sw_mailbox.ko /home/xuzheyuan-DomainA/asp-linux/ramfs/lib/modules/sw_mailbox.ko
	make -C user_test install
clean:
	make -C user_test clean
	-rm *.o  .*.cmd *.mod.c .tmp_versions Module.symvers modules.order
