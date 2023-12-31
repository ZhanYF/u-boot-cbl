// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016 Rockchip Electronics Co., Ltd
 * (C) Copyright 2022 Peter Robinson <pbrobinson at gmail.com>
 */

#include <common.h>
#include <dm.h>
#include <init.h>
#include <fdt_support.h>
#include <linux/libfdt.h>
#include <syscon.h>
#include <led.h>
#include <button.h>
#include <asm/io.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/grf_rk3399.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/misc.h>
#include <hang.h>
#include <sysreset.h>
#include <power/regulator.h>
#include <linux/delay.h>
#include <power/rk8xx_pmic.h>
#include <power/pmic.h>

#define GRF_IO_VSEL_BT565_GPIO2AB 1
#define GRF_IO_VSEL_AUDIO_GPIO3D4A 2
#define PMUGRF_CON0_VOLSEL_SHIFT 8
#define PMUGRF_CON0_VOL_SHIFT 9

static void setup_iodomain(void)
{
	struct rk3399_grf_regs *grf =
	   syscon_get_first_range(ROCKCHIP_SYSCON_GRF);
	struct rk3399_pmugrf_regs *pmugrf =
	   syscon_get_first_range(ROCKCHIP_SYSCON_PMUGRF);

	/* BT565 is in 1.8v domain */
	rk_setreg(&grf->io_vsel,
		  GRF_IO_VSEL_BT565_GPIO2AB | GRF_IO_VSEL_AUDIO_GPIO3D4A);

	/* Set GPIO1 1.8v/3.0v source select to PMU1830_VOL */
	rk_setreg(&pmugrf->soc_con0, 1 << PMUGRF_CON0_VOLSEL_SHIFT | 1 << PMUGRF_CON0_VOL_SHIFT);
}

#ifdef CONFIG_MISC_INIT_R
int misc_init_r(void)
{
	const u32 cpuid_offset = 0x7;
	const u32 cpuid_length = 0x10;
	u8 cpuid[cpuid_length];
	int ret;

	ret = rockchip_cpuid_from_efuse(cpuid_offset, cpuid_length, cpuid);
	if (ret)
		return ret;

	ret = rockchip_cpuid_set(cpuid, cpuid_length);
	if (ret)
		return ret;

	return ret;
}
#endif

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, struct bd_info *bd)
{
#ifdef CONFIG_ROCKCHIP_EXTERNAL_TPL
	int rc = fdt_find_and_setprop(blob, "/memory-controller",
					"status", "okay", sizeof("okay"), 1);
	if (rc)
		printf("Unable to enable DMC err=%s\n", fdt_strerror(rc));
#endif

	return 0;
}
#endif

#ifdef CONFIG_SPL_BUILD

#define VB_MON_REG              0x21
#define THERMAL_REG             0x22
#define SUP_STS_REG             0xa0
#define USB_CTRL_REG            0xa1
#define CHRG_CTRL_REG1          0xa3
#define CHRG_CTRL_REG2          0xa4
#define CHRG_CTRL_REG3          0xa5
#define BAT_CTRL_REG            0xa6
#define BAT_HTS_TS_REG          0xa8
#define BAT_LTS_TS_REG          0xa9
#define TS_CTRL_REG             0xac
#define ADC_CTRL_REG            0xad
#define GGCON_REG               0xb0
#define GGSTS_REG               0xb1
#define ZERO_CUR_ADC_REGH       0xb2
#define ZERO_CUR_ADC_REGL       0xb3
#define BAT_CUR_AVG_REGH        0xbc
#define BAT_CUR_AVG_REGL        0xbd
#define TS_ADC_REGH             0xbe
#define TS_ADC_REGL             0xbf
#define RK818_TS2_ADC_REGH      0xc0
#define RK818_TS2_ADC_REGL      0xc1
#define RK816_USB_ADC_REGH      0xc0
#define RK816_USB_ADC_REGL      0xc1
#define BAT_OCV_REGH            0xc2
#define BAT_OCV_REGL            0xc3
#define BAT_VOL_REGH            0xc4
#define BAT_VOL_REGL            0xc5
#define RELAX_ENTRY_THRES_REGH  0xc6
#define RELAX_ENTRY_THRES_REGL  0xc7
#define RELAX_EXIT_THRES_REGH   0xc8
#define RELAX_EXIT_THRES_REGL   0xc9
#define RELAX_VOL1_REGH         0xca
#define RELAX_VOL1_REGL         0xcb
#define RELAX_VOL2_REGH         0xcc
#define RELAX_VOL2_REGL         0xcd
#define RELAX_CUR1_REGH         0xce
#define RELAX_CUR1_REGL         0xcf
#define RELAX_CUR2_REGH         0xd0
#define RELAX_CUR2_REGL         0xd1
#define CAL_OFFSET_REGH         0xd2
#define CAL_OFFSET_REGL         0xd3
#define NON_ACT_TIMER_CNT_REG   0xd4
#define VCALIB0_REGH            0xd5
#define VCALIB0_REGL            0xd6
#define VCALIB1_REGH            0xd7
#define VCALIB1_REGL            0xd8
#define IOFFSET_REGH            0xdd
#define IOFFSET_REGL            0xde

/* firmware data regs */
#define POFFSET_REG             0xed

/* SUP_STS_REG */
#define USB_EFF                 (1 << 0)
#define USB_EXIST               (1 << 1)
#define USB_CLIMIT_EN           (1 << 2)
#define USB_VLIMIT_EN           (1 << 3)
#define BAT_EXS                 (1 << 7)

#define CHARGE_OFF              0x00
#define DEAD_CHARGE             0x01
#define TRICKLE_CHARGE          0x02
#define CC_OR_CV                0x03
#define CHARGE_FINISH           0x04
#define USB_OVER_VOL            0x05
#define BAT_TMP_ERR             0x06
#define TIMER_ERR               0x07
#define BAT_STATUS_MSK          0x7
#define BAT_STATUS_OFF          4

/* VB_MON_REG */
#define PLUG_IN_STS             (1 << 6)

/* meaning of life... */
#define DEFAULT_POFFSET                 42
#define DEFAULT_COFFSET                 0x832
#define INVALID_COFFSET_MIN             0x780
#define INVALID_COFFSET_MAX             0x980

static int rk818_get_bat_vol(struct udevice *pmic)
{
        int val = pmic_reg_read(pmic, BAT_VOL_REGL)
		| pmic_reg_read(pmic, BAT_VOL_REGH) << 8;
        int vcalib0 = pmic_reg_read(pmic, VCALIB0_REGL)
		| pmic_reg_read(pmic, VCALIB0_REGH) << 8;
        int vcalib1 = pmic_reg_read(pmic, VCALIB1_REGL)
		| pmic_reg_read(pmic, VCALIB1_REGH) << 8;
	int diff = vcalib1 - vcalib0;
	if (diff == 0)
		diff = 1;

        int voltage_k = (4200 - 3000) * 1000 / diff;
        int voltage_b = 4200 - (voltage_k * vcalib1) / 1000;

        return voltage_k * val / 1000 + voltage_b;
}

static int rk818_get_bat_cur(struct udevice *pmic)
{
        int val = pmic_reg_read(pmic, BAT_CUR_AVG_REGL)
		| pmic_reg_read(pmic, BAT_CUR_AVG_REGH) << 8;

        if (val & 0x800)
                val -= 4096;

        return val * 2 * 1506 / 1000;
}

static const char* status_str[] = {
        [CHARGE_OFF] = "off",
        [DEAD_CHARGE] = "dead",
        [TRICKLE_CHARGE] = "trickle",
        [CC_OR_CV] = "cc-cv",
        [CHARGE_FINISH] = "finished",
        [USB_OVER_VOL] = "usb-over-voltage",
        [BAT_TMP_ERR] = "bat-temp-error",
        [TIMER_ERR] = "timer-error",
};

static void rk818_calibrate(struct udevice *pmic)
{
	int ioffset = pmic_reg_read(pmic, IOFFSET_REGL)
		| pmic_reg_read(pmic, IOFFSET_REGH) << 8;

	int poffset = pmic_reg_read(pmic, POFFSET_REG);
	if (!poffset)
		poffset = DEFAULT_POFFSET;

        int coffset = poffset + ioffset;
        if (coffset < INVALID_COFFSET_MIN || coffset > INVALID_COFFSET_MAX)
                coffset = DEFAULT_COFFSET;

	//printf("Setting coffset=%d (ioffset=%d)\n", coffset, ioffset);

        pmic_reg_write(pmic, CAL_OFFSET_REGH, (coffset >> 8) & 0xff);
        pmic_reg_write(pmic, CAL_OFFSET_REGL, coffset & 0xff);

	mdelay(300);

#if 0
	/*
	 * calibrate rint
	 *
	 * - adc sample period is about 25-30ms
	 */


	/* disable charger */
        pmic_reg_write(pmic, CHRG_CTRL_REG1, 0x71);

	mdelay(1000);

	int vol_dis = rk818_get_bat_vol(pmic);
	int cur_dis = rk818_get_bat_cur(pmic);

	/* enable charger */
        pmic_reg_write(pmic, CHRG_CTRL_REG1, 0xf1);

	mdelay(1000);

	int vol_en = rk818_get_bat_vol(pmic);
	int cur_en = rk818_get_bat_cur(pmic);

	int rint = 150;
        if (abs(cur_en - cur_dis) > 150)
		rint = 1000 * (vol_en - vol_dis) / (cur_en - cur_dis);

	printf("vol_dis=%d cur_dis=%d vol_en=%d cur_en=%d : rint=%d\n",
	       vol_dis, cur_dis, vol_en, cur_en, rint);
#endif
}

static void blink_led(struct udevice *l, int times, int period)
{
	if (!l)
		return;

	for (int i = 0; i < times; i++) {
		led_set_state(l, LEDST_ON);
		mdelay(period / 2);

		led_set_state(l, LEDST_OFF);
		mdelay(period / 2);
	}
}

struct ppp_power_state {
	struct udevice *vol_up;
	struct udevice *vol_dn;
	struct udevice *power;
	int vol_up_pressed;
	int vol_dn_pressed;
	int power_pressed;
};

static void ppp_mdelay(struct ppp_power_state* s, int delay)
{
	while (true) {
		/*
		if (s->vol_up && button_get_state(s->vol_up) == BUTTON_ON)
			s->vol_up_pressed = 1;
		if (s->vol_up && button_get_state(s->vol_dn) == BUTTON_ON)
			s->vol_dn_pressed = 1;
		if (s->power && button_get_state(s->power) == BUTTON_ON)
			s->power_pressed = 1;
                  */
		if (delay < 5)
			break;

		mdelay(5);
		delay -= 5;
	}
}

void led_setup(void)
{
	struct udevice *pmic = NULL;
	struct udevice *led_g = NULL;
	struct udevice *led_r = NULL;
	struct ppp_power_state s = {};
	int ret;

	setup_iodomain();

	uclass_get_device_by_name(UCLASS_LED, "led-red", &led_r);
	uclass_get_device_by_name(UCLASS_LED, "led-green", &led_g);

	/*
	button_get_by_label("Volume Up", &s.vol_up);
	button_get_by_label("Volume Down", &s.vol_dn);
	button_get_by_label("Power", &s.power);
          */

	/*
	 * Report optimism at first.
	 */
	if (led_g)
		led_set_state(led_g, LEDST_ON);

	ret = uclass_first_device_err(UCLASS_PMIC, &pmic);
	if (ret) {
		printf("ERROR: PMIC not found! (%d)\n", ret);
		return;
	}

	/*
	 * Raise LDO2 voltage to 3V
	 */
	pmic_reg_write(pmic, 0x3d, 0x0c);

        udelay(2000);

	/*
	 * Setup current/voltage measurements, and guess if we can continue from
	 * boot OCV.
	 */
	rk818_calibrate(pmic);

	/* enable charger, Ibatmax = 1.4A   Vbatmax = 4.3V */
        pmic_reg_write(pmic, CHRG_CTRL_REG1, 0xb2);
	/* term = 150mA  trickle timeout = 60min  cc-cv timeout = 6h */
        pmic_reg_write(pmic, CHRG_CTRL_REG2, 0x4a);
	/* enable timers, safe defaults */
        pmic_reg_write(pmic, CHRG_CTRL_REG3, 0x0e);
	/* USB input limits: 850 mA / 3.26V - Just don't put this into a legacy. */
        pmic_reg_write(pmic, USB_CTRL_REG, 0xf2);

#if 0
	/*
	 * To be able to boot without a battery, input current limit must be at
	 * least 2A for a successful boot over the range of possible USB
	 * voltages. Linux will then take over and properly detect the USB port
	 * and available power over PD, or other mechanisms.
	 */
        pmic_reg_write(pmic, CHRG_CTRL_REG3, 0xf7);
#endif

	while (true) {
		int vol = rk818_get_bat_vol(pmic);
		int cur = rk818_get_bat_cur(pmic);
		int ocv = vol - cur * 250 / 1000; /* Rint is selected to be valid for low capacity */
		int plugin = !!(pmic_reg_read(pmic, VB_MON_REG) & PLUG_IN_STS);
		int status = pmic_reg_read(pmic, SUP_STS_REG);
		int chg_status = (status >> BAT_STATUS_OFF) & BAT_STATUS_MSK;
		int usb_fault = !(status & USB_EFF);
		int usb_exist = !!(status & USB_EXIST);
		int bat_exist = !!(status & BAT_EXS);

		printf("Battery status: vol=%d cur=%d ocv=%d plugin=%d status=%s usb_fault=%d usb_exist=%d bat_exist=%d\n",
		       vol, cur, ocv, plugin, status_str[chg_status], usb_fault, usb_exist, bat_exist);

#if 0
		//printf("PMIC regs:\n");
		uint8_t buffer[0xf2 - 0x20 + 1];
		pmic_read(pmic, 0x20, buffer, 0xf2 - 0x20 + 1);
		for (int i = 0x20; i <= 0xf2; i++) {
			if (i == 0xa0 || i == 0xa1)
				printf("%02x: %02x\n", i, (unsigned)buffer[i - 0x20]);
		}
#endif

		/*
		 * XXX: allow to continue boot by pressing volume up
		 */

		if (ocv > 3500) {
			/*
			 * OCV battery voltage above 3.5V == all good, let's
			 * boot.
			 */
			goto continue_boot;
		}

		if (!usb_exist || usb_fault) {
			/*
			 * USB is not plugged in and the battery is low. We
			 * blink and poweroff.
			 */
			goto report_low_power_and_poweroff;
		}

		/*
		 * From now on, we know we're connected to a charger, so we
		 * can't shutdown the phone. All errors must be reported just
		 * by blinking the LEDs and continuing. If the user will remove
		 * the charger, the previous condition will shut down the phone.
		 */

		switch (chg_status) {
		/*
		 * Recovery charging modes.
		 */
		case DEAD_CHARGE:
		case TRICKLE_CHARGE:
			/*
			 * Blink green LED shortly: 100ms/250ms on, 1s off.
			 */
			if (led_g && led_r) {
				led_set_state(led_r, LEDST_OFF);

				led_set_state(led_g, LEDST_ON);
				ppp_mdelay(&s, chg_status == DEAD_CHARGE ? 100 : 200);
				led_set_state(led_g, LEDST_OFF);
			}
			ppp_mdelay(&s, 1000);
			break;

		/*
		 * Fast charging state.
		 */
		case CC_OR_CV:
                        int period = 2000;
			if (cur < 0)
				goto charge_off;

			if (cur > 1900)
				cur = 1900;
			if (cur < 100)
				cur = 100;

			/*
			 * Blink green LED slowly: 1s on, 1s off.
			 */
			if (led_g && led_r) {
				led_set_state(led_r, LEDST_OFF);
				led_set_state(led_g, LEDST_TOGGLE);

				if (led_get_state(led_g) == LEDST_ON) {
					ppp_mdelay(&s, cur);
				} else {
					ppp_mdelay(&s, period - cur);
				}
			} else {
				ppp_mdelay(&s, 1000);
			}
			break;

		/*
		 * Charge is done, nothing to wait for anymore. We only get
		 * here in the weirdest of situations.
		 */
		case CHARGE_FINISH:
			goto continue_boot;

		/*
		 * Charger is off for some reason. Wait for it for 10s
		 * to start, otherwise shut down.
		 */
		charge_off:
		case CHARGE_OFF:
			if (led_g)
				led_set_state(led_g, LEDST_OFF);
			blink_led(led_r, 2, 200);
			ppp_mdelay(&s, 1000);
			break;

		/*
		 * Critical errors we can't continue.
		 */
		case USB_OVER_VOL:
		case BAT_TMP_ERR:
		case TIMER_ERR:
			if (led_g)
				led_set_state(led_g, LEDST_OFF);
			blink_led(led_r, 5, 100);
			ppp_mdelay(&s, 1000);
			break;
		}
	}

continue_boot:
	if (led_r && led_g) {
		led_set_state(led_r, LEDST_OFF);
		led_set_state(led_g, LEDST_ON);
	}
	return;

report_low_power_and_poweroff:
	if (led_g)
		led_set_state(led_g, LEDST_OFF);
	blink_led(led_r, 8, 200);
	sysreset_walk(SYSRESET_POWER_OFF);
	blink_led(led_r, INT_MAX, 200);

	hang();
}

#endif
