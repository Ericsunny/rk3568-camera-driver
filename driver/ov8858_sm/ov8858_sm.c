#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/of.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#include <linux/uaccess.h>
#endif
#include <media/v4l2-async.h>
#include <media/media-entity.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>

#include "ov8858_sm_regs.h"
#include <linux/rk-camera-module.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-device.h>

static bool test_stream;
static bool mode_1080p;
/*
 * 故障注入(仅用于验证错误码能否向上返回,默认 0 = 关闭):
 *   设成 N(>0) 时,stop_stream 的 I2C 写会人为返回 -N,
 *   用来确认"异常停流"能把可定位的错误码传回调用者,而不是被吞掉。
 *   例: echo 5 > /sys/module/ov8858_sm/parameters/inject_stop_err  → 返回 -EIO
 */
static int inject_stop_err;
/*
 * 停流状态(只读,供用户态查询):VIDIOC_STREAMOFF 本身按 V4L2 规范不返回错误
 * (videobuf2 的 stop_streaming 回调是 void),所以把"最近一次停流错误码"和
 * "累计失败次数"导出成只读 module parameter,用户态可直接读取定位。
 *   cat /sys/module/ov8858_sm/parameters/last_stop_err
 *   cat /sys/module/ov8858_sm/parameters/stop_err_cnt
 */
static int last_stop_err;
static int stop_err_cnt;
module_param(mode_1080p, bool, 0644);
MODULE_PARM_DESC(mode_1080p, "use the derived 1920x1080@60 mode instead of 1632x1224");
module_param(test_stream, bool, 0644);
MODULE_PARM_DESC(test_stream, "apply the init sequence at probe (bring-up test)");
module_param(inject_stop_err, int, 0644);
MODULE_PARM_DESC(inject_stop_err, "fault injection: make stop_stream return -N (0=off)");
module_param(last_stop_err, int, 0444);
MODULE_PARM_DESC(last_stop_err, "last stop_stream error code (read-only, 0=ok)");
module_param(stop_err_cnt, int, 0444);
MODULE_PARM_DESC(stop_err_cnt, "cumulative stop_stream failure count (read-only)");

#define OV8858_SM_NAME "ov8858-sm"

/* TODO(L1-B): every number below must come from the OV8858 datasheet. */
/* Mode: 1080p 1920x1080@60, 2-lane RAW10, datasheet table 2-1 says 720 Mbps/lane.
 * link_freq (V4L2) = 720 Mbps / 2 (DDR)      = 360 MHz
 * pixel_rate       = 720 Mbps * 2 lanes / 10 = 144 Mpixel/s
 */
#define OV8858_SM_LINK_FREQ     360000000LL   /* Hz */
#define OV8858_SM_PIXEL_RATE    144000000LL   /* pixel/s */
#define OV8858_SM_EXPOSURE_MIN  1
#define OV8858_SM_EXPOSURE_MAX  0xfffe
#define OV8858_SM_EXPOSURE_DEF  0x100
#define OV8858_SM_GAIN_MIN      0
#define OV8858_SM_GAIN_MAX      0xff
#define OV8858_SM_GAIN_DEF      0

#define OV8858_SM_NUM_SUPPLIES  3

struct ov8858_sm {
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct i2c_client *client;
	struct clk *xvclk;
	struct gpio_desc *pwdn;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *anal_gain;
	bool is_1080p;
	bool powered;		/* 电源/时钟是否已打开,保证上电断电严格配对 */
	bool streaming;		/* 是否正在出流:流期间拒绝改 ACTIVE 格式 */
	unsigned int test_pattern;	/* 彩条缓存:start_stream 重灌表后被 0x5e00=0 擦掉,需重放 */
};

static const struct i2c_device_id ov8858_sm_id[] = {
	{ "ov8858-sm", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov8858_sm_id);

static const struct of_device_id ov8858_sm_of_match[] = {
	{ .compatible = "ovti,ov8858-sm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov8858_sm_of_match);

static int ov8858_sm_power_on(struct ov8858_sm *ov8858);
static void ov8858_sm_power_off(struct ov8858_sm *ov8858);
static int ov8858_sm_read_reg(struct i2c_client *client, u16 reg, u8 *val);

/* ---------------- register access: 16-bit address, 8-bit data ---------------- */

static int ov8858_sm_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = {
		.addr = client->addr, .flags = 0, .len = 3, .buf = buf,
	};
	int ret = i2c_transfer(client->adapter, &msg, 1);

	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static int ov8858_sm_write_array(struct i2c_client *client,
				 const struct ov8858_sm_regval *regs)
{
	int i, ret;

	for (i = 0; regs[i].addr != OV8858_SM_REG_NULL; i++) {
		ret = ov8858_sm_write_reg(client, regs[i].addr, regs[i].val);
		if (ret) {
			dev_err(&client->dev, "write 0x%04x=0x%02x failed: %d\n",
				regs[i].addr, regs[i].val, ret);
			return ret;
		}
	}

	return 0;
}

/* ---------------- streaming ---------------- */

static int ov8858_sm_start_stream(struct ov8858_sm *ov8858)
{
	int ret;

	ret = ov8858_sm_write_array(ov8858->client, ov8858_sm_global_regs);
	if (ret)
		return ret;

	/* the mode table also ends with 0x0100 = 0x01 (stream on) */
	if (ov8858->is_1080p)
		ret = ov8858_sm_write_array(ov8858->client,
					    ov8858_sm_mode_1080p_regs);
	else
		ret = ov8858_sm_write_array(ov8858->client,
					    ov8858_sm_mode_1632x1224_regs);
	if (ret)
		return ret;

	/* 初始化表把 0x5e00 写回 0, 会擦掉用户设的彩条 —— 重放缓存值。
	 * 实测教训: 只在 set_ctrl 写一次的话, 彩条在开流瞬间被表覆盖。 */
	if (ov8858->test_pattern) {
		ret = ov8858_sm_write_reg(ov8858->client, 0x5e00,
					  (u8)((ov8858->test_pattern - 1) | 0x80));
		if (ret)
			return ret;
		dev_info(&ov8858->client->dev, "test_pattern %u re-applied after init table\n",
			 ov8858->test_pattern);
	}

	/* 回读关键寄存器:确认"以为切了模式"和"sensor 真的切了"是一回事 */
	{
		u8 wh = 0, wl = 0, hh = 0, hl = 0, th = 0, tl = 0, vh = 0, vl = 0;
		u8 st = 0, sub = 0, bin = 0;

		ov8858_sm_read_reg(ov8858->client, 0x0100, &st);
		ov8858_sm_read_reg(ov8858->client, 0x3814, &sub);
		ov8858_sm_read_reg(ov8858->client, 0x3821, &bin);
		ov8858_sm_read_reg(ov8858->client, 0x3808, &wh);
		ov8858_sm_read_reg(ov8858->client, 0x3809, &wl);
		ov8858_sm_read_reg(ov8858->client, 0x380a, &hh);
		ov8858_sm_read_reg(ov8858->client, 0x380b, &hl);
		ov8858_sm_read_reg(ov8858->client, 0x380c, &th);
		ov8858_sm_read_reg(ov8858->client, 0x380d, &tl);
		ov8858_sm_read_reg(ov8858->client, 0x380e, &vh);
		ov8858_sm_read_reg(ov8858->client, 0x380f, &vl);

		dev_info(&ov8858->client->dev,
			 "start_stream[%s]: stream=0x%02x 0x3814=0x%02x 0x3821=0x%02x size=%dx%d hts=%d vts=%d\n",
			 ov8858->is_1080p ? "1080p" : "1632x1224",
			 st, sub, bin, (wh << 8) | wl, (hh << 8) | hl,
			 (th << 8) | tl, (vh << 8) | vl);
	}

	return 0;
}

static int ov8858_sm_stop_stream(struct ov8858_sm *ov8858)
{
	int ret;

	if (inject_stop_err > 0) {
		dev_err(&ov8858->client->dev,
			"[fault-inject] stop_stream 人为返回 %d\n", -inject_stop_err);
		ret = -inject_stop_err;
	} else {
		ret = ov8858_sm_write_reg(ov8858->client, 0x0100, 0x00);
	}

	if (ret < 0) {
		/* VIDIOC_STREAMOFF 无法返回错误(vb2 回调是 void),这里落到只读参数里 */
		last_stop_err = ret;
		stop_err_cnt++;
		dev_err(&ov8858->client->dev,
			"stop_stream failed: %d (sensor 可能仍在出流)\n", ret);
	}

	return ret;
}

static int ov8858_sm_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);
	int ret;

	dev_info(sd->dev, "s_stream %d\n", enable);

	if (enable) {
		/* L4: 上电交给 runtime PM —— get_sync 会等 resume 回调把电和时钟打开 */
		ret = pm_runtime_get_sync(&ov8858->client->dev);
		if (ret < 0) {
			dev_err(&ov8858->client->dev, "pm_runtime_get_sync failed: %d\n", ret);
			pm_runtime_put_noidle(&ov8858->client->dev);
			return ret;
		}

		ret = ov8858_sm_start_stream(ov8858);
		if (ret) {
			dev_err(&ov8858->client->dev, "start_stream failed: %d\n", ret);
			pm_runtime_put_sync(&ov8858->client->dev);
			return ret;
		}
		ov8858->streaming = true;
	} else {
		/*
		 * 停流失败必须向上返回:原来这里把返回值丢了,上层(V4L2/rkcif)
		 * 会以为 STREAMOFF 成功,传感器其实还在出流,表现为"停了没停、
		 * 再开就 busy",且日志里毫无线索。现在把驱动返回的真实 errno
		 * (-EIO / -ENXIO / -ETIMEDOUT …) 原样传回。
		 * 注意:即使失败也要归还 pm_runtime 引用,否则会泄漏电源计数。
		 */
		ret = ov8858_sm_stop_stream(ov8858);
		if (ret) {
			dev_err(&ov8858->client->dev,
				"stop_stream failed: %d (sensor 可能仍在出流)\n", ret);
		} else {
			ov8858->streaming = false;
		}
		/* 关流后不立刻断电:标记"最后忙",等 autosuspend 超时再挂起 */
		pm_runtime_mark_last_busy(&ov8858->client->dev);
		pm_runtime_put_autosuspend(&ov8858->client->dev);

		return ret;
	}

	return 0;
}

/*
 * L1-B: rkcif 在打开 video 节点时会问 sensor 一句"你的物理链路是什么样"
 * (MIPI CSI-2 / 几条 lane / 时钟连续还是非连续)。这里必须实现:
 * 不实现的话这次 subdev 调用返回 -ENOIOCTLCMD(-515),而 rkcif 只判断
 * "ret && ret != -ENOIOCTLCMD" 就放过了,却没有把 ret 清零,
 * 最后 return ret 把 -515 当成自己的失败返回 -> /dev/video0 open 失败。
 */
static int ov8858_sm_g_mbus_config(struct v4l2_subdev *sd,
				   struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2;
	cfg->flags = V4L2_MBUS_CSI2_2_LANE |
		     V4L2_MBUS_CSI2_CHANNEL_0 |
		     V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;

	return 0;
}

static int ov8858_sm_g_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi);
static int ov8858_sm_s_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi);

static const struct v4l2_subdev_video_ops ov8858_sm_video_ops = {
	.s_stream = ov8858_sm_s_stream,
	.g_mbus_config = ov8858_sm_g_mbus_config,
	.g_frame_interval = ov8858_sm_g_frame_interval,
	.s_frame_interval = ov8858_sm_s_frame_interval,
};

/* rkcif asks every sensor for its module info when a video node is opened. */
static void ov8858_sm_get_module_inf(struct ov8858_sm *ov8858,
				     struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, "ov8858-sm", sizeof(inf->base.sensor));
	strscpy(inf->base.module, "ov8858-sm", sizeof(inf->base.module));
	strscpy(inf->base.lens, "default", sizeof(inf->base.lens));
}

static long ov8858_sm_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		ov8858_sm_get_module_inf(ov8858, arg);
		return 0;
	case RKMODULE_SET_QUICK_STREAM: {
		u32 stream = *(u32 *)arg;
		int ret;

		/*
		 * L1-B 收尾: quick-stream 是绕过 s_stream() 的快路径,
		 * 不经过 pm_runtime_get_sync(), 传感器可能已被 runtime PM
		 * 挂起(时钟/电源全断)。这里自己把电源生命周期补上,
		 * 否则就是"在断电的传感器上裸写 I2C"。
		 */
		ret = pm_runtime_get_sync(&ov8858->client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&ov8858->client->dev);
			dev_err(&ov8858->client->dev,
				"quick_stream: pm_runtime_get_sync failed: %d\n",
				ret);
			return ret;
		}

		ret = ov8858_sm_write_reg(ov8858->client, 0x0100,
					  stream ? 0x01 : 0x00);

		pm_runtime_mark_last_busy(&ov8858->client->dev);
		pm_runtime_put_autosuspend(&ov8858->client->dev);

		return ret;
	}
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
/*
 * L1-B 收尾: core.compat_ioctl32
 *
 * 两条路径的差别是这里最关键的一课:
 *
 *   64 位: subdev_ioctl() -> video_usercopy()
 *          v4l2 核心先把 ioctl 参数从用户态拷进一块内核缓冲,再调用
 *          sd->ops->core->ioctl()。所以驱动里直接 memset(inf, ...)
 *          拿到的是内核地址,安全。
 *
 *   32 位: subdev_compat_ioctl32() -> v4l2_subdev_call(core, compat_ioctl32)
 *          核心只是把原始 arg 透传下来,没有任何 usercopy。驱动若把 arg
 *          直接当内核指针用,就是在裸写用户地址 —— arm64 上会立刻触发
 *          PAN 保护:
 *          "Unable to handle kernel access to user memory outside
 *           uaccess routines",调用进程收到 SIGSEGV。
 *
 * 因此 compat 分支必须自己搭这层桥:
 *   1) compat_ptr() 把 32 位地址扩成指针(高位清零);
 *   2) GET 类:先在内核缓冲里填好结构,再 copy_to_user();
 *   3) SET 类:先 copy_from_user() 取标量,再把内核地址交给通用 handler。
 *
 * rkmodule_inf 全是定长整型/字符数组(没有指针、没有 long),32/64 位
 * 布局一致,所以用户态看到的字节和 64 位完全一样。
 * 不实现这个回调时,32 位进程调用会拿到 -ENOIOCTLCMD(-515)。
 */
static long ov8858_sm_compat_ioctl32(struct v4l2_subdev *sd,
				     unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO: {
		struct rkmodule_inf *inf = kzalloc(sizeof(*inf), GFP_KERNEL);

		if (!inf)
			return -ENOMEM;
		ret = ov8858_sm_ioctl(sd, cmd, inf);
		if (ret >= 0 && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		return ret;
	}
	case RKMODULE_SET_QUICK_STREAM: {
		u32 stream;

		if (copy_from_user(&stream, up, sizeof(stream)))
			return -EFAULT;
		return ov8858_sm_ioctl(sd, cmd, &stream);
	}
	default:
		return -ENOIOCTLCMD;
	}
}
#endif

static const struct v4l2_subdev_core_ops ov8858_sm_core_ops = {
	.ioctl = ov8858_sm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = ov8858_sm_compat_ioctl32,
#endif
};

/* The sensor always outputs RAW10 Bayer SBGGR. */
#define OV8858_SM_MBUS_CODE	MEDIA_BUS_FMT_SBGGR10_1X10

/*
 * 按"目标模式"填 fmt。注意它**不读也不写** ov8858->is_1080p:
 * set_fmt 的 TRY 分支必须先在一份临时选择上算出结果,再决定要不要提交状态。
 */
static void ov8858_sm_fill_fmt_mode(bool is_1080p,
				    struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = OV8858_SM_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	if (is_1080p) {
		fmt->width = 1920;
		fmt->height = 1080;
	} else {
		fmt->width = 1632;
		fmt->height = 1224;
	}
}

static void ov8858_sm_fill_fmt(struct ov8858_sm *ov8858,
			       struct v4l2_mbus_framefmt *fmt)
{
	ov8858_sm_fill_fmt_mode(ov8858->is_1080p, fmt);
}

static int ov8858_sm_get_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *fmt)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	/* 同 set_fmt: pad 防御 + TRY 的 cfg 判空, 否则管线传播路径可 Oops */
	if (fmt->pad != 0)
		return -EINVAL;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		if (!cfg)
			return -EINVAL;
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		return -ENOTTY;
#endif
	} else {
		ov8858_sm_fill_fmt(ov8858, &fmt->format);
	}

	return 0;
}

static int ov8858_sm_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *fmt)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);
	bool want_1080p;

	/*
	 * TRY/ACTIVE 隔离(本函数是这次内核闭环的修复点):
	 *
	 * 原实现先写 ov8858->is_1080p,再判断 which —— 于是上层任何一次
	 * VIDIOC_SUBDEV_TRY_FMT(1920x1080) 都会把驱动全局模式真的切到 1080p:
	 *   - 之后 G_FMT(ACTIVE) 报 1920x1080(与实际出流不一致);
	 *   - rkcif 按 1632x1224 申请 buffer,而 sensor 按 1080p 出流,
	 *     两边行数对不上(实测 buffer 变成 2048×1080),画面直接错乱。
	 *
	 * 正确语义:TRY 只承诺"如果你真的设,会得到什么",不能产生任何副作用;
	 * 只有 ACTIVE 才允许提交状态,且流期间必须拒绝(-EBUSY)。
	 */
	want_1080p = (fmt->format.width == 1920 && fmt->format.height == 1080);

	/* pad 防御 + TRY 的 cfg 判空:
	 * 管线传播(dphy/rkcif 协商)可能带越界 pad 或 NULL cfg 进来,
	 * 直接解引用 v4l2_subdev_get_try_format 会 Oops(v4l2-compliance
	 * -m 实测打过: ov8858_sm_get_fmt+0x5c ← csi2_dphy_get_set_fmt)。 */
	if (fmt->pad != 0)
		return -EINVAL;

	/*无论 TRY 还是 ACTIVE,都先按请求把 fmt 夹到最近的支持模式 */
	ov8858_sm_fill_fmt_mode(want_1080p, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		if (!cfg)
			return -EINVAL;
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
		return 0;
#else
		return -ENOTTY;
#endif
	}

	if (ov8858->streaming) {
		dev_err(sd->dev, "set_fmt(ACTIVE) refused while streaming\n");
		return -EBUSY;
	}

	/* 到这里才是 ACTIVE:提交驱动状态 */
	ov8858->is_1080p = want_1080p;

	return 0;
}

/*
 * L1-B 收尾: pad.enum_mbus_code
 *
 * 之前没实现它: 标准的 subdev 用户态无法枚举 pad 支持哪些 media bus 格式,
 * 只能靠试。sensor 只有一种输出 —— RAW10 Bayer SBGGR, 枚举表就一项:
 * index 0 返回 OV8858_SM_MBUS_CODE, 其余 index / 非 0 pad 一律 -EINVAL。
 */
static int ov8858_sm_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad != 0)
		return -EINVAL;

	if (code->index >= 1)
		return -EINVAL;

	code->code = OV8858_SM_MBUS_CODE;

	return 0;
}

/*
 * L1-B 收尾: pad.enum_frame_size
 *
 * 厂商驱动实现了它, 我们之前没有: 结果是 v4l2-ctl --list-formats-ext
 * 只能列出格式、列不出 "Size: Discrete"; 上层(摄像头 HAL / 媒体框架)
 * 想在不 try_fmt 的情况下枚举可用分辨率也做不到。
 *
 * 两个模式来自同一份 datasheet 时序: 1632x1224(2-lane R2A 默认) 和
 * 1920x1080(自推, 见 docs/17 §3.7), 都是 100% 有效帧。
 */
static const struct ov8858_sm_frmsize {
	u32 width;
	u32 height;
} ov8858_sm_frmsizes[] = {
	{ 1632, 1224 },
	{ 1920, 1080 },
};

static int ov8858_sm_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ov8858_sm_frmsizes))
		return -EINVAL;

	/* 每个模式都是"离散"的, min == max */
	fse->min_width = fse->max_width = ov8858_sm_frmsizes[fse->index].width;
	fse->min_height = fse->max_height = ov8858_sm_frmsizes[fse->index].height;

	return 0;
}

/*
 * L1-B 收尾: 帧间隔(g/s/enum frame_interval)
 *
 * 为什么必须补: 2022 版 rkaiq 在 get_sensor_caps 阶段枚举帧间隔, 我们没实现,
 * 它一直打印 "@get_sensor_caps /dev/v4l-subdev2: Enum sensor frame interval failed"。
 *
 * 本内核(4.19 + Rockchip 回移)的 ABI 与新版不同:
 *   - G/S_FRAME_INTERVAL 在 video ops(sd, interval), 不在 pad ops;
 *   - pad ops 只有 enum_frame_interval;
 *   - 结构体里是 v4l2_fract interval, 不是 min/max。
 *
 * 帧率不是可调量: 由 datasheet 时序 HTS/VTS 与固定 pixel_rate 决定。
 *   fps = pixel_rate / (2 * HTS * VTS)   (系数 2 见 0x380e 注释 143.9M/(3880*1236))
 * 所以这里只"报告"帧间隔, set 只接受与当前模式一致的值。
 */
static const struct ov8858_sm_timing {
	u32 hts;
	u32 vts;
} ov8858_sm_timings[] = {
	{ 1928, 1244 },		/* 1632x1224: 144M/(2*1928*1244) = 30.02 fps */
	{ 1940, 1236 },		/* 1920x1080: 144M/(2*1940*1236) = 30.03 fps */
};

static u32 ov8858_sm_calc_fps(int idx)
{
	return (u32)div_u64((u64)OV8858_SM_PIXEL_RATE,
			    2ULL * ov8858_sm_timings[idx].hts *
			    ov8858_sm_timings[idx].vts);
}

static int ov8858_sm_enum_frame_interval(struct v4l2_subdev *sd,
					 struct v4l2_subdev_pad_config *cfg,
					 struct v4l2_subdev_frame_interval_enum *fie)
{
	bool size_ok = false;
	int i;

	if (fie->pad != 0)
		return -EINVAL;

	/*
	 * 帧间隔必须按尺寸回答: rkcif 的 video 节点把
	 * VIDIOC_ENUM_FRAMEINTERVALS 原样转发到 subdev(capture.c:3608),
	 * 不校验宽高的话, 任意无效尺寸(如 63x64)也会拿到帧间隔 ——
	 * v4l2-compliance 的 ENUM_FMT/FRAMESIZES/FRAMEINTERVALS 项会抓这个。
	 */
	for (i = 0; i < ARRAY_SIZE(ov8858_sm_frmsizes); i++)
		if (fie->width == ov8858_sm_frmsizes[i].width &&
		    fie->height == ov8858_sm_frmsizes[i].height) {
			size_ok = true;
			break;
		}
	if (!size_ok)
		return -EINVAL;

	if (fie->index >= ARRAY_SIZE(ov8858_sm_timings))
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = ov8858_sm_calc_fps(fie->index);

	return 0;
}

static int ov8858_sm_g_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	fi->interval.numerator = 1;
	fi->interval.denominator =
		ov8858_sm_calc_fps(ov8858->is_1080p ? 1 : 0);

	return 0;
}

static int ov8858_sm_s_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi)
{
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);
	u32 fps = ov8858_sm_calc_fps(ov8858->is_1080p ? 1 : 0);

	/* 只接受当前模式的固定帧率 */
	if (fi->interval.numerator != 1 || fi->interval.denominator != fps) {
		dev_dbg(&ov8858->client->dev,
			"s_frame_interval: %u/%u 不支持(固定 %u fps)\n",
			fi->interval.numerator, fi->interval.denominator, fps);
		fi->interval.numerator = 1;
		fi->interval.denominator = fps;
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops ov8858_sm_pad_ops = {
	.enum_mbus_code = ov8858_sm_enum_mbus_code,
	.enum_frame_size = ov8858_sm_enum_frame_size,
	.enum_frame_interval = ov8858_sm_enum_frame_interval,
	.get_fmt = ov8858_sm_get_fmt,
	.set_fmt = ov8858_sm_set_fmt,
};

/* Must NOT be NULL: v4l2_subdev_init has BUG_ON(!ops) */
static const struct v4l2_subdev_ops ov8858_sm_subdev_ops = {
	.core = &ov8858_sm_core_ops,
	.pad = &ov8858_sm_pad_ops,
	.video = &ov8858_sm_video_ops,
};

/* Link frequency menu, in Hz. TODO(L1-B) */
static const s64 ov8858_sm_link_freq_menu[] = {
	OV8858_SM_LINK_FREQ,
};

/* ---------------- register access: 16-bit address, 8-bit data ---------------- */

static int ov8858_sm_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0,        .len = 2, .buf = buf },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* Datasheet: chip ID is at 0x300B (high byte) and 0x300C (low byte). */
/* ---------------- resources ---------------- */

/* The names must match the "*-supply" suffixes in the dts node. */
static const char * const ov8858_sm_supply_names[OV8858_SM_NUM_SUPPLIES] = {
	"dovdd", "avdd", "dvdd",
};

static int ov8858_sm_get_resources(struct ov8858_sm *ov8858)
{
	struct i2c_client *client = ov8858->client;
	int i, ret;

	for (i = 0; i < OV8858_SM_NUM_SUPPLIES; i++)
		ov8858->supplies[i].supply = ov8858_sm_supply_names[i];

	ret = devm_regulator_bulk_get(&client->dev, OV8858_SM_NUM_SUPPLIES,
				      ov8858->supplies);
	if (ret) {
		/* -EPROBE_DEFER must be returned as is */
		dev_err(&client->dev, "failed to get regulators: %d\n", ret);
		return ret;
	}

	ov8858->xvclk = devm_clk_get(&client->dev, "xvclk");
	if (IS_ERR(ov8858->xvclk)) {
		ret = PTR_ERR(ov8858->xvclk);
		dev_err(&client->dev, "failed to get xvclk: %d\n", ret);
		return ret;
	}

	/* GPIOD_ASIS: take the handle without forcing the pin state */
	ov8858->pwdn = devm_gpiod_get_optional(&client->dev, "pwdn", GPIOD_ASIS);
	if (IS_ERR(ov8858->pwdn)) {
		ret = PTR_ERR(ov8858->pwdn);
		dev_err(&client->dev, "failed to get pwdn gpio: %d\n", ret);
		return ret;
	}

	ov8858->reset = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ov8858->reset)) {
		ret = PTR_ERR(ov8858->reset);
		dev_err(&client->dev, "failed to get reset gpio: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "resources: xvclk=%lu Hz, pwdn=%s, reset=%s\n",
		 clk_get_rate(ov8858->xvclk),
		 ov8858->pwdn ? "ok" : "none",
		 ov8858->reset ? "ok" : "none");

	return 0;
}

/* ---------------- power sequence (datasheet 2.6) ----------------
 * figure 2-3 / tables 2-3, 2-4:
 *  - DOVDD and AVDD order is interchangeable, DVDD only needs both stable
 *    (t0 / t1 / t7 = 0)  -> a single bulk enable is fine
 *  - XSHUTDN (the "reset" pin) may be released as soon as DVDD is stable
 *    (t9 = 0) and must be HIGH for the device to run
 *  - the clock must have run 8192 cycles before the first SCCB access (t3);
 *    t2 is another 8192 cycles. 8192 / 24 MHz ~= 341 us -> use 1 ms
 */
#define OV8858_SM_PWR_WAIT_US	1000

static int ov8858_sm_power_on(struct ov8858_sm *ov8858)
{
	struct i2c_client *client = ov8858->client;
	int ret;

	/* 已经在电上就直接返回:避免 clk/regulator 引用计数被重复 +
	 * 1,也避免对已经在跑的传感器再拉一次 reset */
	if (ov8858->powered)
		return 0;

	/* 1. clock first */
	ret = clk_prepare_enable(ov8858->xvclk);
	if (ret) {
		dev_err(&client->dev, "enable xvclk failed: %d\n", ret);
		return ret;
	}

	/* 2. assert XSHUTDN: low = shutdown */
	if (ov8858->reset)
		gpiod_set_value_cansleep(ov8858->reset, 0);

	/* 3. rails on */
	ret = regulator_bulk_enable(OV8858_SM_NUM_SUPPLIES, ov8858->supplies);
	if (ret) {
		dev_err(&client->dev, "enable regulators failed: %d\n", ret);
		clk_disable_unprepare(ov8858->xvclk);
		return ret;
	}

	/* 4. release XSHUTDN: high = running (t9 = 0 after DVDD is stable) */
	if (ov8858->reset)
		gpiod_set_value_cansleep(ov8858->reset, 1);

	usleep_range(1000, 2000);

	/* 5. PWDNB high = not powered down */
	if (ov8858->pwdn)
		gpiod_set_value_cansleep(ov8858->pwdn, 1);

	/* 6. 8192 XVCLK cycles (~341us @24MHz) before the first SCCB access */
	usleep_range(OV8858_SM_PWR_WAIT_US, OV8858_SM_PWR_WAIT_US * 2);

	ov8858->powered = true;

	return 0;
}

static void ov8858_sm_power_off(struct ov8858_sm *ov8858)
{
	if (!ov8858->powered)
		return;

	ov8858->powered = false;

	/* enter shutdown first, then cut clock and rails */
	if (ov8858->reset)
		gpiod_set_value_cansleep(ov8858->reset, 0);

	clk_disable_unprepare(ov8858->xvclk);
	regulator_bulk_disable(OV8858_SM_NUM_SUPPLIES, ov8858->supplies);
}

static int ov8858_sm_check_sensor_id(struct ov8858_sm *ov8858)
{
	struct i2c_client *client = ov8858->client;
	u8 id_h = 0, id_l = 0;
	int ret;

	ret = ov8858_sm_read_reg(client, 0x300b, &id_h);
	if (ret)
		return ret;

	ret = ov8858_sm_read_reg(client, 0x300c, &id_l);
	if (ret)
		return ret;

	dev_info(&client->dev, "Detected OV8858 id 0x%02x%02x\n", id_h, id_l);

	if (id_h != 0x88 || id_l != 0x58) {
		dev_err(&client->dev, "unexpected sensor id 0x%02x%02x\n", id_h, id_l);
		return -ENODEV;
	}

	return 0;
}

static int ov8858_sm_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov8858_sm *ov8858 =
		container_of(ctrl->handler, struct ov8858_sm, ctrl_handler);
	struct i2c_client *client = ov8858->client;
	u16 val;
	int ret = 0;

	/*
	 * L1-B 收尾: 曝光/增益必须真的写进传感器。
	 *
	 * 寄存器映射(OV8858 8/10bit 模式共用):
	 *   0x3501[7:0] = 曝光高 8 位, 0x3502[7:0] = 曝光低 8 位
	 *   0x3508[4:0] = 增益整数部分, 0x3509[7:4] = 增益小数(1/16 为步长)
	 *
	 * 传感器未上电时不写:power_on() 会重新灌整张初始化表,
	 * 此时写进去的值会在下一次开流被覆盖, 徒增一次 I2C 失败日志。
	 */
	if (ctrl->id == V4L2_CID_TEST_PATTERN)
		ov8858->test_pattern = ctrl->val;	/* 先缓存: 未上电时只记值,
							 * 由 start_stream 重放(实测上电前设值会被 early-return 跳过) */
	if (!ov8858->powered)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		val = clamp_t(u16, ctrl->val,
			      OV8858_SM_EXPOSURE_MIN, OV8858_SM_EXPOSURE_MAX);
		ret = ov8858_sm_write_reg(client, 0x3501, (val >> 8) & 0xff);
		if (!ret)
			ret = ov8858_sm_write_reg(client, 0x3502, val & 0xff);
		if (ret)
			break;
		dev_dbg(&client->dev, "exposure %d -> 0x%04x\n",
			ctrl->val, val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		val = clamp_t(u16, ctrl->val,
			      OV8858_SM_GAIN_MIN, OV8858_SM_GAIN_MAX);
		ret = ov8858_sm_write_reg(client, 0x3508, (val >> 4) & 0x1f);
		if (!ret)
			ret = ov8858_sm_write_reg(client, 0x3509,
						  (val & 0x0f) << 4);
		if (ret)
			break;
		dev_dbg(&client->dev, "gain %d -> 0x%02x (int %u, frac %u/16)\n",
			ctrl->val, val, (val >> 4) & 0x1f, val & 0x0f);
		break;
	case V4L2_CID_TEST_PATTERN: {
		/*
		 * OV8858 内嵌 ISP 的自测图案: 0x5e00 bit7=使能, [2:0]选图案。
		 * 取值依据 = 本 BSP 厂商驱动 ov8858.c(与 datasheet 逐条对照过,
		 * 见 docs/17): val=0 关闭写 0x00; val=N 写 (N-1)|0x80。
		 * 用途: bring-up 时先出内部彩条验证 CSI/DMA, 再查光学与后端。
		 */
		u8 pval = ctrl->val ? ((u8)(ctrl->val - 1) | 0x80) : 0x00;

		ret = ov8858_sm_write_reg(client, 0x5e00, pval);
		if (ret)
			break;
		dev_dbg(&client->dev, "test_pattern %d -> 0x5e00=0x%02x\n",
			ctrl->val, pval);
		break;
	}
	default:
		return 0;
	}

	if (ret)
		dev_err(&client->dev, "set_ctrl(0x%x=%d) failed: %d\n",
			ctrl->id, ctrl->val, ret);

	return ret;
}

static const struct v4l2_ctrl_ops ov8858_sm_ctrl_ops = {
	.s_ctrl = ov8858_sm_set_ctrl,
};

/* 与厂商 ov8858.c 同款菜单(0=关, 1-4=四种竖向彩条), 取值即 menu 索引 */
static const char * const ov8858_sm_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4",
};

static int ov8858_sm_initialize_controls(struct ov8858_sm *ov8858)
{
	struct v4l2_ctrl_handler *handler = &ov8858->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, ov8858_sm_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, OV8858_SM_PIXEL_RATE, 1, OV8858_SM_PIXEL_RATE);

	ov8858->exposure = v4l2_ctrl_new_std(handler, &ov8858_sm_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV8858_SM_EXPOSURE_MIN,
					     OV8858_SM_EXPOSURE_MAX, 1,
					     OV8858_SM_EXPOSURE_DEF);

	ov8858->anal_gain = v4l2_ctrl_new_std(handler, &ov8858_sm_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      OV8858_SM_GAIN_MIN,
					      OV8858_SM_GAIN_MAX, 1,
					      OV8858_SM_GAIN_DEF);

	v4l2_ctrl_new_std_menu_items(handler, &ov8858_sm_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov8858_sm_test_pattern_menu) - 1,
				     0, 0, ov8858_sm_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&ov8858->client->dev, "failed to init controls: %d\n", ret);
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	ov8858->subdev.ctrl_handler = handler;

	return 0;
}

static int ov8858_sm_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ov8858_sm *ov8858;
	int ret;

	ov8858 = devm_kzalloc(&client->dev, sizeof(*ov8858), GFP_KERNEL);
	if (!ov8858)
		return -ENOMEM;

	ov8858->client = client;
	ov8858->is_1080p = mode_1080p;

	ret = ov8858_sm_get_resources(ov8858);
	if (ret)
		return ret;

	ret = ov8858_sm_power_on(ov8858);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&ov8858->subdev, client, &ov8858_sm_subdev_ops);

	/*
	 * L4/M4: Rockchip 的实体名约定是 "m%02d_%c_<sensor> <bus>-<addr>"，
	 * 例如 "m00_b_imx415 1-001a"。librkaiq 的 CamHwIsp20::parse_module_info()
	 * 会逐字符校验：name[0]=='m'、substr(0,3) 是模块号、[3]=='_'、
	 * [4] 是 'b'/'f'、[5]=='_'，后面才是真正的 sensor 名。
	 * v4l2_i2c_subdev_init() 默认给的是 "ov8858-sm 5-0036"，不符合约定：
	 *   - 2022 版库只是打印警告然后跳过（AWB 能用）；
	 *   - 2024 版库会直接拒绝该 sensor（get isp or ispp info fail）。
	 * 这里照抄厂商 imx415.c 的写法覆写一遍，模块号固定 01、朝向 back。
	 */
	snprintf(ov8858->subdev.name, sizeof(ov8858->subdev.name),
		 "m%02d_%s_%s %s", 1, "b", OV8858_SM_NAME,
		 dev_name(&client->dev));

	/* L1-B: 有了这个 flag 才会出现 /dev/v4l-subdevN,方便单独调试 */
	ov8858->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* L1-B: the sensor is powered by now, so it must answer with 0x8858 */
	ret = ov8858_sm_check_sensor_id(ov8858);
	if (ret)
		goto err_power;

	if (test_stream) {
		u8 st = 0, mult = 0, wh = 0, wl = 0, hh = 0, hl = 0;
		u8 th = 0, tl = 0, vh = 0, vl = 0;

		dev_info(&client->dev, "test_stream: applying init sequence\n");
		ret = ov8858_sm_start_stream(ov8858);
		if (ret)
			goto err_power;

		ov8858_sm_read_reg(client, 0x0100, &st);
		ov8858_sm_read_reg(client, 0x0302, &mult);
		ov8858_sm_read_reg(client, 0x3808, &wh);
		ov8858_sm_read_reg(client, 0x3809, &wl);
		ov8858_sm_read_reg(client, 0x380a, &hh);
		ov8858_sm_read_reg(client, 0x380b, &hl);
		ov8858_sm_read_reg(client, 0x380c, &th);
		ov8858_sm_read_reg(client, 0x380d, &tl);
		ov8858_sm_read_reg(client, 0x380e, &vh);
		ov8858_sm_read_reg(client, 0x380f, &vl);

		dev_info(&client->dev,
			 "read back: stream=0x%02x mult=0x%02x size=%dx%d hts=%d vts=%d\n",
			 st, mult, (wh << 8) | wl, (hh << 8) | hl,
			 (th << 8) | tl, (vh << 8) | vl);
	}

	/* probe 只是为了读 ID 才上电,读完必须把电关回去:
	 * 否则这次上电在驱动常驻期间一直挂着,和 s_stream 的一开一关
	 * 配不上,传感器两次开流之间不会真正复位,偶发整帧垃圾数据。 */
	ov8858_sm_power_off(ov8858);

	ret = ov8858_sm_initialize_controls(ov8858);
	if (ret)
		goto err_power;

	ov8858->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ov8858->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov8858->subdev.entity, 1, &ov8858->pad);
	if (ret)
		goto err_free_handler;

	ret = v4l2_async_register_subdev(&ov8858->subdev);
	if (ret)
		goto err_entity;

	/* L4:交给 runtime PM。probe 里已经读过 ID 并断电,
	 * 这里把"当前状态"标成 active 并允许 2s 后自动挂起。 */
	pm_runtime_set_autosuspend_delay(&client->dev, 2000);
	pm_runtime_use_autosuspend(&client->dev);
	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_mark_last_busy(&client->dev);
	pm_runtime_put_autosuspend(&client->dev);

	dev_info(&client->dev, "OV8858 SM camera driver probed (runtime PM on)\n");
	return 0;

err_entity:
	media_entity_cleanup(&ov8858->subdev.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&ov8858->ctrl_handler);
err_power:
	ov8858_sm_power_off(ov8858);
	return ret;
}

static int ov8858_sm_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov8858->ctrl_handler);
	pm_runtime_disable(&client->dev);
	ov8858_sm_power_off(ov8858);

	return 0;
}

/* ---------------- L4:电源管理 ---------------- */

static int __maybe_unused ov8858_sm_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	dev_info(dev, "runtime_resume\n");
	return ov8858_sm_power_on(ov8858);
}

static int __maybe_unused ov8858_sm_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	dev_info(dev, "runtime_suspend\n");
	ov8858_sm_power_off(ov8858);
	return 0;
}

static int __maybe_unused ov8858_sm_system_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov8858_sm *ov8858 = container_of(sd, struct ov8858_sm, subdev);

	dev_info(dev, "system_suspend\n");
	/*
	 * 带流拒绝挂起(-EBUSY): 本板无 RTC/唤醒源, 真实 suspend 只能断电
	 * (docs/27 §7)。之前"静默停流断电再睡"等于把正在跑的流杀了还睡死,
	 * 双重损失; 返回 -EBUSY 让 PM core 中止整个挂起, 上层看得到拒绝。
	 * 注意: 这只是"保护", 不宣称 suspend 流程已可交付 —— 真实唤醒
	 * 仍然无法验证, 验证手段只有 pm_test=devices 的干跑路径。
	 */
	if (ov8858->streaming) {
		dev_err(dev,
			"refuse system suspend while streaming (-EBUSY): stop the stream first\n");
		return -EBUSY;
	}
	if (ov8858->powered) {
		ov8858_sm_power_off(ov8858);
	}
	return 0;
}

static int __maybe_unused ov8858_sm_system_resume(struct device *dev)
{
	dev_info(dev, "system_resume\n");
	/* 真正的恢复由下一次 s_stream -> pm_runtime_get_sync 触发 */
	return 0;
}

static const struct dev_pm_ops ov8858_sm_pm_ops = {
	SET_RUNTIME_PM_OPS(ov8858_sm_runtime_suspend, ov8858_sm_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(ov8858_sm_system_suspend, ov8858_sm_system_resume)
};

static struct i2c_driver ov8858_sm_driver = {
	.driver = {
		.name = OV8858_SM_NAME,
		.of_match_table = of_match_ptr(ov8858_sm_of_match),
		.pm = &ov8858_sm_pm_ops,
	},
	.probe = ov8858_sm_probe,
	.remove = ov8858_sm_remove,
	.id_table = ov8858_sm_id,
};
module_i2c_driver(ov8858_sm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OmniVision OV8858 SM Camera Driver");