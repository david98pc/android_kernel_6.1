// SPDX-License-Identifier: GPL-2.0-only
/* GKI adaptation: use standard QoS for both bounds, with policy lifetime pins.
 * No vendor callback or policy-index array is needed for the frequency limits.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "cpufreq_limits.h"
#include "early_detect.h"

struct game_policy {
	struct cpufreq_policy *policy;
	struct freq_qos_request min_req, max_req;
	unsigned int floor, ceiling;
};
static struct game_policy limits[NR_CPUS];
static DEFINE_MUTEX(limits_lock);
static struct delayed_work init_work, expiry_work;
static bool ready, enabled;
static unsigned int boost_type;
static atomic64_t requests = ATOMIC64_INIT(0);
oplus_game_update_cpu_freq_cb_t oplus_game_update_cpu_freq_cb;
EXPORT_SYMBOL_GPL(oplus_game_update_cpu_freq_cb);

/* Kept for donor link compatibility. The port applies QoS directly instead. */
int oplus_game_get_max_freq(int policy) { return FREQ_QOS_MIN_DEFAULT_VALUE; }
EXPORT_SYMBOL_GPL(oplus_game_get_max_freq);

static void apply_limits(void)
{
	int cpu;
	bool active = ready && enabled && boost_type;
	for_each_possible_cpu(cpu) {
		struct game_policy *p = &limits[cpu];
		unsigned int floor, ceiling;
		if (!p->policy) continue;
		ceiling = active && p->ceiling ? p->ceiling : FREQ_QOS_MAX_DEFAULT_VALUE;
		floor = active ? min(p->floor, p->policy->cpuinfo.max_freq) : 0;
		floor = min(floor, ceiling);
		freq_qos_update_request(&p->max_req, ceiling);
		freq_qos_update_request(&p->min_req, floor);
	}
}
static void expire_limits(struct work_struct *work)
{
	mutex_lock(&limits_lock);
	boost_type = 0;
	apply_limits();
	mutex_unlock(&limits_lock);
}
void do_ed_freq_boost_request(unsigned int type)
{
	mutex_lock(&limits_lock);
	boost_type = type & ED_BOOST_EDB;
	apply_limits();
	if (boost_type) {
		atomic64_inc(&requests);
		mod_delayed_work(system_wq, &expiry_work, 2 * HZ);
	}
	mutex_unlock(&limits_lock);
}
static int limits_show(struct seq_file *m, void *v)
{
	int cpu;
	bool is_max = (unsigned long)m->private;
	mutex_lock(&limits_lock);
	for_each_possible_cpu(cpu) {
		int first = cpu;
		struct cpufreq_policy *p = cpufreq_cpu_get(cpu);
		if (p) { first = cpumask_first(p->related_cpus); cpufreq_cpu_put(p); }
		seq_printf(m, "%d:%u ", cpu, is_max ? limits[first].ceiling : limits[first].floor);
	}
	seq_putc(m, '\n');
	mutex_unlock(&limits_lock);
	return 0;
}
static int limits_open(struct inode *inode, struct file *file)
{
	return single_open(file, limits_show, pde_data(inode));
}
static ssize_t limits_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char *page, *cursor, *token;
	unsigned int *pending;
	struct cpumask changed;
	int ret = 0, cpu;
	bool is_max = (unsigned long)pde_data(file_inode(file));
	if (!count || count > 1024) return -EINVAL;
	page = memdup_user_nul(buf, count);
	if (IS_ERR(page)) return PTR_ERR(page);
	pending = kcalloc(nr_cpu_ids, sizeof(*pending), GFP_KERNEL);
	if (!pending) { kfree(page); return -ENOMEM; }
	cpumask_clear(&changed);
	mutex_lock(&limits_lock);
	if (!ready) { ret = -EAGAIN; goto out; }
	cursor = strim(page);
	while ((token = strsep(&cursor, " \t\n"))) {
		unsigned int id, value;
		char tail;
		struct cpufreq_policy *policy;
		int first;
		if (!*token) continue;
		if (sscanf(token, "%u:%u%c", &id, &value, &tail) != 2 ||
		    id >= nr_cpu_ids || !cpu_possible(id)) { ret = -EINVAL; goto out; }
		policy = cpufreq_cpu_get(id);
		if (!policy) { ret = -EAGAIN; goto out; }
		first = cpumask_first(policy->related_cpus);
		if (value > policy->cpuinfo.max_freq && !(is_max && value == FREQ_QOS_MAX_DEFAULT_VALUE))
			ret = -ERANGE;
		cpufreq_cpu_put(policy);
		if (ret) goto out;
		if (cpumask_test_cpu(first, &changed) && pending[first] != value) { ret = -EINVAL; goto out; }
		pending[first] = value;
		cpumask_set_cpu(first, &changed);
	}
	if (cpumask_empty(&changed)) { ret = -EINVAL; goto out; }
	for_each_cpu(cpu, &changed) {
		if (is_max) limits[cpu].ceiling = pending[cpu];
		else limits[cpu].floor = pending[cpu];
	}
	apply_limits();
out:
	mutex_unlock(&limits_lock);
	kfree(pending);
	kfree(page);
	return ret ? ret : count;
}
static const struct proc_ops limits_ops = {
	.proc_open = limits_open, .proc_read = seq_read, .proc_write = limits_write,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
static int enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enabled=%u ready=%u requests=%lld\n", READ_ONCE(enabled),
		READ_ONCE(ready), atomic64_read(&requests));
	return 0;
}
static int enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, enabled_show, NULL);
}
static ssize_t enabled_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	bool value;
	int ret = kstrtobool_from_user(buf, count, &value);
	if (ret) return ret;
	mutex_lock(&limits_lock);
	enabled = value;
	if (!value) boost_type = 0;
	apply_limits();
	mutex_unlock(&limits_lock);
	return count;
}
static const struct proc_ops enabled_ops = {
	.proc_open = enabled_open, .proc_read = seq_read, .proc_write = enabled_write,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
static void cleanup_limits(void)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct game_policy *p = &limits[cpu];
		if (!p->policy) continue;
		freq_qos_remove_request(&p->max_req);
		freq_qos_remove_request(&p->min_req);
		cpufreq_cpu_put(p->policy);
		p->policy = NULL;
	}
}
static void init_limits(struct work_struct *work)
{
	int cpu, ret = 0;
	struct cpumask seen;
	cpumask_clear(&seen);
	mutex_lock(&limits_lock);
	for_each_possible_cpu(cpu) {
		struct game_policy *p = &limits[cpu];
		struct cpufreq_policy *policy;
		if (cpumask_test_cpu(cpu, &seen)) continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy) { ret = -EAGAIN; break; }
		ret = freq_qos_add_request(&policy->constraints, &p->min_req, FREQ_QOS_MIN, 0);
		if (ret < 0) { cpufreq_cpu_put(policy); break; }
		ret = freq_qos_add_request(&policy->constraints, &p->max_req, FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) { freq_qos_remove_request(&p->min_req); cpufreq_cpu_put(policy); break; }
		p->policy = policy;
		cpumask_or(&seen, &seen, policy->related_cpus);
	}
	if (ret < 0) {
		cleanup_limits();
		mod_delayed_work(system_wq, &init_work, 5 * HZ);
	} else {
		ready = true;
		pr_info("duchamp-cpu-game: QoS ready, boost disabled\n");
	}
	mutex_unlock(&limits_lock);
}
int cpufreq_limits_init(void)
{
	INIT_DELAYED_WORK(&init_work, init_limits);
	INIT_DELAYED_WORK(&expiry_work, expire_limits);
	if (!early_detect_dir) return -ENODEV;
	if (!proc_create_data("edb_cpu_min_freq", 0664, early_detect_dir, &limits_ops, NULL) ||
	    !proc_create_data("edb_cpu_max_freq", 0664, early_detect_dir, &limits_ops, (void *)1UL) ||
	    !proc_create("port_enabled", 0600, early_detect_dir, &enabled_ops)) return -ENOMEM;
	schedule_delayed_work(&init_work, HZ);
	return 0;
}
/* This adaptation is built-in. Keep an exit for source compatibility only. */
void cpufreq_limits_exit(void)
{
	cancel_delayed_work_sync(&init_work);
	cancel_delayed_work_sync(&expiry_work);
	mutex_lock(&limits_lock);
	cleanup_limits();
	mutex_unlock(&limits_lock);
}
