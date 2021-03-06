/* arch/arm/mach-msm/cpufreq.c
 *
 * MSM architecture cpufreq driver
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2007-2010, Code Aurora Forum. All rights reserved.
 * Author: Mike A. Chan <mikechan@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */

#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <mach/socinfo.h>
#include <mach/cpufreq.h>

#include "acpuclock.h"

unsigned int max_capped;
/* initialize to a default cap freq */
static unsigned int screen_off_max_freq = 594000;

#ifdef CONFIG_SMP
struct cpufreq_work_struct {
	struct work_struct work;
	struct cpufreq_policy *policy;
	struct completion complete;
	int frequency;
	int status;
};

static DEFINE_PER_CPU(struct cpufreq_work_struct, cpufreq_work);
static struct workqueue_struct *msm_cpufreq_wq;
#endif

struct cpufreq_suspend_t {
	struct mutex suspend_mutex;
	int device_suspended;
};

static DEFINE_PER_CPU(struct cpufreq_suspend_t, cpufreq_suspend);

static int override_cpu;
struct cpu_freq {
	uint32_t max;
	uint32_t min;
	uint32_t allowed_max;
	uint32_t allowed_min;
	uint32_t limits_init;
};

static DEFINE_PER_CPU(struct cpu_freq, cpu_freq_info);

static int set_cpu_freq(struct cpufreq_policy *policy, unsigned int new_freq)
{
	int ret = 0;
	struct cpufreq_freqs freqs;
	struct cpu_freq *limit = &per_cpu(cpu_freq_info, policy->cpu);

	if (limit->limits_init) {
		if (new_freq > limit->allowed_max) {
			new_freq = limit->allowed_max;
			pr_debug("max: limiting freq to %d\n", new_freq);
		}

		if (new_freq < limit->allowed_min) {
			new_freq = limit->allowed_min;
			pr_debug("min: limiting freq to %d\n", new_freq);
		}
	}

	freqs.old = policy->cur;
	if (override_cpu) {
		if (policy->cur == policy->max)
			return 0;
		else
			freqs.new = policy->max;
	} else
		freqs.new = new_freq;
	freqs.cpu = policy->cpu;
	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	ret = acpuclk_set_rate(policy->cpu, new_freq, SETRATE_CPUFREQ);
	if (!ret)
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	return ret;
}

#ifdef CONFIG_SMP
static void set_cpu_work(struct work_struct *work)
{
	struct cpufreq_work_struct *cpu_work =
		container_of(work, struct cpufreq_work_struct, work);

	cpu_work->status = set_cpu_freq(cpu_work->policy, cpu_work->frequency);
	complete(&cpu_work->complete);
}
#endif

static void msm_cpu_early_suspend(struct early_suspend *h)
{
	unsigned int cur;
	int cpu = 0;

	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);

		if (screen_off_max_freq) {
			max_capped = screen_off_max_freq;

			cur = acpuclk_get_rate(cpu);
			if (cur > max_capped) {
				acpuclk_set_rate(cpu, max_capped,
						SETRATE_CPUFREQ);
			}
		}

		/* disable 2nd core as well since screen is off */
		if (cpu == 0 && num_online_cpus() > 1)
			cpu_down(1);
		mutex_unlock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
	}
}

static void msm_cpu_late_resume(struct early_suspend *h)
{
	unsigned int cur;
	int cpu = 0;

	for_each_possible_cpu(cpu) {

		mutex_lock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);

		if (max_capped) {
			struct cpufreq_work_struct *cpu_work = &per_cpu(cpufreq_work, cpu);
			max_capped = 0;

			cur = acpuclk_get_rate(cpu);
			if (cur != cpu_work->frequency) {
				acpuclk_set_rate(cpu, cpu_work->frequency,
						SETRATE_CPUFREQ);
			}
		}

		/* re-enable 2nd core */
		if (num_online_cpus() < 2 && cpu == 0)
			cpu_up(1);
		mutex_unlock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
	}
}

static struct early_suspend msm_cpu_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = msm_cpu_early_suspend,
	.resume = msm_cpu_late_resume,
};

static int msm_cpufreq_target(struct cpufreq_policy *policy,
				unsigned int target_freq,
				unsigned int relation)
{
	int ret = -EFAULT;
	int index;
	struct cpufreq_frequency_table *table;
#ifdef CONFIG_SMP
	struct cpufreq_work_struct *cpu_work = NULL;
	cpumask_var_t mask;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	if (!cpu_active(policy->cpu)) {
		pr_info("cpufreq: cpu %d is not active.\n", policy->cpu);
		return -ENODEV;
	}
#endif

	mutex_lock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);

	if (per_cpu(cpufreq_suspend, policy->cpu).device_suspended) {
		pr_debug("cpufreq: cpu%d scheduling frequency change "
				"in suspend.\n", policy->cpu);
		ret = -EFAULT;
		goto done;
	}

	table = cpufreq_frequency_get_table(policy->cpu);
	if (cpufreq_frequency_table_target(policy, table, target_freq, relation,
			&index)) {
		pr_err("cpufreq: invalid target_freq: %d\n", target_freq);
		ret = -EINVAL;
		goto done;
	}

#ifdef CONFIG_CPU_FREQ_DEBUG
	pr_debug("CPU[%d] target %d relation %d (%d-%d) selected %d\n",
		policy->cpu, target_freq, relation,
		policy->min, policy->max, table[index].frequency);
#endif

#ifdef CONFIG_SMP
	cpu_work = &per_cpu(cpufreq_work, policy->cpu);
	cpu_work->policy = policy;
	cpu_work->frequency = table[index].frequency;
	cpu_work->status = -ENODEV;

	cpumask_clear(mask);
	cpumask_set_cpu(policy->cpu, mask);
	if (cpumask_equal(mask, &current->cpus_allowed)) {
		ret = set_cpu_freq(cpu_work->policy, cpu_work->frequency);
		goto done;
	} else {
		cancel_work_sync(&cpu_work->work);
		INIT_COMPLETION(cpu_work->complete);
		queue_work_on(policy->cpu, msm_cpufreq_wq, &cpu_work->work);
		wait_for_completion(&cpu_work->complete);
	}

	free_cpumask_var(mask);
	ret = cpu_work->status;
#else
	ret = set_cpu_freq(policy, table[index].frequency);
#endif

done:
	mutex_unlock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);
	return ret;
}

static int msm_cpufreq_verify(struct cpufreq_policy *policy)
{
	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
			policy->cpuinfo.max_freq);
	return 0;
}

static unsigned int msm_cpufreq_get_freq(unsigned int cpu)
{
	return acpuclk_get_rate(cpu);
}

static inline int msm_cpufreq_limits_init(void)
{
	int cpu = 0;
	int i = 0;
	struct cpufreq_frequency_table *table = NULL;
	uint32_t min = (uint32_t) -1;
	uint32_t max = 0;
	struct cpu_freq *limit = NULL;

	for_each_possible_cpu(cpu) {
		limit = &per_cpu(cpu_freq_info, cpu);
		table = cpufreq_frequency_get_table(cpu);
		if (table == NULL) {
			pr_err("%s: error reading cpufreq table for cpu %d\n",
					__func__, cpu);
			continue;
		}
		for (i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++) {
			if (table[i].frequency > max)
				max = table[i].frequency;
			if (table[i].frequency < min)
				min = table[i].frequency;
		}
		limit->allowed_min = min;
		limit->allowed_max = max;
		limit->min = min;
		limit->max = max;
		limit->limits_init = 1;
	}

	return 0;
}

int msm_cpufreq_set_freq_limits(uint32_t cpu, uint32_t min, uint32_t max)
{
	struct cpu_freq *limit = &per_cpu(cpu_freq_info, cpu);

	if (!limit->limits_init)
		msm_cpufreq_limits_init();

	if ((min != MSM_CPUFREQ_NO_LIMIT) &&
		min >= limit->min && min <= limit->max)
		limit->allowed_min = min;
	else
		limit->allowed_min = limit->min;


	if ((max != MSM_CPUFREQ_NO_LIMIT) &&
		max <= limit->max && max >= limit->min)
		limit->allowed_max = max;
	else
		limit->allowed_max = limit->max;

	pr_debug("%s: Limiting cpu %d min = %d, max = %d\n",
			__func__, cpu,
			limit->allowed_min, limit->allowed_max);

	return 0;
}
EXPORT_SYMBOL(msm_cpufreq_set_freq_limits);

static int __cpuinit msm_cpufreq_init(struct cpufreq_policy *policy)
{
	int cur_freq;
	int index;
	struct cpufreq_frequency_table *table;
#ifdef CONFIG_SMP
	struct cpufreq_work_struct *cpu_work = NULL;
#endif

	if (cpu_is_apq8064())
		return -ENODEV;

	table = cpufreq_frequency_get_table(policy->cpu);
	if (cpufreq_frequency_table_cpuinfo(policy, table)) {
#ifdef CONFIG_MSM_CPU_FREQ_SET_MIN_MAX
		policy->cpuinfo.min_freq = CONFIG_MSM_CPU_FREQ_MIN;
		policy->cpuinfo.max_freq = CONFIG_MSM_CPU_FREQ_MAX;
#endif
	}
#ifdef CONFIG_MSM_CPU_FREQ_SET_MIN_MAX
	policy->min = CONFIG_MSM_CPU_FREQ_MIN;
	policy->max = CONFIG_MSM_CPU_FREQ_MAX;
#endif

	cur_freq = acpuclk_get_rate(policy->cpu);
	if (cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_H, &index) &&
	    cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_L, &index)) {
		pr_info("cpufreq: cpu%d at invalid freq: %d\n",
				policy->cpu, cur_freq);
		return -EINVAL;
	}

	if (cur_freq != table[index].frequency) {
		int ret = 0;
		ret = acpuclk_set_rate(policy->cpu, table[index].frequency,
				SETRATE_CPUFREQ);
		if (ret)
			return ret;
		pr_info("cpufreq: cpu%d init at %d switching to %d\n",
				policy->cpu, cur_freq, table[index].frequency);
		cur_freq = table[index].frequency;
	}

	policy->cur = cur_freq;

	policy->cpuinfo.transition_latency =
		acpuclk_get_switch_time() * NSEC_PER_USEC;
#ifdef CONFIG_SMP
	cpu_work = &per_cpu(cpufreq_work, policy->cpu);
	INIT_WORK(&cpu_work->work, set_cpu_work);
	init_completion(&cpu_work->complete);
#endif

	return 0;
}

static int msm_cpufreq_suspend(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
		per_cpu(cpufreq_suspend, cpu).device_suspended = 1;
		mutex_unlock(&per_cpu(cpufreq_suspend, cpu).suspend_mutex);
	}

	return NOTIFY_DONE;
}

static int msm_cpufreq_resume(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(cpufreq_suspend, cpu).device_suspended = 0;
	}

	return NOTIFY_DONE;
}

static int msm_cpufreq_pm_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	switch (event) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		return msm_cpufreq_resume();
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		return msm_cpufreq_suspend();
	default:
		return NOTIFY_DONE;
	}
}

static ssize_t store_mfreq(struct sysdev_class *class,
			struct sysdev_class_attribute *attr,
			const char *buf, size_t count)
{
	u64 val;

	if (strict_strtoull(buf, 0, &val) < 0) {
		pr_err("Invalid parameter to mfreq\n");
		return 0;
	}
	if (val)
		override_cpu = 1;
	else
		override_cpu = 0;
	return count;
}

static SYSDEV_CLASS_ATTR(mfreq, 0200, NULL, store_mfreq);

static ssize_t show_screen_off_freq(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", screen_off_max_freq);
}

static ssize_t store_screen_off_freq(struct cpufreq_policy *policy,
		const char *buf, size_t count)
{
	unsigned int freq = 0;
	int ret;
	int index;
	struct cpufreq_frequency_table *freq_table = cpufreq_frequency_get_table(policy->cpu);

	if (!freq_table)
		return -EINVAL;

	ret = sscanf(buf, "%u", &freq);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);

	ret = cpufreq_frequency_table_target(policy, freq_table, freq,
			CPUFREQ_RELATION_H, &index);
	if (ret)
		goto out;

	screen_off_max_freq = freq_table[index].frequency;

	ret = count;

out:
	mutex_unlock(&per_cpu(cpufreq_suspend, policy->cpu).suspend_mutex);
	return ret;
}

struct freq_attr msm_cpufreq_attr_screen_off_freq = {
	.attr = { .name = "screen_off_max_freq",
		.mode = 0644,
	},
	.show = show_screen_off_freq,
	.store = store_screen_off_freq,
};

static struct freq_attr *msm_freq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&msm_cpufreq_attr_screen_off_freq,
	NULL,
};

static struct cpufreq_driver msm_cpufreq_driver = {
	/* lps calculations are handled here. */
	.flags		= CPUFREQ_STICKY | CPUFREQ_CONST_LOOPS,
	.init		= msm_cpufreq_init,
	.verify		= msm_cpufreq_verify,
	.target		= msm_cpufreq_target,
	.get		= msm_cpufreq_get_freq,
	.name		= "msm",
	.attr		= msm_freq_attr,
};

static struct notifier_block msm_cpufreq_pm_notifier = {
	.notifier_call = msm_cpufreq_pm_event,
};

static int __init msm_cpufreq_register(void)
{
	int cpu;

	int err = sysfs_create_file(&cpu_sysdev_class.kset.kobj,
			&attr_mfreq.attr);
	if (err)
		pr_err("Failed to create sysfs mfreq\n");

	for_each_possible_cpu(cpu) {
		mutex_init(&(per_cpu(cpufreq_suspend, cpu).suspend_mutex));
		per_cpu(cpufreq_suspend, cpu).device_suspended = 0;
	}

#ifdef CONFIG_SMP
	msm_cpufreq_wq = create_workqueue("msm-cpufreq");
#endif

	register_pm_notifier(&msm_cpufreq_pm_notifier);

	register_early_suspend(&msm_cpu_early_suspend_handler);

	return cpufreq_register_driver(&msm_cpufreq_driver);
}

late_initcall(msm_cpufreq_register);
