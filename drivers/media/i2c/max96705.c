// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver for Maxim MAX96705 serializer
 *
 * Copyright (C) 2020 Linaro Ltd.
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
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

/* MAX96705 registers */
#define MAX96705_SERADDR			0x00
#define MAX96705_MAIN_CONTROL			0x04
#define MAX96705_MAIN_CONTROL_FWDCCEN		BIT(0)
#define MAX96705_MAIN_CONTROL_REVCCEN		BIT(1)
#define MAX96705_MAIN_CONTROL_INTTYPE_UART	(1 << 2)
#define MAX96705_MAIN_CONTROL_CLINKEN		BIT(6)
#define MAX96705_MAIN_CONTROL_SEREN		BIT(7)
#define MAX96705_CONFIG				0x07
#define MAX96705_CONFIG_HVEN			BIT(2)
#define MAX96705_CONFIG_DBL			BIT(7)
#define MAX96705_CROSSBAR(x)			(0x20 + x)
#define MAX96705_CROSSBAR_VS			0x40
#define MAX96705_CROSSBAR_VS_INVERT_MUX_VS	BIT(5)

/* Some default values */
#define MAX96705_I2C_ADDRESS		0x40

#define MAX96705_WIDTH			1280
#define MAX96705_HEIGHT			800
#define MAX96705_FORMAT			MEDIA_BUS_FMT_UYVY8_1X16
#define MAX96705_PAD_SINK		0
#define MAX96705_PAD_SOURCE		1

struct max96705_device {
	struct i2c_client		*max96705;
	struct v4l2_subdev		sd;
	struct media_pad		pads[2];
	struct v4l2_mbus_framefmt	mf;
	struct dentry			*debugfs;
	u8				enabled;
};

static inline struct max96705_device *sd_to_max96705(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96705_device, sd);
}

static inline struct max96705_device *i2c_to_max96705(struct i2c_client *client)
{
	return sd_to_max96705(i2c_get_clientdata(client));
}

/* -----------------------------------------------------------------------------
 * MAX96705
 */
static int max96705_write(struct max96705_device *dev, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(dev->max96705, reg, val);
	if (ret < 0)
		dev_err(&dev->max96705->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max96705_read(struct max96705_device *dev, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(dev->max96705, reg);
	if (ret < 0)
		dev_err(&dev->max96705->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max96705_configure_address(struct max96705_device *dev, u8 addr)
{
	int ret;

	ret = max96705_write(dev, MAX96705_SERADDR, addr << 1);
	if (ret < 0)
		return ret;
	dev->max96705->addr = addr;
	usleep_range(3500, 5000);

	return 0;
}

static int max96705_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96705_device *dev = sd_to_max96705(sd);
	int ret;
	u8 control = MAX96705_MAIN_CONTROL_CLINKEN | MAX96705_MAIN_CONTROL_REVCCEN |
		     MAX96705_MAIN_CONTROL_FWDCCEN;

	if (enable == dev->enabled)
		return 0;
	dev->enabled = enable;

	if (enable)
		control |= MAX96705_MAIN_CONTROL_SEREN;

	ret = max96705_write(dev, MAX96705_MAIN_CONTROL, control);
	if (ret < 0)
		return ret;
	msleep(5);

	return 0;
}

static int max96705_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	code->code = MAX96705_FORMAT;

	return 0;
}

static int max96705_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct max96705_device *dev = sd_to_max96705(sd);

	if (format->pad > 1)
		return -EINVAL;

	*mf = dev->mf;

	return 0;
}

static int max96705_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct max96705_device *dev = sd_to_max96705(sd);

	if (format->pad > 1)
		return -EINVAL;

	mf->colorspace		= V4L2_COLORSPACE_SRGB;
	mf->field		= V4L2_FIELD_NONE;
	mf->ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT;
	mf->quantization	= V4L2_QUANTIZATION_DEFAULT;
	mf->xfer_func		= V4L2_XFER_FUNC_DEFAULT;

	/* FIXME: return yuv regardless to make validation happy */
	mf->code = MEDIA_BUS_FMT_UYVY8_1X16;

	dev->mf = *mf;

	return 0;
}

static int max96705_get_mbus_config(struct v4l2_subdev *sd,
				    unsigned int pad,
				    struct v4l2_mbus_config *config)
{
	struct max96705_device *dev = sd_to_max96705(sd);
	struct media_pad *remote;
	struct v4l2_mbus_config mbus_config = { 0 };
	int ret;

	if (pad != MAX96705_PAD_SOURCE)
		return -EINVAL;

	remote = media_entity_remote_pad(&sd->entity.pads[MAX96705_PAD_SINK]);
	if (!remote || !remote->entity)
		return -ENODEV;

	config->type = V4L2_MBUS_GMSL;
	/* Only 24 bit mode works. Hard-code */
	config->flags = V4L2_MBUS_GMSL_BWS_24B;

	ret = v4l2_subdev_call(media_entity_to_v4l2_subdev(remote->entity),
			       pad, get_mbus_config,
			       remote->index, &mbus_config);
	if (ret) {
		if (ret != -ENOIOCTLCMD) {
			dev_err(&dev->max96705->dev,
				"failed to get remote mbus configuration\n");
			return ret;
		}

		dev_info(&dev->max96705->dev,
			 "No remote mbus configuration available\n");
		/* Assume it's active high, compatible to GMSL */
		config->type = V4L2_MBUS_GMSL;
		config->flags |= V4L2_MBUS_GMSL_VSYNC_ACTIVE_HIGH;

		return 0;
	}

	if (mbus_config.type != V4L2_MBUS_PARALLEL) {
		dev_err(&dev->max96705->dev,
			"invalid mbus type %u\n", mbus_config.type);
		return -EINVAL;
	}

	if (mbus_config.flags & V4L2_MBUS_DATA_LSB) {
		unsigned int i;

		/*
		 * FIXME: This swaps the LSB and MSB using the crossbar:
		 * - din0 to dout7, din1 to dout6,,,
		 * - din16 to dout23, dout17 to dout22,,,
		 * as it turns out LSB and MSB are swapped in color component
		 * of captured frames. This is for a specific format, 8bit
		 * yuv422, and configuration (double mode). This can be handled
		 * by looking at the bus format. But such bus format doesn't
		 * exist, so it's hardcoded here to convert the data layout
		 * to supported one for now. It's also possible that it's
		 * swappable in other place such as ISP.
		 */
		for (i = 0; i < 8; i++) {
			ret = max96705_write(dev, MAX96705_CROSSBAR(i), 7 - i);
			if (ret)
				return ret;
		}
		for (i = 0; i < 8; i++) {
			ret = max96705_write(dev, MAX96705_CROSSBAR(16 + i),
					     23 - i);
			if (ret)
				return ret;
		}
	}

	/*
	 * Just propagate the vsync polarity from source to sync, assuming
	 * it's handled at de-serilizer properly. The max96705 can invert
	 * vsync (CXTP at 0x4d or CROSSBAR_VS at 0x40) if needed, to make
	 * sure vsync out is always active high.
	 */
	if (mbus_config.flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH)
		config->flags |= V4L2_MBUS_GMSL_VSYNC_ACTIVE_HIGH;
	else
		config->flags |= V4L2_MBUS_GMSL_VSYNC_ACTIVE_LOW;

	return 0;
}

static struct v4l2_subdev_video_ops max96705_video_ops = {
	.s_stream	= max96705_s_stream,
};

static const struct v4l2_subdev_pad_ops max96705_subdev_pad_ops = {
	.enum_mbus_code		= max96705_enum_mbus_code,
	.get_fmt		= max96705_get_fmt,
	.set_fmt		= max96705_set_fmt,
	.get_mbus_config	= max96705_get_mbus_config,
};

static struct v4l2_subdev_ops max96705_subdev_ops = {
	.video		= &max96705_video_ops,
	.pad		= &max96705_subdev_pad_ops,
};

static int max96705_initialize(struct max96705_device *dev)
{
	int ret;
	u32 addr;

	ret = of_property_read_u32(dev->max96705->dev.of_node, "reg", &addr);
	if (ret < 0) {
		dev_err(&dev->max96705->dev, "Invalid DT reg property\n");
		return ret;
	}

	dev->max96705->addr = MAX96705_I2C_ADDRESS;

	ret = max96705_write(dev, MAX96705_MAIN_CONTROL,
			     MAX96705_MAIN_CONTROL_CLINKEN |
			     MAX96705_MAIN_CONTROL_REVCCEN |
			     MAX96705_MAIN_CONTROL_FWDCCEN);
	if (ret < 0)
		return ret;
	msleep(5);

	ret = max96705_write(dev, MAX96705_CONFIG,
			     MAX96705_CONFIG_DBL | MAX96705_CONFIG_HVEN);
	if (ret < 0)
		return ret;
	msleep(2);

	ret = max96705_configure_address(dev, addr);
	if (ret < 0)
		return ret;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>

struct max96705_debugfs_dir {
	struct dentry *dir;
	int ref_cnt;
};

static struct max96705_debugfs_dir *dir;

static ssize_t max96705_debugfs_read(struct file *f, char __user *buf,
				     size_t size, loff_t *pos)
{
	struct max96705_device *dev = f->f_inode->i_private;
	unsigned int i;

	if (size <= 0)
		return -EINVAL;

	for (i = 0; i < size; i++)
		buf[i] = max96705_read(dev, *pos + i);
	*pos = size + 1;

	return size;
}

static ssize_t max96705_debugfs_write(struct file *f, const char __user *buf,
				      size_t size, loff_t *pos)
{
	struct max96705_device *dev = f->f_inode->i_private;
	unsigned int i;

	if (size <= 0)
		return -EINVAL;

	for (i = 0; i < size; i++)
		max96705_write(dev, *pos + i, buf[i]);
	*pos = size + 1;

	return size;
}

static const struct file_operations max96705_debugfs_fops = {
	.owner = THIS_MODULE,
	.llseek = default_llseek,
	.read = max96705_debugfs_read,
	.write = max96705_debugfs_write,
};

static int max96705_debugfs_init(struct max96705_device *dev)
{
	if (!dir) {
		dir = kzalloc(sizeof(*dir), GFP_KERNEL);
		if (!dir)
			return -ENOMEM;

		dir->dir = debugfs_create_dir("max96705", NULL);
		if (!dir->dir)
			return -ENODEV;
	}
	dir->ref_cnt++;

	dev->debugfs = debugfs_create_file(dev_name(&dev->max96705->dev), 0644,
					   dir->dir, dev,
					   &max96705_debugfs_fops);
	return 0;
}

static void max96705_debugfs_exit(struct max96705_device *dev)
{
	debugfs_remove(dev->debugfs);

	if (--dir->ref_cnt)
		return;

	debugfs_remove_recursive(dir->dir);
	dir = NULL;
}

#else /* CONFIG_DEBUG_FS */

static int max96705_debugfs_init(struct max96705_device *dev)
{
	return 0;
}

static void max96705_debugfs_exit(struct max96705_device *dev)
{
}

#endif /* CONFIG_DEBUG_FS */

static int max96705_probe(struct i2c_client *client)
{
	struct max96705_device *dev;
	struct fwnode_handle *ep;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->max96705 = client;

	/* Initialize the hardware. */
	ret = max96705_initialize(dev);
	if (ret < 0)
		goto error;

	v4l2_i2c_subdev_init(&dev->sd, client, &max96705_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	dev->pads[0].flags = MEDIA_PAD_FL_SINK;
	dev->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	ret = media_entity_pads_init(&dev->sd.entity, 2, dev->pads);
	if (ret < 0)
		goto error;

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

	dev_info(&client->dev, "Vision driver registered\n");

	max96705_debugfs_init(dev);

	return 0;

error:
	media_entity_cleanup(&dev->sd.entity);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int max96705_remove(struct i2c_client *client)
{
	struct max96705_device *dev = i2c_to_max96705(client);

	max96705_debugfs_exit(dev);
	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	media_entity_cleanup(&dev->sd.entity);

	return 0;
}

static void max96705_shutdown(struct i2c_client *client)
{
	struct max96705_device *dev = i2c_to_max96705(client);

	/* make sure stream off during shutdown (reset/reboot) */
	max96705_s_stream(&dev->sd, 0);
}

static const struct of_device_id max96705_of_ids[] = {
	{ .compatible = "maxim,max96705", },
	{ }
};
MODULE_DEVICE_TABLE(of, max96705_of_ids);

static struct i2c_driver max96705_i2c_driver = {
	.driver	= {
		.name	= "max96705",
		.of_match_table = max96705_of_ids,
	},
	.probe_new	= max96705_probe,
	.remove		= max96705_remove,
	.shutdown	= max96705_shutdown,
};

module_i2c_driver(max96705_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96705 GMSL Serializer Driver");
MODULE_AUTHOR("Manivannan Sadhasivam");
MODULE_LICENSE("GPL");
