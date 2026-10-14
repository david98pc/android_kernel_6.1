// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/sysctl.h>
#include "frame_sync.h"
#include <linux/compat.h>
#include <linux/mutex.h>
#include "game_ctrl.h"
#include "task_boost/heavy_task_boost.h"

#define SYNC_READ_SHIFT 1 << 6
DEFINE_CTL_TABLE_POLL(framesync_poll);

struct proc_dir_entry *framesync_dir = NULL;
static DEFINE_MUTEX(sync_mutex);
struct gameopt_frame_data* read_pointer = NULL;
struct gameopt_frame_data produce_data;
struct gameopt_frame_data consume_data;
struct gameopt_frame_data tl_pred_data;

#include "critical_task_boost.h"

int epoll_notify(struct ctl_table* ro_table, int write, void* buffer, size_t* lenp, loff_t* ppos) {
    return 0;
}

struct ctl_table framesync_table[] = {
    {
        .procname       = "epoll_notify",
        .mode           = 0664,
        .proc_handler   = epoll_notify,
        .poll           = &framesync_poll,
    },
    { }
};

static int notify_wait_fd(void) {
    atomic_inc(&framesync_poll.event);
    wake_up_interruptible(&framesync_poll.wait);
    return 0;
}

static int reset_wait_event_cnt(void) {
    atomic_set(&framesync_poll.event, 0);
    wake_up_interruptible(&framesync_poll.wait);
    return 0;
}

static long sync_ctrl_data(unsigned int nr, const struct gameopt_frame_data *input)
{
    struct gameopt_frame_data data = *input;
    long ret = 0;
    mutex_lock(&sync_mutex);
    switch (nr) {
        case NOTIFY_FRAME_PRODUCE:
            data.mode = NOTIFY_FRAME_PRODUCE;
            produce_data = data;
            read_pointer = &produce_data;
             /* read load data befor wakeup the user space */
            cl_notify_frame_produce();
            fl_notify_frame_produce();
            ret = notify_wait_fd();
            htb_notify_frame_produce();
            ctb_notify_frame_produce();
        break;

        case NOTIFY_FRAME_CONSUME:
            data.mode = NOTIFY_FRAME_CONSUME;
            consume_data = data;
            read_pointer = &consume_data;
            ret = notify_wait_fd();
        break;

        case NOTIFY_FRAME_TLPRED:
            tl_pred_data = data;
            read_pointer = &tl_pred_data;
            /* read load data befor wakeup the user space */
            cl_notify_frame_produce();
            fl_notify_frame_produce();
            ret = notify_wait_fd();
            htb_notify_frame_produce();
            ctb_notify_frame_produce();
        break;

        default:
            ret = reset_wait_event_cnt();
        break;
    }

    mutex_unlock(&sync_mutex);
    return ret;
}
static long sync_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct gameopt_frame_data data;
    if (_IOC_TYPE(cmd) != GAMEOPT_EPOLL_MAGIC || _IOC_NR(cmd) >= NOTIFY_FRAME_MAX_ID ||
        _IOC_SIZE(cmd) != sizeof(data) || _IOC_DIR(cmd) != (_IOC_READ | _IOC_WRITE))
        return -EINVAL;
    if (copy_from_user(&data, (void __user *)arg, sizeof(data))) return -EFAULT;
    return sync_ctrl_data(_IOC_NR(cmd), &data);
}

#if IS_ENABLED(CONFIG_COMPAT)
struct compat_gameopt_frame_data {
    s32 mode, bufferN;
    compat_long_t timeStamp1, timeStamp2;
    char reserved[128];
};
static long compat_sync_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct compat_gameopt_frame_data compat;
    struct gameopt_frame_data data = {};
    if (_IOC_TYPE(cmd) != GAMEOPT_EPOLL_MAGIC || _IOC_NR(cmd) >= NOTIFY_FRAME_MAX_ID ||
        _IOC_SIZE(cmd) != sizeof(compat) || _IOC_DIR(cmd) != (_IOC_READ | _IOC_WRITE))
        return -EINVAL;
    if (copy_from_user(&compat, compat_ptr(arg), sizeof(compat))) return -EFAULT;
    data.mode = compat.mode; data.bufferN = compat.bufferN;
    data.timeStamp1 = compat.timeStamp1; data.timeStamp2 = compat.timeStamp2;
    memcpy(data.reserved, compat.reserved, sizeof(data.reserved));
    return sync_ctrl_data(_IOC_NR(cmd), &data);
}
#endif /* CONFIG_COMPAT */

static int sync_ctrl_open(struct inode *inode, struct file *file) {
    return 0;
}

static int sync_ctrl_release(struct inode *inode, struct file *file) {
	return 0;
}

static int produce_read(struct seq_file *m, void *v) {
    mutex_lock(&sync_mutex);
    if (read_pointer != NULL) {
        seq_printf(m, "%d;%ld\n", read_pointer->bufferN, read_pointer->timeStamp1);
    }
    mutex_unlock(&sync_mutex);
    return 0;
}

static int produce_open(struct inode *inode, struct file *file) {
    return single_open(file, produce_read, inode);
}

static int consume_read(struct seq_file *m, void *v) {
    mutex_lock(&sync_mutex);
    if (read_pointer != NULL) {
        seq_printf(m, "%d;%ld\n", read_pointer->bufferN, read_pointer->timeStamp1);
    }
    mutex_unlock(&sync_mutex);
    return 0;
}

static int consume_open(struct inode *inode, struct file *file) {
    return single_open(file, consume_read, inode);
}

static int tl_pred_read(struct seq_file *m, void *v) {
    mutex_lock(&sync_mutex);
    if (read_pointer != NULL) {
        seq_printf(m, "%ld:%ld:%d:%d\n", read_pointer->timeStamp1, read_pointer->timeStamp2, read_pointer->bufferN, read_pointer->mode);
    }
    mutex_unlock(&sync_mutex);
    return 0;
}

static int tl_pred_open(struct inode *inode, struct file *file) {
    return single_open(file, tl_pred_read, inode);
}

static const struct proc_ops sync_ctrl_proc_ops = {
    .proc_ioctl     = sync_ctrl_ioctl,
    .proc_open      = sync_ctrl_open,
    .proc_release   = sync_ctrl_release,
#if IS_ENABLED(CONFIG_COMPAT)
    .proc_compat_ioctl	= compat_sync_ctrl_ioctl,
#endif /* CONFIG_COMPAT */
    .proc_lseek     = default_llseek,
};

static const struct proc_ops produce_proc_ops = {
    .proc_open      = produce_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static const struct proc_ops consume_proc_ops = {
    .proc_open      = consume_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

static const struct proc_ops tl_pred_proc_ops = {
    .proc_open      = tl_pred_open,
    .proc_read		= seq_read,
    .proc_lseek		= seq_lseek,
    .proc_release	= single_release,
};

int frame_sync_init(void) {
    struct ctl_table_header *hdr;
    if (unlikely(!game_opt_dir)) {
        return -ENOTDIR;
    }

    framesync_dir = proc_mkdir("frame_sync", game_opt_dir);

    proc_create_data("sync_ctrl", 0664, framesync_dir, &sync_ctrl_proc_ops, NULL);
    proc_create_data("produce", 0664, framesync_dir, &produce_proc_ops, NULL);
    proc_create_data("consume", 0664, framesync_dir, &consume_proc_ops, NULL);
    proc_create_data("tl_pred", 0664, framesync_dir, &tl_pred_proc_ops, NULL);

    hdr = register_sysctl("game_frame_sync", framesync_table);

    return 0;
}
