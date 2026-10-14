// SPDX-License-Identifier: GPL-2.0-only
/* Duchamp adaptation of the Oplus input-driven CPU frequency floor.
 * Policies are discovered dynamically; updates run in an ordered workqueue.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct touch_policy {
	struct kobject kobj;
	struct cpufreq_policy *policy;
	struct freq_qos_request qos;
	unsigned int floor;
};
static struct touch_policy *policies[NR_CPUS];
static DEFINE_MUTEX(touch_lock);
static struct workqueue_struct *touch_wq;
static struct work_struct boost_work;
static struct delayed_work release_work, init_work;
static struct kobject *control_kobj;
static bool enabled, ready;
static unsigned int duration_ms = 100;
static atomic_t input_devices = ATOMIC_INIT(0);
static atomic64_t events = ATOMIC64_INIT(0), boosts = ATOMIC64_INIT(0);
static atomic64_t last_event = ATOMIC64_INIT(0);

static void release_floors(void)
{
	int cpu;
	for_each_possible_cpu(cpu)
		if (policies[cpu])
			freq_qos_update_request(&policies[cpu]->qos, FREQ_QOS_MIN_DEFAULT_VALUE);
}
static void touch_release(struct work_struct *work)
{
	mutex_lock(&touch_lock);
	release_floors();
	mutex_unlock(&touch_lock);
}
static void touch_apply(struct work_struct *work)
{
	int cpu;
	mutex_lock(&touch_lock);
	if (!enabled || !ready)
		goto out;
	for_each_possible_cpu(cpu) {
		struct touch_policy *p = policies[cpu];
		if (p)
			freq_qos_update_request(&p->qos, min(p->floor, p->policy->cpuinfo.max_freq));
	}
	atomic64_inc(&boosts);
	mod_delayed_work(touch_wq, &release_work, msecs_to_jiffies(duration_ms));
out:
	mutex_unlock(&touch_lock);
}
static ssize_t touch_boost_freq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct touch_policy *p = container_of(kobj, struct touch_policy, kobj);
	return sysfs_emit(buf, "%u\n", READ_ONCE(p->floor));
}
static ssize_t touch_boost_freq_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct touch_policy *p = container_of(kobj, struct touch_policy, kobj);
	unsigned int value;
	if (kstrtouint(buf, 0, &value) || value > p->policy->cpuinfo.max_freq)
		return -EINVAL;
	mutex_lock(&touch_lock);
	p->floor = value;
	mutex_unlock(&touch_lock);
	return count;
}
static ssize_t touch_boost_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(duration_ms));
}
static ssize_t touch_boost_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned int value;
	if (kstrtouint(buf, 0, &value) || !value || value > 500)
		return -EINVAL;
	mutex_lock(&touch_lock);
	duration_ms = value;
	mutex_unlock(&touch_lock);
	return count;
}
static struct kobj_attribute floor_attr = __ATTR(touch_boost_freq, 0644, touch_boost_freq_show, touch_boost_freq_store);
static struct kobj_attribute duration_attr = __ATTR(touch_boost_ms, 0644, touch_boost_ms_show, touch_boost_ms_store);
static struct attribute *touch_attrs[] = { &floor_attr.attr, &duration_attr.attr, NULL };
ATTRIBUTE_GROUPS(touch);
static void touch_policy_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct touch_policy, kobj));
}
static const struct kobj_type touch_type = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = touch_policy_release,
	.default_groups = touch_groups,
};
static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(enabled));
}
static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	bool value;
	if (kstrtobool(buf, &value))
		return -EINVAL;
	mutex_lock(&touch_lock);
	if (value && !ready) {
		mutex_unlock(&touch_lock);
		return -EAGAIN;
	}
	WRITE_ONCE(enabled, value);
	if (!value)
		release_floors();
	mutex_unlock(&touch_lock);
	return count;
}
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "ready=%u enabled=%u devices=%d events=%lld boosts=%lld\n",
		READ_ONCE(ready), READ_ONCE(enabled), atomic_read(&input_devices),
		atomic64_read(&events), atomic64_read(&boosts));
}
static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute status_attr = __ATTR_RO(status);
static struct attribute *control_attrs[] = { &enabled_attr.attr, &status_attr.attr, NULL };
static const struct attribute_group control_group = { .attrs = control_attrs };

static void cleanup_policies(void)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct touch_policy *p = policies[cpu];
		if (!p)
			continue;
		freq_qos_remove_request(&p->qos);
		cpufreq_cpu_put(p->policy);
		policies[cpu] = NULL;
		kobject_put(&p->kobj);
	}
}
static void discover_policies(struct work_struct *work)
{
	struct cpumask seen;
	int cpu, ret = 0;
	mutex_lock(&touch_lock);
	cpumask_clear(&seen);
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct touch_policy *p;
		struct device *dev;
		if (cpumask_test_cpu(cpu, &seen))
			continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy) { ret = -EAGAIN; break; }
		dev = get_cpu_device(cpumask_first(policy->related_cpus));
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p || !dev) { kfree(p); cpufreq_cpu_put(policy); ret = -ENOMEM; break; }
		p->policy = policy;
		ret = freq_qos_add_request(&policy->constraints, &p->qos,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) { kfree(p); cpufreq_cpu_put(policy); break; }
		ret = kobject_init_and_add(&p->kobj, &touch_type, &dev->kobj, "duchamp_touch_boost");
		if (ret) {
			freq_qos_remove_request(&p->qos);
			cpufreq_cpu_put(policy);
			kobject_put(&p->kobj);
			break;
		}
		policies[cpu] = p;
		cpumask_or(&seen, &seen, policy->related_cpus);
	}
	if (ret < 0) {
		cleanup_policies();
		mod_delayed_work(touch_wq, &init_work, 5 * HZ);
	} else {
		WRITE_ONCE(ready, true);
		pr_info("duchamp-touch: policies ready, boost disabled\n");
	}
	mutex_unlock(&touch_lock);
}
static void touch_event(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
	s64 now, prev;
	if (type != EV_SYN || code != SYN_REPORT)
		return;
	atomic64_inc(&events);
	if (!READ_ONCE(enabled) || !READ_ONCE(ready))
		return;
	now = ktime_get_ns();
	prev = atomic64_read(&last_event);
	if (now - prev < 150 * NSEC_PER_MSEC || atomic64_cmpxchg(&last_event, prev, now) != prev)
		return;
	queue_work(touch_wq, &boost_work);
}
static int touch_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "duchamp-oplus-touch";
	ret = input_register_handle(handle);
	if (ret) goto free;
	ret = input_open_device(handle);
	if (ret) { input_unregister_handle(handle); goto free; }
	atomic_inc(&input_devices);
	pr_info("duchamp-touch: matched %s\n", dev->name);
	return 0;
free:
	kfree(handle);
	return ret;
}
static void touch_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	atomic_dec(&input_devices);
	kfree(handle);
}
static bool touch_match(struct input_handler *handler, struct input_dev *dev)
{
	return test_bit(INPUT_PROP_DIRECT, dev->propbit);
}
static const struct input_device_id touch_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
	  .evbit = { BIT_MASK(EV_ABS) },
	  .absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) } },
	{ }
};
static struct input_handler touch_handler = {
	.event = touch_event, .connect = touch_connect, .disconnect = touch_disconnect,
	.match = touch_match, .name = "duchamp-oplus-touch", .id_table = touch_ids,
};
static int __init duchamp_touch_init(void)
{
	int ret;
	touch_wq = alloc_ordered_workqueue("duchamp-touch", WQ_MEM_RECLAIM);
	if (!touch_wq) return -ENOMEM;
	INIT_WORK(&boost_work, touch_apply);
	INIT_DELAYED_WORK(&release_work, touch_release);
	INIT_DELAYED_WORK(&init_work, discover_policies);
	control_kobj = kobject_create_and_add("duchamp_touch_boost", kernel_kobj);
	if (!control_kobj) { ret = -ENOMEM; goto destroy; }
	ret = sysfs_create_group(control_kobj, &control_group);
	if (ret) goto put;
	ret = input_register_handler(&touch_handler);
	if (ret) goto put;
	queue_delayed_work(touch_wq, &init_work, HZ);
	return 0;
put:
	kobject_put(control_kobj);
destroy:
	destroy_workqueue(touch_wq);
	return ret;
}
late_initcall(duchamp_touch_init);
MODULE_LICENSE("GPL");
