/*
 *
 * Copyright (C) 2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/input-polldev.h>
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/mag3110.h>
#include <linux/earlysuspend.h>
#include <linux/regulator/machine.h>

#define MAG3110_DRV_NAME       "mag3110"
#define MAG3110_ID		0xC4
#define MAG3110_XYZ_DATA_LEN	6
#define MAG3110_STATUS_ZYXDR	0x08

#define MAG3110_AC_MASK         (0x01)
#define MAG3110_AC_OFFSET       0
#define MAG3110_DR_MODE_MASK    (0x7 << 5)
#define MAG3110_DR_MODE_OFFSET  5
#define MAG3110_IRQ_USED   0

#define POLL_INTERVAL_MAX	500
#define POLL_INTERVAL		100
#define INT_TIMEOUT   1000
/* register enum for mag3110 registers */
enum {
	MAG3110_DR_STATUS = 0x00,
	MAG3110_OUT_X_MSB,
	MAG3110_OUT_X_LSB,
	MAG3110_OUT_Y_MSB,
	MAG3110_OUT_Y_LSB,
	MAG3110_OUT_Z_MSB,
	MAG3110_OUT_Z_LSB,
	MAG3110_WHO_AM_I,

	MAG3110_OFF_X_MSB,
	MAG3110_OFF_X_LSB,
	MAG3110_OFF_Y_MSB,
	MAG3110_OFF_Y_LSB,
	MAG3110_OFF_Z_MSB,
	MAG3110_OFF_Z_LSB,

	MAG3110_DIE_TEMP,

	MAG3110_CTRL_REG1 = 0x10,
	MAG3110_CTRL_REG2,
};
enum {
	MAG_STANDBY,
	MAG_ACTIVED
};
struct mag3110_data {
	struct i2c_client *client;
	struct input_polled_dev *poll_dev;
	struct device *hwmon_dev;
	struct mag3110_platform_data *pdata;
	wait_queue_head_t waitq;
	bool data_ready;
	u8 ctl_reg1;
	int active;
	int position;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend mag_early_suspend;
#endif
};
static short MAGHAL[8][3][3] =
{
	{{ 0,  1,  0}, {-1,  0,  0}, {0, 0,  1}},
	{{ 1,  0,  0}, { 0,  1,  0}, {0, 0,  1}},
	{{ 0, -1,  0}, { 1,  0,  0}, {0, 0,  1}},
	{{-1,  0,  0}, { 0, -1,  0}, {0, 0,  1}},
	
	{{ 0,  1,  0}, { 1,  0,  0}, {0, 0,  -1}},
	{{ 1,  0,  0}, { 0, -1,  0}, {0, 0,  -1}},
	{{ 0, -1,  0}, {-1,  0,  0}, {0, 0,  -1}},
	{{-1,  0,  0}, { 0,  1,  0}, {0, 0,  -1}},

};

static void mag3110_early_suspend(struct early_suspend *handler);
static void mag3110_early_resume(struct early_suspend *handler);

static struct regulator *compass_1v8;
static struct regulator *compass_vcc;

static bool power_enabled;

static struct mag3110_data *this_data;
/*!
 * This function do one mag3110 register read.
 */
static DEFINE_MUTEX(mag3110_lock);
static int mag3110_adjust_position(short *x, short *y, short *z)
{
   short rawdata[3],data[3];
   int i,j;
   int position = this_data->position ;
   if(position < 0 || position > 7 )
   		position = 0;
   rawdata [0] = *x ; rawdata [1] = *y ; rawdata [2] = *z ;  
   for(i = 0; i < 3 ; i++)
   {
   	data[i] = 0;
   	for(j = 0; j < 3; j++)
		data[i] += rawdata[j] * MAGHAL[position][i][j];
   }
   *x = data[0];
   *y = data[1];
   *z = data[2];
   return 0;
}
static int mag3110_read_reg(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

/*!
 * This function do one mag3110 register write.
 */
static int mag3110_write_reg(struct i2c_client *client, u8 reg, char value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, reg, value);
	if (ret < 0)
		dev_err(&client->dev, "i2c write failed\n");
	return ret;
}

/*!
 * This function do multiple mag3110 registers read.
 */
static int mag3110_read_block_data(struct i2c_client *client, u8 reg,
				   int count, u8 *addr)
{
	if (i2c_smbus_read_i2c_block_data
	     (client, reg, count, addr) < count) {
		dev_err(&client->dev, "i2c block read failed\n");
		return -1;
	}

	return count;
}

/*
 * Initialization function
 */
static int mag3110_init_client(struct i2c_client *client)
{
	int val, ret;

	/* enable automatic resets */
	val = 0x80;
	ret = mag3110_write_reg(client, MAG3110_CTRL_REG2, val);

	/* set default data rate to 10HZ */
	val = mag3110_read_reg(client, MAG3110_CTRL_REG1);
	val |= (0x0 << MAG3110_DR_MODE_OFFSET);
	ret = mag3110_write_reg(client, MAG3110_CTRL_REG1, val);

	return ret;
}

static void mag3110_enable(bool en)
{
	if (power_enabled == en)
		return;

	if (en) {
		if (!IS_ERR(compass_1v8)) {
			regulator_enable(compass_1v8);
		}
		if (!IS_ERR(compass_vcc)) {
			regulator_enable(compass_vcc);
		}
	} else {
		if (!IS_ERR(compass_1v8)) {
			regulator_disable(compass_1v8);
		}
		if (!IS_ERR(compass_vcc)) {
			regulator_disable(compass_vcc);
		}
	}

	power_enabled = en;
}

/***************************************************************
*
* read sensor data from mag3110
*
***************************************************************/
static int mag3110_read_data(short *x, short *y, short *z)
{
	struct mag3110_data *data;
	int retry = 3;
	u8 tmp_data[MAG3110_XYZ_DATA_LEN];
    int result;
	if (!this_data || this_data->active == MAG_STANDBY)
		return -EINVAL;

	data = this_data;
#if MAG3110_IRQ_USED
	if (!wait_event_interruptible_timeout
	    (data->waitq, data->data_ready != 0,
	     msecs_to_jiffies(INT_TIMEOUT))) {
		dev_dbg(&data->client->dev, "interrupt not received\n");
		return -ETIME;
	}
#else
    do {
		msleep(1);
		result = i2c_smbus_read_byte_data(data->client,
					     MAG3110_DR_STATUS);
		retry --; 
	} while (!(result & MAG3110_STATUS_ZYXDR) && retry >0);
	/* Clear data_ready flag after data is read out */
	if(retry == 0){
		printk("magd wait data ready timeout....\n");
		return -EINVAL;
	}
#endif

	data->data_ready = 0;

	if (mag3110_read_block_data(data->client,
			MAG3110_OUT_X_MSB, MAG3110_XYZ_DATA_LEN, tmp_data) < 0)
		return -1;

	*x = ((tmp_data[0] << 8) & 0xff00) | tmp_data[1];
	*y = ((tmp_data[2] << 8) & 0xff00) | tmp_data[3];
	*z = ((tmp_data[4] << 8) & 0xff00) | tmp_data[5];

	return 0;
}

static void report_abs(void)
{
	struct input_dev *idev;
	short x, y, z;
    
	mutex_lock(&mag3110_lock);
	if (mag3110_read_data(&x, &y, &z) != 0)
		goto out;
	mag3110_adjust_position(&x, &y, &z);
	idev = this_data->poll_dev->input;
	input_report_abs(idev, ABS_X, x);
	input_report_abs(idev, ABS_Y, y);
	input_report_abs(idev, ABS_Z, z);
	input_sync(idev);
out:
	mutex_unlock(&mag3110_lock);
}

static void mag3110_dev_poll(struct input_polled_dev *dev)
{
	report_abs();
}

#if MAG3110_IRQ_USED
static irqreturn_t mag3110_irq_handler(int irq, void *dev_id)
{
	this_data->data_ready = 1;
	wake_up_interruptible(&this_data->waitq);

	return IRQ_HANDLED;
}
#endif
static ssize_t mag3110_enable_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i2c_client *client;
	int val;
	mutex_lock(&mag3110_lock);
	client = this_data->client;
	val = mag3110_read_reg(client, MAG3110_CTRL_REG1) & MAG3110_AC_MASK;
	
	mutex_unlock(&mag3110_lock);
	return sprintf(buf, "%d\n", val);
}

static ssize_t mag3110_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct i2c_client *client;
	int reg, ret, enable;
	u8 tmp_data[MAG3110_XYZ_DATA_LEN];

	enable = simple_strtoul(buf, NULL, 10);
	mutex_lock(&mag3110_lock);
    client = this_data->client;
	reg = mag3110_read_reg(client, MAG3110_CTRL_REG1);
	if (enable &&  this_data->active == MAG_STANDBY){
		reg |= MAG3110_AC_MASK;
		ret = mag3110_write_reg(client, MAG3110_CTRL_REG1, reg);
		if(!ret)
			this_data->active = MAG_ACTIVED;
		printk("mag3110 set active\n");
	}
	else if(!enable && this_data->active == MAG_ACTIVED){
		reg &= ~MAG3110_AC_MASK;
		ret = mag3110_write_reg(client, MAG3110_CTRL_REG1, reg);
		if(!ret)
			this_data->active = MAG_STANDBY;
		printk("mag3110 set inactive\n");
		
	}		
	
	if (this_data->active == MAG_ACTIVED) {
		msleep(100);
		/* Read out MSB data to clear interrupt flag automatically */
		 mag3110_read_block_data(client, MAG3110_OUT_X_MSB,
					 MAG3110_XYZ_DATA_LEN, tmp_data);
	}
	mutex_unlock(&mag3110_lock);
	return count;
}

static DEVICE_ATTR(enable, S_IWUSR | S_IRUGO,
		   mag3110_enable_show, mag3110_enable_store);

static ssize_t mag3110_dr_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct i2c_client *client;
	int val;
    
	client = this_data->client;
	val = (mag3110_read_reg(client, MAG3110_CTRL_REG1)
		    & MAG3110_DR_MODE_MASK) >> MAG3110_DR_MODE_OFFSET;

	return sprintf(buf, "%d\n", val);
}

static ssize_t mag3110_dr_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct i2c_client *client ;
	int reg, ret;
	unsigned long val;

	/* This must be done when mag3110 is disabled */
	if ((strict_strtoul(buf, 10, &val) < 0) || (val > 7))
		return -EINVAL;
	
	client = this_data->client;
	reg = mag3110_read_reg(client, MAG3110_CTRL_REG1) &
		~MAG3110_DR_MODE_MASK;
	reg |= (val << MAG3110_DR_MODE_OFFSET);
	/* MAG3110_CTRL_REG1 bit 5-7: data rate mode */
	ret = mag3110_write_reg(client, MAG3110_CTRL_REG1, reg);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR(dr_mode, S_IWUSR | S_IRUGO,
		   mag3110_dr_mode_show, mag3110_dr_mode_store);

static ssize_t mag3110_position_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int val;
	mutex_lock(&mag3110_lock);
	val = this_data->position;
	mutex_unlock(&mag3110_lock);
	return sprintf(buf, "%d\n", val);
}
static ssize_t mag3110_position_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int  position;
	position = simple_strtoul(buf, NULL, 10);
	mutex_lock(&mag3110_lock);
    this_data->position = position;
	mutex_unlock(&mag3110_lock);
	return count;
}
static DEVICE_ATTR(position, S_IWUSR | S_IRUGO,
		   mag3110_position_show, mag3110_position_store);

static struct attribute *mag3110_attributes[] = {
	&dev_attr_enable.attr,
	&dev_attr_dr_mode.attr,
	&dev_attr_position.attr,
	NULL
};

static const struct attribute_group mag3110_attr_group = {
	.attrs = mag3110_attributes,
};

static int __devinit mag3110_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter;
	struct input_dev *idev;
	struct mag3110_data *data;
	int ret = 0;

	adapter = to_i2c_adapter(client->dev.parent);
	if (!i2c_check_functionality(adapter,
				     I2C_FUNC_SMBUS_BYTE |
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EIO;

	dev_info(&client->dev, "check mag3110 chip ID\n");
	ret = mag3110_read_reg(client, MAG3110_WHO_AM_I);

	if (MAG3110_ID != ret) {
		dev_err(&client->dev,
			"read chip ID 0x%x is not equal to 0x%x!\n", ret,
			MAG3110_ID);
		return -EINVAL;
	}
	data = kzalloc(sizeof(struct mag3110_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdata = (struct mag3110_platform_data*) client->dev.platform_data;
	if (!data->pdata) {
		printk(KERN_ERR "mag3110_probe: No platform data !!\n");
		ret = -ENODEV;
		goto error_free_data;
	}

	/* regulators */
	compass_1v8 = regulator_get(&client->dev, "COMPASS_1V8");
	if (IS_ERR(compass_1v8))
		dev_dbg(&client->dev, "no COMPASS_1V8 for this compass\n");

	compass_vcc = regulator_get(&client->dev, "COMPASS_VCC");
	if (IS_ERR(compass_vcc))
		dev_dbg(&client->dev, "no COMPASS_VCC for this compass\n");

	mag3110_enable(true);

	data->client = client;
	i2c_set_clientdata(client, data);
	/* Init queue */
	init_waitqueue_head(&data->waitq);

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		dev_err(&client->dev, "hwmon register failed!\n");
		ret = PTR_ERR(data->hwmon_dev);
		goto error_rm_dev_sysfs;
	}

	/*input poll device register */
	data->poll_dev = input_allocate_polled_device();
	if (!data->poll_dev) {
		dev_err(&client->dev, "alloc poll device failed!\n");
		ret = -ENOMEM;
		goto error_rm_hwmon_dev;
	}
	data->poll_dev->poll = mag3110_dev_poll;
	data->poll_dev->poll_interval = POLL_INTERVAL;
	data->poll_dev->poll_interval_max = POLL_INTERVAL_MAX;
	idev = data->poll_dev->input;
	idev->name = MAG3110_DRV_NAME;
	idev->id.bustype = BUS_I2C;
	idev->evbit[0] = BIT_MASK(EV_ABS);
	input_set_abs_params(idev, ABS_X, -15000, 15000, 0, 0);
	input_set_abs_params(idev, ABS_Y, -15000, 15000, 0, 0);
	input_set_abs_params(idev, ABS_Z, -15000, 15000, 0, 0);
	ret = input_register_polled_device(data->poll_dev);
	if (ret) {
		dev_err(&client->dev, "register poll device failed!\n");
		goto error_free_poll_dev;
	}
    
	/*create device group in sysfs as user interface */
	ret = sysfs_create_group(&idev->dev.kobj, &mag3110_attr_group);
	if (ret) {
		dev_err(&client->dev, "create device file failed!\n");
		ret = -EINVAL;
		goto error_rm_poll_dev;
	}
	/* set irq type to edge rising */
#if MAG3110_IRQ_USED
	ret = request_irq(data->pdata->irq, mag3110_irq_handler,
			  IRQF_TRIGGER_RISING, client->dev.driver->name, idev);
	if (ret < 0) {
		dev_err(&client->dev, "failed to register irq %d!\n",
			data->pdata->irq);
		goto error_rm_dev_sysfs;
	}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->mag_early_suspend.suspend = mag3110_early_suspend;
	data->mag_early_suspend.resume = mag3110_early_resume;
	register_early_suspend(&data->mag_early_suspend);
#endif

	/* Initialize mag3110 chip */
	mag3110_init_client(client);
	this_data = data;
	this_data->active = MAG_STANDBY;
	this_data->position = data->pdata->position;
	dev_info(&client->dev, "mag3110 is probed\n");
	return 0;
error_rm_dev_sysfs:
	sysfs_remove_group(&client->dev.kobj, &mag3110_attr_group);
error_rm_poll_dev:
	input_unregister_polled_device(data->poll_dev);
error_free_poll_dev:
	input_free_polled_device(data->poll_dev);
error_rm_hwmon_dev:
	hwmon_device_unregister(data->hwmon_dev);

error_free_data:
	kfree(data);
	this_data = NULL;

	return ret;
}

static int __devexit mag3110_remove(struct i2c_client *client)
{
	struct mag3110_data *data;
	int ret;

	data = i2c_get_clientdata(client);

	data->ctl_reg1 = mag3110_read_reg(client, MAG3110_CTRL_REG1);
	ret = mag3110_write_reg(client, MAG3110_CTRL_REG1,
				    data->ctl_reg1 & ~MAG3110_AC_MASK);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->mag_early_suspend);
#endif
	free_irq(client->irq, data);
	input_unregister_polled_device(data->poll_dev);
	input_free_polled_device(data->poll_dev);
	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &mag3110_attr_group);
	mag3110_enable(false);
	if (!IS_ERR(compass_1v8)) {
		regulator_put(compass_1v8);
	}
	if (!IS_ERR(compass_vcc)) {
		regulator_put(compass_vcc);
	}

	kfree(data);
	this_data = NULL;

	return ret;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mag3110_early_suspend(struct early_suspend *handler)
{
	int ret = 0;
	struct mag3110_data *data = this_data;
    if(data->active == MAG_ACTIVED){
		data->ctl_reg1 = mag3110_read_reg(data->client, MAG3110_CTRL_REG1);
		ret = mag3110_write_reg(data->client, MAG3110_CTRL_REG1,
					   data->ctl_reg1 & ~MAG3110_AC_MASK);
    }
}

static void mag3110_early_resume(struct early_suspend *handler)
{
	int ret = 0;
	u8 tmp_data[MAG3110_XYZ_DATA_LEN];
	struct mag3110_data *data = this_data;
    if(data->active == MAG_ACTIVED){
		ret = mag3110_write_reg(data->client, MAG3110_CTRL_REG1,
					   data->ctl_reg1);

		if (data->ctl_reg1 & MAG3110_AC_MASK) {
			/* Read out MSB data to clear interrupt flag automatically */
			mag3110_read_block_data(data->client, MAG3110_OUT_X_MSB,
						MAG3110_XYZ_DATA_LEN, tmp_data);
		}
    }
}
#endif				/* CONFIG_HAS_EARLYSUSPEND */

#ifdef CONFIG_PM
static int mag3110_suspend(struct i2c_client *client, pm_message_t mesg)
{
	if (power_enabled) {
		if (!IS_ERR(compass_1v8)) {
			regulator_disable(compass_1v8);
		}
		if (!IS_ERR(compass_vcc)) {
			regulator_disable(compass_vcc);
		}
	}

	return 0;
}

static int mag3110_resume(struct i2c_client *client)
{
	if (power_enabled) {
		if (!IS_ERR(compass_1v8)) {
			regulator_enable(compass_1v8);
		}
		if (!IS_ERR(compass_vcc)) {
			regulator_enable(compass_vcc);
		}
	}

	return 0;
}

#else
#define mag3110_suspend        NULL
#define mag3110_resume         NULL
#endif				/* CONFIG_PM */

static const struct i2c_device_id mag3110_id[] = {
	{MAG3110_DRV_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, mag3110_id);
static struct i2c_driver mag3110_driver = {
	.driver = {.name = MAG3110_DRV_NAME,
		   .owner = THIS_MODULE,},
	.suspend = mag3110_suspend,
	.resume = mag3110_resume,
	.probe = mag3110_probe,
	.remove = __devexit_p(mag3110_remove),
	.id_table = mag3110_id,
};

static int __init mag3110_init(void)
{
	return i2c_add_driver(&mag3110_driver);
}

static void __exit mag3110_exit(void)
{
	i2c_del_driver(&mag3110_driver);
}

module_init(mag3110_init);
module_exit(mag3110_exit);
MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Freescale mag3110 3-axis magnetometer driver");
MODULE_LICENSE("GPL");
