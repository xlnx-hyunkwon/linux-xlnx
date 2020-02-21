// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver for Aptina AP0202AT ISP
 *
 * Copyright (C) 2020 Linaro Ltd.
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

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
#define AP0202AT_I2C_ADDRESS		0x5d

#define AP0202AT_WIDTH			1280
#define AP0202AT_HEIGHT			800
#define AP0202AT_FORMAT			MEDIA_BUS_FMT_UYVY8_1X16
#define AP0202AT_PAD_SOURCE			0

struct ap0202at_device {
	struct i2c_client		*ap0202at;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_mbus_framefmt	mf;
	struct dentry			*debugfs;
	u8				enabled;
};

static inline struct ap0202at_device *sd_to_ap0202at(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ap0202at_device, sd);
}

static inline struct ap0202at_device *i2c_to_ap0202at(struct i2c_client *client)
{
	return sd_to_ap0202at(i2c_get_clientdata(client));
}

/* -----------------------------------------------------------------------------
 * AP0202AT
 */
static int ap0202at_write8(struct ap0202at_device *dev, u16 reg, u8 val)
{
	u8 regbuf[3];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val;

	ret = i2c_master_send(dev->ap0202at, regbuf, 3);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: write8 reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

__maybe_unused static int ap0202at_write(struct ap0202at_device *dev, u16 reg, u16 val)
{
	u8 regbuf[4];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val >> 8;
	regbuf[3] = val & 0xff;

	ret = i2c_master_send(dev->ap0202at, regbuf, 4);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

__maybe_unused static int ap0202at_read(struct ap0202at_device *dev, u16 reg)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(dev->ap0202at, regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: send reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	ret = i2c_master_recv(dev->ap0202at, regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return (regbuf[1] | (regbuf[0] << 8));
}

static u8 ap0202at_read8(struct ap0202at_device *dev, u16 reg)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(dev->ap0202at, regbuf, 2);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: send reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	ret = i2c_master_recv(dev->ap0202at, regbuf, 1);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "%s: read reg error %d: reg=%x",
			__func__, ret, reg);
		return ret;
	}

	msleep(100);

	return regbuf[0];
}

/* -----------------------------------------------------------------------------
 * Common
 */
static int ap0202at_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ap0202at_device *dev = sd_to_ap0202at(sd);

	if (enable == dev->enabled)
		return 0;
	dev->enabled = enable;

	return 0;
}

static int ap0202at_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	code->code = AP0202AT_FORMAT;

	return 0;
}

static int ap0202at_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct ap0202at_device *dev = sd_to_ap0202at(sd);

	if (format->pad)
		return -EINVAL;

	*mf = dev->mf;

	return 0;
}

static int ap0202at_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct ap0202at_device *dev = sd_to_ap0202at(sd);
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
	dev->mf = *mf;

	return 0;
}

static int ap0202at_get_mbus_config(struct v4l2_subdev *sd,
			       unsigned int pad,
			       struct v4l2_mbus_pad_config *config)
{
	if (pad != AP0202AT_PAD_SOURCE)
		return -EINVAL;

	config->type = V4L2_MBUS_PARALLEL;
	config->parallel.vsync_active = true;;
	config->parallel.msb_align_d0 = true;;

	return 0;
}

static struct v4l2_subdev_video_ops ap0202at_video_ops = {
	.s_stream	= ap0202at_s_stream,
};

static const struct v4l2_subdev_pad_ops ap0202at_subdev_pad_ops = {
	.enum_mbus_code		= ap0202at_enum_mbus_code,
	.get_fmt		= ap0202at_get_fmt,
	.set_fmt		= ap0202at_set_fmt,
	.get_mbus_config	= ap0202at_get_mbus_config,
};

static struct v4l2_subdev_ops ap0202at_subdev_ops = {
	.video		= &ap0202at_video_ops,
	.pad		= &ap0202at_subdev_pad_ops,
};

static int ap0202at_initialize(struct ap0202at_device *dev)
{
	int ret;
	u32 addr;

	ret = of_property_read_u32(dev->ap0202at->dev.of_node, "reg", &addr);
	if (ret < 0) {
		dev_err(&dev->ap0202at->dev, "Invalid DT reg property\n");
		return ret;
	}

	dev->ap0202at->addr = addr;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>

struct ap0202at_debugfs_dir {
	struct dentry *dir;
	int ref_cnt;
};

static struct ap0202at_debugfs_dir *dir;

static ssize_t ap0202at_debugfs_read(struct file *f, char __user *buf,
				     size_t size, loff_t *pos)
{
	struct ap0202at_device *dev = f->f_inode->i_private;
	unsigned int i;

	if (size <= 0)
		return -EINVAL;

	for (i = 0; i < size; i++)
		buf[i] = ap0202at_read8(dev, *pos + i);
	*pos = size + 1;

	return size;
}

static ssize_t ap0202at_debugfs_write(struct file *f, const char __user *buf,
				      size_t size, loff_t *pos)
{
	struct ap0202at_device *dev = f->f_inode->i_private;
	unsigned int i;

	if (size <= 0)
		return -EINVAL;

	for (i = 0; i < size; i++)
		ap0202at_write8(dev, *pos + i, buf[i]);
	*pos = size + 1;

	return size;
}

static const struct file_operations ap0202at_debugfs_fops = {
	.owner = THIS_MODULE,
	.llseek = default_llseek,
	.read = ap0202at_debugfs_read,
	.write = ap0202at_debugfs_write,
};

static int ap0202at_debugfs_init(struct ap0202at_device *dev)
{
	if (!dir) {
		dir = kzalloc(sizeof(*dir), GFP_KERNEL);
		if (!dir)
			return -ENOMEM;

		dir->dir = debugfs_create_dir("ap0202at", NULL);
		if (!dir->dir)
			return -ENODEV;
	}
	dir->ref_cnt++;

	dev->debugfs = debugfs_create_file(dev_name(&dev->ap0202at->dev), 0644,
					   dir->dir, dev,
					   &ap0202at_debugfs_fops);
	return 0;
}

static void ap0202at_debugfs_exit(struct ap0202at_device *dev)
{
	debugfs_remove(dev->debugfs);

	if (--dir->ref_cnt)
		return;

	debugfs_remove_recursive(dir->dir);
	dir = NULL;
}

#else /* CONFIG_DEBUG_FS */

static int ap0202at_debugfs_init(struct ap0202at_device *dev)
{
	return 0;
}

static void ap0202at_debugfs_exit(struct ap0202at_device *dev)
{
}

#endif /* CONFIG_DEBUG_FS */

static int ap0202at_probe(struct i2c_client *client)
{
	struct ap0202at_device *dev;
	struct fwnode_handle *ep;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->ap0202at = client;

	/* Initialize the hardware. */
	ret = ap0202at_initialize(dev);
	if (ret < 0)
		goto error;

	v4l2_i2c_subdev_init(&dev->sd, client, &ap0202at_subdev_ops);
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
	dev->mf.width = AP0202AT_WIDTH;
	dev->mf.height = AP0202AT_HEIGHT;
	dev->mf.code = AP0202AT_FORMAT;

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

	dev_info(&client->dev, "AP0202AT driver registered\n");

	ap0202at_debugfs_init(dev);

	return 0;

error:
	media_entity_cleanup(&dev->sd.entity);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int ap0202at_remove(struct i2c_client *client)
{
	struct ap0202at_device *dev = i2c_to_ap0202at(client);

	ap0202at_debugfs_exit(dev);
	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	media_entity_cleanup(&dev->sd.entity);

	return 0;
}

static void ap0202at_shutdown(struct i2c_client *client)
{
	struct ap0202at_device *dev = i2c_to_ap0202at(client);

	/* make sure stream off during shutdown (reset/reboot) */
	ap0202at_s_stream(&dev->sd, 0);
}

static const struct of_device_id ap0202at_of_ids[] = {
	{ .compatible = "aptina,ap0202at", },
	{ }
};
MODULE_DEVICE_TABLE(of, ap0202at_of_ids);

static struct i2c_driver ap0202at_i2c_driver = {
	.driver	= {
		.name	= "ap0202at",
		.of_match_table = ap0202at_of_ids,
	},
	.probe_new	= ap0202at_probe,
	.remove		= ap0202at_remove,
	.shutdown	= ap0202at_shutdown,
};

module_i2c_driver(ap0202at_i2c_driver);

MODULE_DESCRIPTION("AP0202AT Camera driver");
MODULE_LICENSE("GPL");
