// SPDX-License-Identifier: GPL-2.0
/*
 * ideapad_fan_pwm - fan PWM control for the Lenovo IdeaPad 3 14ALC6 (82KT)
 *
 * The laptop's embedded controller (EC) is an ITE IT8227. Its PWM duty
 * register DCR4 (0x1806 in the EC's internal register space) drives the
 * fan: 0 = stopped, 200 = full speed. The register is reached through the
 * chip's "I2EC" window on the Super I/O ports 0x4E/0x4F.
 *
 * User space can only touch those ports with Secure Boot off. A kernel
 * module can always do it, and a module signed with a key the owner has
 * enrolled (MOK) loads with Secure Boot on. This module exposes the fan as
 * a standard hwmon device:
 *
 *   fan1_input   measured speed in rpm (EC RAM byte 0x06, units of 100 rpm)
 *   pwm1         duty, 0..255
 *   pwm1_enable  1 = manual (hold pwm1), 2 = automatic (firmware control)
 *
 * The firmware keeps running and moves the duty one step per second towards
 * its own target, so in manual mode the module rewrites it four times a
 * second.
 *
 * Fail-safe: manual mode ends by itself when pwm1 has not been written for
 * `watchdog` seconds (default 30), so a crashed control program cannot leave
 * the fan stuck at a low speed. watchdog=0 disables this.
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define DRV_NAME	"ideapad_fan_pwm"

#define SIO_INDEX	0x4E
#define SIO_DATA	0x4F
#define EC_CHIP_ID	0x8227
#define EC_CHIPID1	0x2000
#define EC_CHIPID2	0x2001
#define EC_DCR4		0x1806	/* fan PWM duty */
#define EC_DUTY_MAX	200
#define EC_RAM_FANS	0x06	/* measured speed, units of 100 rpm */

#define HOLD_INTERVAL	(HZ / 4)

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Load on models other than the 14ALC6 (82KT); the EC chip id is still checked");

static unsigned int watchdog = 30;
module_param(watchdog, uint, 0644);
MODULE_PARM_DESC(watchdog, "Seconds without a pwm1 write before returning to automatic (0 = never)");

struct fan_data {
	struct mutex lock;		/* protects the fields below and the I2EC window */
	bool manual;
	u8 duty;			/* 0..EC_DUTY_MAX */
	unsigned long last_write;	/* jiffies of the last pwm1 write */
	struct delayed_work hold;
};

static struct platform_device *fan_pdev;

/* --- I2EC: one byte of EC internal register space. Caller holds the lock. --- */

static void i2ec_select(u16 addr)
{
	outb(0x2E, SIO_INDEX); outb(0x11, SIO_DATA);
	outb(0x2F, SIO_INDEX); outb(addr >> 8, SIO_DATA);
	outb(0x2E, SIO_INDEX); outb(0x10, SIO_DATA);
	outb(0x2F, SIO_INDEX); outb(addr & 0xFF, SIO_DATA);
	outb(0x2E, SIO_INDEX); outb(0x12, SIO_DATA);
	outb(0x2F, SIO_INDEX);
}

static int i2ec_read(u16 addr, u8 *val)
{
	if (!request_muxed_region(SIO_INDEX, 2, DRV_NAME))
		return -EBUSY;
	i2ec_select(addr);
	*val = inb(SIO_DATA);
	release_region(SIO_INDEX, 2);
	return 0;
}

static int i2ec_write(u16 addr, u8 val)
{
	if (!request_muxed_region(SIO_INDEX, 2, DRV_NAME))
		return -EBUSY;
	i2ec_select(addr);
	outb(val, SIO_DATA);
	release_region(SIO_INDEX, 2);
	return 0;
}

static void fan_hold(struct work_struct *work)
{
	struct fan_data *data = container_of(to_delayed_work(work), struct fan_data, hold);

	mutex_lock(&data->lock);
	if (data->manual && watchdog &&
	    time_after(jiffies, data->last_write + watchdog * HZ)) {
		data->manual = false;
		pr_info(DRV_NAME ": no pwm1 write for %u s, back to automatic\n", watchdog);
	}
	if (data->manual) {
		i2ec_write(EC_DCR4, data->duty);
		schedule_delayed_work(&data->hold, HOLD_INTERVAL);
	}
	mutex_unlock(&data->lock);
}

/* --- hwmon --- */

static umode_t fan_is_visible(const void *drvdata, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	if (type == hwmon_fan && attr == hwmon_fan_input)
		return 0444;
	if (type == hwmon_pwm && (attr == hwmon_pwm_input || attr == hwmon_pwm_enable))
		return 0644;
	return 0;
}

static int fan_read(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val)
{
	struct fan_data *data = dev_get_drvdata(dev);
	u8 v;
	int ret;

	if (type == hwmon_fan && attr == hwmon_fan_input) {
		ret = ec_read(EC_RAM_FANS, &v);
		if (ret)
			return ret;
		*val = v * 100;
		return 0;
	}
	if (type == hwmon_pwm && attr == hwmon_pwm_input) {
		mutex_lock(&data->lock);
		ret = i2ec_read(EC_DCR4, &v);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;
		*val = DIV_ROUND_CLOSEST(min_t(int, v, EC_DUTY_MAX) * 255, EC_DUTY_MAX);
		return 0;
	}
	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		mutex_lock(&data->lock);
		*val = data->manual ? 1 : 2;
		mutex_unlock(&data->lock);
		return 0;
	}
	return -EOPNOTSUPP;
}

static int fan_write(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long val)
{
	struct fan_data *data = dev_get_drvdata(dev);
	int ret = 0;
	u8 v;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	if (attr == hwmon_pwm_input) {
		if (val < 0 || val > 255)
			return -EINVAL;
		mutex_lock(&data->lock);
		data->duty = DIV_ROUND_CLOSEST(val * EC_DUTY_MAX, 255);
		data->last_write = jiffies;
		if (data->manual)
			ret = i2ec_write(EC_DCR4, data->duty);
		mutex_unlock(&data->lock);
		return ret;
	}

	if (attr == hwmon_pwm_enable) {
		if (val == 2) {
			mutex_lock(&data->lock);
			data->manual = false;
			mutex_unlock(&data->lock);
			cancel_delayed_work_sync(&data->hold);
			return 0;
		}
		if (val != 1)
			return -EINVAL;
		mutex_lock(&data->lock);
		if (!data->manual) {
			/* start from the duty the fan has now, until pwm1 is written */
			ret = i2ec_read(EC_DCR4, &v);
			if (!ret) {
				data->duty = min_t(u8, v, EC_DUTY_MAX);
				data->last_write = jiffies;
				data->manual = true;
				schedule_delayed_work(&data->hold, HOLD_INTERVAL);
			}
		}
		mutex_unlock(&data->lock);
		return ret;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops fan_hwmon_ops = {
	.is_visible = fan_is_visible,
	.read = fan_read,
	.write = fan_write,
};

static const struct hwmon_channel_info * const fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info fan_chip_info = {
	.ops = &fan_hwmon_ops,
	.info = fan_hwmon_info,
};

/* --- platform driver --- */

static int fan_probe(struct platform_device *pdev)
{
	struct fan_data *data;
	struct device *hwmon;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->hold, fan_hold);
	platform_set_drvdata(pdev, data);

	hwmon = devm_hwmon_device_register_with_info(&pdev->dev, "ideapad_fan", data,
						     &fan_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}

static void fan_remove(struct platform_device *pdev)
{
	struct fan_data *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	data->manual = false;
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->hold);
	/* the firmware now walks the duty back to its own target */
}

static struct platform_driver fan_driver = {
	.driver = { .name = DRV_NAME },
	.probe = fan_probe,
	.remove = fan_remove,
};

static const struct dmi_system_id fan_dmi_table[] = {
	{
		.ident = "Lenovo IdeaPad 3 14ALC6",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "82KT"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, fan_dmi_table);

static int __init fan_init(void)
{
	u8 hi = 0, lo = 0;
	int ret;

	if (!dmi_check_system(fan_dmi_table) && !force)
		return -ENODEV;

	ret = i2ec_read(EC_CHIPID1, &hi);
	if (!ret)
		ret = i2ec_read(EC_CHIPID2, &lo);
	if (ret)
		return ret;
	if (((hi << 8) | lo) != EC_CHIP_ID) {
		pr_info(DRV_NAME ": EC chip id 0x%02x%02x is not IT8227, not loading\n", hi, lo);
		return -ENODEV;
	}

	ret = platform_driver_register(&fan_driver);
	if (ret)
		return ret;
	fan_pdev = platform_device_register_simple(DRV_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(fan_pdev)) {
		platform_driver_unregister(&fan_driver);
		return PTR_ERR(fan_pdev);
	}
	return 0;
}

static void __exit fan_exit(void)
{
	platform_device_unregister(fan_pdev);
	platform_driver_unregister(&fan_driver);
}

module_init(fan_init);
module_exit(fan_exit);

MODULE_DESCRIPTION("Fan PWM control for the Lenovo IdeaPad 3 14ALC6 (ITE IT8227 EC)");
MODULE_LICENSE("GPL");
