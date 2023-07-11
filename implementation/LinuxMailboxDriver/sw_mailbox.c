#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <linux/poll.h>
#include <linux/io.h>
#include <asm/io.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/delay.h>
#include <linux/list.h>
static DECLARE_WAIT_QUEUE_HEAD(mailbox_waitq);
/* lock for procfs read access */
static DEFINE_MUTEX(read_lock);
/* lock for procfs write access */
static DEFINE_MUTEX(write_lock);

/* fifo size in elements (bytes) */
#define FIFO_SIZE 512
/* define name for device and driver */
#define DEVICE_NAME "sw_mailbox"
#define DEVICE_INTERRUPT 3
/* mailbox address space*/
#define C2AMAILBOX_REG_NUM 62
#define C2AMAILBOX_CSR 0x1F8
#define C2AMAILBOX_IR 0x1F0
#define C2AMAILBOX_BASE 0x000
#define C2AMAILBOX_INT_ENA 0x8000000000000000ull

#define A2CMAILBOX_REG_NUM 62
#define A2CMAILBOX_CSR 0x3F8
#define A2CMAILBOX_IR 0x3F0
#define A2CMAILBOX_BASE 0x200
#define A2CMAILBOX_INT_ENA 0x8000000000000000ull

static int irq;
static size_t start, end, size;
static unsigned char __iomem *membase; /* read/write[bwl] */
static struct cdev mailbox_cdev;
static struct class *mailbox_class = NULL;
static struct device *mailbox_device = NULL;

static int mailbox_major = 0;
static int mailbox_minor = 0;
static volatile int mailbox_event = 0;
// static char mailbox_buf[568];
// static uint64_t count,max_msg;
// struct kfifo_rec_ptr_2 mailbox_fifo;

// struct kfifo mailbox_fifo;

typedef struct
{
    uint64_t idx;
    uint64_t val;
} Msg;

static DECLARE_KFIFO_PTR(mailbox_fifo, Msg);

#define UINT64(addr, offset) ((uint64_t *)(size_t)(addr))[offset]

bool interrupt_halting = false;

static irqreturn_t mailbox_interrupt(int irq, void *dev_id)
{

    // sbi_printf("interrupt\n");
    /*****find valid message*****/
    uint64_t mailbox_csr = readq(membase + C2AMAILBOX_CSR);
    uint64_t mailbox_valid_flags = mailbox_csr & (~(1ull << 63));
    /*****disable mailbox interrupt*****/
    writeq(0, membase + C2AMAILBOX_CSR);
    /*****read all pieces of message*****/
    Msg msg;
    int i;
    // sbi_printf("kfifo_avail before %d \n",kfifo_avail(&mailbox_fifo));
    for (i = 0; mailbox_valid_flags != 0; i++)
    {

        if (mailbox_valid_flags & 0x1ull)
        {
            if (kfifo_avail(&mailbox_fifo) > 0)
            {
                msg.idx = i;
                msg.val = readq(membase + C2AMAILBOX_BASE + i * 8);
                kfifo_put(&mailbox_fifo, msg);
            }
            else
            {
                printk("mailbox warning: receive buffer is full, message discarded");
            }
        }
        mailbox_valid_flags = mailbox_valid_flags >> 1;
    }
    // sbi_printf("kfifo_avail after %d \n",kfifo_avail(&mailbox_fifo));

    wake_up_interruptible(&mailbox_waitq);

    if (kfifo_avail(&mailbox_fifo) >= 200)
    {
        // sbi_printf("*");
        writeq(mailbox_csr, membase + C2AMAILBOX_CSR);
    }
    else
    {
        interrupt_halting = 1;
        // sbi_printf("[x]");
        writeq((mailbox_csr & (~(1ull << 63))), membase + C2AMAILBOX_CSR);
    }

    return IRQ_HANDLED;
}

static int mailbox_open(struct inode *inode, struct file *file)
{
    if (inode == NULL || file == NULL)
        return -1;
    printk("sw_mailbox: mailbox opened!\n");
    writeq(0x7fffffffffffffff, membase + A2CMAILBOX_CSR);
    kfifo_reset(&mailbox_fifo);
    writeq(0xffffffffffffffff, membase + A2CMAILBOX_CSR);
    return 0;
}

static int mailbox_close(struct inode *inode, struct file *file)
{
    writeq(0x7fffffffffffffff, membase + A2CMAILBOX_CSR);
    return 0;
}

//size:需要多少个寄存器
static ssize_t mailbox_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos)
{
    // sbi_printf("pos %lx\n",*ppos);
    if (*ppos == 0)
    {
        uint64_t msg[1024]; /*之后可以用一个kfifo代替，以容纳无限长的消息*/
        int msg_ptr; //用于标识当前的msg发送到哪了
        int i;
        uint64_t mailbox_csr;
        uint64_t mailbox_invalid_flags;

        mailbox_csr = readq(membase + C2AMAILBOX_CSR);
        mailbox_invalid_flags = mailbox_csr & (~(1ull << 63));
        if (mailbox_invalid_flags != 0) //如果接收方关闭了中断使能，则不发送
            return 0;

        
        copy_from_user(msg, buf, size * sizeof(uint64_t));
        msg_ptr = 0; 

        printk("from upper module:");
        for(i=0;i<size;++i){
            printk("%llx",msg[i]);
        }printk("\n");

        while(msg_ptr != size) {
            uint64_t receiver_info_reg = readq(membase + C2AMAILBOX_IR);
            uint64_t sender_info_reg = readq(membase + A2CMAILBOX_IR);
            int rx_tail_from_receiver =  (receiver_info_reg & 0xff00) >> 8;
            int rx_head_from_sender =  sender_info_reg & 0x00ff;
            int valid_regs_num = C2AMAILBOX_REG_NUM - 1 -
                (rx_tail_from_receiver - rx_head_from_sender + C2AMAILBOX_REG_NUM) % C2AMAILBOX_REG_NUM;
            if(valid_regs_num > 0){
                printk("valid regs are:%d\n",valid_regs_num);
                if(valid_regs_num < size - msg_ptr) { //这个if简化为 regs_to_write = min(size - msg_ptr, valid_regs_num)
                    for (i=0;i<valid_regs_num; ++i) {
                        writeq(msg[msg_ptr+i], 
                            membase + C2AMAILBOX_BASE + ((rx_tail_from_receiver + i)% C2AMAILBOX_REG_NUM)*8);
                    }
                    msg_ptr += valid_regs_num;
                    rx_tail_from_receiver += valid_regs_num;
                    rx_tail_from_receiver %= C2AMAILBOX_REG_NUM;
                } else {
                    int remain_regs = size - msg_ptr;
                    for(i=0;i<remain_regs;++i) {
                        printk("sw_mailnbox: writing %llx to reg_addr %d\n",msg[msg_ptr+i], C2AMAILBOX_BASE + ((rx_tail_from_receiver + i)% C2AMAILBOX_REG_NUM)*8);
                        writeq(msg[msg_ptr+i], 
                            membase + C2AMAILBOX_BASE + ((rx_tail_from_receiver + i)% C2AMAILBOX_REG_NUM)*8);
                    }
                    msg_ptr += remain_regs;
                    rx_tail_from_receiver += remain_regs;
                    rx_tail_from_receiver %= C2AMAILBOX_REG_NUM;
                }

                rx_tail_from_receiver <<= 8;//按格式还原
                //printk("sw_mailbox: rx_tail_from_receiver %llx\n",rx_tail_from_receiver);
                receiver_info_reg = readq(membase + C2AMAILBOX_IR);
                //printk("sw_mailbox: receiver_info_reg %llx\n",receiver_info_reg);
                receiver_info_reg &= 0xffffffffffff00ff; //清空原本的tail指针
                //printk("sw_mailbox: receiver_info_reg %llx\n",receiver_info_reg);
                receiver_info_reg |= rx_tail_from_receiver; //置位新的tail指针
                //printk("sw_mailbox: receiver_info_reg %llx\n",receiver_info_reg);
                printk("sw_mailbox: writing %llx to asp's csr\n",receiver_info_reg);
                writeq(receiver_info_reg, membase + C2AMAILBOX_IR); //将新指针信息更新到接收方InfoReg中
                writeq(0xffffffffffffffff, membase+C2AMAILBOX_CSR);//触发中断
            }
        }



        // int msg_idx = C2AMAILBOX_BASE + 0;
        // printk("sw_mailbox: writing msg:%llx to mb's reg%d\n",msg, msg_idx);
        // writeq(msg[0], membase + C2AMAILBOX_BASE + msg_idx * 8);
        // writeq(0xffffffffffffffff, membase + C2AMAILBOX_CSR); //触发ASP侧中断
    }
    else if (*ppos == 1)
    {
        writeq(0xffffffffffffffff, membase + C2AMAILBOX_CSR);
        sbi_printf("mailbox:started\n");
    }

    else if (*ppos == 2)
    {
        writeq(0x7fffffffffffffff, membase + C2AMAILBOX_CSR);
        kfifo_reset(&mailbox_fifo);
        sbi_printf("mailbox:stoped\n");
    }

    (*ppos) = 0; // seek to 0
    return size;
}

static ssize_t mailbox_read(struct file *file, char __user *buf, size_t size, loff_t *offp)
{

    int err;
    int msg_size;
    loff_t len;

    if (*offp == 0)
    {
        msg_size = sizeof(Msg);
        uint64_t n;
        n = min(size / msg_size, kfifo_len(&mailbox_fifo));
        len = n * msg_size;
        err = 0;
        int copied;
        if (len != 0)
            err = kfifo_to_user(&mailbox_fifo, buf, len, &copied);

        if (interrupt_halting && (kfifo_len(&mailbox_fifo) == 0))
        {
            writeq(A2CMAILBOX_INT_ENA, membase + A2CMAILBOX_CSR);
            interrupt_halting = false;
        }
    }
    else if (*offp == 1)
    {
        uint64_t mailbox_csr = readq(membase + A2CMAILBOX_CSR);
        uint64_t mailbox_invalid_flags = mailbox_csr & (~(1ull << 63));
        uint64_t writable_entries = ~mailbox_invalid_flags;
        err = copy_to_user(buf, &writable_entries, 8);
        len = 8;
    }
    (*offp) = 0; // seek to 0
    return err ? -EFAULT : len;
}

static unsigned int mailbox_poll(struct file *file, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    bool readable;
    poll_wait(file, &mailbox_waitq, wait);

    readable = kfifo_len(&mailbox_fifo) != 0;
    if (readable)
    {
        mask = POLLIN | POLLRDNORM;
    }

    // sbi_printf("poll waked\n");

    return mask;
}

/* probe platform driver */
static int mailbox_probe(struct platform_device *pdev)
{
    int ret;
    struct device_node *np = pdev->dev.of_node;
    struct resource *res;
    printk("sw_mailbox: mailbox probe\n");
    /* Obtain interrupt ID from DTS */
    irq = of_irq_get(np, 0);
    ret = request_irq(irq, mailbox_interrupt, IRQF_TRIGGER_FALLING, DEVICE_NAME, NULL); // IRQF_ONESHOT
    if (ret != 0)
    {
        printk("sw_mailbox: register interrupt failed");
        free_irq(irq, NULL);
        return -EBUSY;
    }

    printk("sw_mailbox: open and register interrupt");
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (res)
    {
        start = res->start;
        end = res->end;
        size = res->end - res->start + 1;
    }
    printk("sw_mailbox: mailbox start: %#lx, end: %#lx, size: %#lx", start, end, size);
    membase = ioremap(start, size);
    return 0;
}

// /* remove platform driver */
static int mailbox_remove(struct platform_device *pdev)
{
    /* Release Interrupt */
    free_irq(irq, NULL);
    /* Unmap Iomem */
    iounmap(membase);
    return 0;
}

static const struct of_device_id mailbox_of_match[] = {
    {
        .compatible = "asp,asp_mailbox",
    },
    {},
};
MODULE_DEVICE_TABLE(of, mailbox_of_match);

/* platform driver information */
static struct platform_driver mailbox_driver = {
    .probe = mailbox_probe,
    .remove = mailbox_remove,
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = mailbox_of_match,
    },
};

static const struct file_operations mailbox_fops = {
    .owner = THIS_MODULE,
    .open = mailbox_open,
    .read = mailbox_read,
    .write = mailbox_write,
    .poll = mailbox_poll,
    .release = mailbox_close,
    .llseek = default_llseek};

static int mailbox_setup_cdev(struct cdev *cdev, dev_t devno)
{
    int ret = 0;

    cdev_init(cdev, &mailbox_fops);
    cdev->owner = THIS_MODULE;
    ret = cdev_add(cdev, devno, 1);

    return ret;
}

static int __init mailbox_init(void)
{
    int ret;
    dev_t devno;

    printk("sw_mailbox: mailbox 20230710 driver init...\n");

    platform_driver_register(&mailbox_driver);

    // get devno
    if (mailbox_major)
    {
        devno = MKDEV(mailbox_major, mailbox_minor);
        ret = register_chrdev_region(devno, 1, DEVICE_NAME);
    }
    else
    {
        ret = alloc_chrdev_region(&devno, mailbox_minor, 1, DEVICE_NAME);
        mailbox_major = MAJOR(devno);
    }
    printk("sw_mailbox: mailbox - major: %d\n", mailbox_major);

    if (ret < 0)
    {
        printk("sw_mailbox: get mailbox major failed\n");
        return ret;
    }

    // setup cdev
    ret = mailbox_setup_cdev(&mailbox_cdev, devno);
    if (ret)
    {
        printk("sw_mailbox: mailbox setup cdev failed, ret = %d\n", ret);
        goto cdev_add_fail;
    }

    // create class
    mailbox_class = class_create(THIS_MODULE, DEVICE_NAME);
    ret = IS_ERR(mailbox_class);
    if (ret)
    {
        printk(KERN_WARNING "sw_mailbox: class create failed\n");
        goto class_create_fail;
    }

    // create device
    mailbox_device = device_create(mailbox_class, NULL, devno, NULL, DEVICE_NAME);
    ret = IS_ERR(mailbox_device);
    if (ret)
    {
        printk(KERN_WARNING "sw_mailbox: mailbox device create failed, error code %ld \n", PTR_ERR(mailbox_device));
        goto device_create_fail;
    }
    else
    {
        printk("sw_mailbox: succeed to create /dev/%s \n", DEVICE_NAME);
    }
    // create kfifo
    ret = kfifo_alloc(&mailbox_fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret)
    {
        printk(KERN_ERR "sw_mailbox: error kfifo_alloc\n");
        return ret;
    }
    printk("sw_mailbox: driver MSG buffer size %#d\n", kfifo_size(&mailbox_fifo));

    // /*test sending msg*/
    // Msg msg;
    // msg.val = 0xaabbccddeeff1122;
    // printk("sw_mailbox: sending msg=%lx", msg.val);
    // writeq(msg.val, membase + 0);

    return 0;
device_create_fail:
    class_destroy(mailbox_class);
class_create_fail:
    cdev_del(&mailbox_cdev);
cdev_add_fail:
    unregister_chrdev_region(devno, 1);
    return ret;
}

static void __exit mailbox_exit(void)
{
    dev_t devno;

    printk("sw_mailbox: mailbox driver exit...\n");
    kfifo_free(&mailbox_fifo);

    devno = MKDEV(mailbox_major, mailbox_minor);
    device_destroy(mailbox_class, devno);
    class_destroy(mailbox_class);
    cdev_del(&mailbox_cdev);
    unregister_chrdev_region(devno, 1);
    platform_driver_unregister(&mailbox_driver);
}

// module_platform_driver(mailbox_driver);
module_init(mailbox_init);
module_exit(mailbox_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LPN BYK YCC");
MODULE_DESCRIPTION("ASP mailbox");
