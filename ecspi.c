/*
  Copyright Scott Ellis, 2011
 
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/moduleparam.h>
#include <linux/hrtimer.h>

#define NANOSECS_PER_SEC 1000000000
#define SPI_BUFF_SIZE	512
#define USER_BUFF_SIZE	128

#define SPI_BUS 1
#define SPI_BUS_CS1 1
#define SPI_BUS_SPEED 12000000

#define DEFAULT_WRITE_FREQUENCY 100
static int write_frequency = DEFAULT_WRITE_FREQUENCY;
module_param(write_frequency, int, S_IRUGO);
MODULE_PARM_DESC(write_frequency, "Test write frequency in Hz");


const char this_driver_name[] = "ecspi";

struct ecspi_control {
	struct spi_message msg;
	struct spi_transfer transfer[2];
	u32 busy;
	u32 spi_callbacks;
	u32 busy_counter;
	u8 *tx_buff; 
};

static struct ecspi_control ecspi_ctl;

struct ecspi_dev {
	spinlock_t spi_lock;
	struct semaphore fop_sem;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct spi_device *spi_device;
	struct hrtimer timer;
	u32 timer_period_sec;
	u32 timer_period_ns;
	u32 running;
	char *user_buff;
};

static struct ecspi_dev ecspi_dev;


static void ecspi_completion_handler(void *arg)
{	
	ecspi_ctl.spi_callbacks++;
	ecspi_ctl.busy = 0;
}

static int ecspi_queue_spi_write(void)
{
	int status, i;
	unsigned long flags;

	spi_message_init(&ecspi_ctl.msg);

	/* this gets called when the spi_message completes */
	ecspi_ctl.msg.complete = ecspi_completion_handler;
	ecspi_ctl.msg.context = NULL;

	/* the data is bogus for initial testing */
	for (i = 0; i < 512; i++)
		ecspi_ctl.tx_buff[i] = i;
		
	ecspi_ctl.transfer[0].tx_buf = ecspi_ctl.tx_buff;
	ecspi_ctl.transfer[0].rx_buf = NULL;
	ecspi_ctl.transfer[0].len = 192;
        /* pulse the cs line between the two transfers */
	ecspi_ctl.transfer[0].cs_change = 1;

	ecspi_ctl.transfer[1].tx_buf = &ecspi_ctl.tx_buff[256];
	ecspi_ctl.transfer[1].rx_buf = NULL;
	ecspi_ctl.transfer[1].len = 192;
	
	spi_message_add_tail(&ecspi_ctl.transfer[0], &ecspi_ctl.msg);
	spi_message_add_tail(&ecspi_ctl.transfer[1], &ecspi_ctl.msg);

	spin_lock_irqsave(&ecspi_dev.spi_lock, flags);

	if (ecspi_dev.spi_device)
		status = spi_async(ecspi_dev.spi_device, &ecspi_ctl.msg);
	else
		status = -ENODEV;

	spin_unlock_irqrestore(&ecspi_dev.spi_lock, flags);
	
	if (status == 0)
		ecspi_ctl.busy = 1;
	
	return status;	
}

static enum hrtimer_restart ecspi_timer_callback(struct hrtimer *timer)
{
	if (!ecspi_dev.running) {
		return HRTIMER_NORESTART;
	}

	/* busy means the previous message has not completed */
	if (ecspi_ctl.busy) {
		ecspi_ctl.busy_counter++;
	}
	else if (ecspi_queue_spi_write() != 0) {
		return HRTIMER_NORESTART;
	}

	/* in the real implementation, this next delay will be variable */
	hrtimer_forward_now(&ecspi_dev.timer, 
		ktime_set(ecspi_dev.timer_period_sec, 
			ecspi_dev.timer_period_ns));
	
	return HRTIMER_RESTART;
}

static ssize_t ecspi_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	ssize_t status = 0;

	if (!buff) 
		return -EFAULT;

	if (*offp > 0) 
		return 0;

	if (down_interruptible(&ecspi_dev.fop_sem)) 
		return -ERESTARTSYS;

	sprintf(ecspi_dev.user_buff, 
			"%s|%u|%u\n",
			ecspi_dev.running ? "Running" : "Stopped",
			ecspi_ctl.spi_callbacks,
			ecspi_ctl.busy_counter);
		
	len = strlen(ecspi_dev.user_buff);
 
	if (len < count) 
		count = len;

	if (copy_to_user(buff, ecspi_dev.user_buff, count))  {
		printk(KERN_ALERT "ecspi_read(): copy_to_user() failed\n");
		status = -EFAULT;
	} else {
		*offp += count;
		status = count;
	}

	up(&ecspi_dev.fop_sem);

	return status;	
}

/*
 * We accept two commands 'start' or 'stop' and ignore anything else.
 */
static ssize_t ecspi_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos)
{
	size_t len;	
	ssize_t status = 0;

	if (down_interruptible(&ecspi_dev.fop_sem))
		return -ERESTARTSYS;

	memset(ecspi_dev.user_buff, 0, 16);
	len = count > 8 ? 8 : count;

	if (copy_from_user(ecspi_dev.user_buff, buff, len)) {
		status = -EFAULT;
		goto ecspi_write_done;
	}

	/* we'll act as if we looked at all the data */
	status = count;

	/* but we only care about the first 5 characters */
	if (!strnicmp(ecspi_dev.user_buff, "start", 5)) {
		if (ecspi_dev.running) {
			printk(KERN_ALERT "already running\n");
			goto ecspi_write_done;
		}

		if (ecspi_ctl.busy) {
			printk(KERN_ALERT "waiting on a spi transaction\n");
			goto ecspi_write_done;
		}

		ecspi_ctl.spi_callbacks = 0;		
		ecspi_ctl.busy_counter = 0;

		hrtimer_start(&ecspi_dev.timer, 
				ktime_set(ecspi_dev.timer_period_sec, 
					ecspi_dev.timer_period_ns),
        	               	HRTIMER_MODE_REL);

		ecspi_dev.running = 1; 
	} 
	else if (!strnicmp(ecspi_dev.user_buff, "stop", 4)) {
		hrtimer_cancel(&ecspi_dev.timer);
		ecspi_dev.running = 0;
	}
	/* The real implementation will also accept the raw data with
	   delay intervals via this write or maybe from an ioctl. TBD.
	*/

ecspi_write_done:

	up(&ecspi_dev.fop_sem);

	return status;
}

static int ecspi_open(struct inode *inode, struct file *filp)
{	
	int status = 0;

	if (down_interruptible(&ecspi_dev.fop_sem)) 
		return -ERESTARTSYS;

	if (!ecspi_dev.user_buff) {
		ecspi_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!ecspi_dev.user_buff) 
			status = -ENOMEM;
	}	

	up(&ecspi_dev.fop_sem);

	return status;
}

static int ecspi_probe(struct spi_device *spi_device)
{
	unsigned long flags;

	spin_lock_irqsave(&ecspi_dev.spi_lock, flags);
	ecspi_dev.spi_device = spi_device;
	spin_unlock_irqrestore(&ecspi_dev.spi_lock, flags);

	return 0;
}

static int ecspi_remove(struct spi_device *spi_device)
{
	unsigned long flags;

	if (ecspi_dev.running) {
		ecspi_dev.running = 0;
		hrtimer_cancel(&ecspi_dev.timer);
	}

	spin_lock_irqsave(&ecspi_dev.spi_lock, flags);
	ecspi_dev.spi_device = NULL;
	spin_unlock_irqrestore(&ecspi_dev.spi_lock, flags);

	return 0;
}

static int __init add_ecspi_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	char buff[64];
	int status = 0;

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master(%d) returned NULL\n",
			SPI_BUS);
		printk(KERN_ALERT "Missing modprobe omap2_mcspi?\n");
		return -1;
	}

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) {
		put_device(&spi_master->dev);
		printk(KERN_ALERT "spi_alloc_device() failed\n");
		return -1;
	}

	spi_device->chip_select = SPI_BUS_CS1;

	/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff, sizeof(buff), "%s.%u", 
			dev_name(&spi_device->master->dev),
			spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
 	if (pdev) {
		/* We are not going to use this spi_device, so free it */ 
		spi_dev_put(spi_device);
		
		/* 
		 * There is already a device configured for this bus.cs  
		 * It is okay if it us, otherwise complain and fail.
		 */
		if (pdev->driver && pdev->driver->name && 
				strcmp(this_driver_name, pdev->driver->name)) {
			printk(KERN_ALERT 
				"Driver [%s] already registered for %s\n",
				pdev->driver->name, buff);
			status = -1;
		} 
	} else {
		spi_device->max_speed_hz = SPI_BUS_SPEED;
		spi_device->mode = SPI_MODE_0;
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name, SPI_NAME_SIZE);
		
		status = spi_add_device(spi_device);		
		if (status < 0) {	
			spi_dev_put(spi_device);
			printk(KERN_ALERT "spi_add_device() failed: %d\n", 
				status);		
		}				
	}

	put_device(&spi_master->dev);

	return status;
}

static struct spi_driver ecspi_driver = {
	.driver = {
		.name =	this_driver_name,
		.owner = THIS_MODULE,
	},
	.probe = ecspi_probe,
	.remove = __devexit_p(ecspi_remove),	
};

static int __init ecspi_init_spi(void)
{
	int error;

	ecspi_ctl.tx_buff = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
	if (!ecspi_ctl.tx_buff) {
		error = -ENOMEM;
		goto ecspi_init_error;
	}

	error = spi_register_driver(&ecspi_driver);
	if (error < 0) {
		printk(KERN_ALERT "spi_register_driver() failed %d\n", error);
		goto ecspi_init_error;
	}

	error = add_ecspi_device_to_bus();
	if (error < 0) {
		printk(KERN_ALERT "add_ecspi_to_bus() failed\n");
		spi_unregister_driver(&ecspi_driver);
		goto ecspi_init_error;	
	}

	return 0;

ecspi_init_error:

	if (ecspi_ctl.tx_buff) {
		kfree(ecspi_ctl.tx_buff);
		ecspi_ctl.tx_buff = 0;
	}

	return error;
}

static const struct file_operations ecspi_fops = {
	.owner =	THIS_MODULE,
	.read = 	ecspi_read,
	.write = 	ecspi_write,
	.open =		ecspi_open,	
};

static int __init ecspi_init_cdev(void)
{
	int error;

	ecspi_dev.devt = MKDEV(0, 0);

	error = alloc_chrdev_region(&ecspi_dev.devt, 0, 1, this_driver_name);
	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n", 
			error);
		return -1;
	}

	cdev_init(&ecspi_dev.cdev, &ecspi_fops);
	ecspi_dev.cdev.owner = THIS_MODULE;
	
	error = cdev_add(&ecspi_dev.cdev, ecspi_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(ecspi_dev.devt, 1);
		return -1;
	}	

	return 0;
}

static int __init ecspi_init_class(void)
{
	ecspi_dev.class = class_create(THIS_MODULE, this_driver_name);

	if (!ecspi_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(ecspi_dev.class, NULL, ecspi_dev.devt, NULL, 	
			this_driver_name)) {
		printk(KERN_ALERT "device_create(..., %s) failed\n",
			this_driver_name);
		class_destroy(ecspi_dev.class);
		return -1;
	}

	return 0;
}

static int __init ecspi_init(void)
{
	memset(&ecspi_dev, 0, sizeof(ecspi_dev));
	memset(&ecspi_ctl, 0, sizeof(ecspi_ctl));

	spin_lock_init(&ecspi_dev.spi_lock);
	sema_init(&ecspi_dev.fop_sem, 1);
	
	if (ecspi_init_cdev() < 0) 
		goto fail_1;
	
	if (ecspi_init_class() < 0)  
		goto fail_2;

	if (ecspi_init_spi() < 0) 
		goto fail_3;

	/* enforce some range to the write frequency, this is arbitrary */
	if (write_frequency < 1 || write_frequency > 10000) {
		printk(KERN_ALERT "write_frequency reset to %d", 
			DEFAULT_WRITE_FREQUENCY);

		write_frequency = DEFAULT_WRITE_FREQUENCY;
	}

	if (write_frequency == 1)
		ecspi_dev.timer_period_sec = 1;
	else
		ecspi_dev.timer_period_ns = NANOSECS_PER_SEC / write_frequency; 

	hrtimer_init(&ecspi_dev.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ecspi_dev.timer.function = ecspi_timer_callback;
	/* leave the ecspi_dev.timer.data field = NULL, not needed */

	return 0;

fail_3:
	device_destroy(ecspi_dev.class, ecspi_dev.devt);
	class_destroy(ecspi_dev.class);

fail_2:
	cdev_del(&ecspi_dev.cdev);
	unregister_chrdev_region(ecspi_dev.devt, 1);

fail_1:
	return -1;
}
module_init(ecspi_init);

static void __exit ecspi_exit(void)
{
	if (ecspi_dev.running) {
		ecspi_dev.running = 0;
		hrtimer_cancel(&ecspi_dev.timer);
	}
	
	spi_unregister_driver(&ecspi_driver);

	device_destroy(ecspi_dev.class, ecspi_dev.devt);
	class_destroy(ecspi_dev.class);

	cdev_del(&ecspi_dev.cdev);
	unregister_chrdev_region(ecspi_dev.devt, 1);

	if (ecspi_ctl.tx_buff)
		kfree(ecspi_ctl.tx_buff);

	if (ecspi_dev.user_buff)
		kfree(ecspi_dev.user_buff);
}
module_exit(ecspi_exit);

MODULE_AUTHOR("Scott Ellis");
MODULE_DESCRIPTION("ecspi module for Elias Crespin kinetic art");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

