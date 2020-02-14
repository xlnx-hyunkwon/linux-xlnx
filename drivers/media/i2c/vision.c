// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver is the combination of AR0231 + AP0202 + MAX96705
 */

#include <linux/debugfs.h>
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
#define MAX96705_FORMAT			MEDIA_BUS_FMT_UYVY8_1X16

struct vision_device {
	struct i2c_client		*client; /* Client is MAX9286 */
	struct i2c_client		*max96705[4];
	struct i2c_client		*ap0202[4];
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_ctrl_handler	ctrls;
	struct v4l2_mbus_framefmt	mf;
	u8				num_sensors;
	u8				num_isps;
};

//TODO: remove
static int ap0202_configure(struct vision_device *dev, u8 addr, u8 index);

static inline struct vision_device *sd_to_vision(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vision_device, sd);
}

static inline struct vision_device *i2c_to_vision(struct i2c_client *client)
{
	return sd_to_vision(i2c_get_clientdata(client));
}

static int max96705_write(struct vision_device *dev, u8 reg, u8 val, u8 index)
{
	int ret;

	ret = i2c_smbus_write_byte_data(dev->max96705[index], reg, val);
	if (ret < 0)
		dev_err(&dev->max96705[index]->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max96705_read(struct vision_device *dev, u8 reg, u8 index)
{
	int ret;

	ret = i2c_smbus_read_byte_data(dev->max96705[index], reg);
	if (ret < 0)
		dev_err(&dev->max96705[index]->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int ap0202_write8(struct vision_device *dev, u16 reg, u8 val, u8 index)
{
	u8 regbuf[3];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val;

	ret = i2c_master_send(dev->ap0202[index], regbuf, 3);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: write8 reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ap0202_write(struct vision_device *dev, u16 reg, u16 val, u8 index)
{
	u8 regbuf[4];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val >> 8;
	regbuf[3] = val & 0xff;

	ret = i2c_master_send(dev->ap0202[index], regbuf, 4);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ap0202_read(struct vision_device *dev, u16 reg, u8 index)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(dev->ap0202[index], regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: send reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	ret = i2c_master_recv(dev->ap0202[index], regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return (regbuf[1] | (regbuf[0] << 8));
}

static u8 ap0202_read8(struct vision_device *dev, u16 reg, u8 index)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(dev->ap0202[index], regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: send reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	ret = i2c_master_recv(dev->ap0202[index], regbuf, 1);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return regbuf[0];
}

static int ap0202_config_change(struct vision_device *dev, u8 index)
{
	int ret;

	ret = ap0202_write(dev, 0xfc00, 0x2800, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0x0040, 0x8100, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	return 0;
}

static int vision_s_stream(struct v4l2_subdev *sd, int enable)
{
	/* Nothing yet */
	if (enable) {
		pr_info("Enabling\n");

	} else {
		pr_info("Disabling\n");
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
	struct vision_device *dev = sd_to_vision(sd);

	if (format->pad)
		return -EINVAL;

	*mf = dev->mf;

	return 0;
}

static int vision_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct vision_device *dev = sd_to_vision(sd);
	u8 cam_output_format;

	if (format->pad)
		return -EINVAL;

	mf->colorspace		= V4L2_COLORSPACE_SRGB;
	mf->field		= V4L2_FIELD_NONE;
	mf->ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT;
	mf->quantization	= V4L2_QUANTIZATION_DEFAULT;
	mf->xfer_func		= V4L2_XFER_FUNC_DEFAULT;

	/* FIXME: temporary format handling for debugging */
	switch (mf->code) {
	case MEDIA_BUS_FMT_UYVY8_1X16:
		cam_output_format = 0;
		break;
	case MEDIA_BUS_FMT_RBG888_1X24:
		cam_output_format = 1;
		break;
	case MEDIA_BUS_FMT_Y8_1X8:
		cam_output_format = 2;
		break;
	default:
		/* default to yuv */
		cam_output_format = 2;
		break;
	}
	/* FIXME: return yuv regardless to make validation happy */
	mf->code = MEDIA_BUS_FMT_UYVY8_1X16;
	ap0202_write8(dev, 0xcaea, cam_output_format, 0x0);

	ap0202_write(dev, 0xcae4, mf->width, 0x0);
	ap0202_write(dev, 0xcae6, mf->height, 0x0);
	ap0202_config_change(dev, 0x0);

	dev->mf = *mf;

	return 0;
}

static struct v4l2_subdev_video_ops vision_video_ops = {
	.s_stream	= vision_s_stream,
};

static const struct v4l2_subdev_pad_ops vision_subdev_pad_ops = {
	.enum_mbus_code = vision_enum_mbus_code,
	.get_fmt	= vision_get_fmt,
	.set_fmt	= vision_set_fmt,
};

static struct v4l2_subdev_ops vision_subdev_ops = {
	.video		= &vision_video_ops,
	.pad		= &vision_subdev_pad_ops,
};

static int max96705_configure_address(struct vision_device *dev, u8 addr, u8 index)
{
	int ret;

	/* Change the MAX96705 I2C address. */
	ret = max96705_write(dev, 0x00, addr << 1, index);
	if (ret < 0) {
		dev_err(&dev->max96705[index]->dev,
			"MAX96705 I2C address change failed (%d)\n", ret);
		return ret;
	}
	dev->max96705[index]->addr = addr;
	usleep_range(3500, 5000);

	return 0;
}

static int max96705_configure(struct vision_device *dev, u8 index)
{
	int ret;

	ret = max96705_write(dev, 0x04, 0x47, index);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	msleep(8);

	ret = max96705_write(dev, 0x07, 0x84, index);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	msleep(8);

	/* Reset the serializer */
	ret = max96705_write(dev, 0x0e, 0x02, index);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	msleep(10);
/*	ret = max96705_write(dev, 0x0f, 0x00, index);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}

	ret = max96705_write(dev, 0x0f, 0x02, index);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to write MAX96705\n");
		return ret;
	}
*/
	msleep(10);

	return 0;
}

static int ap0202_configure(struct vision_device *dev, u8 addr, u8 index)
{
	int ret;

	ret = ap0202_write(dev, 0xc804, 0x40, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc806, 0x4, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc808, 0x477, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc80a, 0x783, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc814, 0x4b0, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc816, 0x960, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc8a0, 0x0, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc8a2, 0x0, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc8a4, 0x780, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xc8a6, 0x438, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xcae4, 0x500, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xcae6, 0x2d0, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0xfc00, 0x2800, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0x0040, 0x8100, index);
	if (ret < 0) {
		dev_err(&dev->ap0202[index]->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	return 0;
}

static int camera_config(struct vision_device *dev)
{
	int i, ret, num_sensors, num_isps;
	u32 *sensor_addrs;
	u32 *isp_addrs;

	/* FIXME: isp / sensor arrays will be removed as this driver will take care one sensor only */
	ret = of_property_count_u32_elems(dev->client->dev.of_node, "sensor-reg");
	if (ret < 0) {
		dev_err(&dev->client->dev, "Invalid sensor-reg property\n");
		return ret;
	}

	num_sensors = ret;
	dev->num_sensors = num_sensors;
	pr_info("Declared %d sensors in devicetree!\n", num_sensors);

	sensor_addrs = devm_kcalloc(&dev->client->dev, num_sensors,
				    sizeof(*sensor_addrs), GFP_KERNEL);

	ret = of_property_read_u32_array(dev->client->dev.of_node, "sensor-reg",
					sensor_addrs, num_sensors);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Invalid sensor-reg property\n");
		return ret;
	}

	ret = of_property_count_u32_elems(dev->client->dev.of_node, "isp-reg");
	if (ret < 0) {
		dev_err(&dev->client->dev, "Invalid DT reg property\n");
		return ret;
	}

	num_isps = ret;
	dev->num_isps = num_isps;
	pr_info("Declared %d ISPs in devicetree!\n", num_isps);

	isp_addrs = devm_kcalloc(&dev->client->dev, num_isps,
				    sizeof(*isp_addrs), GFP_KERNEL);

	ret = of_property_read_u32_array(dev->client->dev.of_node, "isp-reg",
					isp_addrs, num_isps);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Invalid sensor-reg property\n");
		return ret;
	}

	if (num_sensors != num_isps) {
		pr_err("Number of ISPs (%d) should match sensors (%d)!",
		       num_sensors, num_isps);
		return -ENXIO;
	}

	for (i = 0; i < num_sensors; i++) {
		/* Create the dummy I2C client for each MAX96705. */
		dev->max96705[i] = i2c_new_dummy(dev->client->adapter, MAX96705_I2C_ADDRESS);
		if (!dev->max96705[i]) {
			return -ENXIO;
		}
	}

	for (i = 0; i < num_isps; i++) {
		/* Create the dummy I2C client for each AP0202. */
		dev->ap0202[i] = i2c_new_dummy(dev->client->adapter, AP0202_I2C_ADDRESS);
		if (!dev->ap0202[i]) {
			return -ENXIO;
		}
	}

	/* Configure sensors and ISPs one by one */
	for (i = 0; i < num_sensors; i++) {
		/* Configure the serializer */
		ret = max96705_configure(dev, i);
		if (ret < 0) {
			dev_err(&dev->max96705[i]->dev, "Unable to configure MAX96705\n");
			return ret;
		}

		msleep(10);

		ret = max96705_configure_address(dev, sensor_addrs[i], i);
		if (ret < 0) {
			dev_err(&dev->client->dev, "Unable to configure MAX96705 address\n");
			return ret;
		}

		pr_info("Configured MAX96705!!");

		msleep(10);

		ret = max96705_write(dev, 0x04, 0x87, i);
		if (ret < 0) {
			dev_err(&dev->max96705[i]->dev, "Unable to write MAX96705\n");
			return ret;
		}

		msleep(5);

#if 0
		/* Configure the ISP */
		ret = ap0202_configure(dev, isp_addrs[i], i);
		if (ret < 0) {
			dev_err(&dev->ap0202[i]->dev, "Unable to configure AP0202\n");
			return ret;
		}
#endif

		usleep_range(5000, 8000);
	}

	return 0;
}

static int vision_initialize(struct vision_device *dev)
{
	int ret;

	ret = camera_config(dev);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Unable to configure cameras\n");
		return ret;
	}
/*
	pr_info("0xCAEA: %04x\n", ap0202_read(dev, 0xcaea));
	pr_info("0xCAFC: %04x\n", ap0202_read(dev, 0xcafc));
	pr_info("0xCAE4: %04x\n", ap0202_read(dev, 0xcae4));
	pr_info("0xCAE6: %04x\n", ap0202_read(dev, 0xcae6));

	pr_info("Configured AP0202!!");
*/
	return 0;
}

static int ap0202_read(struct vision_device *dev, u16 reg, u8 index);
struct vision_device *dev_debug;

static ssize_t ultra96_vision_debugfs_write(struct file *f,
		const char __user *buf, size_t size, loff_t *pos)
{
	char *kern_buff, *kern_buff_start;
	char *cmd, *width, *addr, *val, *idx;
	u32 _width = 0x7, _addr = 0x7;
	u32 _val = 0x7, _idx = 0x7;
	int ret;

	if (*pos != 0 || size <= 0)
		return -EINVAL;

	kern_buff = kzalloc(size, GFP_KERNEL);
	if (!kern_buff)
		return -ENOMEM;
	kern_buff_start = kern_buff;

	ret = strncpy_from_user(kern_buff, buf, size);
	if (ret < 0) {
		kfree(kern_buff_start);
		return ret;
	}

	cmd = strsep(&kern_buff, " ");
	width = strsep(&kern_buff, " ");
	if (width)
		kstrtou32(width, 10, &_width);
	addr = strsep(&kern_buff, " ");
	if (addr)
		kstrtou32(addr, 16, &_addr);
	val = strsep(&kern_buff, " ");
	if (val)
		kstrtou32(val, 16, &_val);
	idx = strsep(&kern_buff, " ");
	if (idx)
		kstrtou32(idx, 16, &_idx);
//	pr_err("%s %d %x\n", __FUNCTION__, __LINE__, dev_debug);
//	pr_err("%s %d %s %s %s %s\n", __FUNCTION__, __LINE__, cmd, addr, val, idx);
//	pr_err("%s %d %x %x %x\n", __FUNCTION__, __LINE__, _addr, _val, _idx);

	if (!strcasecmp(cmd, "a0r")) {
		if (_width == 8) {
			pr_err("%s %d ap202:r:: 0x%x @ 0x%x, idx = %u\n",
					__FUNCTION__, __LINE__,
					ap0202_read8(dev_debug, _addr, _idx),
					_addr, _idx);
		} else {
			pr_err("%s %d ap202:r:: 0x%x @ 0x%x, idx = %u\n",
					__FUNCTION__, __LINE__,
					ap0202_read(dev_debug, _addr, _idx),
					_addr, _idx);
		}
	} else if (!strcasecmp(cmd, "a0w")) {
		pr_err("%s %d ap202:w:: %s %s %s %s %s\n", __FUNCTION__, __LINE__, cmd, width, addr, val, idx);
		if (_width == 8) {
			ap0202_write8(dev_debug, _addr, _val, _idx);
		} else {
			ap0202_write(dev_debug, _addr, _val, _idx);
		}
	} else if (!strcasecmp(cmd, "m1r")) {
		pr_err("max96705 %s %d 0x%x @ 0x%x, idx = %u\n",
				__FUNCTION__, __LINE__,
				max96705_read(dev_debug, _addr, _idx), _addr, _idx);
	} else if (!strcasecmp(cmd, "m1w")) {
		max96705_write(dev_debug, _addr, _val, _idx);
	} else {
		pr_err("%s %d\n", __FUNCTION__, __LINE__);
	}

	kfree(kern_buff_start);
	return size;
}

static const struct file_operations fops_ultra96_vision_dbgfs = {
	.owner = THIS_MODULE,
	.write = ultra96_vision_debugfs_write,
};

static int ultra96_vision_debugfs_init(struct vision_device *dev)
{
	int err;
	struct dentry *ultra96_vision_debugfs_dir, *ultra96_vision_debugfs_file;

	ultra96_vision_debugfs_dir = debugfs_create_dir("ultra96_vision", NULL);
	if (!ultra96_vision_debugfs_dir) {
		pr_err("debugfs_create_dir failed\n");
		return -ENODEV;
	}

	ultra96_vision_debugfs_file =
		debugfs_create_file("testcase", 0444,
				    ultra96_vision_debugfs_dir, NULL,
				    &fops_ultra96_vision_dbgfs);
	if (!ultra96_vision_debugfs_file) {
		pr_err("debugfs_create_file failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}
	dev_debug = dev;
	return 0;

err_dbgfs:
	debugfs_remove_recursive(ultra96_vision_debugfs_dir);
	ultra96_vision_debugfs_dir = NULL;
	return err;
}

static int vision_probe(struct i2c_client *client)
{
	struct vision_device *dev;
	int ret;
	unsigned int i;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;

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

	/* default format */
	dev->mf.colorspace = V4L2_COLORSPACE_SRGB;
	dev->mf.field = V4L2_FIELD_NONE;
	dev->mf.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	dev->mf.quantization = V4L2_QUANTIZATION_DEFAULT;
	dev->mf.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	/* these are user configuration in set_fmt() */
	dev->mf.width = MAX96705_WIDTH;
	dev->mf.height = MAX96705_HEIGHT;
	dev->mf.code = MAX96705_FORMAT;

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error_free_ctrls;

	pr_info("Vision driver registered\n");

	ultra96_vision_debugfs_init(dev);

	return 0;

error_free_ctrls:
	v4l2_ctrl_handler_free(&dev->ctrls);
error:
	media_entity_cleanup(&dev->sd.entity);
	for (i = 0; i < dev->num_sensors; i++) {
		if (dev->max96705[i])
			i2c_unregister_device(dev->max96705[i]);
	}

	for (i = 0; i < dev->num_isps; i++) {
		if (dev->ap0202[i])
			i2c_unregister_device(dev->ap0202[i]);
	}


	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int vision_remove(struct i2c_client *client)
{
	struct vision_device *dev = i2c_to_vision(client);
	unsigned int i;

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrls);
	media_entity_cleanup(&dev->sd.entity);

	for (i = 0; i < dev->num_sensors; i++) {
		if (dev->max96705[i])
			i2c_unregister_device(dev->max96705[i]);
	}

	for (i = 0; i < dev->num_isps; i++) {
		if (dev->ap0202[i])
			i2c_unregister_device(dev->ap0202[i]);
	}

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
