// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016 Rockchip Electronics Co., Ltd
 * (C) Copyright 2020 Peter Robinson <pbrobinson at gmail.com>
 */

#include <common.h>
#include <dm.h>
#include <spl_gpio.h>
#include <syscon.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/grf_rk3399.h>
#include <asm/arch-rockchip/gpio.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/misc.h>
#include <power/regulator.h>

#define GRF_IO_VSEL_BT565_SHIFT 0
#define PMUGRF_CON0_VSEL_SHIFT 8

#ifndef CONFIG_SPL_BUILD
int board_early_init_f(void)
{
	struct udevice *regulator;
	int ret;

	ret = regulator_get_by_platname("vcc5v0_usb", &regulator);
	if (ret) {
		pr_debug("%s vcc5v0_usb init fail! ret %d\n", __func__, ret);
		goto out;
	}

	ret = regulator_set_enable(regulator, true);
	if (ret)
		pr_debug("%s vcc5v0-host-en-gpio set fail! ret %d\n", __func__, ret);

out:
	return 0;
}
#else

#define GPIO0_BASE	0xff720000

void led_setup(void)
{
	struct rockchip_gpio_regs * const gpio0 = (void *)GPIO0_BASE;

	// Light up the red LED
	// <&gpio0 RK_PA2 GPIO_ACTIVE_HIGH>;
	spl_gpio_output(gpio0, GPIO(BANK_A, 2), 1);
	// Turn off green LED (from kept reboot state)
	// <&gpio0 RK_PB3 GPIO_ACTIVE_HIGH>;
	spl_gpio_output(gpio0, GPIO(BANK_B, 3), 0);
}

#define GPIO1_BASE	0xff730000

void setup_gpio_pins(void)
{
	struct rockchip_gpio_regs * const gpio1 = (void *)GPIO1_BASE;

	// Turns the display power supply off
	// It is `always-on` in DT, but a `reboot` will not turn it off.
	// When turned on at boot, the current implementation doesn't play well.
	// <&gpio1 RK_PC6 GPIO_ACTIVE_HIGH>;
	spl_gpio_output(gpio1, GPIO(BANK_C, 6), 0);
}

#endif

#ifdef CONFIG_MISC_INIT_R
static void setup_iodomain(void)
{
	struct rk3399_grf_regs *grf =
	   syscon_get_first_range(ROCKCHIP_SYSCON_GRF);
	struct rk3399_pmugrf_regs *pmugrf =
	   syscon_get_first_range(ROCKCHIP_SYSCON_PMUGRF);

	/* BT565 is in 1.8v domain */
	rk_setreg(&grf->io_vsel, 1 << GRF_IO_VSEL_BT565_SHIFT);

	/* Set GPIO1 1.8v/3.0v source select to PMU1830_VOL */
	rk_setreg(&pmugrf->soc_con0, 1 << PMUGRF_CON0_VSEL_SHIFT);
}

int misc_init_r(void)
{
	const u32 cpuid_offset = 0x7;
	const u32 cpuid_length = 0x10;
	u8 cpuid[cpuid_length];
	int ret;

	setup_iodomain();

	ret = rockchip_cpuid_from_efuse(cpuid_offset, cpuid_length, cpuid);
	if (ret)
		return ret;

	ret = rockchip_cpuid_set(cpuid, cpuid_length);
	if (ret)
		return ret;

	return ret;
}
#endif
