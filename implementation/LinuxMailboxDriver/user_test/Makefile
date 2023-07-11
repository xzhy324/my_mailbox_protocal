CURRENT_PATH := $(shell pwd)
build:
	riscv64-unknown-linux-gnu-gcc user_test.c -o mailbox_test
install:build
	cp mailbox_test /home/xuzheyuan-DomainA/asp-linux/ramfs/root

