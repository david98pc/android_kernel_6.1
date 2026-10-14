/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DUCHAMP_GAME_COMPAT_H
#define _DUCHAMP_GAME_COMPAT_H
#include <linux/proc_fs.h>
#include <linux/uidgid.h>
static inline struct proc_dir_entry *duchamp_game_proc_create_data(const char *name,
	umode_t mode, struct proc_dir_entry *parent, const struct proc_ops *ops, void *data)
{
	struct proc_dir_entry *entry = proc_create_data(name, mode, parent, ops, data);
	if (entry && (mode & 0222))
		proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return entry;
}
#define proc_create_data duchamp_game_proc_create_data
#endif
