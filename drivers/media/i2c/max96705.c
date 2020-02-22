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
#define MAX96705_CROSSBAR_VS			0x40
#define MAX96705_CROSSBAR_VS_INVERT_MUX_VS	BIT(5)

/* Some default values */
#define MAX96705_I2C_ADDRESS		0x40

#define MAX96705_WIDTH			1280
#define MAX96705_HEIGHT			800
#define MAX96705_FORMAT			MEDIA_BUS_FMT_UYVY8_1X16

struct max96705_device {
	struct i2c_client		*max96705;
	struct v4l2_subdev		sd;
	struct media_pad		pads[2];
	struct v4l2_mbus_framefmt	mf;
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
	u8 cam_output_format;

	if (format->pad > 1)
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

static struct v4l2_subdev_video_ops max96705_video_ops = {
	.s_stream	= max96705_s_stream,
};

static const struct v4l2_subdev_pad_ops max96705_subdev_pad_ops = {
	.enum_mbus_code = max96705_enum_mbus_code,
	.get_fmt	= max96705_get_fmt,
	.set_fmt	= max96705_set_fmt,
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

	/*
	 * Invert VSYNC through cross-bar mux configuration.
	 * Note, this has to align between sensor and de-serializer.
	 * Don't distrupt bits other than of interest, bit 5.
	 */
	ret = max96705_write(dev, MAX96705_CROSSBAR_VS,
			     max96705_read(dev, MAX96705_CROSSBAR_VS) |
			     MAX96705_CROSSBAR_VS_INVERT_MUX_VS);
	if (ret < 0)
		return ret;
	msleep(8);

	ret = max96705_configure_address(dev, addr);
	if (ret < 0)
		return ret;

	return 0;
}

static struct max96705_device *dev_debug;

static ssize_t ultra96_max96705_debugfs_write(struct file *f,
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

	if (!strcasecmp(cmd, "m1r")) {
		pr_err("max96705 %s %d 0x%x @ 0x%x\n",
				__FUNCTION__, __LINE__,
				max96705_read(dev_debug, _addr), _addr);
	} else if (!strcasecmp(cmd, "m1w")) {
		max96705_write(dev_debug, _addr, _val);
	} else {
		pr_err("%s %d\n", __FUNCTION__, __LINE__);
	}

	kfree(kern_buff_start);
	return size;
}

static const struct file_operations fops_ultra96_max96705_dbgfs = {
	.owner = THIS_MODULE,
	.write = ultra96_max96705_debugfs_write,
};

static int ultra96_max96705_debugfs_init(struct max96705_device *dev)
{
	int err;
	struct dentry *ultra96_max96705_debugfs_dir, *ultra96_max96705_debugfs_file;

	ultra96_max96705_debugfs_dir = debugfs_create_dir("ultra96_max96705", NULL);
	if (!ultra96_max96705_debugfs_dir) {
		pr_err("debugfs_create_dir failed\n");
		return -ENODEV;
	}

	ultra96_max96705_debugfs_file =
		debugfs_create_file("testcase", 0444,
				    ultra96_max96705_debugfs_dir, NULL,
				    &fops_ultra96_max96705_dbgfs);
	if (!ultra96_max96705_debugfs_file) {
		pr_err("debugfs_create_file failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}
	dev_debug = dev;
	return 0;

err_dbgfs:
	debugfs_remove_recursive(ultra96_max96705_debugfs_dir);
	ultra96_max96705_debugfs_dir = NULL;
	return err;
}

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
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
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

	ultra96_max96705_debugfs_init(dev);

	return 0;

error:
	media_entity_cleanup(&dev->sd.entity);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int max96705_remove(struct i2c_client *client)
{
	struct max96705_device *dev = i2c_to_max96705(client);

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
