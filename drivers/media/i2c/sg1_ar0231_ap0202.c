// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver is for AR0231 + AP0202 for SG1 camera sensor
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
#include <media/v4l2-subdev.h>

/* Some default values */
#define AP0202_I2C_ADDRESS		0x5d

#define SG1_WIDTH			1280
#define SG1_HEIGHT			800
#define SG1_FORMAT			MEDIA_BUS_FMT_UYVY8_1X16

struct sg1_device {
	struct i2c_client		*sg1;
	struct i2c_client		*ap0202;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_mbus_framefmt	mf;
};

static inline struct sg1_device *sd_to_sg1(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sg1_device, sd);
}

static inline struct sg1_device *i2c_to_sg1(struct i2c_client *client)
{
	return sd_to_sg1(i2c_get_clientdata(client));
}

/* -----------------------------------------------------------------------------
 * AP0202
 */
static int ap0202_write8(struct sg1_device *dev, u16 reg, u8 val)
{
	u8 regbuf[3];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val;

	ret = i2c_master_send(dev->ap0202, regbuf, 3);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "%s: write8 reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ap0202_write(struct sg1_device *dev, u16 reg, u16 val)
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

static int ap0202_read(struct sg1_device *dev, u16 reg)
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

static u8 ap0202_read8(struct sg1_device *dev, u16 reg)
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

	ret = i2c_master_recv(dev->ap0202, regbuf, 1);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return regbuf[0];
}

static int ap0202_config_change(struct sg1_device *dev)
{
	int ret;

	ret = ap0202_write(dev, 0xfc00, 0x2800);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	ret = ap0202_write(dev, 0x0040, 0x8100);
	if (ret < 0) {
		dev_err(&dev->ap0202->dev, "Unable to write AP0202\n");
		return ret;
	}

	msleep(100);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Common
 */
static int sg1_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int sg1_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	code->code = SG1_FORMAT;

	return 0;
}

static int sg1_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct sg1_device *dev = sd_to_sg1(sd);

	if (format->pad)
		return -EINVAL;

	*mf = dev->mf;

	return 0;
}

static int sg1_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct sg1_device *dev = sd_to_sg1(sd);
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
	ap0202_write8(dev, 0xcaea, cam_output_format);

	ap0202_write(dev, 0xcae4, mf->width);
	ap0202_write(dev, 0xcae6, mf->height);
	ap0202_config_change(dev);

	dev->mf = *mf;

	return 0;
}

static struct v4l2_subdev_video_ops sg1_video_ops = {
	.s_stream	= sg1_s_stream,
};

static const struct v4l2_subdev_pad_ops sg1_subdev_pad_ops = {
	.enum_mbus_code = sg1_enum_mbus_code,
	.get_fmt	= sg1_get_fmt,
	.set_fmt	= sg1_set_fmt,
};

static struct v4l2_subdev_ops sg1_subdev_ops = {
	.video		= &sg1_video_ops,
	.pad		= &sg1_subdev_pad_ops,
};

static int sg1_initialize(struct sg1_device *dev)
{
	int ret;
	u32 addrs[2];

	ret = of_property_read_u32_array(dev->sg1->dev.of_node, "reg",
					addrs, ARRAY_SIZE(addrs));
	if (ret < 0) {
		dev_err(&dev->sg1->dev, "Invalid DT reg property\n");
		return ret;
	}

	/* Create the dummy I2C client for each AP0202. */
	dev->ap0202 = i2c_new_dummy(dev->sg1->adapter, AP0202_I2C_ADDRESS);
	if (!dev->ap0202)
		return -ENXIO;

	return 0;
}

static struct sg1_device *dev_debug;

static ssize_t ultra96_sg1_debugfs_write(struct file *f,
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
			pr_err("%s %d ap202:r:: 0x%x @ 0x%x\n",
					__FUNCTION__, __LINE__,
					ap0202_read8(dev_debug, _addr),
					_addr);
		} else {
			pr_err("%s %d ap202:r:: 0x%x @ 0x%x\n",
					__FUNCTION__, __LINE__,
					ap0202_read(dev_debug, _addr),
					_addr);
		}
	} else if (!strcasecmp(cmd, "a0w")) {
		pr_err("%s %d ap202:w:: %s %s %s %s\n", __FUNCTION__, __LINE__, cmd, width, addr, val);
		if (_width == 8) {
			ap0202_write8(dev_debug, _addr, _val);
		} else {
			ap0202_write(dev_debug, _addr, _val);
		}
	} else {
		pr_err("%s %d\n", __FUNCTION__, __LINE__);
	}

	kfree(kern_buff_start);
	return size;
}

static const struct file_operations fops_ultra96_sg1_dbgfs = {
	.owner = THIS_MODULE,
	.write = ultra96_sg1_debugfs_write,
};

static int ultra96_sg1_debugfs_init(struct sg1_device *dev)
{
	int err;
	struct dentry *ultra96_sg1_debugfs_dir, *ultra96_sg1_debugfs_file;

	ultra96_sg1_debugfs_dir = debugfs_create_dir("ultra96_sg1", NULL);
	if (!ultra96_sg1_debugfs_dir) {
		pr_err("debugfs_create_dir failed\n");
		return -ENODEV;
	}

	ultra96_sg1_debugfs_file =
		debugfs_create_file("testcase", 0444,
				    ultra96_sg1_debugfs_dir, NULL,
				    &fops_ultra96_sg1_dbgfs);
	if (!ultra96_sg1_debugfs_file) {
		pr_err("debugfs_create_file failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}
	dev_debug = dev;
	return 0;

err_dbgfs:
	debugfs_remove_recursive(ultra96_sg1_debugfs_dir);
	ultra96_sg1_debugfs_dir = NULL;
	return err;
}

static int sg1_probe(struct i2c_client *client)
{
	struct sg1_device *dev;
	struct fwnode_handle *ep;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->sg1 = client;

	/* Initialize the hardware. */
	ret = sg1_initialize(dev);
	if (ret < 0)
		goto error;

	v4l2_i2c_subdev_init(&dev->sd, client, &sg1_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret < 0)
		goto error;

	/* default format */
	dev->mf.colorspace = V4L2_COLORSPACE_SRGB;
	dev->mf.field = V4L2_FIELD_NONE;
	dev->mf.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	dev->mf.quantization = V4L2_QUANTIZATION_DEFAULT;
	dev->mf.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	/* these are user configuration in set_fmt() */
	dev->mf.width = SG1_WIDTH;
	dev->mf.height = SG1_HEIGHT;
	dev->mf.code = SG1_FORMAT;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!ep) {
		dev_err(&client->dev,
			"Unable to get endpoint in node %pOF: %ld\n",
			client->dev.of_node, PTR_ERR(ep));
		ret = -ENOENT;
		goto error;
	}
	dev->sd.fwnode = dev_fwnode(&client->dev);

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error;

	dev_info(&client->dev, "SG1 driver registered\n");

	ultra96_sg1_debugfs_init(dev);

	return 0;

error:
	media_entity_cleanup(&dev->sd.entity);
	if (dev->ap0202)
		i2c_unregister_device(dev->ap0202);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int sg1_remove(struct i2c_client *client)
{
	struct sg1_device *dev = i2c_to_sg1(client);

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	media_entity_cleanup(&dev->sd.entity);

	if (dev->ap0202)
		i2c_unregister_device(dev->ap0202);

	return 0;
}

static void sg1_shutdown(struct i2c_client *client)
{
	struct sg1_device *dev = i2c_to_sg1(client);

	/* make sure stream off during shutdown (reset/reboot) */
	sg1_s_stream(&dev->sd, 0);
}

static const struct of_device_id sg1_of_ids[] = {
	{ .compatible = "sensing,sg1", },
	{ }
};
MODULE_DEVICE_TABLE(of, sg1_of_ids);

static struct i2c_driver sg1_i2c_driver = {
	.driver	= {
		.name	= "sg1",
		.of_match_table = sg1_of_ids,
	},
	.probe_new	= sg1_probe,
	.remove		= sg1_remove,
	.shutdown	= sg1_shutdown,
};

module_i2c_driver(sg1_i2c_driver);

MODULE_DESCRIPTION("SG1 Camera driver for AR0231 and AP0202AT");
MODULE_LICENSE("GPL");
