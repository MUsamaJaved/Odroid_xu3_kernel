/*
 * Afatech AF9033 demodulator driver
 *
 * Copyright (C) 2009 Antti Palosaari <crope@iki.fi>
 * Copyright (C) 2012 Antti Palosaari <crope@iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License along
 *    with this program; if not, write to the Free Software Foundation, Inc.,
 *    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "af9033_priv.h"

/* Max transfer size done by I2C transfer functions */
#define MAX_XFER_SIZE  64

struct af9033_dev {
	struct i2c_client *client;
	struct dvb_frontend fe;
	struct af9033_config cfg;

	u32 bandwidth_hz;
	bool ts_mode_parallel;
	bool ts_mode_serial;

	u32 ber;
	u32 ucb;
	unsigned long last_stat_check;
};

/* write multiple registers */
static int af9033_wr_regs(struct af9033_dev *dev, u32 reg, const u8 *val,
		int len)
{
	int ret;
	u8 buf[MAX_XFER_SIZE];
	struct i2c_msg msg[1] = {
		{
			.addr = dev->client->addr,
			.flags = 0,
			.len = 3 + len,
			.buf = buf,
		}
	};

	if (3 + len > sizeof(buf)) {
		dev_warn(&dev->client->dev,
				"i2c wr reg=%04x: len=%d is too big!\n",
				reg, len);
		return -EINVAL;
	}

	buf[0] = (reg >> 16) & 0xff;
	buf[1] = (reg >>  8) & 0xff;
	buf[2] = (reg >>  0) & 0xff;
	memcpy(&buf[3], val, len);

	ret = i2c_transfer(dev->client->adapter, msg, 1);
	if (ret == 1) {
		ret = 0;
	} else {
		dev_warn(&dev->client->dev, "i2c wr failed=%d reg=%06x len=%d\n",
				ret, reg, len);
		ret = -EREMOTEIO;
	}

	return ret;
}

/* read multiple registers */
static int af9033_rd_regs(struct af9033_dev *dev, u32 reg, u8 *val, int len)
{
	int ret;
	u8 buf[3] = { (reg >> 16) & 0xff, (reg >> 8) & 0xff,
			(reg >> 0) & 0xff };
	struct i2c_msg msg[2] = {
		{
			.addr = dev->client->addr,
			.flags = 0,
			.len = sizeof(buf),
			.buf = buf
		}, {
			.addr = dev->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = val
		}
	};

	ret = i2c_transfer(dev->client->adapter, msg, 2);
	if (ret == 2) {
		ret = 0;
	} else {
		dev_warn(&dev->client->dev, "i2c rd failed=%d reg=%06x len=%d\n",
				ret, reg, len);
		ret = -EREMOTEIO;
	}

	return ret;
}


/* write single register */
static int af9033_wr_reg(struct af9033_dev *dev, u32 reg, u8 val)
{
	return af9033_wr_regs(dev, reg, &val, 1);
}

/* read single register */
static int af9033_rd_reg(struct af9033_dev *dev, u32 reg, u8 *val)
{
	return af9033_rd_regs(dev, reg, val, 1);
}

/* write single register with mask */
static int af9033_wr_reg_mask(struct af9033_dev *dev, u32 reg, u8 val,
		u8 mask)
{
	int ret;
	u8 tmp;

	/* no need for read if whole reg is written */
	if (mask != 0xff) {
		ret = af9033_rd_regs(dev, reg, &tmp, 1);
		if (ret)
			return ret;

		val &= mask;
		tmp &= ~mask;
		val |= tmp;
	}

	return af9033_wr_regs(dev, reg, &val, 1);
}

/* read single register with mask */
static int af9033_rd_reg_mask(struct af9033_dev *dev, u32 reg, u8 *val,
		u8 mask)
{
	int ret, i;
	u8 tmp;

	ret = af9033_rd_regs(dev, reg, &tmp, 1);
	if (ret)
		return ret;

	tmp &= mask;

	/* find position of the first bit */
	for (i = 0; i < 8; i++) {
		if ((mask >> i) & 0x01)
			break;
	}
	*val = tmp >> i;

	return 0;
}

/* write reg val table using reg addr auto increment */
static int af9033_wr_reg_val_tab(struct af9033_dev *dev,
		const struct reg_val *tab, int tab_len)
{
#define MAX_TAB_LEN 212
	int ret, i, j;
	u8 buf[1 + MAX_TAB_LEN];

	dev_dbg(&dev->client->dev, "tab_len=%d\n", tab_len);

	if (tab_len > sizeof(buf)) {
		dev_warn(&dev->client->dev, "tab len %d is too big\n", tab_len);
		return -EINVAL;
	}

	for (i = 0, j = 0; i < tab_len; i++) {
		buf[j] = tab[i].val;

		if (i == tab_len - 1 || tab[i].reg != tab[i + 1].reg - 1) {
			ret = af9033_wr_regs(dev, tab[i].reg - j, buf, j + 1);
			if (ret < 0)
				goto err;

			j = 0;
		} else {
			j++;
		}
	}

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static u32 af9033_div(struct af9033_dev *dev, u32 a, u32 b, u32 x)
{
	u32 r = 0, c = 0, i;

	dev_dbg(&dev->client->dev, "a=%d b=%d x=%d\n", a, b, x);

	if (a > b) {
		c = a / b;
		a = a - c * b;
	}

	for (i = 0; i < x; i++) {
		if (a >= b) {
			r += 1;
			a -= b;
		}
		a <<= 1;
		r <<= 1;
	}
	r = (c << (u32)x) + r;

	dev_dbg(&dev->client->dev, "a=%d b=%d x=%d r=%d r=%x\n", a, b, x, r, r);

	return r;
}

static int af9033_init(struct dvb_frontend *fe)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret, i, len;
	const struct reg_val *init;
	u8 buf[4];
	u32 adc_cw, clock_cw;
	struct reg_val_mask tab[] = {
		{ 0x80fb24, 0x00, 0x08 },
		{ 0x80004c, 0x00, 0xff },
		{ 0x00f641, dev->cfg.tuner, 0xff },
		{ 0x80f5ca, 0x01, 0x01 },
		{ 0x80f715, 0x01, 0x01 },
		{ 0x00f41f, 0x04, 0x04 },
		{ 0x00f41a, 0x01, 0x01 },
		{ 0x80f731, 0x00, 0x01 },
		{ 0x00d91e, 0x00, 0x01 },
		{ 0x00d919, 0x00, 0x01 },
		{ 0x80f732, 0x00, 0x01 },
		{ 0x00d91f, 0x00, 0x01 },
		{ 0x00d91a, 0x00, 0x01 },
		{ 0x80f730, 0x00, 0x01 },
		{ 0x80f778, 0x00, 0xff },
		{ 0x80f73c, 0x01, 0x01 },
		{ 0x80f776, 0x00, 0x01 },
		{ 0x00d8fd, 0x01, 0xff },
		{ 0x00d830, 0x01, 0xff },
		{ 0x00d831, 0x00, 0xff },
		{ 0x00d832, 0x00, 0xff },
		{ 0x80f985, dev->ts_mode_serial, 0x01 },
		{ 0x80f986, dev->ts_mode_parallel, 0x01 },
		{ 0x00d827, 0x00, 0xff },
		{ 0x00d829, 0x00, 0xff },
		{ 0x800045, dev->cfg.adc_multiplier, 0xff },
	};

	/* program clock control */
	clock_cw = af9033_div(dev, dev->cfg.clock, 1000000ul, 19ul);
	buf[0] = (clock_cw >>  0) & 0xff;
	buf[1] = (clock_cw >>  8) & 0xff;
	buf[2] = (clock_cw >> 16) & 0xff;
	buf[3] = (clock_cw >> 24) & 0xff;

	dev_dbg(&dev->client->dev, "clock=%d clock_cw=%08x\n",
			dev->cfg.clock, clock_cw);

	ret = af9033_wr_regs(dev, 0x800025, buf, 4);
	if (ret < 0)
		goto err;

	/* program ADC control */
	for (i = 0; i < ARRAY_SIZE(clock_adc_lut); i++) {
		if (clock_adc_lut[i].clock == dev->cfg.clock)
			break;
	}

	adc_cw = af9033_div(dev, clock_adc_lut[i].adc, 1000000ul, 19ul);
	buf[0] = (adc_cw >>  0) & 0xff;
	buf[1] = (adc_cw >>  8) & 0xff;
	buf[2] = (adc_cw >> 16) & 0xff;

	dev_dbg(&dev->client->dev, "adc=%d adc_cw=%06x\n",
			clock_adc_lut[i].adc, adc_cw);

	ret = af9033_wr_regs(dev, 0x80f1cd, buf, 3);
	if (ret < 0)
		goto err;

	/* program register table */
	for (i = 0; i < ARRAY_SIZE(tab); i++) {
		ret = af9033_wr_reg_mask(dev, tab[i].reg, tab[i].val,
				tab[i].mask);
		if (ret < 0)
			goto err;
	}

	/* clock output */
	if (dev->cfg.dyn0_clk) {
		ret = af9033_wr_reg(dev, 0x80fba8, 0x00);
		if (ret < 0)
			goto err;
	}

	/* settings for TS interface */
	if (dev->cfg.ts_mode == AF9033_TS_MODE_USB) {
		ret = af9033_wr_reg_mask(dev, 0x80f9a5, 0x00, 0x01);
		if (ret < 0)
			goto err;

		ret = af9033_wr_reg_mask(dev, 0x80f9b5, 0x01, 0x01);
		if (ret < 0)
			goto err;
	} else {
		ret = af9033_wr_reg_mask(dev, 0x80f990, 0x00, 0x01);
		if (ret < 0)
			goto err;

		ret = af9033_wr_reg_mask(dev, 0x80f9b5, 0x00, 0x01);
		if (ret < 0)
			goto err;
	}

	/* load OFSM settings */
	dev_dbg(&dev->client->dev, "load ofsm settings\n");
	switch (dev->cfg.tuner) {
	case AF9033_TUNER_IT9135_38:
	case AF9033_TUNER_IT9135_51:
	case AF9033_TUNER_IT9135_52:
		len = ARRAY_SIZE(ofsm_init_it9135_v1);
		init = ofsm_init_it9135_v1;
		break;
	case AF9033_TUNER_IT9135_60:
	case AF9033_TUNER_IT9135_61:
	case AF9033_TUNER_IT9135_62:
		len = ARRAY_SIZE(ofsm_init_it9135_v2);
		init = ofsm_init_it9135_v2;
		break;
	default:
		len = ARRAY_SIZE(ofsm_init);
		init = ofsm_init;
		break;
	}

	ret = af9033_wr_reg_val_tab(dev, init, len);
	if (ret < 0)
		goto err;

	/* load tuner specific settings */
	dev_dbg(&dev->client->dev, "load tuner specific settings\n");
	switch (dev->cfg.tuner) {
	case AF9033_TUNER_TUA9001:
		len = ARRAY_SIZE(tuner_init_tua9001);
		init = tuner_init_tua9001;
		break;
	case AF9033_TUNER_FC0011:
		len = ARRAY_SIZE(tuner_init_fc0011);
		init = tuner_init_fc0011;
		break;
	case AF9033_TUNER_MXL5007T:
		len = ARRAY_SIZE(tuner_init_mxl5007t);
		init = tuner_init_mxl5007t;
		break;
	case AF9033_TUNER_TDA18218:
		len = ARRAY_SIZE(tuner_init_tda18218);
		init = tuner_init_tda18218;
		break;
	case AF9033_TUNER_FC2580:
		len = ARRAY_SIZE(tuner_init_fc2580);
		init = tuner_init_fc2580;
		break;
	case AF9033_TUNER_FC0012:
		len = ARRAY_SIZE(tuner_init_fc0012);
		init = tuner_init_fc0012;
		break;
	case AF9033_TUNER_IT9135_38:
		len = ARRAY_SIZE(tuner_init_it9135_38);
		init = tuner_init_it9135_38;
		break;
	case AF9033_TUNER_IT9135_51:
		len = ARRAY_SIZE(tuner_init_it9135_51);
		init = tuner_init_it9135_51;
		break;
	case AF9033_TUNER_IT9135_52:
		len = ARRAY_SIZE(tuner_init_it9135_52);
		init = tuner_init_it9135_52;
		break;
	case AF9033_TUNER_IT9135_60:
		len = ARRAY_SIZE(tuner_init_it9135_60);
		init = tuner_init_it9135_60;
		break;
	case AF9033_TUNER_IT9135_61:
		len = ARRAY_SIZE(tuner_init_it9135_61);
		init = tuner_init_it9135_61;
		break;
	case AF9033_TUNER_IT9135_62:
		len = ARRAY_SIZE(tuner_init_it9135_62);
		init = tuner_init_it9135_62;
		break;
	default:
		dev_dbg(&dev->client->dev, "unsupported tuner ID=%d\n",
				dev->cfg.tuner);
		ret = -ENODEV;
		goto err;
	}

	ret = af9033_wr_reg_val_tab(dev, init, len);
	if (ret < 0)
		goto err;

	if (dev->cfg.ts_mode == AF9033_TS_MODE_SERIAL) {
		ret = af9033_wr_reg_mask(dev, 0x00d91c, 0x01, 0x01);
		if (ret < 0)
			goto err;

		ret = af9033_wr_reg_mask(dev, 0x00d917, 0x00, 0x01);
		if (ret < 0)
			goto err;

		ret = af9033_wr_reg_mask(dev, 0x00d916, 0x00, 0x01);
		if (ret < 0)
			goto err;
	}

	switch (dev->cfg.tuner) {
	case AF9033_TUNER_IT9135_60:
	case AF9033_TUNER_IT9135_61:
	case AF9033_TUNER_IT9135_62:
		ret = af9033_wr_reg(dev, 0x800000, 0x01);
		if (ret < 0)
			goto err;
	}

	dev->bandwidth_hz = 0; /* force to program all parameters */

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_sleep(struct dvb_frontend *fe)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret, i;
	u8 tmp;

	ret = af9033_wr_reg(dev, 0x80004c, 1);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x800000, 0);
	if (ret < 0)
		goto err;

	for (i = 100, tmp = 1; i && tmp; i--) {
		ret = af9033_rd_reg(dev, 0x80004c, &tmp);
		if (ret < 0)
			goto err;

		usleep_range(200, 10000);
	}

	dev_dbg(&dev->client->dev, "loop=%d\n", i);

	if (i == 0) {
		ret = -ETIMEDOUT;
		goto err;
	}

	ret = af9033_wr_reg_mask(dev, 0x80fb24, 0x08, 0x08);
	if (ret < 0)
		goto err;

	/* prevent current leak (?) */
	if (dev->cfg.ts_mode == AF9033_TS_MODE_SERIAL) {
		/* enable parallel TS */
		ret = af9033_wr_reg_mask(dev, 0x00d917, 0x00, 0x01);
		if (ret < 0)
			goto err;

		ret = af9033_wr_reg_mask(dev, 0x00d916, 0x01, 0x01);
		if (ret < 0)
			goto err;
	}

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_get_tune_settings(struct dvb_frontend *fe,
		struct dvb_frontend_tune_settings *fesettings)
{
	/* 800 => 2000 because IT9135 v2 is slow to gain lock */
	fesettings->min_delay_ms = 2000;
	fesettings->step_size = 0;
	fesettings->max_drift = 0;

	return 0;
}

static int af9033_set_frontend(struct dvb_frontend *fe)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int ret, i, spec_inv, sampling_freq;
	u8 tmp, buf[3], bandwidth_reg_val;
	u32 if_frequency, freq_cw, adc_freq;

	dev_dbg(&dev->client->dev, "frequency=%d bandwidth_hz=%d\n",
			c->frequency, c->bandwidth_hz);

	/* check bandwidth */
	switch (c->bandwidth_hz) {
	case 6000000:
		bandwidth_reg_val = 0x00;
		break;
	case 7000000:
		bandwidth_reg_val = 0x01;
		break;
	case 8000000:
		bandwidth_reg_val = 0x02;
		break;
	default:
		dev_dbg(&dev->client->dev, "invalid bandwidth_hz\n");
		ret = -EINVAL;
		goto err;
	}

	/* program tuner */
	if (fe->ops.tuner_ops.set_params)
		fe->ops.tuner_ops.set_params(fe);

	/* program CFOE coefficients */
	if (c->bandwidth_hz != dev->bandwidth_hz) {
		for (i = 0; i < ARRAY_SIZE(coeff_lut); i++) {
			if (coeff_lut[i].clock == dev->cfg.clock &&
				coeff_lut[i].bandwidth_hz == c->bandwidth_hz) {
				break;
			}
		}
		ret =  af9033_wr_regs(dev, 0x800001,
				coeff_lut[i].val, sizeof(coeff_lut[i].val));
	}

	/* program frequency control */
	if (c->bandwidth_hz != dev->bandwidth_hz) {
		spec_inv = dev->cfg.spec_inv ? -1 : 1;

		for (i = 0; i < ARRAY_SIZE(clock_adc_lut); i++) {
			if (clock_adc_lut[i].clock == dev->cfg.clock)
				break;
		}
		adc_freq = clock_adc_lut[i].adc;

		/* get used IF frequency */
		if (fe->ops.tuner_ops.get_if_frequency)
			fe->ops.tuner_ops.get_if_frequency(fe, &if_frequency);
		else
			if_frequency = 0;

		sampling_freq = if_frequency;

		while (sampling_freq > (adc_freq / 2))
			sampling_freq -= adc_freq;

		if (sampling_freq >= 0)
			spec_inv *= -1;
		else
			sampling_freq *= -1;

		freq_cw = af9033_div(dev, sampling_freq, adc_freq, 23ul);

		if (spec_inv == -1)
			freq_cw = 0x800000 - freq_cw;

		if (dev->cfg.adc_multiplier == AF9033_ADC_MULTIPLIER_2X)
			freq_cw /= 2;

		buf[0] = (freq_cw >>  0) & 0xff;
		buf[1] = (freq_cw >>  8) & 0xff;
		buf[2] = (freq_cw >> 16) & 0x7f;

		/* FIXME: there seems to be calculation error here... */
		if (if_frequency == 0)
			buf[2] = 0;

		ret = af9033_wr_regs(dev, 0x800029, buf, 3);
		if (ret < 0)
			goto err;

		dev->bandwidth_hz = c->bandwidth_hz;
	}

	ret = af9033_wr_reg_mask(dev, 0x80f904, bandwidth_reg_val, 0x03);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x800040, 0x00);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x800047, 0x00);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg_mask(dev, 0x80f999, 0x00, 0x01);
	if (ret < 0)
		goto err;

	if (c->frequency <= 230000000)
		tmp = 0x00; /* VHF */
	else
		tmp = 0x01; /* UHF */

	ret = af9033_wr_reg(dev, 0x80004b, tmp);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x800000, 0x00);
	if (ret < 0)
		goto err;

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_get_frontend(struct dvb_frontend *fe)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int ret;
	u8 buf[8];

	dev_dbg(&dev->client->dev, "\n");

	/* read all needed registers */
	ret = af9033_rd_regs(dev, 0x80f900, buf, sizeof(buf));
	if (ret < 0)
		goto err;

	switch ((buf[0] >> 0) & 3) {
	case 0:
		c->transmission_mode = TRANSMISSION_MODE_2K;
		break;
	case 1:
		c->transmission_mode = TRANSMISSION_MODE_8K;
		break;
	}

	switch ((buf[1] >> 0) & 3) {
	case 0:
		c->guard_interval = GUARD_INTERVAL_1_32;
		break;
	case 1:
		c->guard_interval = GUARD_INTERVAL_1_16;
		break;
	case 2:
		c->guard_interval = GUARD_INTERVAL_1_8;
		break;
	case 3:
		c->guard_interval = GUARD_INTERVAL_1_4;
		break;
	}

	switch ((buf[2] >> 0) & 7) {
	case 0:
		c->hierarchy = HIERARCHY_NONE;
		break;
	case 1:
		c->hierarchy = HIERARCHY_1;
		break;
	case 2:
		c->hierarchy = HIERARCHY_2;
		break;
	case 3:
		c->hierarchy = HIERARCHY_4;
		break;
	}

	switch ((buf[3] >> 0) & 3) {
	case 0:
		c->modulation = QPSK;
		break;
	case 1:
		c->modulation = QAM_16;
		break;
	case 2:
		c->modulation = QAM_64;
		break;
	}

	switch ((buf[4] >> 0) & 3) {
	case 0:
		c->bandwidth_hz = 6000000;
		break;
	case 1:
		c->bandwidth_hz = 7000000;
		break;
	case 2:
		c->bandwidth_hz = 8000000;
		break;
	}

	switch ((buf[6] >> 0) & 7) {
	case 0:
		c->code_rate_HP = FEC_1_2;
		break;
	case 1:
		c->code_rate_HP = FEC_2_3;
		break;
	case 2:
		c->code_rate_HP = FEC_3_4;
		break;
	case 3:
		c->code_rate_HP = FEC_5_6;
		break;
	case 4:
		c->code_rate_HP = FEC_7_8;
		break;
	case 5:
		c->code_rate_HP = FEC_NONE;
		break;
	}

	switch ((buf[7] >> 0) & 7) {
	case 0:
		c->code_rate_LP = FEC_1_2;
		break;
	case 1:
		c->code_rate_LP = FEC_2_3;
		break;
	case 2:
		c->code_rate_LP = FEC_3_4;
		break;
	case 3:
		c->code_rate_LP = FEC_5_6;
		break;
	case 4:
		c->code_rate_LP = FEC_7_8;
		break;
	case 5:
		c->code_rate_LP = FEC_NONE;
		break;
	}

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;
	u8 tmp;

	*status = 0;

	/* radio channel status, 0=no result, 1=has signal, 2=no signal */
	ret = af9033_rd_reg(dev, 0x800047, &tmp);
	if (ret < 0)
		goto err;

	/* has signal */
	if (tmp == 0x01)
		*status |= FE_HAS_SIGNAL;

	if (tmp != 0x02) {
		/* TPS lock */
		ret = af9033_rd_reg_mask(dev, 0x80f5a9, &tmp, 0x01);
		if (ret < 0)
			goto err;

		if (tmp)
			*status |= FE_HAS_SIGNAL | FE_HAS_CARRIER |
					FE_HAS_VITERBI;

		/* full lock */
		ret = af9033_rd_reg_mask(dev, 0x80f999, &tmp, 0x01);
		if (ret < 0)
			goto err;

		if (tmp)
			*status |= FE_HAS_SIGNAL | FE_HAS_CARRIER |
					FE_HAS_VITERBI | FE_HAS_SYNC |
					FE_HAS_LOCK;
	}

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret, i, len;
	u8 buf[3], tmp;
	u32 snr_val;
	const struct val_snr *snr_lut;

	/* read value */
	ret = af9033_rd_regs(dev, 0x80002c, buf, 3);
	if (ret < 0)
		goto err;

	snr_val = (buf[2] << 16) | (buf[1] << 8) | buf[0];

	/* read current modulation */
	ret = af9033_rd_reg(dev, 0x80f903, &tmp);
	if (ret < 0)
		goto err;

	switch ((tmp >> 0) & 3) {
	case 0:
		len = ARRAY_SIZE(qpsk_snr_lut);
		snr_lut = qpsk_snr_lut;
		break;
	case 1:
		len = ARRAY_SIZE(qam16_snr_lut);
		snr_lut = qam16_snr_lut;
		break;
	case 2:
		len = ARRAY_SIZE(qam64_snr_lut);
		snr_lut = qam64_snr_lut;
		break;
	default:
		goto err;
	}

	for (i = 0; i < len; i++) {
		tmp = snr_lut[i].snr;

		if (snr_val < snr_lut[i].val)
			break;
	}

	*snr = tmp * 10; /* dB/10 */

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_read_signal_strength(struct dvb_frontend *fe, u16 *strength)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;
	u8 strength2;

	/* read signal strength of 0-100 scale */
	ret = af9033_rd_reg(dev, 0x800048, &strength2);
	if (ret < 0)
		goto err;

	/* scale value to 0x0000-0xffff */
	*strength = strength2 * 0xffff / 100;

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_update_ch_stat(struct af9033_dev *dev)
{
	int ret = 0;
	u32 err_cnt, bit_cnt;
	u16 abort_cnt;
	u8 buf[7];

	/* only update data every half second */
	if (time_after(jiffies, dev->last_stat_check + msecs_to_jiffies(500))) {
		ret = af9033_rd_regs(dev, 0x800032, buf, sizeof(buf));
		if (ret < 0)
			goto err;
		/* in 8 byte packets? */
		abort_cnt = (buf[1] << 8) + buf[0];
		/* in bits */
		err_cnt = (buf[4] << 16) + (buf[3] << 8) + buf[2];
		/* in 8 byte packets? always(?) 0x2710 = 10000 */
		bit_cnt = (buf[6] << 8) + buf[5];

		if (bit_cnt < abort_cnt) {
			abort_cnt = 1000;
			dev->ber = 0xffffffff;
		} else {
			/*
			 * 8 byte packets, that have not been rejected already
			 */
			bit_cnt -= (u32)abort_cnt;
			if (bit_cnt == 0) {
				dev->ber = 0xffffffff;
			} else {
				err_cnt -= (u32)abort_cnt * 8 * 8;
				bit_cnt *= 8 * 8;
				dev->ber = err_cnt * (0xffffffff / bit_cnt);
			}
		}
		dev->ucb += abort_cnt;
		dev->last_stat_check = jiffies;
	}

	return 0;
err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;

	ret = af9033_update_ch_stat(dev);
	if (ret < 0)
		return ret;

	*ber = dev->ber;

	return 0;
}

static int af9033_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;

	ret = af9033_update_ch_stat(dev);
	if (ret < 0)
		return ret;

	*ucblocks = dev->ucb;

	return 0;
}

static int af9033_i2c_gate_ctrl(struct dvb_frontend *fe, int enable)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;

	dev_dbg(&dev->client->dev, "enable=%d\n", enable);

	ret = af9033_wr_reg_mask(dev, 0x00fa04, enable, 0x01);
	if (ret < 0)
		goto err;

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_pid_filter_ctrl(struct dvb_frontend *fe, int onoff)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;

	dev_dbg(&dev->client->dev, "onoff=%d\n", onoff);

	ret = af9033_wr_reg_mask(dev, 0x80f993, onoff, 0x01);
	if (ret < 0)
		goto err;

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static int af9033_pid_filter(struct dvb_frontend *fe, int index, u16 pid,
		int onoff)
{
	struct af9033_dev *dev = fe->demodulator_priv;
	int ret;
	u8 wbuf[2] = {(pid >> 0) & 0xff, (pid >> 8) & 0xff};

	dev_dbg(&dev->client->dev, "index=%d pid=%04x onoff=%d\n",
			index, pid, onoff);

	if (pid > 0x1fff)
		return 0;

	ret = af9033_wr_regs(dev, 0x80f996, wbuf, 2);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x80f994, onoff);
	if (ret < 0)
		goto err;

	ret = af9033_wr_reg(dev, 0x80f995, index);
	if (ret < 0)
		goto err;

	return 0;

err:
	dev_dbg(&dev->client->dev, "failed=%d\n", ret);

	return ret;
}

static const struct dvb_frontend_ops af9033_ops = {
	.delsys = { SYS_DVBT },
	.info = {
		.name = "Afatech AF9033 (DVB-T)",
		.frequency_min = 174000000,
		.frequency_max = 862000000,
		.frequency_stepsize = 250000,
		.frequency_tolerance = 0,
		.caps =	FE_CAN_FEC_1_2 |
			FE_CAN_FEC_2_3 |
			FE_CAN_FEC_3_4 |
			FE_CAN_FEC_5_6 |
			FE_CAN_FEC_7_8 |
			FE_CAN_FEC_AUTO |
			FE_CAN_QPSK |
			FE_CAN_QAM_16 |
			FE_CAN_QAM_64 |
			FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO |
			FE_CAN_HIERARCHY_AUTO |
			FE_CAN_RECOVER |
			FE_CAN_MUTE_TS
	},

	.init = af9033_init,
	.sleep = af9033_sleep,

	.get_tune_settings = af9033_get_tune_settings,
	.set_frontend = af9033_set_frontend,
	.get_frontend = af9033_get_frontend,

	.read_status = af9033_read_status,
	.read_snr = af9033_read_snr,
	.read_signal_strength = af9033_read_signal_strength,
	.read_ber = af9033_read_ber,
	.read_ucblocks = af9033_read_ucblocks,

	.i2c_gate_ctrl = af9033_i2c_gate_ctrl,
};

static int af9033_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct af9033_config *cfg = client->dev.platform_data;
	struct af9033_dev *dev;
	int ret;
	u8 buf[8];
	u32 reg;

	/* allocate memory for the internal state */
	dev = kzalloc(sizeof(struct af9033_dev), GFP_KERNEL);
	if (dev == NULL) {
		ret = -ENOMEM;
		dev_err(&client->dev, "Could not allocate memory for state\n");
		goto err;
	}

	/* setup the state */
	dev->client = client;
	memcpy(&dev->cfg, cfg, sizeof(struct af9033_config));

	if (dev->cfg.clock != 12000000) {
		ret = -ENODEV;
		dev_err(&dev->client->dev,
				"unsupported clock %d Hz, only 12000000 Hz is supported currently\n",
				dev->cfg.clock);
		goto err_kfree;
	}

	/* firmware version */
	switch (dev->cfg.tuner) {
	case AF9033_TUNER_IT9135_38:
	case AF9033_TUNER_IT9135_51:
	case AF9033_TUNER_IT9135_52:
	case AF9033_TUNER_IT9135_60:
	case AF9033_TUNER_IT9135_61:
	case AF9033_TUNER_IT9135_62:
		reg = 0x004bfc;
		break;
	default:
		reg = 0x0083e9;
		break;
	}

	ret = af9033_rd_regs(dev, reg, &buf[0], 4);
	if (ret < 0)
		goto err_kfree;

	ret = af9033_rd_regs(dev, 0x804191, &buf[4], 4);
	if (ret < 0)
		goto err_kfree;

	dev_info(&dev->client->dev,
			"firmware version: LINK %d.%d.%d.%d - OFDM %d.%d.%d.%d\n",
			buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
			buf[7]);

	/* sleep */
	switch (dev->cfg.tuner) {
	case AF9033_TUNER_IT9135_38:
	case AF9033_TUNER_IT9135_51:
	case AF9033_TUNER_IT9135_52:
	case AF9033_TUNER_IT9135_60:
	case AF9033_TUNER_IT9135_61:
	case AF9033_TUNER_IT9135_62:
		/* IT9135 did not like to sleep at that early */
		break;
	default:
		ret = af9033_wr_reg(dev, 0x80004c, 1);
		if (ret < 0)
			goto err_kfree;

		ret = af9033_wr_reg(dev, 0x800000, 0);
		if (ret < 0)
			goto err_kfree;
	}

	/* configure internal TS mode */
	switch (dev->cfg.ts_mode) {
	case AF9033_TS_MODE_PARALLEL:
		dev->ts_mode_parallel = true;
		break;
	case AF9033_TS_MODE_SERIAL:
		dev->ts_mode_serial = true;
		break;
	case AF9033_TS_MODE_USB:
		/* usb mode for AF9035 */
	default:
		break;
	}

	/* create dvb_frontend */
	memcpy(&dev->fe.ops, &af9033_ops, sizeof(struct dvb_frontend_ops));
	dev->fe.demodulator_priv = dev;
	*cfg->fe = &dev->fe;
	if (cfg->ops) {
		cfg->ops->pid_filter = af9033_pid_filter;
		cfg->ops->pid_filter_ctrl = af9033_pid_filter_ctrl;
	}
	i2c_set_clientdata(client, dev);

	dev_info(&dev->client->dev, "Afatech AF9033 successfully attached\n");
	return 0;
err_kfree:
	kfree(dev);
err:
	dev_dbg(&client->dev, "failed=%d\n", ret);
	return ret;
}

static int af9033_remove(struct i2c_client *client)
{
	struct af9033_dev *dev = i2c_get_clientdata(client);

	dev_dbg(&dev->client->dev, "\n");

	dev->fe.ops.release = NULL;
	dev->fe.demodulator_priv = NULL;
	kfree(dev);

	return 0;
}

static const struct i2c_device_id af9033_id_table[] = {
	{"af9033", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, af9033_id_table);

static struct i2c_driver af9033_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "af9033",
	},
	.probe		= af9033_probe,
	.remove		= af9033_remove,
	.id_table	= af9033_id_table,
};

module_i2c_driver(af9033_driver);

MODULE_AUTHOR("Antti Palosaari <crope@iki.fi>");
MODULE_DESCRIPTION("Afatech AF9033 DVB-T demodulator driver");
MODULE_LICENSE("GPL");
