/*
 * Copyright (C) 2010 MediaTek, Inc.
 *
 * Author: Terry Chang <terry.chang@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "hall.h"
#include <linux/wakelock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <soc/mediatek/hall.h>
#include <linux/of_gpio.h>
#define HALL_NAME	"mtk-hall"
#define MTK_KP_WAKESOURCE	/* this is for auto set wake up source */

static unsigned int hall_irqnr;
unsigned int hallgpiopin, halldebounce;
unsigned int hall_eint_type;
static bool hall_suspend;
static char call_status;
struct wake_lock hall_suspend_lock;	/* For suspend usage */

static int hall_pdrv_probe(struct platform_device *pdev);
static int hall_pdrv_remove(struct platform_device *pdev);
#ifndef USE_EARLY_SUSPEND
static int hall_pdrv_suspend(struct platform_device *pdev, pm_message_t state);
static int hall_pdrv_resume(struct platform_device *pdev);
#endif
extern void mt_irq_set_polarity(unsigned int irq, unsigned int polarity);

struct platform_device *fcoverPltFmDev;
struct pinctrl *fcoverctrl = NULL;
struct pinctrl_state *hall_pins_default;
struct pinctrl_state *hall_pins_cfg;

static struct switch_dev fcover_data;
static struct work_struct fcover_work;
static struct workqueue_struct *fcover_workqueue = NULL;
static DEFINE_SPINLOCK(fcover_lock);
static int new_fcover = HALL_FCOVER_OPEN;
static int fcover_close_flag = HALL_FCOVER_OPEN;
extern struct input_dev *kpd_accdet_dev;
bool hall_fcover_lid_closed = false;
static int initVal = 0;

static BLOCKING_NOTIFIER_HEAD(hall_notifier_list);

/**
 *      hall_register_client - register a client notifier
 *      @nb: notifier block to callback on events
 */
int hall_register_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&hall_notifier_list, nb);
}
EXPORT_SYMBOL(hall_register_client);

/**
 *      hall_unregister_client - unregister a client notifier
 *      @nb: notifier block to callback on events
 */
int hall_unregister_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&hall_notifier_list, nb);
}
EXPORT_SYMBOL(hall_unregister_client);

/**
 * hall_notifier_call_chain - notify clients of lid switch events
 *
 */
int hall_notifier_call_chain(unsigned long val, void *v)
{
	return blocking_notifier_call_chain(&hall_notifier_list, val, v);
}
EXPORT_SYMBOL_GPL(hall_notifier_call_chain);


static void hall_work_handler(struct work_struct *work)
{
	new_fcover = gpio_get_value(hallgpiopin);
	HALL_LOG("hall_work_handler new_fcover=%d , fcover_close_flag=%d",new_fcover, fcover_close_flag);

	if (initVal < 5)
	     initVal++;
	HALL_LOG("hall_work_handler init %d", initVal);
	if((fcover_close_flag != new_fcover) || initVal < 5)
	{
		spin_lock(&fcover_lock);
		fcover_close_flag = new_fcover;
		hall_fcover_lid_closed = (fcover_close_flag == HALL_FCOVER_CLOSE);
		spin_unlock(&fcover_lock);

		hall_notifier_call_chain(fcover_close_flag, NULL);

		input_report_switch(kpd_accdet_dev, SW_LID, (fcover_close_flag == HALL_FCOVER_CLOSE));
		input_sync(kpd_accdet_dev);
		if (fcover_close_flag == HALL_FCOVER_CLOSE)
		{
			HALL_LOG("=======HALL_FCOVER_CLOSE==== %d\n", (int)kpd_accdet_dev->swbit[SW_LID]);
		}
		else
		{
			HALL_LOG("=======HALL_FCOVER_OPEN==== %d\n", (int)kpd_accdet_dev->swbit[SW_LID]);
		}
		switch_set_state((struct switch_dev *)&fcover_data, fcover_close_flag);
	}
	if(new_fcover)
		irq_set_irq_type(hall_irqnr, IRQ_TYPE_LEVEL_LOW);
	else
		irq_set_irq_type(hall_irqnr, IRQ_TYPE_LEVEL_HIGH);

	gpio_set_debounce(hallgpiopin, halldebounce);
	enable_irq(hall_irqnr);
}

static irqreturn_t hall_fcover_eint_handler(int irq, void *dev_id)
{
	/* use _nosync to avoid deadlock */
	disable_irq_nosync(hall_irqnr);
	queue_work(fcover_workqueue, &fcover_work);	
	
	return IRQ_HANDLED;
}

static const struct of_device_id hall_of_match[] = {
	{.compatible = "mediatek, hall-eint"},
	{},
};

static struct platform_driver hall_pdrv = {
	.probe = hall_pdrv_probe,
	.remove = hall_pdrv_remove,
#ifndef USE_EARLY_SUSPEND
	.suspend = hall_pdrv_suspend,
	.resume = hall_pdrv_resume,
#endif
	.driver = {
		   .name = HALL_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = hall_of_match,
		   },
};

/*****************************************************************************************/
#if 0
int hall_dev_open(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations hall_dev_fops = {
	.owner = THIS_MODULE,
	//.unlocked_ioctl = hall_dev_ioctl,
	.open = hall_dev_open,
};

/*********************************************************************/
static struct miscdevice hall_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = HALL_NAME,
	.fops = &hall_dev_fops,
};
#endif

#if 0
static int hall_open(struct input_dev *dev)
{
	//hall_slide_qwerty_init();	/* API 1 for hall slide qwerty init settings */
	return 0;
}
#endif

static int hall_pdrv_probe(struct platform_device *pdev)
{
	int err = 0;
	int ret = 0;
	u32 ints[2] = { 0, 0 };
	u32 ints1[2] = { 0, 0 };
	struct device_node *node = NULL;

	HALL_LOG("hall probe start!!!");
	
	fcoverPltFmDev = pdev;

	/* initialize and register input device (/dev/input/eventX) */
//	hall_input_dev = input_allocate_device();
//	if (!hall_input_dev) {
//		printk("input allocate device fail.\n");
//		return -ENOMEM;
//	}

	__set_bit(EV_SW, kpd_accdet_dev->evbit);
	__set_bit(SW_LID, kpd_accdet_dev->swbit);

//	hall_input_dev->id.bustype = BUS_HOST;
//	hall_input_dev->name = HALL_NAME;
	//kpd_input_dev->id.vendor = 0x2454;
	//kpd_input_dev->id.product = 0x6500;
	//kpd_input_dev->id.version = 0x0010;
	//hall_input_dev->open = hall_open;

//	hall_input_dev->dev.parent = &pdev->dev;
//	r = input_register_device(hall_input_dev);
//	if (r) {
//		printk("register input device failed (%d)\n", r);
//		input_free_device(hall_input_dev);
//		return r;
//	}

	/* register device (/dev/mt6575-hall) */
//	hall_dev.parent = &pdev->dev;
//	r = misc_register(&hall_dev);
//	if (r) {
//		printk("register device failed (%d)\n", r);
//		input_unregister_device(hall_input_dev);
//		return r;
//	}
	wake_lock_init(&hall_suspend_lock, WAKE_LOCK_SUSPEND, "hall wakelock");

	fcover_workqueue = create_singlethread_workqueue("fcover");
	INIT_WORK(&fcover_work, hall_work_handler);

	fcover_close_flag = gpio_get_value(hallgpiopin);

	node = of_find_matching_node(node, hall_of_match);
	if (node) {
		of_property_read_u32_array(node, "debounce", ints, ARRAY_SIZE(ints));
		of_property_read_u32_array(node, "interrupts", ints1, ARRAY_SIZE(ints1));

		hallgpiopin = of_get_named_gpio(node, "deb-gpios", 0);//ints[0];
		halldebounce = ints[1];
		hall_eint_type = ints1[1];
		gpio_set_debounce(hallgpiopin, halldebounce);
		hall_irqnr = irq_of_parse_and_map(node, 0);
		ret = request_irq(hall_irqnr, (irq_handler_t)hall_fcover_eint_handler, IRQF_TRIGGER_NONE, "hall-eint", NULL);
		if (ret != 0) {
			HALL_LOG("EINT IRQ LINE NOT AVAILABLE");
		} else {
			HALL_LOG("hall set EINT finished, hall_irqnr=%d, hallgpiopin=%d, halldebounce=%d, hall_eint_type=%d",
				     hall_irqnr, hallgpiopin, halldebounce, hall_eint_type);
		}
	} else {
		HALL_LOG("%s can't find compatible node", __func__);
	}

	HALL_LOG("hall_fcover_eint_handler done..");
	
	fcover_data.name = "hall";
	fcover_data.index = 0;
	fcover_data.state = fcover_close_flag;
	
	err = switch_dev_register(&fcover_data);
	if(err)
	{
		printk(KERN_ERR HALL_TAG "switch_dev_register returned:%d", err);
//		return 1;
	}

	switch_set_state((struct switch_dev *)&fcover_data, fcover_close_flag);
	enable_irq_wake(hall_irqnr);
	enable_irq(hall_irqnr);
	HALL_LOG("====%s success=====." , __func__);
	return 0;
}

/* should never be called */
static int hall_pdrv_remove(struct platform_device *pdev)
{
	return 0;
}

#ifndef USE_EARLY_SUSPEND
static int hall_pdrv_suspend(struct platform_device *pdev, pm_message_t state)
{
	hall_suspend = true;
#ifdef MTK_KP_WAKESOURCE
	if (call_status == 2) {
		HALL_LOG("hall_pdrv_suspend wake up source enable!! (%d, %d)", hall_suspend, call_status);
	} else {
		kpd_wakeup_src_setting(0);
		HALL_LOG("hall_pdrv_suspend wake up source disable!! (%d, %d)", hall_suspend, call_status);
	}
#endif
	HALL_LOG("suspend!! (%d)", hall_suspend);
	return 0;
}

static int hall_pdrv_resume(struct platform_device *pdev)
{
	hall_suspend = false;
#ifdef MTK_KP_WAKESOURCE
	if (call_status == 2) {
		HALL_LOG("hall_pdrv_resume wake up source enable!! (%d, %d)", hall_suspend, call_status);
	} else {
		HALL_LOG("hall_pdrv_resume wake up source resume!! (%d, %d)", hall_suspend, call_status);
		kpd_wakeup_src_setting(1);
	}
#endif
	HALL_LOG("resume!! (%d)", hall_suspend);
	return 0;
}
#else
#define hall_pdrv_suspend NULL
#define hall_pdrv_resume NULL
#endif

#ifdef USE_EARLY_SUSPEND
static void hall_early_suspend(struct early_suspend *h)
{
	hall_suspend = true;
#ifdef MTK_KP_WAKESOURCE
	if (call_status == 2) {
		HALL_LOG("hall_early_suspend wake up source enable!! (%d, %d)", hall_suspend, call_status);
	} else {
		/* hall_wakeup_src_setting(0); */
		HALL_LOG("hall_early_suspend wake up source disable!! (%d), %d", hall_suspend, call_status);
	}
#endif
	HALL_LOG("early suspend!! (%d)", hall_suspend);
}

static void hall_early_resume(struct early_suspend *h)
{
	hall_suspend = false;
#ifdef MTK_KP_WAKESOURCE
	if (call_status == 2) {
		HALL_LOG("hall_early_resume wake up source resume!! (%d, %d)", hall_suspend, call_status);
	} else {
		HALL_LOG("hall_early_resume wake up source enable!! (%d, %d)", hall_suspend, call_status);
		/* hall_wakeup_src_setting(1); */
	}
#endif
	HALL_LOG("early resume!! (%d)", hall_suspend);
}

static struct early_suspend hall_early_suspend_desc = {
	//.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = hall_early_suspend,
	.resume = hall_early_resume,
};
#endif

static int __init hall_mod_init(void)
{
	int r;

	r = platform_driver_register(&hall_pdrv);
	if (r) {
		printk(KERN_ERR HALL_TAG "register driver failed (%d)", r);
		return r;
	}
#ifdef USE_EARLY_SUSPEND
	register_early_suspend(&hall_early_suspend_desc);
#endif

	return 0;
}

/* should never be called */
static void __exit hall_mod_exit(void)
{
}

module_init(hall_mod_init);
module_exit(hall_mod_exit);
MODULE_AUTHOR("yucong.xiong <yucong.xiong@mediatek.com>");
MODULE_DESCRIPTION("MTK Keypad (hall) Driver v0.4");
MODULE_LICENSE("GPL");
