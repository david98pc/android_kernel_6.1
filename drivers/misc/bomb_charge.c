// SPDX-License-Identifier: GPL-2.0
/*
 * bomb_charge - real MTK charger-class bypass charging control.
 *
 * Exposes bypass_charging on the real "battery"
 * power_supply device, backed entirely by real charger_dev calls
 * (charger_dev_enable, charger_dev_enable_powerpath, cp_* controls).
 * No fake sysfs classes, no OPlus/ColorOS compat nodes, no /proc shims.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>

#define BCG_NAME "bomb_charge"

static int bcg_psy_get_int(const char *psy_name,
			   enum power_supply_property psp,
			   int *out)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	if (!out)
		return -EINVAL;

	psy = power_supply_get_by_name(psy_name);
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, psp, &val);
	power_supply_put(psy);

	if (ret < 0)
		return ret;

	*out = val.intval;
	return 0;
}

/*
 * Minimal power_supply aliases some ROM userspace expects to exist
 * (ac / pc_port / wireless). Backed by real usb/primary_chg online
 * state, not fabricated.
 */
static enum power_supply_property bcg_online_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int bcg_supply_online_get_property(struct power_supply *psy,
					  enum power_supply_property psp,
					  union power_supply_propval *val)
{
	int online = 0;
	int ret;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	ret = bcg_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		ret = bcg_psy_get_int("primary_chg", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		online = 0;

	val->intval = online > 0 ? 1 : 0;
	return 0;
}

static const struct power_supply_desc bcg_ac_power_supply_desc = {
	.name		= "ac",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= bcg_online_props,
	.num_properties	= ARRAY_SIZE(bcg_online_props),
	.get_property	= bcg_supply_online_get_property,
};

static const struct power_supply_desc bcg_pc_port_power_supply_desc = {
	.name		= "pc_port",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= bcg_online_props,
	.num_properties	= ARRAY_SIZE(bcg_online_props),
	.get_property	= bcg_supply_online_get_property,
};

static int bcg_wireless_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = 0;
	return 0;
}

static const struct power_supply_desc bcg_wireless_power_supply_desc = {
	.name		= "wireless",
	.type		= POWER_SUPPLY_TYPE_WIRELESS,
	.properties	= bcg_online_props,
	.num_properties	= ARRAY_SIZE(bcg_online_props),
	.get_property	= bcg_wireless_get_property,
};

static struct power_supply *bcg_ac_psy;
static struct power_supply *bcg_pc_port_psy;
static struct power_supply *bcg_wireless_psy;

static void bcg_register_power_supply_aliases(void)
{
	struct power_supply_config cfg = {};

	bcg_ac_psy = power_supply_register(NULL, &bcg_ac_power_supply_desc, &cfg);
	if (IS_ERR(bcg_ac_psy)) {
		pr_warn(BCG_NAME ": ac power_supply alias failed: %ld\n",
			PTR_ERR(bcg_ac_psy));
		bcg_ac_psy = NULL;
	}

	bcg_pc_port_psy =
		power_supply_register(NULL, &bcg_pc_port_power_supply_desc, &cfg);
	if (IS_ERR(bcg_pc_port_psy)) {
		pr_warn(BCG_NAME ": pc_port power_supply alias failed: %ld\n",
			PTR_ERR(bcg_pc_port_psy));
		bcg_pc_port_psy = NULL;
	}

	bcg_wireless_psy =
		power_supply_register(NULL, &bcg_wireless_power_supply_desc, &cfg);
	if (IS_ERR(bcg_wireless_psy)) {
		pr_warn(BCG_NAME ": wireless power_supply alias failed: %ld\n",
			PTR_ERR(bcg_wireless_psy));
		bcg_wireless_psy = NULL;
	}
}

static void bcg_unregister_power_supply_aliases(void)
{
	if (bcg_wireless_psy) {
		power_supply_unregister(bcg_wireless_psy);
		bcg_wireless_psy = NULL;
	}
	if (bcg_pc_port_psy) {
		power_supply_unregister(bcg_pc_port_psy);
		bcg_pc_port_psy = NULL;
	}
	if (bcg_ac_psy) {
		power_supply_unregister(bcg_ac_psy);
		bcg_ac_psy = NULL;
	}
}

/*
 * Real MTK charger-class backend.
 * Does not use battery/input_suspend (that cuts USB input entirely,
 * which is fake bypass). This only disables/enables charger IC
 * charging while keeping the power path alive.
 */
struct charger_device;

typedef struct charger_device *(*bcg_get_charger_by_name_t)(const char *name);
typedef int (*bcg_charger_dev_enable_t)(struct charger_device *chg_dev, bool en);
typedef int (*bcg_charger_dev_enable_powerpath_t)(struct charger_device *chg_dev, bool en);
typedef int (*bcg_charger_dev_cp_enable_adc_t)(struct charger_device *chg_dev, bool en);

static bcg_get_charger_by_name_t bcg_get_charger_by_name_fn;
static bcg_charger_dev_enable_t bcg_charger_dev_enable_fn;
static bcg_charger_dev_enable_powerpath_t bcg_charger_dev_enable_powerpath_fn;
static bcg_charger_dev_cp_enable_adc_t bcg_charger_dev_cp_enable_adc_fn;

static struct charger_device *bcg_primary_chgdev;
static struct charger_device *bcg_cp_master_chgdev;
static bool bcg_bypass_guard_active;
static bool bcg_real_bypass_cached;
static bool bcg_kp_charger_enable_registered;
static bool bcg_kp_charger_powerpath_registered;
static DEFINE_MUTEX(bcg_state_lock);

static struct kprobe bcg_kp_charger_enable = {
	.symbol_name = "charger_dev_enable",
};
static struct kprobe bcg_kp_charger_powerpath = {
	.symbol_name = "charger_dev_enable_powerpath",
};

#ifdef CONFIG_KPROBES
typedef unsigned long (*bcg_kallsyms_lookup_name_t)(const char *name);

static unsigned long bcg_lookup_symbol_addr(const char *name)
{
	static bcg_kallsyms_lookup_name_t lookup_fn;
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	if (!lookup_fn) {
		ret = register_kprobe(&kp);
		if (ret < 0 || !kp.addr) {
			pr_warn(BCG_NAME ": kallsyms kprobe failed: %d\n", ret);
			return 0;
		}

		lookup_fn = (bcg_kallsyms_lookup_name_t)kp.addr;
		unregister_kprobe(&kp);
	}

	return lookup_fn ? lookup_fn(name) : 0;
}
#else
static unsigned long bcg_lookup_symbol_addr(const char *name)
{
	return 0;
}
#endif

static int bcg_resolve_charger_backend(void)
{
	const char * const names[] = {
		"primary_chg",
		"primary_charger",
		"mtk-master-charger",
		"mt6375-chg",
		"mt6375_chg",
	};
	int i;

	if (!bcg_get_charger_by_name_fn) {
		bcg_get_charger_by_name_fn =
			(bcg_get_charger_by_name_t)bcg_lookup_symbol_addr("get_charger_by_name");
		if (!bcg_get_charger_by_name_fn) {
			pr_warn(BCG_NAME ": get_charger_by_name lookup failed\n");
			return -EPROBE_DEFER;
		}
	}

	if (!bcg_charger_dev_enable_fn) {
		bcg_charger_dev_enable_fn =
			(bcg_charger_dev_enable_t)bcg_lookup_symbol_addr("charger_dev_enable");
		if (!bcg_charger_dev_enable_fn) {
			pr_warn(BCG_NAME ": charger_dev_enable lookup failed\n");
			return -EPROBE_DEFER;
		}
	}

	if (!bcg_charger_dev_enable_powerpath_fn) {
		bcg_charger_dev_enable_powerpath_fn =
			(bcg_charger_dev_enable_powerpath_t)bcg_lookup_symbol_addr("charger_dev_enable_powerpath");
		if (!bcg_charger_dev_enable_powerpath_fn) {
			pr_warn(BCG_NAME ": charger_dev_enable_powerpath lookup failed\n");
			return -EOPNOTSUPP;
		}
	}

	if (IS_ERR_OR_NULL(bcg_primary_chgdev))
		bcg_primary_chgdev = NULL;

	if (!bcg_primary_chgdev) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			bcg_primary_chgdev = bcg_get_charger_by_name_fn(names[i]);
			if (!IS_ERR_OR_NULL(bcg_primary_chgdev)) {
				pr_info(BCG_NAME ": charger backend resolved: %s\n", names[i]);
				break;
			}
			bcg_primary_chgdev = NULL;
		}
	}

	if (!bcg_primary_chgdev) {
		pr_warn(BCG_NAME ": no primary charger device found\n");
		return -ENODEV;
	}

	return 0;
}

static int bcg_resolve_cp_master_backend(void)
{
	static const char * const names[] = {
		"cp_master",
		"sc858x-master",
		"sc858x_master",
		"bq25985-master",
		"bq25985_master",
	};
	int i;

	if (!bcg_get_charger_by_name_fn) {
		bcg_get_charger_by_name_fn =
			(bcg_get_charger_by_name_t)bcg_lookup_symbol_addr("get_charger_by_name");
		if (!bcg_get_charger_by_name_fn) {
			pr_warn(BCG_NAME ": cp backend get_charger_by_name lookup failed\n");
			return -ENOENT;
		}
	}

	if (!bcg_charger_dev_cp_enable_adc_fn) {
		bcg_charger_dev_cp_enable_adc_fn =
			(bcg_charger_dev_cp_enable_adc_t)bcg_lookup_symbol_addr("charger_dev_cp_enable_adc");
		if (!bcg_charger_dev_cp_enable_adc_fn)
			pr_info(BCG_NAME ": cp ADC control unavailable, continuing\n");
	}

	if (!bcg_cp_master_chgdev) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			bcg_cp_master_chgdev = bcg_get_charger_by_name_fn(names[i]);
			if (!IS_ERR_OR_NULL(bcg_cp_master_chgdev)) {
				pr_info(BCG_NAME ": cp backend resolved: %s\n", names[i]);
				break;
			}
			bcg_cp_master_chgdev = NULL;
		}
	}

	if (!bcg_cp_master_chgdev) {
		pr_warn(BCG_NAME ": cp_master charger device not found\n");
		return -ENODEV;
	}

	return 0;
}

static inline bool bcg_is_cp_master_arg(struct charger_device *chg)
{
	return bcg_cp_master_chgdev && chg == bcg_cp_master_chgdev;
}

static inline bool bcg_is_primary_charger_arg(struct charger_device *chg)
{
	return bcg_primary_chgdev && chg == bcg_primary_chgdev;
}

static int bcg_guard_charger_enable_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!READ_ONCE(bcg_bypass_guard_active))
		return 0;

	/* Never alter unrelated charger devices such as et7480_chg. */
	if (!bcg_is_primary_charger_arg(chg) && !bcg_is_cp_master_arg(chg))
		return 0;

	if (regs->regs[1] == 0)
		return 0;

	regs->regs[1] = 0;
	pr_debug(BCG_NAME ": guard blocked charger enable chg=%px\n", chg);

	return 0;
}

static int bcg_guard_powerpath_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!READ_ONCE(bcg_bypass_guard_active))
		return 0;

	/* Only the MT6375 primary charger owns the system power path. */
	if (!bcg_is_primary_charger_arg(chg))
		return 0;

	if (regs->regs[1] != 0)
		return 0;

	regs->regs[1] = 1;
	pr_debug(BCG_NAME ": guard kept primary powerpath enabled\n");

	return 0;
}

static int bcg_register_bypass_guards(void)
{
	int ret;

	if (bcg_kp_charger_enable_registered &&
	    bcg_kp_charger_powerpath_registered)
		return 0;

	bcg_kp_charger_enable.pre_handler = bcg_guard_charger_enable_pre;
	bcg_kp_charger_powerpath.pre_handler = bcg_guard_powerpath_pre;

	ret = register_kprobe(&bcg_kp_charger_enable);
	pr_info(BCG_NAME ": guard register charger_enable ret=%d\n", ret);
	if (ret)
		return ret;
	bcg_kp_charger_enable_registered = true;

	ret = register_kprobe(&bcg_kp_charger_powerpath);
	pr_info(BCG_NAME ": guard register charger_powerpath ret=%d\n", ret);
	if (ret) {
		unregister_kprobe(&bcg_kp_charger_enable);
		bcg_kp_charger_enable_registered = false;
		return ret;
	}
	bcg_kp_charger_powerpath_registered = true;

	return 0;
}

static void bcg_unregister_bypass_guards(void)
{
	if (bcg_kp_charger_powerpath_registered) {
		unregister_kprobe(&bcg_kp_charger_powerpath);
		bcg_kp_charger_powerpath_registered = false;
	}

	if (bcg_kp_charger_enable_registered) {
		unregister_kprobe(&bcg_kp_charger_enable);
		bcg_kp_charger_enable_registered = false;
	}
}

static int bcg_stop_cp_master_for_bypass(void)
{
	int ret;

	ret = bcg_resolve_cp_master_backend();
	if (ret) {
		pr_warn(BCG_NAME ": cp_master unavailable ret=%d\n", ret);
		return ret;
	}

	if (bcg_charger_dev_cp_enable_adc_fn) {
		ret = bcg_charger_dev_cp_enable_adc_fn(bcg_cp_master_chgdev, false);
		if (ret)
			pr_debug(BCG_NAME ": cp_master ADC disable returned %d\n", ret);
	}

	ret = bcg_charger_dev_enable_fn(bcg_cp_master_chgdev, false);
	pr_info(BCG_NAME ": cp_master enable false ret=%d\n", ret);
	return ret;
}

/*
 * Reafirmador periódico: cierra la ventana donde un stack de
 * step-charge/JEITA reactiva charger_dev_enable() por una ruta que
 * no coincide exactamente con el instante en que actúa el kprobe
 * guard. Mientras el bypass esté activo, se reafirma cada
 * BCG_BYPASS_REASSERT_MS en vez de depender solo de la intercepción
 * reactiva de los kprobes.
 */
#define BCG_BYPASS_REASSERT_MS 2000
static void bcg_bypass_reassert_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(bcg_bypass_reassert_work, bcg_bypass_reassert_workfn);

static int bcg_real_bypass_set(bool enable)
{
	int ret = 0;
	int restore_ret;

	mutex_lock(&bcg_state_lock);
	if (!enable && !READ_ONCE(bcg_real_bypass_cached) &&
	    !READ_ONCE(bcg_bypass_guard_active))
		goto out_unlock;

	ret = bcg_resolve_charger_backend();
	if (ret)
		goto out_unlock;

	if (enable) {
		if (READ_ONCE(bcg_real_bypass_cached))
			goto out_unlock;

		ret = bcg_resolve_cp_master_backend();
		if (ret)
			goto out_unlock;

		ret = bcg_register_bypass_guards();
		if (ret) {
			pr_warn(BCG_NAME ": unable to install bypass guards: %d\n", ret);
			goto out_unlock;
		}

		/* The handlers remain inert until the complete backend is ready. */
		WRITE_ONCE(bcg_bypass_guard_active, true);

		ret = bcg_charger_dev_enable_powerpath_fn(bcg_primary_chgdev, true);
		if (ret) {
			pr_warn(BCG_NAME ": cannot keep primary powerpath on: %d\n", ret);
			goto rollback_enable;
		}

		ret = bcg_stop_cp_master_for_bypass();
		if (ret)
			goto rollback_enable;

		ret = bcg_charger_dev_enable_fn(bcg_primary_chgdev, false);
		if (ret) {
			pr_warn(BCG_NAME ": primary charger disable failed: %d\n", ret);
			goto rollback_enable;
		}

		WRITE_ONCE(bcg_real_bypass_cached, true);
		mod_delayed_work(system_wq, &bcg_bypass_reassert_work,
				 msecs_to_jiffies(BCG_BYPASS_REASSERT_MS));
		pr_info(BCG_NAME ": bypass enabled\n");
		goto out_unlock;
	}

	/* Stop the worker completely before restoring normal charging. */
	WRITE_ONCE(bcg_bypass_guard_active, false);
	cancel_delayed_work_sync(&bcg_bypass_reassert_work);

	ret = bcg_charger_dev_enable_powerpath_fn(bcg_primary_chgdev, true);
	if (ret)
		pr_warn(BCG_NAME ": powerpath restore returned %d\n", ret);

	restore_ret = bcg_charger_dev_enable_fn(bcg_primary_chgdev, true);
	if (restore_ret) {
		pr_warn(BCG_NAME ": primary charger restore failed: %d\n",
			restore_ret);
		if (!ret)
			ret = restore_ret;
	} else {
		WRITE_ONCE(bcg_real_bypass_cached, false);
		pr_info(BCG_NAME ": bypass disabled\n");
	}
	goto out_unlock;

rollback_enable:
	WRITE_ONCE(bcg_bypass_guard_active, false);
	cancel_delayed_work_sync(&bcg_bypass_reassert_work);
	bcg_charger_dev_enable_powerpath_fn(bcg_primary_chgdev, true);
	bcg_charger_dev_enable_fn(bcg_primary_chgdev, true);
	WRITE_ONCE(bcg_real_bypass_cached, false);
out_unlock:
	mutex_unlock(&bcg_state_lock);
	return ret;
}

static void bcg_bypass_reassert_workfn(struct work_struct *work)
{
	int ret;

	if (!READ_ONCE(bcg_bypass_guard_active) || !bcg_primary_chgdev)
		return;

	ret = bcg_charger_dev_enable_powerpath_fn(bcg_primary_chgdev, true);
	if (ret)
		pr_warn_ratelimited(BCG_NAME ": reassert powerpath failed: %d\n", ret);

	if (bcg_cp_master_chgdev) {
		if (bcg_charger_dev_cp_enable_adc_fn)
			bcg_charger_dev_cp_enable_adc_fn(bcg_cp_master_chgdev, false);
		bcg_charger_dev_enable_fn(bcg_cp_master_chgdev, false);
	}

	/* bcg_bypass_guard_active == true implica bypass activo => no cargar */
	ret = bcg_charger_dev_enable_fn(bcg_primary_chgdev, false);
	if (ret)
		pr_warn_ratelimited(BCG_NAME ": reassert charger disable failed: %d\n",
				    ret);

	if (READ_ONCE(bcg_bypass_guard_active))
		mod_delayed_work(system_wq, &bcg_bypass_reassert_work,
				 msecs_to_jiffies(BCG_BYPASS_REASSERT_MS));
}

static ssize_t bypass_charging_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  READ_ONCE(bcg_real_bypass_cached) ? 1 : 0);
}

static ssize_t bypass_charging_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	ret = bcg_real_bypass_set(enable);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR(bypass_charging, 0664, bypass_charging_show, bypass_charging_store);

static struct power_supply *bcg_bypass_battery_psy;
static bool bcg_bypass_attrs_created;
static int bcg_bypass_retry_count;

static void bcg_bypass_retry_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(bcg_bypass_retry_work, bcg_bypass_retry_workfn);

static int bcg_create_real_bypass_attrs(void)
{
	int ret;

	bcg_bypass_battery_psy = power_supply_get_by_name("battery");
	if (!bcg_bypass_battery_psy) {
		pr_warn(BCG_NAME ": battery power_supply not ready for bypass attrs\n");
		return -EPROBE_DEFER;
	}

	ret = device_create_file(&bcg_bypass_battery_psy->dev,
				 &dev_attr_bypass_charging);
	if (ret) {
		pr_warn(BCG_NAME ": bypass_charging create failed: %d\n", ret);
		goto err_put_psy;
	}

	bcg_bypass_attrs_created = true;
	pr_info(BCG_NAME ": bypass_charging attached to battery\n");
	return 0;

err_put_psy:
	power_supply_put(bcg_bypass_battery_psy);
	bcg_bypass_battery_psy = NULL;
	return ret;
}

static void bcg_bypass_retry_workfn(struct work_struct *work)
{
	int ret;

	if (bcg_bypass_attrs_created)
		return;

	ret = bcg_create_real_bypass_attrs();
	if (ret == -EEXIST)
		return;

	if (!bcg_bypass_attrs_created && bcg_bypass_retry_count++ < 30) {
		pr_info(BCG_NAME ": bypass attrs not ready, retry=%d\n",
			bcg_bypass_retry_count);
		schedule_delayed_work(&bcg_bypass_retry_work,
				      msecs_to_jiffies(2000));
	}
}

static void bcg_remove_real_bypass_attrs(void)
{
	if (bcg_bypass_attrs_created && bcg_bypass_battery_psy) {
		device_remove_file(&bcg_bypass_battery_psy->dev,
				   &dev_attr_bypass_charging);
		bcg_bypass_attrs_created = false;
	}

	if (bcg_bypass_battery_psy) {
		power_supply_put(bcg_bypass_battery_psy);
		bcg_bypass_battery_psy = NULL;
	}
}

static int __init bomb_charge_init(void)
{
	int ret;

	bcg_register_power_supply_aliases();

	ret = bcg_create_real_bypass_attrs();
	if (ret == -EPROBE_DEFER) {
		schedule_delayed_work(&bcg_bypass_retry_work,
				      msecs_to_jiffies(2000));
	} else if (ret) {
		bcg_unregister_power_supply_aliases();
		return ret;
	}

	pr_info(BCG_NAME ": loaded\n");
	return 0;
}

static void __exit bomb_charge_exit(void)
{
	int ret;

	cancel_delayed_work_sync(&bcg_bypass_retry_work);

	/* Never leave a module-owned bypass active after removing the module. */
	if (READ_ONCE(bcg_real_bypass_cached) ||
	    READ_ONCE(bcg_bypass_guard_active)) {
		ret = bcg_real_bypass_set(false);
		if (ret)
			pr_warn(BCG_NAME ": charger restore during unload failed: %d\n",
				ret);
	} else {
		cancel_delayed_work_sync(&bcg_bypass_reassert_work);
	}

	bcg_unregister_bypass_guards();
	bcg_remove_real_bypass_attrs();
	bcg_unregister_power_supply_aliases();
}

module_init(bomb_charge_init);
module_exit(bomb_charge_exit);

MODULE_DESCRIPTION("Real charger-class bypass charging control");
MODULE_LICENSE("GPL");
