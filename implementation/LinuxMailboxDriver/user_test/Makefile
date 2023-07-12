.PHONY: build install

build:user_test.c auto_mailbox_test.c
	riscv64-unknown-linux-gnu-gcc user_test.c -o build/mailbox_test
	riscv64-unknown-linux-gnu-gcc auto_mailbox_test.c -o build/auto_mailbox_test

install:build
	cp build/mailbox_test /home/xuzheyuan-DomainA/asp-linux/ramfs/root
	cp build/auto_mailbox_test /home/xuzheyuan-DomainA/asp-linux/ramfs/root

clean:
	-rm build/auto_mailbox_test build/mailbox_test

