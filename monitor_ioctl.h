#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define CONTAINER_MONITOR_MAGIC 'c'

struct container_info {
    pid_t pid;
    unsigned int soft_mib;
    unsigned int hard_mib;
};

// ioctls
#define IOCTL_REGISTER_CONTAINER _IOW(CONTAINER_MONITOR_MAGIC, 1, struct container_info)
#define IOCTL_UNREGISTER_CONTAINER _IOW(CONTAINER_MONITOR_MAGIC, 2, pid_t)

#endif
