// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Goodix Touchscreens
 *
 * Copyright (c) 2023 Ondřej Jirman <megi@xff.cz>
 *
 * Loosely based on Linux goodix.c driver:
 *
 * Copyright (c) 2023 Red Hat Inc.
 * Copyright (c) 2015 K. Merker <merker@debian.org>
 *
 * This code is based on gt9xx.c authored by andrew@goodix.com:
 *
 * 2010 - 2012 Goodix Technology.
 */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <input.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <command.h>
#include <i2c.h>
#include <touchpanel.h>
#include <power/regulator.h>
#include <linux/unaligned/access_ok.h>

DECLARE_GLOBAL_DATA_PTR;

/* Register defines */
#define GOODIX_REG_MISCTL_DSP_CTL		0x4010
#define GOODIX_REG_MISCTL_SRAM_BANK		0x4048
#define GOODIX_REG_MISCTL_MEM_CD_EN		0x4049
#define GOODIX_REG_MISCTL_CACHE_EN		0x404B
#define GOODIX_REG_MISCTL_TMR0_EN		0x40B0
#define GOODIX_REG_MISCTL_SWRST			0x4180
#define GOODIX_REG_MISCTL_CPU_SWRST_PULSE	0x4184
#define GOODIX_REG_MISCTL_BOOTCTL		0x4190
#define GOODIX_REG_MISCTL_BOOT_OPT		0x4218
#define GOODIX_REG_MISCTL_BOOT_CTL		0x5094

#define GOODIX_REG_FW_SIG			0x8000
#define GOODIX_FW_SIG_LEN			10

#define GOODIX_REG_MAIN_CLK			0x8020
#define GOODIX_MAIN_CLK_LEN			6

#define GOODIX_REG_COMMAND			0x8040
#define GOODIX_CMD_SCREEN_OFF			0x05

#define GOODIX_REG_SW_WDT			0x8041

#define GOODIX_REG_REQUEST			0x8043
#define GOODIX_RQST_RESPONDED			0x00
#define GOODIX_RQST_CONFIG			0x01
#define GOODIX_RQST_BAK_REF			0x02
#define GOODIX_RQST_RESET			0x03
#define GOODIX_RQST_MAIN_CLOCK			0x04
/*
 * Unknown request which gets send by the controller aprox.
 * every 34 seconds once it is up and running.
 */
#define GOODIX_RQST_UNKNOWN			0x06
#define GOODIX_RQST_IDLE			0xFF

#define GOODIX_REG_STATUS			0x8044

#define GOODIX_GT1X_REG_CONFIG_DATA		0x8050
#define GOODIX_GT9X_REG_CONFIG_DATA		0x8047
#define GOODIX_REG_ID				0x8140
#define GOODIX_READ_COOR_ADDR			0x814E
#define GOODIX_REG_BAK_REF			0x99D0

#define GOODIX_ID_MAX_LEN			4
#define GOODIX_CONFIG_MAX_LENGTH		240
#define GOODIX_MAX_KEYS				7

#define GOODIX_MAX_HEIGHT		4096
#define GOODIX_MAX_WIDTH		4096
#define GOODIX_INT_TRIGGER		1
#define GOODIX_CONTACT_SIZE		8
#define GOODIX_MAX_CONTACT_SIZE		9
#define GOODIX_MAX_CONTACTS		10

#define GOODIX_CONFIG_MIN_LENGTH	186
#define GOODIX_CONFIG_911_LENGTH	186
#define GOODIX_CONFIG_967_LENGTH	228
#define GOODIX_CONFIG_GT9X_LENGTH	240

#define GOODIX_BUFFER_STATUS_READY	BIT(7)
#define GOODIX_HAVE_KEY			BIT(4)
#define GOODIX_BUFFER_STATUS_TIMEOUT	20

#define RESOLUTION_LOC		1
#define MAX_CONTACTS_LOC	5
#define TRIGGER_LOC		6

struct goodix_chip_data {
	u16 config_addr;
	int config_len;
};

struct goodix_chip_id {
	const char *id;
	const struct goodix_chip_data *data;
};

static const struct goodix_chip_data gt1x_chip_data = {
	.config_addr		= GOODIX_GT1X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_GT9X_LENGTH,
};

static const struct goodix_chip_data gt911_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_911_LENGTH,
};

static const struct goodix_chip_data gt967_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_967_LENGTH,
};

static const struct goodix_chip_data gt9x_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_GT9X_LENGTH,
};

static const struct goodix_chip_id goodix_chip_ids[] = {
	{ .id = "1151", .data = &gt1x_chip_data },
	{ .id = "1158", .data = &gt1x_chip_data },
	{ .id = "5663", .data = &gt1x_chip_data },
	{ .id = "5688", .data = &gt1x_chip_data },
	{ .id = "917S", .data = &gt1x_chip_data },
	{ .id = "9286", .data = &gt1x_chip_data },

	{ .id = "911", .data = &gt911_chip_data },
	{ .id = "9271", .data = &gt911_chip_data },
	{ .id = "9110", .data = &gt911_chip_data },
	{ .id = "9111", .data = &gt911_chip_data },
	{ .id = "927", .data = &gt911_chip_data },
	{ .id = "928", .data = &gt911_chip_data },

	{ .id = "912", .data = &gt967_chip_data },
	{ .id = "9147", .data = &gt967_chip_data },
	{ .id = "967", .data = &gt967_chip_data },
	{ }
};

struct goodix_priv {
        struct udevice* dev;

	struct udevice *reg_avdd;
	struct udevice *reg_vddio;
	struct gpio_desc reset_gpio;
	struct gpio_desc irq_gpio;

	const struct goodix_chip_data *chip;
	unsigned int max_touch_num;

	char id[GOODIX_ID_MAX_LEN + 1];

	u8 config[GOODIX_CONFIG_MAX_LENGTH];
};

/**
 * goodix_i2c_read - read data from a register of the i2c slave device.
 *
 * @ts: i2c device.
 * @reg: the register to read from.
 * @buf: raw write data buffer.
 * @len: length of the buffer to write
 */
int goodix_i2c_read(struct goodix_priv *ts, u16 reg, u8 *buf, int len)
{
	struct dm_i2c_chip *chip = dev_get_parent_plat(ts->dev);
	struct i2c_msg msgs[2];
	__be16 wbuf = cpu_to_be16(reg);
	int ret;

	msgs[0].flags = 0;
	msgs[0].addr  = chip->chip_addr;
	msgs[0].len   = 2;
	msgs[0].buf   = (u8 *)&wbuf;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = chip->chip_addr;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = dm_i2c_xfer(ts->dev, msgs, 2);
	if (ret)
		dev_err(ts->dev, "Error reading %d bytes from 0x%04x: %d\n",
			len, reg, ret);
	return ret;
}

/**
 * goodix_i2c_write - write data to a register of the i2c slave device.
 *
 * @client: i2c device.
 * @reg: the register to write to.
 * @buf: raw data buffer to write.
 * @len: length of the buffer to write
 */
int goodix_i2c_write(struct goodix_priv *ts, u16 reg, const u8 *buf, int len)
{
	struct dm_i2c_chip *chip = dev_get_parent_plat(ts->dev);
	u8 *addr_buf;
	struct i2c_msg msg;
	int ret;

	addr_buf = malloc(len + 2);
	if (!addr_buf)
		return -ENOMEM;

	addr_buf[0] = reg >> 8;
	addr_buf[1] = reg & 0xFF;
	memcpy(&addr_buf[2], buf, len);

	msg.flags = 0;
	msg.addr = chip->chip_addr;
	msg.buf = addr_buf;
	msg.len = len + 2;

	ret = dm_i2c_xfer(ts->dev, &msg, 1);
	free(addr_buf);
	if (ret)
		dev_err(ts->dev, "Error writing %d bytes to 0x%04x: %d\n",
			len, reg, ret);
	return ret;
}

int goodix_i2c_write_u8(struct goodix_priv *ts, u16 reg, u8 value)
{
	return goodix_i2c_write(ts, reg, &value, sizeof(value));
}

static const struct goodix_chip_data *goodix_get_chip_data(const char *id)
{
	unsigned int i;

	for (i = 0; goodix_chip_ids[i].id; i++) {
		if (!strcmp(goodix_chip_ids[i].id, id))
			return goodix_chip_ids[i].data;
	}

	return &gt9x_chip_data;
}

static int goodix_ts_read_input_report(struct goodix_priv *ts, u8 *data)
{
	int touch_num;
	int error;
	u16 addr = GOODIX_READ_COOR_ADDR;
	/*
	 * We are going to read 1-byte header,
	 * GOODIX_CONTACT_SIZE * max(1, touch_num) bytes of coordinates
	 * and 1-byte footer which contains the touch-key code.
	 */
	const int header_contact_keycode_size = 1 + GOODIX_CONTACT_SIZE + 1;

	/*
	 * The 'buffer status' bit, which indicates that the data is valid, is
	 * not set as soon as the interrupt is raised, but slightly after.
	 * This takes around 10 ms to happen, so we poll for 20 ms.
	 */
	int retries = GOODIX_BUFFER_STATUS_TIMEOUT;
	while (retries-- > 0) {
		error = goodix_i2c_read(ts, addr, data,
					header_contact_keycode_size);
		if (error)
			return error;

		if (data[0] & GOODIX_BUFFER_STATUS_READY) {
			touch_num = data[0] & 0x0f;
			if (touch_num > ts->max_touch_num)
				return -EPROTO;

			if (touch_num > 1) {
				addr += header_contact_keycode_size;
				data += header_contact_keycode_size;
				error = goodix_i2c_read(ts,
						addr, data,
						GOODIX_CONTACT_SIZE *
							(touch_num - 1));
				if (error)
					return error;
			}

			return touch_num;
		}

		udelay(1000);
	}

	/*
	 * The Goodix panel will send spurious interrupts after a
	 * 'finger up' event, which will always cause a timeout.
	 */
	return -ENOMSG;
}

static int goodix_get_touches(struct udevice* dev,
			      struct touchpanel_touch* touches, int max_touches)
{
	struct goodix_priv *ts = dev_get_priv(dev);
	int touches_count = 0;
	u8  point_data[2 + GOODIX_MAX_CONTACT_SIZE * GOODIX_MAX_CONTACTS];
	int touch_num;
	int i;

	touch_num = goodix_ts_read_input_report(ts, point_data);
	if (touch_num < 0) {
		if (touch_num == -ENOMSG)
			return 0;

		dev_err(dev, "Error reading input report: %d\n", touch_num);
		return touch_num;
	}

	for (i = 0; i < touch_num; i++) {
		u8 *coor_data = &point_data[1 + GOODIX_CONTACT_SIZE * i];

		int id = coor_data[0] & 0x0F;
		int input_x = get_unaligned_le16(&coor_data[1]);
		int input_y = get_unaligned_le16(&coor_data[3]);

		if (max_touches > touches_count) {
			if (touches) {
				touches[touches_count].x = input_x;
				touches[touches_count].y = input_y;
				touches[touches_count].id = id;
			}
			touches_count++;
		}
	}

	goodix_i2c_write_u8(ts, GOODIX_READ_COOR_ADDR, 0);

	return touches_count;
}

static int goodix_start(struct udevice *dev)
{
	debug("%s: started\n", __func__);
	
	/* flush previous readings if any */
	while (true) {
		int ret = goodix_get_touches(dev, NULL, 1);
		if (ret <= 0)
			break;
	}
	
	return 0;
}

static int goodix_stop(struct udevice *dev)
{
	debug("%s: stopped\n", __func__);
	return 0;
}

static int goodix_probe(struct udevice *dev)
{
	struct touchpanel_priv *uc_priv = dev_get_uclass_priv(dev);
	struct goodix_priv *ts = dev_get_priv(dev);
	int ret;

	ts->dev = dev;

	/*
	 * Power up sequence: (reset is active low)
	 *
	 * - all low (INT, RST, power rails, ...)
	 * - AVDD first, then VDDIO at any time
	 * - > 10ms before address selection
	 * - INT signal H or L to pick address 0x14 or 0x5d
	 * - > 100us 
	 * - RST high
	 * - 5-10ms
	 * - INT signal low
	 * - 50ms
	 * - INT input
	 */

	if (dm_gpio_is_valid(&ts->reset_gpio)) {
		ret = dm_gpio_set_value(&ts->reset_gpio, 0);
		if (ret)
			return ret;

		ret = dm_gpio_set_value(&ts->irq_gpio, 0);
		if (ret)
			return ret;
	}

	if (CONFIG_IS_ENABLED(DM_REGULATOR) && ts->reg_avdd) {
		ret = regulator_set_enable(ts->reg_avdd, true);
		if (ret) {
			debug("%s: Cannot enable AVDD28 regulator for touchpanel '%s'\n",
			      __func__, dev->name);
			return ret;
		}
	}

	if (CONFIG_IS_ENABLED(DM_REGULATOR) && ts->reg_vddio) {
		ret = regulator_set_enable(ts->reg_vddio, true);
		if (ret) {
			debug("%s: Cannot enable VDDIO regulator for touchpanel '%s'\n",
			      __func__, dev->name);
			return ret;
		}
	}

	udelay(30 * 1000);

	if (dm_gpio_is_valid(&ts->reset_gpio)) {
		// select address 0x14
		ret = dm_gpio_set_value(&ts->irq_gpio, 1);
		if (ret)
			return ret;

		udelay(150);

		ret = dm_gpio_set_value(&ts->reset_gpio, 1);
		if (ret)
			return ret;

		udelay(7500);

		ret = dm_gpio_set_value(&ts->irq_gpio, 0);
		if (ret)
			return ret;

		udelay(50 * 1000);

		ret = dm_gpio_set_dir_flags(&ts->irq_gpio, GPIOD_IS_IN);
		if (ret)
			return ret;

		udelay(1000);
	}

	// read id and find chip data
	ret = goodix_i2c_read(ts, GOODIX_REG_ID, ts->id, sizeof(ts->id));
	if (ret) {
		dev_err(ts->dev, "Error reading ID\n");
		return ret;
	}

	ts->id[GOODIX_ID_MAX_LEN] = 0;
	ts->chip = goodix_get_chip_data(ts->id);

	/* Read configuration and apply touchscreen parameters */

	ret = goodix_i2c_read(ts, ts->chip->config_addr,
			      ts->config, ts->chip->config_len);
	if (ret) {
		dev_err(ts->dev, "Error reading config\n");
		return ret;
	}

	ts->max_touch_num = ts->config[MAX_CONTACTS_LOC] & 0x0f;

	uc_priv->size_x = get_unaligned_le16(&ts->config[RESOLUTION_LOC]);
	uc_priv->size_y = get_unaligned_le16(&ts->config[RESOLUTION_LOC + 2]);

	debug("touchscreen of size %dx%d found\n",
	      uc_priv->size_x, uc_priv->size_y);

	debug("%s: ready\n", __func__);
	return 0;
}

static int goodix_of_to_plat(struct udevice *dev)
{
	struct goodix_priv *ts = dev_get_priv(dev);
	int ret;

	debug("%s: start\n", __func__);
	
	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "AVDD28-supply", &ts->reg_avdd);
	if (ret) {
		debug("%s: Cannot get AVDD28 supply: ret=%d\n", __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "VDDIO-supply", &ts->reg_vddio);
	if (ret) {
		debug("%s: Cannot get VDDIO supply: ret=%d\n", __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &ts->reset_gpio,
				   GPIOD_IS_OUT);
	if (ret) {
		debug("%s: Warning: cannot get reset GPIO: ret=%d\n",
		      __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	ret = gpio_request_by_name(dev, "irq-gpios", 0, &ts->irq_gpio,
				   GPIOD_IS_OUT);
	if (ret) {
		debug("%s: Warning: cannot get irq GPIO: ret=%d\n",
		      __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	debug("%s: done\n", __func__);
	return 0;
}

static const struct touchpanel_ops goodix_ops = {
	.start = goodix_start,
	.stop = goodix_stop,
	.get_touches = goodix_get_touches,
};

static const struct udevice_id goodix_ids[] = {
	{ .compatible = "goodix,gt1151" },
	{ .compatible = "goodix,gt1158" },
	{ .compatible = "goodix,gt5663" },
	{ .compatible = "goodix,gt5688" },
	{ .compatible = "goodix,gt911" },
	{ .compatible = "goodix,gt9110" },
	{ .compatible = "goodix,gt912" },
	{ .compatible = "goodix,gt9147" },
	{ .compatible = "goodix,gt917s" },
	{ .compatible = "goodix,gt927" },
	{ .compatible = "goodix,gt9271" },
	{ .compatible = "goodix,gt928" },
	{ .compatible = "goodix,gt9286" },
	{ .compatible = "goodix,gt967" },
	{ }
};

U_BOOT_DRIVER(goodix) = {
	.name = "touchpanel-goodix",
	.id = UCLASS_TOUCHPANEL,
	.of_match = goodix_ids,
	.probe = goodix_probe,
	.ops = &goodix_ops,
	.of_to_plat = goodix_of_to_plat,
	.priv_auto = sizeof(struct goodix_priv),
};
