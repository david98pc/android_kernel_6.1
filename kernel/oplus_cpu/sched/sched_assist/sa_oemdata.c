// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2022 Oplus. All rights reserved.
 * Duchamp compatibility: private per-task data, independent of OEM slots.
 */
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/sched/cputime.h>
#include <linux/hashtable.h>
#include <linux/rcupdate.h>
#include <kernel/sched/sched.h>
#include "sa_oemdata.h"
#include "sched_assist.h"
#include "sa_common.h"
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
#include "sa_balance.h"
#endif

struct kmem_cache *oplus_task_struct_cachep;
EXPORT_SYMBOL(oplus_task_struct_cachep);
static DEFINE_HASHTABLE(duchamp_task_state, 12);
static DEFINE_RAW_SPINLOCK(duchamp_task_state_lock);
static void init_oplus_task_struct(void *ptr);

/* Entries stay alive until task destruction and a subsequent RCU grace period.
 * The caller must hold a task reference or the usual tasklist/RCU protection.
 */
struct oplus_task_struct *get_oplus_task_struct(struct task_struct *tsk)
{
	struct oplus_task_struct *ots, *found = NULL;

	if (!tsk)
		return NULL;
	rcu_read_lock();
	hash_for_each_possible_rcu(duchamp_task_state, ots,
				  duchamp_hash_node, (unsigned long)tsk) {
		if (READ_ONCE(ots->task) == tsk) {
			found = ots;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}
EXPORT_SYMBOL_GPL(get_oplus_task_struct);

static int attach_task_state(struct task_struct *tsk)
{
	struct oplus_task_struct *ots, *entry;
	unsigned long flags;

	if (get_oplus_task_struct(tsk))
		return 0;
	ots = kmem_cache_alloc(oplus_task_struct_cachep, GFP_ATOMIC);
	if (!ots)
		return -ENOMEM;
	init_oplus_task_struct(ots);
	ots->task = tsk;
	INIT_HLIST_NODE(&ots->duchamp_hash_node);
	raw_spin_lock_irqsave(&duchamp_task_state_lock, flags);
	hash_for_each_possible(duchamp_task_state, entry,
			       duchamp_hash_node, (unsigned long)tsk) {
		if (entry->task == tsk) {
			raw_spin_unlock_irqrestore(&duchamp_task_state_lock, flags);
			kmem_cache_free(oplus_task_struct_cachep, ots);
			return 0;
		}
	}
	hash_add_rcu(duchamp_task_state, &ots->duchamp_hash_node,
		     (unsigned long)tsk);
	raw_spin_unlock_irqrestore(&duchamp_task_state_lock, flags);
	return 0;
}

static void release_task_state_rcu(struct rcu_head *rcu)
{
	struct oplus_task_struct *ots = container_of(rcu,
				struct oplus_task_struct, duchamp_rcu);
	kmem_cache_free(oplus_task_struct_cachep, ots);
}

void android_vh_dup_task_struct_handler(void *unused,
		struct task_struct *tsk, struct task_struct *orig)
{
	if (tsk && orig && attach_task_state(tsk))
		pr_warn_ratelimited("duchamp-oplus: task state allocation failed\n");
}

void android_vh_free_task_handler(void *unused, struct task_struct *tsk)
{
	struct oplus_task_struct *ots;
	unsigned long flags;

	ots = get_oplus_task_struct(tsk);
	if (!ots)
		return;
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
	remove_rt_boost_task(tsk);
#endif
	raw_spin_lock_irqsave(&duchamp_task_state_lock, flags);
	hash_del_rcu(&ots->duchamp_hash_node);
	raw_spin_unlock_irqrestore(&duchamp_task_state_lock, flags);
#ifdef CONFIG_LOCKING_PROTECT
	list_del_init(&ots->locking_entry);
#endif
	list_del_init(&ots->fbg_list);
	call_rcu(&ots->duchamp_rcu, release_task_state_rcu);
}

static void init_oplus_task_struct(void *ptr)
{
	struct oplus_task_struct *ots = ptr;

	memset(ots, 0, sizeof(struct oplus_task_struct));

	RB_CLEAR_NODE(&ots->ux_entry);
	RB_CLEAR_NODE(&ots->exec_time_node);
	INIT_LIST_HEAD(&ots->fbg_list);
	atomic64_set(&ots->inherit_ux, 0);
	ots->ux_priority = -1;
	ots->ux_nice = -1;
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG)
	ots->abnormal_flag = 0;
#endif
#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
	ots->lb_state = 0;
	ots->ld_flag = 0;
#endif
	cpumask_clear(&ots->cpus_requested);
	ots->target_process = -1;
	ots->update_running_start_time = false;
/*#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)*/
	memset(&ots->lkinfo, 0, sizeof(struct locking_info));
	INIT_LIST_HEAD(&ots->lkinfo.node);
/*#endif*/

#ifdef CONFIG_LOCKING_PROTECT
	INIT_LIST_HEAD(&ots->locking_entry);
	ots->locking_start_time = 0;
	ots->locking_depth = 0;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
	/* for loadbalance */
	plist_node_init(&ots->rtb, MAX_IM_FLAG_PRIO);
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_PIPELINE)
	atomic_set(&ots->pipeline_cpu, -1);
#endif


#if IS_ENABLED(CONFIG_ARM64_AMU_EXTN) && IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)
	ots->amu_cycle = 0;
	ots->amu_instruct = 0;
#endif
	raw_spin_lock_init(&ots->fbg_list_entry_lock);
	ots->preferred_cluster_id = -1;
	ots->fbg_depth = -1;
}


static int alloc_existing_task_state(void)
{
	struct task_struct *p, *g;
	int cpu, ret = 0;

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		if (attach_task_state(p)) {
			ret = -ENOMEM;
			break;
		}
	}
	if (!ret) {
		for_each_possible_cpu(cpu) {
			if (attach_task_state(cpu_rq(cpu)->idle)) {
				ret = -ENOMEM;
				break;
			}
		}
	}
	read_unlock(&tasklist_lock);
	return ret;
}

static void unregister_oemdata_hooks(void)
{
	unregister_trace_android_vh_dup_task_struct(
		android_vh_dup_task_struct_handler, NULL);
	unregister_trace_android_vh_free_task(android_vh_free_task_handler, NULL);
	tracepoint_synchronize_unregister();
}

void __maybe_unused sa_oemdata_deinit(void)
{
	struct oplus_task_struct *ots;
	struct hlist_node *tmp;
	unsigned long flags;
	int bucket;

	unregister_oemdata_hooks();
	raw_spin_lock_irqsave(&duchamp_task_state_lock, flags);
	hash_for_each_safe(duchamp_task_state, bucket, tmp, ots, duchamp_hash_node) {
		hash_del_rcu(&ots->duchamp_hash_node);
		call_rcu(&ots->duchamp_rcu, release_task_state_rcu);
	}
	raw_spin_unlock_irqrestore(&duchamp_task_state_lock, flags);
	rcu_barrier();
	kmem_cache_destroy(oplus_task_struct_cachep);
}

int sa_oemdata_init(void)
{
	int ret;

	oplus_task_struct_cachep = kmem_cache_create("duchamp_oplus_task_struct",
		sizeof(struct oplus_task_struct), 0, SLAB_ACCOUNT, NULL);
	if (!oplus_task_struct_cachep)
		return -ENOMEM;
	/* Cover task creation before walking existing tasks, not afterwards. */
	ret = register_trace_android_vh_free_task(android_vh_free_task_handler, NULL);
	if (ret)
		goto destroy_cache;
	ret = register_trace_android_vh_dup_task_struct(
		android_vh_dup_task_struct_handler, NULL);
	if (ret) {
		unregister_trace_android_vh_free_task(android_vh_free_task_handler, NULL);
		tracepoint_synchronize_unregister();
		goto destroy_cache;
	}
	ret = alloc_existing_task_state();
	if (ret) {
		sa_oemdata_deinit();
		return ret;
	}
	pr_info("duchamp-oplus: private task state initialized\n");
	return 0;

destroy_cache:
	kmem_cache_destroy(oplus_task_struct_cachep);
	return ret;
}
