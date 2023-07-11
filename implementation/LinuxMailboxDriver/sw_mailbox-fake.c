#include  <linux/cdev.h>
#include  <linux/module.h>
#include  <linux/kernel.h>
#include  <linux/init.h>
#include  <linux/fs.h>
#include  <linux/irq.h>
#include  <linux/platform_device.h>
#include  <linux/of_irq.h>
#include  <linux/poll.h>
#include  <linux/io.h>
#include  <asm/io.h>
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
#define FIFO_SIZE	512
/* define name for device and driver */
#define DEVICE_NAME "sw_mailbox"
#define DEVICE_INTERRUPT 3
/* mailbox address space*/
#define C2AMAILBOX_REG_NUM 63
#define C2AMAILBOX_CSR 0x3F8
#define C2AMAILBOX_BASE 0x200
#define C2AMAILBOX_INT_ENA 0x8000000000000000ull

#define A2CMAILBOX_REG_NUM 63
#define A2CMAILBOX_CSR 0x1F8
#define A2CMAILBOX_BASE 0x000
#define A2CMAILBOX_INT_ENA 0x8000000000000000ull



static int irq;
static size_t start, end, size;
static unsigned char __iomem	*membase; /* read/write[bwl] */
static struct cdev mailbox_cdev;
static struct class *mailbox_class = NULL;
static struct device *mailbox_device = NULL;

static int mailbox_major = 0;
static int mailbox_minor = 0;
static volatile int mailbox_event = 0;
//static char mailbox_buf[568];
//static uint64_t count,max_msg;
//struct kfifo_rec_ptr_2 mailbox_fifo;

//struct kfifo mailbox_fifo;

typedef  struct {uint64_t idx;uint64_t val ;} Msg;
static DECLARE_KFIFO_PTR(mailbox_fifo, Msg);

#define UINT64(addr,offset) ((uint64_t*)(size_t)(addr))[offset]










static int mailbox_open(struct inode *inode, struct file *file)
{
    if(inode == NULL || file == NULL)
        return -1;
    
    return 0;
}

static int mailbox_close(struct inode *inode, struct file *file)
{
    
    return 0;
}




static ssize_t mailbox_write(struct file *file, const char __user * buf, size_t size, loff_t * ppos)
{
    Msg msg;
    //sbi_printf("pos %lx\n",*ppos);
    if(*ppos == 0 )
    {

        if(size != sizeof(Msg))
            return 0;
        if(kfifo_avail(&mailbox_fifo) > 0)
        {
            copy_from_user(&msg,buf,size);
            kfifo_put(&mailbox_fifo,msg);
            if(kfifo_len (&mailbox_fifo) >0)
            {
                wake_up_interruptible(&mailbox_waitq);
            }
            
            return size;
        }
        else
        {
            return 0;
        }  

    }
    else if(*ppos == 1)
    {
        
        kfifo_reset(&mailbox_fifo);
        sbi_printf("mailbox:kfifo cleared\n");
        
    }
    

    (* ppos) = 0;//seek to 0
    return size;

}



static ssize_t mailbox_read(struct file *file, char __user * buf, size_t size, loff_t *offp)
{

    int err;
    int msg_size;
    Msg msg;
    
    
    msg_size=sizeof(Msg);
    loff_t n; n = min(size/msg_size,kfifo_len (&mailbox_fifo));
    loff_t len= n*msg_size;
    err=0;
    int copied;
    if(len != 0)
        err = kfifo_to_user(&mailbox_fifo, buf, len, &copied);
    (* offp) = 0;//seek to 0
    return err ? -EFAULT: len;
}

static unsigned int mailbox_poll(struct file *file, struct poll_table_struct *wait)
{

    bool readable;
    unsigned int mask = 0;

    
    poll_wait(file, &mailbox_waitq, wait);
    
    readable = kfifo_len(&mailbox_fifo) != 0;
   if(readable){
        mask = POLLIN | POLLRDNORM;
    }

    //sbi_printf("poll waked\n");

    return mask;
}

/* probe platform driver */
static int mailbox_probe(struct platform_device *pdev)
{
    return 0;
}

// /* remove platform driver */
static int mailbox_remove(struct platform_device *pdev)
{
    return 0;
}

static const struct of_device_id mailbox_of_match[] = {
    { .compatible = "asp,asp_mailbox", },
    { },
};
MODULE_DEVICE_TABLE(of, mailbox_of_match);

/* platform driver information */
static struct platform_driver mailbox_driver = {
    .probe  = mailbox_probe,
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
    .llseek = default_llseek
};

static int mailbox_setup_cdev(struct cdev *cdev, dev_t devno)
{
    int ret = 0;

    cdev_init(cdev, &mailbox_fops);
    cdev->owner = THIS_MODULE;
    ret = cdev_add(cdev,devno, 1);

    return ret;
}

static int __init mailbox_init(void)
{
    


    int ret;
    dev_t devno;
    
    printk("fake sw_mailbox: mailbox 20210322 driver init...\n");

    platform_driver_register(&mailbox_driver);

    // get devno
    if(mailbox_major){
        devno = MKDEV(mailbox_major, mailbox_minor);
        ret = register_chrdev_region(devno, 1, DEVICE_NAME);
    } else {
        ret = alloc_chrdev_region(&devno, mailbox_minor, 1, DEVICE_NAME);
        mailbox_major = MAJOR(devno);
    }
    printk("sw_mailbox: mailbox - major: %d\n", mailbox_major);

    if(ret <0){
        printk("sw_mailbox: get mailbox major failed\n");
        return ret;
    }
 
    // setup cdev
    ret = mailbox_setup_cdev(&mailbox_cdev, devno);
    if(ret) {
        printk("sw_mailbox: mailbox setup cdev failed, ret = %d\n",ret);
        goto cdev_add_fail;
    }

    // create class
    mailbox_class = class_create(THIS_MODULE, DEVICE_NAME);
    ret = IS_ERR(mailbox_class);
    if(ret)
    {
        printk(KERN_WARNING "sw_mailbox: class create failed\n");
        goto class_create_fail;
    }

    // create device
    mailbox_device = device_create(mailbox_class, NULL, devno, NULL, DEVICE_NAME);
    ret = IS_ERR(mailbox_device);
    if(ret)
    {
        printk(KERN_WARNING "sw_mailbox: mailbox device create failed, error code %ld \n", PTR_ERR(mailbox_device));
        goto device_create_fail;
    } 
    else 
    {
        printk("sw_mailbox: succeed to create /dev/%s \n",DEVICE_NAME);
    }
    //create kfifo
    ret = kfifo_alloc(&mailbox_fifo, FIFO_SIZE, GFP_KERNEL); 
    if(ret)
    {
        printk(KERN_ERR "sw_mailbox: error kfifo_alloc\n");
        return ret;
    }
    printk("sw_mailbox: kfifo_size = %#x\n",kfifo_size(&mailbox_fifo));

    
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
