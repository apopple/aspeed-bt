#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#define DEVICE_NAME	"bt"
#define BT_NUM_DEVS	1

#define BT_CTRL		0
#define   BT_CTRL_B_BUSY		0x80
#define   BT_CTRL_H_BUSY		0x40
#define   BT_CTRL_OEM0			0x20
#define   BT_CTRL_SMS_ATN		0x10
#define   BT_CTRL_B2H_ATN		0x08
#define   BT_CTRL_H2B_ATN		0x04
#define   BT_CTRL_CLR_RD_PTR		0x02
#define   BT_CTRL_CLR_WR_PTR		0x01
#define BT_BMC2HOST	4
#define BT_INTMASK	8
#define   BT_INTMASK_B2H_IRQEN		0x01
#define   BT_INTMASK_B2H_IRQ		0x02
#define   BT_INTMASK_BMC_HWRST		0x80

dev_t bt_host_devt;

struct bt_host {
	struct device dev;
	struct cdev cdev;
	void *base;
	int open_count;
	wait_queue_head_t queue;
	unsigned int ctrl;
	struct timer_list poll_timer;
};

static struct bt_host *bt_host;

static char bt_inb(struct bt_host *bt_host, int reg)
{
	return ioread8(bt_host->base + reg);
}

static void bt_outb(struct bt_host *bt_host, char data, int reg)
{
	iowrite8(data, bt_host->base + reg);
}

static void clr_rd_ptr(struct bt_host *bt_host)
{
	bt_outb(bt_host, BT_CTRL_CLR_RD_PTR, BT_CTRL);
}

static void clr_wr_ptr(struct bt_host *bt_host)
{
	bt_outb(bt_host, BT_CTRL_CLR_WR_PTR, BT_CTRL);
}

static int h2b_atn(struct bt_host *bt_host)
{
	return !!(bt_inb(bt_host, BT_CTRL) & BT_CTRL_H2B_ATN);
}

static void clr_h2b_atn(struct bt_host *bt_host)
{
	bt_outb(bt_host, BT_CTRL_H2B_ATN, BT_CTRL);
}

static void set_b_busy(struct bt_host *bt_host)
{
	if (!(bt_inb(bt_host, BT_CTRL) & BT_CTRL_B_BUSY))
		bt_outb(bt_host, BT_CTRL_B_BUSY, BT_CTRL);}

static void clr_b_busy(struct bt_host *bt_host)
{
	if (bt_inb(bt_host, BT_CTRL) & BT_CTRL_B_BUSY)
		bt_outb(bt_host, BT_CTRL_B_BUSY, BT_CTRL);
}

static void set_b2h_atn(struct bt_host *bt_host)
{
	bt_outb(bt_host, BT_CTRL_B2H_ATN, BT_CTRL);
}

static int b2h_atn(struct bt_host *bt_host)
{
	return !!(bt_inb(bt_host, BT_CTRL) & BT_CTRL_B2H_ATN);
}

static int h_busy(struct bt_host *bt_host)
{
	return !!(bt_inb(bt_host, BT_CTRL) & BT_CTRL_H_BUSY);
}

static char bt_read(struct bt_host *bt_host)
{
	char result = bt_inb(bt_host, BT_BMC2HOST);

	return result;
}

static void bt_write(struct bt_host *bt_host, char c)
{
	bt_outb(bt_host, c, BT_BMC2HOST);
}

static int bt_host_open(struct inode *inode, struct file *file)
{
	file->private_data = bt_host;

	clr_b_busy(bt_host);

	return 0;
}

static ssize_t bt_host_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	char __user *p = buf;
	char len;

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	if (wait_event_interruptible(bt_host->queue,
				     bt_inb(bt_host, BT_CTRL)
				     & BT_CTRL_H2B_ATN))
		return -ERESTARTSYS;

	set_b_busy(bt_host);
	clr_h2b_atn(bt_host);
	clr_rd_ptr(bt_host);

	len = bt_read(bt_host);
	__put_user(len, p++);

	/* We pass the length back as well */
	if (len + 1 > count)
		len = count - 1;

	while(len) {
		if (__put_user(bt_read(bt_host), p))
			return -EFAULT;
		len--; p++;
	}

	clr_b_busy(bt_host);

	return p - buf;
}

static ssize_t bt_host_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	const char __user *p = buf;
	char c;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	/* There's no interrupt for clearing host busy so we have to
	 * poll */
	if (wait_event_interruptible(bt_host->queue,
				     !(bt_inb(bt_host, BT_CTRL) &
				       (BT_CTRL_H_BUSY | BT_CTRL_B2H_ATN))))
	    return -ERESTARTSYS;

	clr_wr_ptr(bt_host);

	while (count) {
		if (__get_user(c, p))
			return -EFAULT;

		bt_write(bt_host, c);
		count--; p++;
	}

	set_b2h_atn(bt_host);

	return p - buf;
}

static int bt_host_release(struct inode *inode, struct file *file)
{
       set_b_busy(bt_host);

       return 0;
}

static unsigned int bt_host_poll(struct file *file, poll_table *wait)
{
	uint8_t ctrl = bt_inb(bt_host, BT_CTRL);
	unsigned int mask = 0;

	poll_wait(file, &bt_host->queue, wait);

	if (ctrl & BT_CTRL_H2B_ATN)
		mask |= POLLIN;

	if (!(ctrl & (BT_CTRL_H_BUSY | BT_CTRL_B2H_ATN)))
		mask |= POLLOUT;

	return mask;
}

static const struct file_operations bt_host_fops = {
	.owner		= THIS_MODULE,
	.open		= bt_host_open,
	.read		= bt_host_read,
	.write		= bt_host_write,
	.release	= bt_host_release,
	.poll		= bt_host_poll,
};

static struct miscdevice bt_host_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "bt",
	.fops  		= &bt_host_fops,
};

static void poll_timer(unsigned long data)
{
	bt_host->ctrl = bt_inb(bt_host, BT_CTRL);
	bt_host->poll_timer.expires += msecs_to_jiffies(500);
	wake_up(&bt_host->queue);
	add_timer(&bt_host->poll_timer);
}

static int bt_host_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct resource *res;
	int rc, devno = MKDEV(MAJOR(bt_host_devt), 0);

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	dev = &pdev->dev;
	dev_info(dev, "Found bt host device\n");

	if (bt_host) {
		dev_err(dev, "Multiple bt hosts not supported\n");
		return -ENOMEM;
	}

	bt_host = devm_kzalloc(dev, sizeof(*bt_host), GFP_KERNEL);
	if (!bt_host)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Unable to find resources\n");
		rc = -ENXIO;
		goto out_free;
	}

	bt_host->base = devm_ioremap_nocache(&pdev->dev, res->start,
					     resource_size(res));
	if (!bt_host->base) {
		rc = -ENOMEM;
		goto out_free;
	}

	init_waitqueue_head(&bt_host->queue);

	bt_host_miscdev.parent = dev;
	rc = misc_register(&bt_host_miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		goto out_unmap;
	}

	init_timer(&bt_host->poll_timer);

	bt_host->poll_timer.function = poll_timer;
	bt_host->poll_timer.expires = jiffies + msecs_to_jiffies(10);
	add_timer(&bt_host->poll_timer);

	clr_b_busy(bt_host);

	return 0;

out_unmap:
	devm_iounmap(&pdev->dev, bt_host->base);

out_free:
	devm_kfree(dev, bt_host);
	return rc;

}

static int bt_host_remove(struct platform_device *pdev)
{
	del_timer_sync(&bt_host->poll_timer);
	misc_deregister(&bt_host_miscdev);
	devm_iounmap(&pdev->dev, bt_host->base);
	devm_kfree(&pdev->dev, bt_host);
	bt_host = NULL;

	return 0;
}

static const struct of_device_id bt_host_match[] = {
	{ .compatible = "bt-host" },
	{ },
};

static struct platform_driver bt_host_driver = {
	.driver = {
		.name		= "bt-host",
		.owner		= THIS_MODULE,
		.of_match_table = bt_host_match,
	},
	.probe = bt_host_probe,
	.remove = bt_host_remove,
};

static int __init bt_host_init(void)
{
	int rc;

	rc = alloc_chrdev_region(&bt_host_devt, 0, BT_NUM_DEVS, "bt");
	if (rc) {
		pr_err("bt-host: Could not allocate chardev region\n");
		return rc;
	}

	rc = platform_driver_register(&bt_host_driver);
	if (rc) {
		pr_err("bt-host: Could not register platform device\n");
		goto out_chardev;
	}

	return 0;

out_chardev:
	unregister_chrdev_region(bt_host_devt, BT_NUM_DEVS);
	return rc;
}
module_init(bt_host_init);

static void __exit bt_host_exit(void)
{
	platform_driver_unregister(&bt_host_driver);
}
module_exit(bt_host_exit);

MODULE_DEVICE_TABLE(of, bt_host_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alistair Popple <alistair@popple.id.au>");
MODULE_DESCRIPTION("Linux device interface to the BT interface");
