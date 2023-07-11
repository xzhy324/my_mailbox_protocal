#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned long long uint64_t;

int main()
{
    int fd; // 文件描述符

    // 打开设备文件
    fd = open("/dev/sw_mailbox", O_RDWR);
    if (fd == -1) {
        printf("mailbox_test: cannot open /dev/sw_mailbox");
        return 1;
    }

    printf("please enter (size, u64 msg):\n");
    uint64_t input;
    int size;
    uint64_t msg[1024];
    scanf("%d", &size);
    for (int i = 0; i < size; ++i) {
        scanf("%llx", &msg[i]);
    }

    int bytesWritten = write(fd, (const void *)&msg, size);
    if (bytesWritten < 0) {
        printf("mailbox_test: write failed!");
        close(fd);
        return 1;
    }

    // 关闭设备文件
    close(fd);

    return 0;
}
