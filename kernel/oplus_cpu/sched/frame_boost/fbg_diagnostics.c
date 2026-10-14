// SPDX-License-Identifier: GPL-2.0-only
/* Read-only integration diagnostics for the duchamp Oplus port. */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/cputime.h>
#include <kernel/sched/sched.h>
#include "frame_boost.h"
#include "frame_ioctl.h"
#include "../sched_assist/sa_common.h"

extern struct frame_group *frame_boost_groups[MAX_NUM_FBG_ID];

static int fbg_status_show(struct seq_file *m, void *unused)
{
	unsigned int total = 0;
	u64 enqueues, dequeues;
	int i;

	seq_puts(m, "port_diagnostics=5\n");
	seq_printf(m, "sched_assist_ready=%d\nsched_assist_enabled=%d\nscene=%d\n",
		oplus_sched_assist_ready(), READ_ONCE(global_sched_assist_enabled),
		READ_ONCE(global_sched_assist_scene));
	oplus_sa_ux_counters(&enqueues, &dequeues);
	seq_printf(m, "sa_ux_enqueue_events=%llu\nsa_ux_dequeue_events=%llu\n",
		enqueues, dequeues);
	seq_printf(m, "frame_boost_enabled=%d\noplus_frequency_callback_registered=%d\n",
		frame_boost_enabled(), !!READ_ONCE(fbg_hook.update_freq));
	seq_puts(m, "frequency_fallback=cpufreq_update_util\n");
	fbg_ioctl_diagnostics(m);
	seq_printf(m, "clusters=%d\n", num_sched_clusters);
	for (i = 0; i < num_sched_clusters && i < MAX_CLS_NUM; i++) {
		struct oplus_sched_cluster *cluster = fb_cluster[i];
		int cpu;

		if (!cluster)
			continue;
		cpu = cpumask_first(&cluster->cpus);
		seq_printf(m, "cluster=%d cpus=%*pbl arch_capacity=%lu rq_capacity=%lu\n",
			cluster->id, cpumask_pr_args(&cluster->cpus),
			arch_scale_cpu_capacity(cpu), capacity_orig_of(cpu));
	}
	for (i = 1; i < MAX_NUM_FBG_ID; i++) {
		struct frame_group *grp = frame_boost_groups[i];
		struct oplus_task_struct *ots;
		unsigned long flags;
		unsigned int tasks = 0;
		int ui, render, running, preferred, available;
		u64 window;

		if (!grp)
			continue;
		raw_spin_lock_irqsave(&grp->lock, flags);
		list_for_each_entry(ots, &grp->tasks, fbg_list)
			tasks++;
		ui = grp->ui_pid;
		render = grp->render_pid;
		running = grp->nr_running;
		window = grp->window_size;
		preferred = grp->preferred_cluster ? grp->preferred_cluster->id : -1;
		available = grp->available_cluster ? grp->available_cluster->id : -1;
		raw_spin_unlock_irqrestore(&grp->lock, flags);
		total += tasks;
		seq_printf(m, "group=%d tasks=%u ui=%d render=%d running=%d window_ns=%llu policy_util=%lld preferred=%d available=%d\n",
			i, tasks, ui, render, running, window,
			atomic64_read(&grp->policy_util), preferred, available);
	}
	seq_printf(m, "total_group_tasks=%u\n", total);
	return 0;
}

static int fbg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, fbg_status_show, NULL);
}

static const struct proc_ops fbg_status_ops = {
	.proc_open = fbg_status_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int fbg_diagnostics_init(void)
{
	return proc_create("duchamp_oplus_status", 0400, NULL, &fbg_status_ops) ?
		0 : -ENOMEM;
}
