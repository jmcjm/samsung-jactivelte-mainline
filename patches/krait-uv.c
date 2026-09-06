// SPDX-License-Identifier: GPL-2.0
/*
 * krait-uv - runtime adjustment of CPU operating point voltages
 *
 * Changes the voltage of CPU operating points on a running system, so that
 * undervolting can be explored without editing the device tree and rebooting.
 * A new voltage takes effect on the next frequency transition of each CPU.
 * Unloading the module restores the device tree values.
 *
 * /sys/kernel/krait_uv/table   one line per operating point:
 *                              "<khz> <current uV> <default uV>"
 * /sys/kernel/krait_uv/set     "<khz> <uV>"     one operating point, all CPUs
 *                              "all <delta uV>" every point: default + delta,
 *                                               clamped to the allowed range
 *                              "reset"          back to the device tree values
 */

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/sysfs.h>

#define KRAIT_UV_MAX_OPPS	32
/* SAW regulator step and the constraints of the CPU supplies in the DT. */
#define KRAIT_UV_STEP		12500
#define KRAIT_UV_MIN		850000
#define KRAIT_UV_MAX		1300000

static struct kobject *krait_uv_kobj;
static unsigned long opp_freq[KRAIT_UV_MAX_OPPS];
static unsigned long opp_default_uv[KRAIT_UV_MAX_OPPS];
static int opp_count;
static DEFINE_MUTEX(krait_uv_lock);

static bool krait_uv_valid(unsigned long uv)
{
	return uv >= KRAIT_UV_MIN && uv <= KRAIT_UV_MAX && !(uv % KRAIT_UV_STEP);
}

static int krait_uv_apply(unsigned long freq, unsigned long uv)
{
	int cpu, ret;

	if (!krait_uv_valid(uv))
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!dev)
			continue;
		ret = dev_pm_opp_adjust_voltage(dev, freq, uv, uv, uv);
		if (ret) {
			pr_err("krait_uv: cpu%d %lu kHz -> %lu uV failed (%d)\n",
			       cpu, freq / 1000, uv, ret);
			return ret;
		}
	}
	return 0;
}

static int krait_uv_index(unsigned long freq)
{
	int i;

	for (i = 0; i < opp_count; i++)
		if (opp_freq[i] == freq)
			return i;
	return -ENOENT;
}

static ssize_t table_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct device *dev = get_cpu_device(0);
	ssize_t len = 0;
	int i;

	if (!dev)
		return -ENODEV;

	guard(mutex)(&krait_uv_lock);
	for (i = 0; i < opp_count; i++) {
		struct dev_pm_opp *opp;
		unsigned long cur = 0;

		opp = dev_pm_opp_find_freq_exact(dev, opp_freq[i], true);
		if (!IS_ERR(opp)) {
			cur = dev_pm_opp_get_voltage(opp);
			dev_pm_opp_put(opp);
		}
		len += sysfs_emit_at(buf, len, "%lu %lu %lu\n",
				     opp_freq[i] / 1000, cur, opp_default_uv[i]);
	}
	return len;
}

static ssize_t set_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned long khz, uv;
	long delta;
	int i, ret;

	guard(mutex)(&krait_uv_lock);

	if (sysfs_streq(buf, "reset")) {
		for (i = 0; i < opp_count; i++) {
			ret = krait_uv_apply(opp_freq[i], opp_default_uv[i]);
			if (ret)
				return ret;
		}
		return count;
	}

	if (sscanf(buf, "all %ld", &delta) == 1) {
		if (delta % KRAIT_UV_STEP)
			return -EINVAL;
		for (i = 0; i < opp_count; i++) {
			long target = (long)opp_default_uv[i] + delta;

			target = clamp(target, (long)KRAIT_UV_MIN,
				       (long)KRAIT_UV_MAX);
			ret = krait_uv_apply(opp_freq[i], target);
			if (ret)
				return ret;
		}
		return count;
	}

	if (sscanf(buf, "%lu %lu", &khz, &uv) != 2)
		return -EINVAL;
	i = krait_uv_index(khz * 1000);
	if (i < 0)
		return i;
	ret = krait_uv_apply(khz * 1000, uv);
	return ret ? ret : count;
}

static struct kobj_attribute table_attr = __ATTR_RO(table);
static struct kobj_attribute set_attr = __ATTR_WO(set);

static struct attribute *krait_uv_attrs[] = {
	&table_attr.attr,
	&set_attr.attr,
	NULL,
};

static const struct attribute_group krait_uv_group = {
	.attrs = krait_uv_attrs,
};

static int __init krait_uv_init(void)
{
	struct device *dev = get_cpu_device(0);
	unsigned long freq = 0;
	int ret;

	if (!dev)
		return -ENODEV;

	while (opp_count < KRAIT_UV_MAX_OPPS) {
		struct dev_pm_opp *opp = dev_pm_opp_find_freq_ceil(dev, &freq);

		if (IS_ERR(opp))
			break;
		opp_freq[opp_count] = freq;
		opp_default_uv[opp_count] = dev_pm_opp_get_voltage(opp);
		dev_pm_opp_put(opp);
		opp_count++;
		freq++;
	}
	if (!opp_count) {
		pr_err("krait_uv: cpu0 has no operating points\n");
		return -ENODEV;
	}

	krait_uv_kobj = kobject_create_and_add("krait_uv", kernel_kobj);
	if (!krait_uv_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(krait_uv_kobj, &krait_uv_group);
	if (ret) {
		kobject_put(krait_uv_kobj);
		return ret;
	}

	pr_info("krait_uv: %d operating points, %lu-%lu kHz\n", opp_count,
		opp_freq[0] / 1000, opp_freq[opp_count - 1] / 1000);
	return 0;
}

static void __exit krait_uv_exit(void)
{
	int i;

	mutex_lock(&krait_uv_lock);
	for (i = 0; i < opp_count; i++)
		krait_uv_apply(opp_freq[i], opp_default_uv[i]);
	mutex_unlock(&krait_uv_lock);

	sysfs_remove_group(krait_uv_kobj, &krait_uv_group);
	kobject_put(krait_uv_kobj);
}

module_init(krait_uv_init);
module_exit(krait_uv_exit);

MODULE_DESCRIPTION("Runtime CPU operating point voltage adjustment");
MODULE_LICENSE("GPL");
