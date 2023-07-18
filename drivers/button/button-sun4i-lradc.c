// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Samuel Dionne-Riel
 *
 * Based on the following patch:
 *
 * > Copyright (C) 2020 Ondrej Jirman <megous@megous.com>
 * > https://megous.com/git/u-boot/commit/?h=opi-v2020.01&id=0ab6225154c3d8b74f06fb3b181b52a9a64b4602
 *
 * Loosely based on button-adc.c:
 *
 * > Copyright (C) 2021 Samsung Electronics Co., Ltd.
 * >  	http://www.samsung.com
 * > Author: Marek Szyprowski <m.szyprowski@samsung.com>
 */

#include <common.h>
#include <asm/io.h>
#include <button.h>
#include <log.h>
#include <dm.h>
#include <dm/lists.h>
#include <dm/of_access.h>
#include <dm/uclass-internal.h>

#define THRESHOLD 100000

#define LRADC_BASE		0x1c21800

#define LRADC_CTRL		(LRADC_BASE + 0x00)
#define LRADC_INTC		(LRADC_BASE + 0x04)
#define LRADC_INTS		(LRADC_BASE + 0x08)
#define LRADC_DATA0		(LRADC_BASE + 0x0c)
#define LRADC_DATA1		(LRADC_BASE + 0x10)

/* LRADC_CTRL bits */
#define FIRST_CONVERT_DLY(x)	((x) << 24) /* 8 bits */
#define CHAN_SELECT(x)		((x) << 22) /* 2 bits */
#define CONTINUE_TIME_SEL(x)	((x) << 16) /* 4 bits */
#define KEY_MODE_SEL(x)		((x) << 12) /* 2 bits */
#define LEVELA_B_CNT(x)		((x) << 8)  /* 4 bits */
#define HOLD_KEY_EN(x)		((x) << 7)
#define HOLD_EN(x)		((x) << 6)
#define LEVELB_VOL(x)		((x) << 4)  /* 2 bits */
#define SAMPLE_RATE(x)		((x) << 2)  /* 2 bits */
#define ENABLE(x)		((x) << 0)

/* LRADC_INTC and LRADC_INTS bits */
#define CHAN1_KEYUP_IRQ		BIT(12)
#define CHAN1_ALRDY_HOLD_IRQ	BIT(11)
#define CHAN1_HOLD_IRQ		BIT(10)
#define	CHAN1_KEYDOWN_IRQ	BIT(9)
#define CHAN1_DATA_IRQ		BIT(8)
#define CHAN0_KEYUP_IRQ		BIT(4)
#define CHAN0_ALRDY_HOLD_IRQ	BIT(3)
#define CHAN0_HOLD_IRQ		BIT(2)
#define	CHAN0_KEYDOWN_IRQ	BIT(1)
#define CHAN0_DATA_IRQ		BIT(0)


/**
 * struct button_sun4i_lradc_priv - private data for button-adc driver.
 *
 * @voltage: maximum uV value to consider button as pressed.
 */
struct button_sun4i_lradc_priv {
	int voltage;
};

void lradc_enable(void)
{
	// aldo3 is always on and defaults to 3V

	writel(0xffffffff, LRADC_INTS);
	writel(0, LRADC_INTC);

	/*
	 * Set sample time to 4 ms / 250 Hz. Wait 2 * 4 ms for key to
	 * stabilize on press, wait (1 + 1) * 4 ms for key release
	 */
	writel(FIRST_CONVERT_DLY(0) | LEVELA_B_CNT(0) | HOLD_EN(0) |
		SAMPLE_RATE(0) | ENABLE(1), LRADC_CTRL);

}

void lradc_disable(void)
{
	writel(0xffffffff, LRADC_INTS);
	writel(0, LRADC_INTC);

	/* Disable lradc, leave other settings unchanged */
	writel(FIRST_CONVERT_DLY(2) | LEVELA_B_CNT(1) | HOLD_EN(1) |
		SAMPLE_RATE(2), LRADC_CTRL);
}


static enum button_state_t button_sun4i_lradc_get_state(struct udevice *dev)
{
	struct button_sun4i_lradc_priv *priv = dev_get_priv(dev);

	uint32_t uV;
	uint32_t vref = 3000000 * 2 / 3;

	uV = readl(LRADC_DATA0) & 0x3f;
	uV = uV * vref / 63;

	return (uV >= (priv->voltage - THRESHOLD) && uV < priv->voltage) ? BUTTON_ON : BUTTON_OFF;
}

static int button_sun4i_lradc_of_to_plat(struct udevice *dev)
{
	struct button_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct button_sun4i_lradc_priv *priv = dev_get_priv(dev);
	u32 voltage;
	int ret;

	/* Ignore the top-level button node */
	if (!uc_plat->label)
		return 0;

	ret = ofnode_read_u32(dev_ofnode(dev), "voltage",
			      &voltage);
	if (ret)
		return ret;

	priv->voltage = voltage;

	return ret;
}

static int button_sun4i_lradc_bind(struct udevice *parent)
{
	struct udevice *dev;
	ofnode node;
	int ret;

	lradc_enable();

	dev_for_each_subnode(node, parent) {
		struct button_uc_plat *uc_plat;
		const char *label;

		label = ofnode_read_string(node, "label");
		if (!label) {
			debug("%s: node %s has no label\n", __func__,
			      ofnode_get_name(node));
			return -EINVAL;
		}
		ret = device_bind_driver_to_node(parent, "button_sun4i_lradc",
						 ofnode_get_name(node),
						 node, &dev);
		if (ret)
			return ret;
		uc_plat = dev_get_uclass_plat(dev);
		uc_plat->label = label;
	}

	return 0;
}

static const struct button_ops button_sun4i_lradc_ops = {
	.get_state	= button_sun4i_lradc_get_state,
};

static const struct udevice_id button_sun4i_lradc_ids[] = {
	{ .compatible = "allwinner,sun8i-a83t-r-lradc" },
	{ }
};

U_BOOT_DRIVER(button_sun4i_lradc) = {
	.name		= "button_sun4i_lradc",
	.id		= UCLASS_BUTTON,
	.of_match	= button_sun4i_lradc_ids,
	.ops		= &button_sun4i_lradc_ops,
	.priv_auto	= sizeof(struct button_sun4i_lradc_priv),
	.bind		= button_sun4i_lradc_bind,
	.of_to_plat	= button_sun4i_lradc_of_to_plat,
};
