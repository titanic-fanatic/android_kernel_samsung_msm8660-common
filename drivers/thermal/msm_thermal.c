/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>

#define DEF_TEMP_SENSOR      0

static int enabled;

//Highest thermal limit
#define DEF_ALLOWED_MAX_HIGH 76
#define DEF_ALLOWED_MAX_FREQ 384000
static int allowed_max_high = DEF_ALLOWED_MAX_HIGH;
static int allowed_max_low = (DEF_ALLOWED_MAX_HIGH - 4);
static int allowed_max_freq = DEF_ALLOWED_MAX_FREQ;

//mid thermal limit
#define DEF_ALLOWED_MID_HIGH 72
#define DEF_ALLOWED_MID_FREQ 648000
static int allowed_mid_high = DEF_ALLOWED_MID_HIGH;
static int allowed_mid_low = (DEF_ALLOWED_MID_HIGH - 4);
static int allowed_mid_freq = DEF_ALLOWED_MID_FREQ;

//low thermal limit
#define DEF_ALLOWED_LOW_HIGH 70
#define DEF_ALLOWED_LOW_FREQ 972000
static int allowed_low_high = DEF_ALLOWED_LOW_HIGH;
static int allowed_low_low = (DEF_ALLOWED_LOW_HIGH - 4);
static int allowed_low_freq = DEF_ALLOWED_LOW_FREQ;

//Sampling interval
#define DEF_THERMAL_CHECK_MS 1000
static int check_interval_ms = DEF_THERMAL_CHECK_MS;

//Throttling indicator, 0=not throttled, 1=low, 2=mid, 3=max
static int thermal_throttled = 0;

//Safe the cpu max freq before throttling
static int pre_throttled_max = 0;

module_param(allowed_max_high, int, 0);
module_param(allowed_max_freq, int, 0);
module_param(check_interval_ms, int, 0);

static struct delayed_work check_temp_work;

static int update_cpu_max_freq(int cpu, uint32_t max_freq)
{
	int ret = 0;

	ret = msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, max_freq);
	if (ret)
		return ret;

	ret = cpufreq_update_policy(cpu);
	if (ret)
		return ret;

	limited_max_freq = max_freq;
	if (max_freq != MSM_CPUFREQ_NO_LIMIT)
		pr_info("msm_thermal: Limiting cpu%d max frequency to %d\n",
				cpu, max_freq);
	else
		pr_info("msm_thermal: Max frequency reset for cpu%d\n", cpu);

	return ret;
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	unsigned long temp = 0;
	uint32_t max_freq = limited_max_freq;
	int cpu = 0;
	int ret = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_err("msm_thermal: Unable to read TSENS sensor %d\n",
				tsens_dev.sensor_num);
		goto reschedule;
	}

	if (temp >= msm_thermal_info.limit_temp)
		max_freq = msm_thermal_info.limit_freq;
	else if (temp <
		msm_thermal_info.limit_temp - msm_thermal_info.temp_hysteresis)
		max_freq = MSM_CPUFREQ_NO_LIMIT;

	if (max_freq == limited_max_freq)
		goto reschedule;

	/* Update new limits */
	for_each_possible_cpu(cpu) {
		update_policy = 0;
		cpu_policy = cpufreq_cpu_get(cpu);
		if (!cpu_policy) {
			pr_debug("msm_thermal: NULL policy on cpu %d\n", cpu);
			continue;
		}

		//low trip point
		if ((temp >= allowed_low_high) &&
		    (temp < allowed_mid_high) &&
		    (cpu_policy->max > allowed_low_freq)) {
			update_policy = 1;
			/* save pre-throttled max freq value */
			pre_throttled_max = cpu_policy->max;
			max_freq = allowed_low_freq;
			thermal_throttled = 1;
			pr_warn("msm_thermal: Thermal Throttled (low)! temp: %lu\n", temp);
		//low clr point
		} else if ((temp < allowed_low_low) &&
			   (thermal_throttled > 0)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				if (pre_throttled_max != 0)
					max_freq = pre_throttled_max;
				else {
					max_freq = 1566000;
					pr_warn("msm_thermal: ERROR! pre_throttled_max=0, falling back to %u\n", max_freq);
				}
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 0;
				pr_warn("msm_thermal: Low Thermal Throttling Ended! temp: %lu\n", temp);
			}
		//mid trip point
		} else if ((temp >= allowed_low_high) &&
			   (temp < allowed_mid_low) &&
			   (cpu_policy->max > allowed_mid_freq)) {
			update_policy = 1;
			max_freq = allowed_low_freq;
			thermal_throttled = 2;
			pr_warn("msm_thermal: Thermal Throttled (mid)! temp: %lu\n", temp);
		//mid clr point
		} else if ( (temp < allowed_mid_low) &&
			   (thermal_throttled > 1)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				max_freq = allowed_low_freq;
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 1;
				pr_warn("msm_thermal: Mid Thermal Throttling Ended! temp: %lu\n", temp);
			}
		//max trip point
		} else if ((temp >= allowed_max_high) &&
			   (cpu_policy->max > allowed_max_freq)) {
			update_policy = 1;
			max_freq = allowed_max_freq;
			thermal_throttled = 3;
			pr_warn("msm_thermal: Thermal Throttled (max)! temp: %lu\n", temp);
		//max clr point
		} else if ((temp < allowed_max_low) &&
			   (thermal_throttled > 2)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				max_freq = allowed_mid_freq;
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 2;
				pr_warn("msm_thermal: Max Thermal Throttling Ended! temp: %lu\n", temp);
			}
		}

		if (update_policy)
			update_cpu_max_freq(cpu_policy, cpu, max_freq);

		cpufreq_cpu_put(cpu_policy);
	}

reschedule:
	if (enabled)
		schedule_delayed_work(&check_temp_work,
				msecs_to_jiffies(msm_thermal_info.poll_ms));
}

static void disable_msm_thermal(void)
{
	int cpu = 0;

	/* make sure check_temp is no longer running */
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	if (limited_max_freq == MSM_CPUFREQ_NO_LIMIT)
		return;

	/* make sure check_temp is no longer running */
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	for_each_possible_cpu(cpu) {
		update_cpu_max_freq(cpu, MSM_CPUFREQ_NO_LIMIT);
	}
}

static int set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	if (!enabled)
		disable_msm_thermal();
	else
		pr_info("msm_thermal: no action for enabled = %d\n", enabled);

	pr_info("msm_thermal: enabled = %d\n", enabled);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");

int __init msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0;

	BUG_ON(!pdata);
	BUG_ON(pdata->sensor_id >= TSENS_MAX_SENSORS);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

	enabled = 1;
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 0);

	return ret;
}
