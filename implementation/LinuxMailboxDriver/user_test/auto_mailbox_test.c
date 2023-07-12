#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned long long uint64_t;

#define TEST_N 5

int main()
{
    int fd; // 文件描述符

    // 打开设备文件
    fd = open("/dev/sw_mailbox", O_RDWR);
    if (fd == -1) {
        printf("mailbox_test: cannot open /dev/sw_mailbox");
        return 1;
    }

    int size[TEST_N];
    int test_n,i;
    uint64_t msg[TEST_N][1024];



    for(test_n = 0; test_n < TEST_N; ++test_n){
        int bytesWritten = write(fd, (const void *)&(msg[test_n]), size[test_n]);
    }
    
    // 关闭设备文件
    close(fd);

    return 0;
}
