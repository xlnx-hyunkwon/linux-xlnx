// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver is the combination of AR0231 + AP0202 + MAX96705 + MAX9286
 */

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define MAX96705_I2C_ADDRESS		0x40
#define AP0202_I2C_ADDRESS		0x5d

#define MAX96705_WIDTH			1280
#define MAX96705_HEIGHT			800
#define MAX96705_FORMAT			MEDIA_BUS_FMT_UYVY8_2X8

/* Register 0x00 */
#define MAX9286_MSTLINKSEL_AUTO		(7 << 5)
#define MAX9286_MSTLINKSEL(n)		((n) << 5)
#define MAX9286_EN_VS_GEN		BIT(4)
#define MAX9286_LINKEN(n)		(1 << (n))
/* Register 0x01 */
#define MAX9286_FSYNCMODE_ECU		(3 << 6)
#define MAX9286_FSYNCMODE_EXT		(2 << 6)
#define MAX9286_FSYNCMODE_INT_OUT	(1 << 6)
#define MAX9286_FSYNCMODE_INT_HIZ	(0 << 6)
#define MAX9286_GPIEN			BIT(5)
#define MAX9286_ENLMO_RSTFSYNC		BIT(2)
#define MAX9286_FSYNCMETH_AUTO		(2 << 0)
#define MAX9286_FSYNCMETH_SEMI_AUTO	(1 << 0)
#define MAX9286_FSYNCMETH_MANUAL	(0 << 0)
#define MAX9286_REG_FSYNC_PERIOD_L	0x06
#define MAX9286_REG_FSYNC_PERIOD_M	0x07
#define MAX9286_REG_FSYNC_PERIOD_H	0x08
/* Register 0x0a */
#define MAX9286_FWDCCEN(n)		(1 << ((n) + 4))
#define MAX9286_REVCCEN(n)		(1 << (n))
/* Register 0x0c */
#define MAX9286_HVEN			BIT(7)
#define MAX9286_EDC_6BIT_HAMMING	(2 << 5)
#define MAX9286_EDC_6BIT_CRC		(1 << 5)
#define MAX9286_EDC_1BIT_PARITY		(0 << 5)
#define MAX9286_DESEL			BIT(4)
#define MAX9286_INVVS			BIT(3)
#define MAX9286_INVHS			BIT(2)
#define MAX9286_HVSRC_D0		(2 << 0)
#define MAX9286_HVSRC_D14		(1 << 0)
#define MAX9286_HVSRC_D18		(0 << 0)
/* Register 0x12 */
#define MAX9286_CSILANECNT(n)		(((n) - 1) << 6)
#define MAX9286_CSIDBL			BIT(5)
#define MAX9286_DBL			BIT(4)
#define MAX9286_DATATYPE_USER_8BIT	(11 << 0)
#define MAX9286_DATATYPE_USER_YUV_12BIT	(10 << 0)
#define MAX9286_DATATYPE_USER_24BIT	(9 << 0)
#define MAX9286_DATATYPE_RAW14		(8 << 0)
#define MAX9286_DATATYPE_RAW11		(7 << 0)
#define MAX9286_DATATYPE_RAW10		(6 << 0)
#define MAX9286_DATATYPE_RAW8		(5 << 0)
#define MAX9286_DATATYPE_YUV422_10BIT	(4 << 0)
#define MAX9286_DATATYPE_YUV422_8BIT	(3 << 0)
#define MAX9286_DATATYPE_RGB555		(2 << 0)
#define MAX9286_DATATYPE_RGB565		(1 << 0)
#define MAX9286_DATATYPE_RGB888		(0 << 0)
/* Register 0x15 */
#define MAX9286_VC(n)			((n) << 5)
#define MAX9286_VCTYPE			BIT(4)
#define MAX9286_CSIOUTEN		BIT(3)
#define MAX9286_0X15_RESV		(3 << 0)
/* Register 0x1b */
#define MAX9286_SWITCHIN(n)		(1 << ((n) + 4))
#define MAX9286_ENEQ(n)			(1 << (n))
/* Register 0x27 */
#define MAX9286_LOCKED			BIT(7)
/* Register 0x31 */
#define MAX9286_FSYNC_LOCKED		BIT(6)
/* Register 0x34 */
#define MAX9286_I2CLOCACK		BIT(7)
#define MAX9286_I2CSLVSH_1046NS_469NS	(3 << 5)
#define MAX9286_I2CSLVSH_938NS_352NS	(2 << 5)
#define MAX9286_I2CSLVSH_469NS_234NS	(1 << 5)
#define MAX9286_I2CSLVSH_352NS_117NS	(0 << 5)
#define MAX9286_I2CMSTBT_837KBPS	(7 << 2)
#define MAX9286_I2CMSTBT_533KBPS	(6 << 2)
#define MAX9286_I2CMSTBT_339KBPS	(5 << 2)
#define MAX9286_I2CMSTBT_173KBPS	(4 << 2)
#define MAX9286_I2CMSTBT_105KBPS	(3 << 2)
#define MAX9286_I2CMSTBT_84KBPS		(2 << 2)
#define MAX9286_I2CMSTBT_28KBPS		(1 << 2)
#define MAX9286_I2CMSTBT_8KBPS		(0 << 2)
#define MAX9286_I2CSLVTO_NONE		(3 << 0)
#define MAX9286_I2CSLVTO_1024US		(2 << 0)
#define MAX9286_I2CSLVTO_256US		(1 << 0)
#define MAX9286_I2CSLVTO_64US		(0 << 0)
/* Register 0x3b */
#define MAX9286_REV_TRF(n)		((n) << 4)
#define MAX9286_REV_AMP(n)		((((n) - 30) / 10) << 1) /* in mV */
#define MAX9286_REV_AMP_X		BIT(0)
/* Register 0x3f */
#define MAX9286_EN_REV_CFG		BIT(6)
#define MAX9286_REV_FLEN(n)		((n) - 20)
/* Register 0x49 */
#define MAX9286_VIDEO_DETECT_MASK	0x0f
/* Register 0x69 */
#define MAX9286_LFLTBMONMASKED		BIT(7)
#define MAX9286_LOCKMONMASKED		BIT(6)
#define MAX9286_AUTOCOMBACKEN		BIT(5)
#define MAX9286_AUTOMASKEN		BIT(4)
#define MAX9286_MASKLINK(n)		((n) << 0)

#define MAX9286_NUM_GMSL		4
#define MAX9286_N_SINKS			4
#define MAX9286_N_PADS			5
#define MAX9286_SRC_PAD			4

#define MAXIM_I2C_I2C_SPEED_400KHZ	MAX9286_I2CMSTBT_339KBPS
#define MAXIM_I2C_I2C_SPEED_100KHZ	MAX9286_I2CMSTBT_105KBPS
#define MAXIM_I2C_SPEED			MAXIM_I2C_I2C_SPEED_100KHZ

#define SOURCE_MASK			BIT(0)
#define ROUTE_MASK			BIT(0)
#define CSI2_DATA_LANES			4

struct vision_device {
	struct i2c_client		*client; /* Client is MAX9286 */
	struct i2c_client		*max96705;
	struct i2c_client		*ap0202;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_ctrl_handler	ctrls;
};

//TODO: remove
static int ap0202_configure(struct vision_device *dev);

static inline struct vision_device *sd_to_vision(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vision_device, sd);
}

static inline struct vision_device *i2c_to_vision(struct i2c_client *client)
{
	return sd_to_vision(i2c_get_clientdata(client));
}

static int max9286_read(struct vision_device *dev, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(dev->client, reg);
	if (ret < 0)
		dev_err(&dev->client->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max9286_write(struct vision_device *dev, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(dev->client, reg, val);
	if (ret < 0)
		dev_err(&dev->client->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max96705_write(struct vision_device *dev, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(dev->max96705, reg, val);
	if (ret < 0)
		dev_err(&dev->max96705->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max96705_read(struct vision_device *dev, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(dev->max96705, reg);
	if (ret < 0)
		dev_err(&dev->max96705->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int ap0202_write(struct vision_device *dev, u16 reg, u16 val)
{
	u8 regbuf[4];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val >> 8;
	regbuf[3] = val & 0xff;

	ret = i2c_master_send(dev->ap0202, regbuf, 4);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ap0202_read(struct vision_device *dev, u16 reg)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(dev->ap0202, regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "%s: send reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	ret = i2c_master_recv(dev->ap0202, regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return (regbuf[1] | (regbuf[0] << 8));
}

static int max9286_check_video_links(struct vision_device *dev)
{
	unsigned int i;
	int ret;

	/*
	 * Make sure valid video links are detected.
	 * The delay is not characterized in de-serializer manual, wait up
	 * to 5 ms.
	 */
	for (i = 0; i < 10; i++) {
		ret = max9286_read(dev, 0x49);
		if (ret < 0)
			return -EIO;

		if ((ret & MAX9286_VIDEO_DETECT_MASK) == SOURCE_MASK)
			break;

		usleep_range(3500, 5000);
	}

	if (i == 10) {
		dev_err(&dev->client->dev,
			"Unable to detect video links 0x49: 0x%02x\n", ret);
		return -EIO;
	}

	/* Make sure all enabled links are locked (4ms max). */
	for (i = 0; i < 10; i++) {
		ret = max9286_read(dev, 0x27);
		if (ret < 0)
			return -EIO;

		if (ret & MAX9286_LOCKED)
			break;

		usleep_range(3500, 4500);
	}

	if (i == 10) {
		dev_err(&dev->client->dev, "Not all enabled links locked\n");
		return -EIO;
	}

	return 0;
}

void print_max9286_regs(struct vision_device *dev)
{
       int i;

        for (i = 0x00; i <= 0xff; i++)
               pr_info("MAX9286: 0x%x: 0x%x", i, max9286_read(dev, i));
}

void print_max96705_regs(struct vision_device *dev)
{
       int i;

        for (i = 0x00; i <= 0xff; i++)
               pr_info("MAX96705: 0x%x: 0x%x", i, max96705_read(dev, i));
}

static int vision_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vision_device *dev = sd_to_vision(sd);
	unsigned int i;
	bool sync = false;
	int ret;

	/* Nothing yet */
	if (enable) {
		ret = max9286_check_video_links(dev);
		if (ret)
			return ret;

		/*
		 * Wait until frame synchronization is locked.
		 *
		 * Manual says frame sync locking should take ~6 VTS.
		 * From pratical experience at least 8 are required. Give
		 * 12 complete frames time (~33ms at 30 fps) to achieve frame
		 * locking before returning error.
		 */
		for (i = 0; i < 36; i++) {
			if (max9286_read(dev, 0x31) & MAX9286_FSYNC_LOCKED) {
				sync = true;
				break;
			}
			usleep_range(9000, 11000);
		}

		if (!sync) {
			dev_err(&dev->client->dev,
				"Failed to get frame synchronization\n");
			return -EINVAL;
		}

		pr_info("Enabling\n");
		max9286_write(dev, 0x15, 0x80 | MAX9286_VCTYPE |
			      MAX9286_CSIOUTEN | MAX9286_0X15_RESV);

	} else {
		pr_info("Disabling\n");
		max9286_write(dev, 0x15, MAX9286_VCTYPE | MAX9286_0X15_RESV);
	}

	return 0;
}

static int vision_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	code->code = MAX96705_FORMAT;

	return 0;
}

static int vision_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;

	if (format->pad)
		return -EINVAL;

	mf->width		= MAX96705_WIDTH;
	mf->height		= MAX96705_HEIGHT;
	mf->code		= MAX96705_FORMAT;
	mf->colorspace		= V4L2_COLORSPACE_SRGB;
	mf->field		= V4L2_FIELD_NONE;
	mf->ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT;
	mf->quantization	= V4L2_QUANTIZATION_DEFAULT;
	mf->xfer_func		= V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static struct v4l2_subdev_video_ops vision_video_ops = {
	.s_stream	= vision_s_stream,
};

static const struct v4l2_subdev_pad_ops vision_subdev_pad_ops = {
	.enum_mbus_code = vision_enum_mbus_code,
	.get_fmt	= vision_get_fmt,
	.set_fmt	= vision_get_fmt,
};

static struct v4l2_subdev_ops vision_subdev_ops = {
	.video		= &vision_video_ops,
	.pad		= &vision_subdev_pad_ops,
};

static void max9286_configure_i2c(struct vision_device *dev, bool localack)
{
	u8 config = MAX9286_I2CSLVSH_469NS_234NS | MAX9286_I2CSLVTO_1024US |
		    MAXIM_I2C_SPEED;

	if (localack)
		config |= MAX9286_I2CLOCACK;

	max9286_write(dev, 0x34, config);
	usleep_range(3000, 5000);
}

static const u8 link_order[] = {
	(3 << 6) | (2 << 4) | (1 << 2) | (0 << 0), /* xxxx */
	(3 << 6) | (2 << 4) | (1 << 2) | (0 << 0), /* xxx0 */
	(3 << 6) | (2 << 4) | (0 << 2) | (1 << 0), /* xx0x */
	(3 << 6) | (2 << 4) | (1 << 2) | (0 << 0), /* xx10 */
	(3 << 6) | (0 << 4) | (2 << 2) | (1 << 0), /* x0xx */
	(3 << 6) | (1 << 4) | (2 << 2) | (0 << 0), /* x1x0 */
	(3 << 6) | (1 << 4) | (0 << 2) | (2 << 0), /* x10x */
	(3 << 6) | (1 << 4) | (1 << 2) | (0 << 0), /* x210 */
	(0 << 6) | (3 << 4) | (2 << 2) | (1 << 0), /* 0xxx */
	(1 << 6) | (3 << 4) | (2 << 2) | (0 << 0), /* 1xx0 */
	(1 << 6) | (3 << 4) | (0 << 2) | (2 << 0), /* 1x0x */
	(2 << 6) | (3 << 4) | (1 << 2) | (0 << 0), /* 2x10 */
	(1 << 6) | (0 << 4) | (3 << 2) | (2 << 0), /* 10xx */
	(2 << 6) | (1 << 4) | (3 << 2) | (0 << 0), /* 21x0 */
	(2 << 6) | (1 << 4) | (0 << 2) | (3 << 0), /* 210x */
	(3 << 6) | (2 << 4) | (1 << 2) | (0 << 0), /* 3210 */
};

static int max9286_configure(struct vision_device *dev)
{
	int ret;

	ret = max9286_write(dev, 0x0a, 0x11);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(5);

	ret = max9286_write(dev, 0x34, 0xb6);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	usleep_range(5000, 8000);
	ret = max9286_write(dev, 0x15, 0x03);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	usleep_range(5000, 8000);

	max9286_write(dev, 0x0b, link_order[0]);
	mdelay(5);

	ret = max9286_write(dev, 0x12, 0xf3);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	usleep_range(5000, 8000);
	ret = max9286_write(dev, 0x00, 0x81);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(5);
	max9286_write(dev, 0x69, 0x0e);
	mdelay(5);
	max9286_write(dev, 0x01, 0x22);

	mdelay(5);
	ret = max9286_write(dev, 0x63, 0x00);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(5);
	ret = max9286_write(dev, 0x64, 0x00);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(5);

	return max9286_write(dev, 0x1c, 0xf4);
}

static int max96705_configure_address(struct vision_device *dev, u8 addr)
{
	int ret;

	/* Change the MAX96705 I2C address. */
	ret = max96705_write(dev, 0x00, addr << 1);
	if (ret < 0) {
		dev_err(&dev->max96705->dev,
			"MAX96705 I2C address change failed (%d)\n", ret);
		return ret;
	}
	dev->max96705->addr = addr;
	usleep_range(3500, 5000);

	return 0;
}

static int max96705_configure(struct vision_device *dev)
{
	int ret;

	ret = max96705_write(dev, 0x04, 0x47);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	mdelay(8);

	ret = max96705_write(dev, 0x07, 0x84);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	mdelay(8);

	/* Reset the serializer */
	ret = max96705_write(dev, 0x0e, 0x02);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	mdelay(10);
/*	ret = max96705_write(dev, 0x0f, 0x00);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	ret = max96705_write(dev, 0x0f, 0x02);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}
*/
	mdelay(10);

	return 0;
}

static int ap0202_configure(struct vision_device *dev)
{
	int ret;

	ret = ap0202_write(dev, 0xc804, 0x40);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc806, 0x4);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc808, 0x477);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc80a, 0x783);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc814, 0x4b0);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc816, 0x960);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc8a0, 0x0);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc8a2, 0x0);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc8a4, 0x780);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xc8a6, 0x438);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xcae4, 0x500);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xcae6, 0x2d0);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0xfc00, 0x2800);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	ret = ap0202_write(dev, 0x0040, 0x8100);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	mdelay(100);

	return 0;
}

static int vision_initialize(struct vision_device *dev)
{
	u32 addr;
	int ret;

	ret = of_property_read_u32(dev->client->dev.of_node, "camera-reg",
					 &addr);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Invalid DT reg property\n");
		return ret;
	}

	/* Configure the de-serializer */
	ret = max9286_configure(dev);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(10);

	pr_info("Configured MAX9286!!");

	/* Configure the serializer */
	ret = max96705_configure(dev);
	if (ret < 0) {
		dev_err(&dev->max96705->dev, "Unable to configure MAX96705\n");
		return ret;
	}

	mdelay(10);

	ret = max96705_configure_address(dev, addr);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX96705 address\n");
		return ret;
	}

	pr_info("Configured MAX96705!!");

	mdelay(10);

	ret = max96705_write(dev, 0x04, 0x87);
	if (ret < 0) {
		dev_err(&dev->max96705->dev, "Unable to write MAX96705\n");
		return ret;
	}

	mdelay(5);

	ret = max9286_write(dev, 0x0c, 0x91);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure MAX9286\n");
		return ret;
	}

	mdelay(5);

	/* Configure the ISP */
	ret = ap0202_configure(dev);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to configure AP0202\n");
		return ret;
	}

	pr_info("0xCAEA: %04x\n", ap0202_read(dev, 0xcaea));
	pr_info("0xCAFC: %04x\n", ap0202_read(dev, 0xcafc));
	pr_info("0xCAE4: %04x\n", ap0202_read(dev, 0xcae4));
	pr_info("0xCAE6: %04x\n", ap0202_read(dev, 0xcae6));

	pr_info("Configured AP0202!!");

	max9286_configure_i2c(dev, false);

	return 0;
}

static int vision_probe(struct i2c_client *client)
{
	struct vision_device *dev;
	struct fwnode_handle *ep;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;

	/* Create the dummy I2C client for the MAX96705. */
	dev->max96705 = i2c_new_dummy(client->adapter, MAX96705_I2C_ADDRESS);
	if (!dev->max96705) {
		ret = -ENXIO;
		goto error;
	}

	/* Create the dummy I2C client for the AP0202. */
	dev->ap0202 = i2c_new_dummy(client->adapter, AP0202_I2C_ADDRESS);
	if (!dev->ap0202) {
		ret = -ENXIO;
		goto error;
	}

	/* Initialize the hardware. */
	ret = vision_initialize(dev);
	if (ret < 0)
		goto error;

	v4l2_ctrl_handler_init(&dev->ctrls, 1);

	v4l2_ctrl_new_std(&dev->ctrls, NULL, V4L2_CID_PIXEL_RATE, 50000000,
                          50000000, 1, 50000000);

	dev->sd.ctrl_handler = &dev->ctrls;

	ret = dev->ctrls.error;
	if (ret)
		goto error_free_ctrls;

	v4l2_i2c_subdev_init(&dev->sd, client, &vision_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.dev = &client->dev;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret < 0)
		goto error_free_ctrls;

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error_free_ctrls;

	pr_info("Vision driver registered\n");

	return 0;

error_free_ctrls:
	v4l2_ctrl_handler_free(&dev->ctrls);
error:
	media_entity_cleanup(&dev->sd.entity);
	if (dev->max96705)
		i2c_unregister_device(dev->max96705);

	if (dev->ap0202)
		i2c_unregister_device(dev->ap0202);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int vision_remove(struct i2c_client *client)
{
	struct vision_device *dev = i2c_to_vision(client);

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrls);
	media_entity_cleanup(&dev->sd.entity);
	i2c_unregister_device(dev->max96705);

	return 0;
}

static void vision_shutdown(struct i2c_client *client)
{
	struct vision_device *dev = i2c_to_vision(client);

	/* make sure stream off during shutdown (reset/reboot) */
	vision_s_stream(&dev->sd, 0);
}

static const struct of_device_id vision_of_ids[] = {
	{ .compatible = "sensing,vision", },
	{ }
};
MODULE_DEVICE_TABLE(of, vision_of_ids);

static struct i2c_driver vision_i2c_driver = {
	.driver	= {
		.name	= "vision",
		.of_match_table = vision_of_ids,
	},
	.probe_new	= vision_probe,
	.remove		= vision_remove,
	.shutdown	= vision_shutdown,
};

module_i2c_driver(vision_i2c_driver);

MODULE_DESCRIPTION("GMSL Camera driver for AR0231");
MODULE_AUTHOR("Manivannan Sadhasivam");
MODULE_LICENSE("GPL");
