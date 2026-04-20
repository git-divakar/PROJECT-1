#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Container Memory Monitor");

struct monitored_container {
    pid_t pid;
    unsigned int soft_mib;
    unsigned int hard_mib;
    struct task_struct *task;
    struct list_head list;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(container_mutex);
static struct class *container_class;
static struct device *container_device;

static int monitor_open(struct inode *inode, struct file *file) { return 0; }
static int monitor_release(struct inode *inode, struct file *file) { return 0; }

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    if (cmd == IOCTL_REGISTER_CONTAINER) {
        struct container_info info;
        if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
            return -EFAULT;

        struct monitored_container *c = kmalloc(sizeof(*c), GFP_KERNEL);
        if (!c) return -ENOMEM;

        c->pid = info.pid;
        c->soft_mib = info.soft_mib;
        c->hard_mib = info.hard_mib;
        c->task = pid_task(find_vpid(info.pid), PIDTYPE_PID);
        INIT_LIST_HEAD(&c->list);

        mutex_lock(&container_mutex);
        list_add_tail(&c->list, &container_list);
        mutex_unlock(&container_mutex);

        printk(KERN_INFO "Monitor: Registered container PID %d\n", info.pid);
        return 0;
    } else if (cmd == IOCTL_UNREGISTER_CONTAINER) {
        pid_t pid;
        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid))) return -EFAULT;

        mutex_lock(&container_mutex);
        struct monitored_container *c, *tmp;
        list_for_each_entry_safe(c, tmp, &container_list, list) {
            if (c->pid == pid) {
                list_del(&c->list);
                kfree(c);
                printk(KERN_INFO "Monitor: Unregistered container PID %d\n", pid);
            }
        }
        mutex_unlock(&container_mutex);
        return 0;
    }
    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = monitor_open,
    .release = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void) {
    int major = register_chrdev(0, "container_monitor", &fops);
    container_class = class_create(THIS_MODULE, "container_monitor");
    container_device = device_create(container_class, NULL, MKDEV(major, 0), NULL, "container_monitor");
    printk(KERN_INFO "Container monitor loaded\n");
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_container *c, *tmp;
    mutex_lock(&container_mutex);
    list_for_each_entry_safe(c, tmp, &container_list, list) {
        list_del(&c->list);
        kfree(c);
    }
    mutex_unlock(&container_mutex);
    device_destroy(container_class, container_device->devt);
    class_destroy(container_class);
    unregister_chrdev(0, "container_monitor");
    printk(KERN_INFO "Container monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
