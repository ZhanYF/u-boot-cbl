// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2018 Ondrej Jirman <megous@megous.com>
 *
 * Based on the Linux driver drivers/input/touchscreen/edt-ft5x06.c (v4.18):
 *
 * Copyright (C) 2012 Simon Budig, <simon.budig@kernelconcepts.de>
 * Daniel Wagener <daniel.wagener@kernelconcepts.de> (M09 firmware support)
 * Lothar Waßmann <LW@KARO-electronics.de> (DT support)
 */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <errno.h>
#include <input.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <command.h>
#include <i2c.h>
#include <touchpanel.h>
#include <power/regulator.h>

DECLARE_GLOBAL_DATA_PTR;

#define WORK_REGISTER_THRESHOLD		0x00
#define WORK_REGISTER_REPORT_RATE	0x08
#define WORK_REGISTER_GAIN		0x30
#define WORK_REGISTER_OFFSET		0x31
#define WORK_REGISTER_NUM_X		0x33
#define WORK_REGISTER_NUM_Y		0x34

#define M09_REGISTER_THRESHOLD		0x80
#define M09_REGISTER_GAIN		0x92
#define M09_REGISTER_OFFSET		0x93
#define M09_REGISTER_NUM_X		0x94
#define M09_REGISTER_NUM_Y		0x95

#define NO_REGISTER			0xff

#define WORK_REGISTER_OPMODE		0x3c
#define FACTORY_REGISTER_OPMODE		0x01

#define TOUCH_EVENT_DOWN		0x00
#define TOUCH_EVENT_UP			0x01
#define TOUCH_EVENT_ON			0x02
#define TOUCH_EVENT_RESERVED		0x03

#define EDT_NAME_LEN			23
#define EDT_SWITCH_MODE_RETRIES		10
#define EDT_SWITCH_MODE_DELAY		5 /* msec */
#define EDT_RAW_DATA_RETRIES		100
#define EDT_RAW_DATA_DELAY		1000 /* usec */

enum edt_ver {
	EDT_M06,
	EDT_M09,
	EDT_M12,
	GENERIC_FT,
};

struct edt_reg_addr {
	int reg_threshold;
	int reg_report_rate;
	int reg_gain;
	int reg_offset;
	int reg_num_x;
	int reg_num_y;
};

struct ft5x06_priv {
	struct udevice *reg;
	struct gpio_desc reset_gpio;

	u16 num_x;
	u16 num_y;

	int threshold;
	int gain;
	int offset;
	int report_rate;
	int max_support_points;

	char name[EDT_NAME_LEN];

	struct edt_reg_addr reg_addr;
	enum edt_ver version;
};

static int ft5x06_readwrite(struct udevice *dev,
			    u16 wr_len, void *wr_buf,
			    u16 rd_len, void *rd_buf)
{
	struct dm_i2c_chip *chip = dev_get_parent_plat(dev);
	struct i2c_msg wrmsg[2];
	int ret, i = 0;

	if (wr_len) {
		wrmsg[i].addr  = chip->chip_addr;
		wrmsg[i].flags = 0;
		wrmsg[i].len = wr_len;
		wrmsg[i].buf = wr_buf;
		i++;
	}

	if (rd_len) {
		wrmsg[i].addr  = chip->chip_addr;
		wrmsg[i].flags = I2C_M_RD;
		wrmsg[i].len = rd_len;
		wrmsg[i].buf = rd_buf;
		i++;
	}

	ret = dm_i2c_xfer(dev, wrmsg, i);
	if (ret < 0)
		return ret;

	return 0;
}

static int ft5x06_register_write(struct udevice *dev, u8 addr, u8 value)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	u8 wrbuf[4];

	switch (priv->version) {
	case EDT_M06:
		wrbuf[0] = 0xfc;
		wrbuf[1] = addr & 0x3f;
		wrbuf[2] = value;
		wrbuf[3] = wrbuf[0] ^ wrbuf[1] ^ wrbuf[2];
		return ft5x06_readwrite(dev, 4, wrbuf, 0, NULL);
	case EDT_M09:
	case EDT_M12:
	case GENERIC_FT:
		wrbuf[0] = addr;
		wrbuf[1] = value;

		return ft5x06_readwrite(dev, 2, wrbuf, 0, NULL);

	default:
		return -EINVAL;
	}
}

static int ft5x06_register_read(struct udevice *dev, u8 addr)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	u8 wrbuf[2], rdbuf[2];
	int error;

	switch (priv->version) {
	case EDT_M06:
		wrbuf[0] = 0xfc;
		wrbuf[1] = addr & 0x3f;
		wrbuf[1] |= 0x40;

		error = ft5x06_readwrite(dev, 2, wrbuf, 2, rdbuf);
		if (error)
			return error;

		if ((wrbuf[0] ^ wrbuf[1] ^ rdbuf[0]) != rdbuf[1]) {
			dev_err(dev,
				"crc error: 0x%02x expected, got 0x%02x\n",
				wrbuf[0] ^ wrbuf[1] ^ rdbuf[0],
				rdbuf[1]);
			return -EIO;
		}
		break;

	case EDT_M09:
	case EDT_M12:
	case GENERIC_FT:
		wrbuf[0] = addr;
		error = ft5x06_readwrite(dev, 1, wrbuf, 1, rdbuf);
		if (error)
			return error;
		break;

	default:
		return -EINVAL;
	}

	return rdbuf[0];
}

static int ft5x06_identify(struct udevice *dev, char *fw_version)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	u8 rdbuf[EDT_NAME_LEN];
	char *p;
	int error;
	char *model_name = priv->name;

	/* see what we find if we assume it is a M06 *
	 * if we get less than EDT_NAME_LEN, we don't want
	 * to have garbage in there
	 */
	memset(rdbuf, 0, sizeof(rdbuf));
	error = ft5x06_readwrite(dev, 1, "\xBB", EDT_NAME_LEN - 1, rdbuf);
	if (error)
		return error;

	/* Probe content for something consistent.
	 * M06 starts with a response byte, M12 gives the data directly.
	 * M09/Generic does not provide model number information.
	 */
	if (!strncasecmp(rdbuf + 1, "EP0", 3)) {
		priv->version = EDT_M06;

		/* remove last '$' end marker */
		rdbuf[EDT_NAME_LEN - 1] = '\0';
		if (rdbuf[EDT_NAME_LEN - 2] == '$')
			rdbuf[EDT_NAME_LEN - 2] = '\0';

		/* look for Model/Version separator */
		p = strchr(rdbuf, '*');
		if (p)
			*p++ = '\0';
		strlcpy(model_name, rdbuf + 1, EDT_NAME_LEN);
		strlcpy(fw_version, p ? p : "", EDT_NAME_LEN);
	} else if (!strncasecmp(rdbuf, "EP0", 3)) {
		priv->version = EDT_M12;

		/* remove last '$' end marker */
		rdbuf[EDT_NAME_LEN - 2] = '\0';
		if (rdbuf[EDT_NAME_LEN - 3] == '$')
			rdbuf[EDT_NAME_LEN - 3] = '\0';

		/* look for Model/Version separator */
		p = strchr(rdbuf, '*');
		if (p)
			*p++ = '\0';
		strlcpy(model_name, rdbuf, EDT_NAME_LEN);
		strlcpy(fw_version, p ? p : "", EDT_NAME_LEN);
	} else {
		/* If it is not an EDT M06/M12 touchscreen, then the model
		 * detection is a bit hairy. The different ft5x06
		 * firmares around don't reliably implement the
		 * identification registers. Well, we'll take a shot.
		 *
		 * The main difference between generic focaltec based
		 * touches and EDT M09 is that we know how to retrieve
		 * the max coordinates for the latter.
		 */
		priv->version = GENERIC_FT;

		error = ft5x06_readwrite(dev, 1, "\xA6", 2, rdbuf);
		if (error)
			return error;

		strlcpy(fw_version, rdbuf, 2);

		error = ft5x06_readwrite(dev, 1, "\xA8", 1, rdbuf);
		if (error)
			return error;

		/* This "model identification" is not exact. Unfortunately
		 * not all firmwares for the ft5x06 put useful values in
		 * the identification registers.
		 */
		switch (rdbuf[0]) {
		case 0x35:   /* EDT EP0350M09 */
		case 0x43:   /* EDT EP0430M09 */
		case 0x50:   /* EDT EP0500M09 */
		case 0x57:   /* EDT EP0570M09 */
		case 0x70:   /* EDT EP0700M09 */
			priv->version = EDT_M09;
			snprintf(model_name, EDT_NAME_LEN, "EP0%i%i0M09",
				rdbuf[0] >> 4, rdbuf[0] & 0x0F);
			break;
		case 0xa1:   /* EDT EP1010ML00 */
			priv->version = EDT_M09;
			snprintf(model_name, EDT_NAME_LEN, "EP%i%i0ML00",
				rdbuf[0] >> 4, rdbuf[0] & 0x0F);
			break;
		case 0x5a:   /* Solomon Goldentek Display */
			snprintf(model_name, EDT_NAME_LEN, "GKTW50SCED1R0");
			break;
		default:
			snprintf(model_name, EDT_NAME_LEN,
				 "generic ft5x06 (%02x)",
				 rdbuf[0]);
			break;
		}
	}

	return 0;
}

static void ft5x06_get_defaults(struct udevice *dev)
{
//XXX: not supported yet, should be read from DT
#if 0
	struct ft5x06_priv *priv = dev_get_priv(dev);
	struct edt_reg_addr *reg_addr = &priv->reg_addr;
	u32 val;
	int error;

	error = device_property_read_u32(dev, "threshold", &val);
	if (!error) {
		ft5x06_register_write(dev, reg_addr->reg_threshold, val);
		priv->threshold = val;
	}

	error = device_property_read_u32(dev, "gain", &val);
	if (!error) {
		ft5x06_register_write(dev, reg_addr->reg_gain, val);
		priv->gain = val;
	}

	error = device_property_read_u32(dev, "offset", &val);
	if (!error) {
		ft5x06_register_write(dev, reg_addr->reg_offset, val);
		priv->offset = val;
	}
#endif
}

static void ft5x06_get_parameters(struct udevice *dev)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	struct edt_reg_addr *reg_addr = &priv->reg_addr;

	priv->threshold = ft5x06_register_read(dev, reg_addr->reg_threshold);
	priv->gain = ft5x06_register_read(dev, reg_addr->reg_gain);
	priv->offset = ft5x06_register_read(dev, reg_addr->reg_offset);
	if (reg_addr->reg_report_rate != NO_REGISTER)
		priv->report_rate = ft5x06_register_read(dev,
						reg_addr->reg_report_rate);
	if (priv->version == EDT_M06 ||
	    priv->version == EDT_M09 ||
	    priv->version == EDT_M12) {
		priv->num_x = ft5x06_register_read(dev, reg_addr->reg_num_x);
		priv->num_y = ft5x06_register_read(dev, reg_addr->reg_num_y);
	} else {
		priv->num_x = -1;
		priv->num_y = -1;
	}
}

static void ft5x06_set_regs(struct udevice *dev)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	struct edt_reg_addr *reg_addr = &priv->reg_addr;

	switch (priv->version) {
	case EDT_M06:
		reg_addr->reg_threshold = WORK_REGISTER_THRESHOLD;
		reg_addr->reg_report_rate = WORK_REGISTER_REPORT_RATE;
		reg_addr->reg_gain = WORK_REGISTER_GAIN;
		reg_addr->reg_offset = WORK_REGISTER_OFFSET;
		reg_addr->reg_num_x = WORK_REGISTER_NUM_X;
		reg_addr->reg_num_y = WORK_REGISTER_NUM_Y;
		break;

	case EDT_M09:
	case EDT_M12:
		reg_addr->reg_threshold = M09_REGISTER_THRESHOLD;
		reg_addr->reg_report_rate = NO_REGISTER;
		reg_addr->reg_gain = M09_REGISTER_GAIN;
		reg_addr->reg_offset = M09_REGISTER_OFFSET;
		reg_addr->reg_num_x = M09_REGISTER_NUM_X;
		reg_addr->reg_num_y = M09_REGISTER_NUM_Y;
		break;

	case GENERIC_FT:
		/* this is a guesswork */
		reg_addr->reg_threshold = M09_REGISTER_THRESHOLD;
		reg_addr->reg_gain = M09_REGISTER_GAIN;
		reg_addr->reg_offset = M09_REGISTER_OFFSET;
		break;
	}
}

static bool ft5x06_check_crc(struct udevice *dev, u8 *buf, int buflen)
{
	int i;
	u8 crc = 0;

	for (i = 0; i < buflen - 1; i++)
		crc ^= buf[i];

	if (crc != buf[buflen-1]) {
		dev_err(dev, "crc error: 0x%02x expected, got 0x%02x\n",
			crc, buf[buflen-1]);
		return false;
	}

	return true;
}

static int ft5x06_get_touches(struct udevice* dev,
			      struct touchpanel_touch* touches, int max_touches)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	u8 cmd;
	u8 rdbuf[63];
	int i, type, x, y, id;
	int offset, tplen, datalen, crclen;
	int error;
	int touches_count = 0;

	switch (priv->version) {
	case EDT_M06:
		cmd = 0xf9; /* tell the controller to send touch data */
		offset = 5; /* where the actual touch data starts */
		tplen = 4;  /* data comes in so called frames */
		crclen = 1; /* length of the crc data */
		break;

	case EDT_M09:
	case EDT_M12:
	case GENERIC_FT:
		cmd = 0x0;
		offset = 3;
		tplen = 6;
		crclen = 0;
		break;

	default:
		goto out;
	}

	memset(rdbuf, 0, sizeof(rdbuf));
	datalen = tplen * priv->max_support_points + offset + crclen;

	error = ft5x06_readwrite(dev, sizeof(cmd), &cmd, datalen, rdbuf);
	if (error) {
		dev_err(dev, "Unable to fetch data, error: %d\n", error);
		goto out;
	}

	/* M09/M12 does not send header or CRC */
	if (priv->version == EDT_M06) {
		if (rdbuf[0] != 0xaa || rdbuf[1] != 0xaa ||
			rdbuf[2] != datalen) {
			dev_err(dev, "Unexpected header: %02x%02x%02x!\n",
				rdbuf[0], rdbuf[1], rdbuf[2]);
			goto out;
		}

		if (!ft5x06_check_crc(dev, rdbuf, datalen))
			goto out;
	}

	for (i = 0; i < priv->max_support_points; i++) {
		u8 *buf = &rdbuf[i * tplen + offset];
		bool down;

		type = buf[0] >> 6;
		/* ignore Reserved events */
		if (type == TOUCH_EVENT_RESERVED)
			continue;

		/* M06 sometimes sends bogus coordinates in TOUCH_DOWN */
		if (priv->version == EDT_M06 && type == TOUCH_EVENT_DOWN)
			continue;

		x = ((buf[0] << 8) | buf[1]) & 0x0fff;
		y = ((buf[2] << 8) | buf[3]) & 0x0fff;
		id = (buf[2] >> 4) & 0x0f;
		down = type != TOUCH_EVENT_UP;

		if (!down)
			continue;

		if (max_touches > touches_count) {
			touches[touches_count].x = x;
			touches[touches_count].y = y;
			touches[touches_count].id = id;
			touches_count++;
		}
	}

out:
	return touches_count;
}

static int ft5x06_start(struct udevice *dev)
{
	debug("%s: started\n", __func__);
	return 0;
}

static int ft5x06_stop(struct udevice *dev)
{
	debug("%s: stopped\n", __func__);
	return 0;
}

/**
 * Set up the touch panel.
 *
 * @return 0 if ok, -ERRNO on error
 */
static int ft5x06_probe(struct udevice *dev)
{
	struct touchpanel_priv *uc_priv = dev_get_uclass_priv(dev);
	struct ft5x06_priv *priv = dev_get_priv(dev);
	int ret;

	priv->max_support_points = 5;

	if (priv->reg && CONFIG_IS_ENABLED(DM_REGULATOR)) {
		ret = regulator_set_enable(priv->reg, true);
		if (ret) {
			debug("%s: Cannot enable regulator for touchpanel '%s'\n",
			      __func__, dev->name);
			return ret;
		}

		udelay(20 * 1000);
	}

	if (dm_gpio_is_valid(&priv->reset_gpio)) {
		ret = dm_gpio_set_value(&priv->reset_gpio, 0);
		if (ret)
			return ret;
	}
	udelay(300 * 1000);

	char fw_version[EDT_NAME_LEN];
	ret = ft5x06_identify(dev, fw_version);
	if (ret) {
		dev_err(dev, "touchscreen probe failed %d\n", ret);
		return ret;
	}

	ft5x06_set_regs(dev);
	ft5x06_get_defaults(dev);
	ft5x06_get_parameters(dev);

	debug("Model \"%s\", Rev. \"%s\", %dx%d sensors\n",
		priv->name, fw_version, priv->num_x, priv->num_y);

	if (priv->version == EDT_M06 ||
	    priv->version == EDT_M09 ||
	    priv->version == EDT_M12) {
		uc_priv->size_x = priv->num_x * 64;
		uc_priv->size_y = priv->num_y * 64;
	} else {
		//XXX: perhaps check that the user set the values in DT
	}

	debug("%s: ready\n", __func__);
	return 0;
}

static int ft5x06_of_to_plat(struct udevice *dev)
{
	struct ft5x06_priv *priv = dev_get_priv(dev);
	int ret;

	debug("%s: start\n", __func__);

	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "power-supply", &priv->reg);
	if (ret) {
		debug("%s: Cannot get power supply: ret=%d\n", __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &priv->reset_gpio,
				   GPIOD_IS_OUT);
	if (ret) {
		debug("%s: Warning: cannot get enable GPIO: ret=%d\n",
		      __func__, ret);
		if (ret != -ENOENT)
			return ret;
	}

	debug("%s: done\n", __func__);
	return 0;
}

static const struct touchpanel_ops ft5x06_ops = {
	.start = ft5x06_start,
	.stop = ft5x06_stop,
	.get_touches = ft5x06_get_touches,
};

static const struct udevice_id ft5x06_ids[] = {
	{ .compatible = "edt,edt-ft5x06" },
	{ }
};

U_BOOT_DRIVER(ft5x06) = {
	.name = "touchpanel-ft5x06",
	.id = UCLASS_TOUCHPANEL,
	.of_match = ft5x06_ids,
	.probe = ft5x06_probe,
	.ops = &ft5x06_ops,
	.of_to_plat = ft5x06_of_to_plat,
	.priv_auto = sizeof(struct ft5x06_priv),
};
