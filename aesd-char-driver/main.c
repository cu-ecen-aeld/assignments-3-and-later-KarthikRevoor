/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"
#include "aesd-circular-buffer.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Dan Walkes");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_read;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (!entry) {
        retval = 0;
        goto out;
    }

    bytes_to_read = entry->size - entry_offset;
    if (bytes_to_read > count)
        bytes_to_read = count;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_read)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_read;
    retval = bytes_to_read;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = count;
    const char *newline_ptr;
    char *new_buff = NULL;
    size_t write_size, new_size;
    struct aesd_buffer_entry entry = {0};

    new_buff = kmalloc(count, GFP_KERNEL);
    if (!new_buff)
        return -ENOMEM;

    if (copy_from_user(new_buff, buf, count)) {
        kfree(new_buff);
        return -EFAULT;
    }

    newline_ptr = memchr(new_buff, '\n', count);

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(new_buff);
        return -ERESTARTSYS;
    }

    if (!newline_ptr) {
        new_size = dev->working_entry.size + count;
        dev->working_entry.buffptr = krealloc(dev->working_entry.buffptr, new_size, GFP_KERNEL);
        if (!dev->working_entry.buffptr) {
            retval = -ENOMEM;
            goto out_unlock;
        }

        memcpy(dev->working_entry.buffptr + dev->working_entry.size, new_buff, count);
        dev->working_entry.size = new_size;
        goto out_unlock;
    }

    write_size = (newline_ptr - new_buff) + 1;
    new_size = dev->working_entry.size + write_size;

    dev->working_entry.buffptr = krealloc(dev->working_entry.buffptr, new_size, GFP_KERNEL);
    if (!dev->working_entry.buffptr) {
        retval = -ENOMEM;
        goto out_unlock;
    }

    memcpy(dev->working_entry.buffptr + dev->working_entry.size, new_buff, write_size);
    dev->working_entry.size = new_size;

    entry.buffptr = dev->working_entry.buffptr;
    entry.size = dev->working_entry.size;
    dev->working_entry.buffptr = NULL;
    dev->working_entry.size = 0;

    if (dev->buffer.full) {
        struct aesd_buffer_entry *oldest = &dev->buffer.entry[dev->buffer.out_offs];
        if (oldest->buffptr)
            kfree(oldest->buffptr);
    }

    aesd_circular_buffer_add_entry(&dev->buffer, &entry);

out_unlock:
    mutex_unlock(&dev->lock);
    kfree(new_buff);
    return retval;
}


loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    size_t total_size = 0;
    uint8_t idx;
    struct aesd_buffer_entry *entry;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, idx) {
        total_size += entry->size;
    }

    newpos = fixed_size_llseek(filp, off, whence, total_size);

    mutex_unlock(&dev->lock);
    return newpos;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    struct aesd_buffer_entry *entry;
    size_t total_offset = 0;
    uint8_t idx, current_idx;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;

    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;

    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
        return -EFAULT;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    uint8_t valid_entries = dev->buffer.full ?
        AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED : dev->buffer.in_offs;

    if (seekto.write_cmd >= valid_entries) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    current_idx = dev->buffer.out_offs;

    for (idx = 0; idx < seekto.write_cmd; idx++) {
        entry = &dev->buffer.entry[current_idx];
        total_offset += entry->size;
        current_idx = (current_idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    entry = &dev->buffer.entry[current_idx];
    if (seekto.write_cmd_offset >= entry->size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    total_offset += seekto.write_cmd_offset;
    filp->f_pos = total_offset;

    mutex_unlock(&dev->lock);
    return 0;
}

struct file_operations aesd_fops = {
    .owner =           THIS_MODULE,
    .read =            aesd_read,
    .write =           aesd_write,
    .open =            aesd_open,
    .release =         aesd_release,
    .llseek =          aesd_llseek,
    .unlocked_ioctl =  aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    struct aesd_buffer_entry *entry;
    uint8_t index;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr)
            kfree(entry->buffptr);
    }
    if (aesd_device.working_entry.buffptr)
        kfree(aesd_device.working_entry.buffptr);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
