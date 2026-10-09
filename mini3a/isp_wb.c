// isp_wb.c — RK3568 ISP21 AWB 数字增益的用户态控制接口(sysfs)
// 用途:mini3a 加餐阶段的白平衡执行器——用户态算灰世界增益,写 sysfs,本模块落到寄存器。
// 寄存器(内核 isp_params_v21.c isp_awbgain_config 的直译):
//   0xfdff0138 GAIN0_G  = (gb << 16) | gr     ISP21_AWB_GAIN0_G
//   0xfdff013c GAIN0_RB = (b  << 16) | r      ISP21_AWB_GAIN0_RB
//   使能:CIF_ISP_CTRL(0xfdff0000) 置 BIT(7) CIF_ISP_CTRL_ISP_AWB_ENA
// 增益定点:u16,0x100 = 1.0x(与 IQ 文件 awb_gain 同口径)
// 用法:
//   insmod isp_wb.ko
//   echo 0x180 > /sys/module/isp_wb/parameters/gain_r   # 立即生效
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/param.h>

#define ISP_BASE        0xfdff0000UL
#define R_ISP_CTRL      0x0000
#define R_AWB_GAIN0_G   0x0138
#define R_AWB_GAIN0_RB  0x013c
#define B_AWB_ENA       BIT(7)

static void __iomem *base;
static u32 gain_r = 0x100, gain_gr = 0x100, gain_gb = 0x100, gain_b = 0x100;

static void apply_gains(void)
{
	u32 g, rb, ctrl;
	if (!base)
		return;
	g  = ((gain_gb & 0xffff) << 16) | (gain_gr & 0xffff);
	rb = ((gain_b  & 0xffff) << 16) | (gain_r  & 0xffff);
	writel(g,  base + R_AWB_GAIN0_G);
	writel(rb, base + R_AWB_GAIN0_RB);
	/* 使能位置位(ISP_ENABLE 保持不动) */
	ctrl = readl(base + R_ISP_CTRL);
	if (!(ctrl & B_AWB_ENA))
		writel(ctrl | B_AWB_ENA, base + R_ISP_CTRL);
	pr_info("isp_wb: gains r=0x%x gr=0x%x gb=0x%x b=0x%x (ctrl=0x%x)\n",
		gain_r, gain_gr, gain_gb, gain_b, (unsigned int)(ctrl | B_AWB_ENA));
}

static int param_set_gain(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);
	if (ret == 0)
		apply_gains();
	return ret;
}

static const struct kernel_param_ops param_ops_gain = {
	.set = param_set_gain,
	.get = param_get_uint,
};
module_param_cb(gain_r,  &param_ops_gain, &gain_r,  0644);
module_param_cb(gain_gr, &param_ops_gain, &gain_gr, 0644);
module_param_cb(gain_gb, &param_ops_gain, &gain_gb, 0644);
module_param_cb(gain_b,  &param_ops_gain, &gain_b,  0644);
MODULE_PARM_DESC(gain_r, "AWB red gain, u16 fixpoint 0x100=1.0");
MODULE_PARM_DESC(gain_gr, "AWB green-red gain");
MODULE_PARM_DESC(gain_gb, "AWB green-blue gain");
MODULE_PARM_DESC(gain_b, "AWB blue gain");

static int __init isp_wb_init(void)
{
	base = ioremap(ISP_BASE, 0x10000);
	if (!base)
		return -ENOMEM;
	apply_gains();
	pr_info("isp_wb: loaded\n");
	return 0;
}

static void __exit isp_wb_exit(void)
{
	if (base) {
		u32 ctrl = readl(base + R_ISP_CTRL);
		writel(ctrl & ~B_AWB_ENA, base + R_ISP_CTRL);
		iounmap(base);
	}
	pr_info("isp_wb: unloaded\n");
}
module_init(isp_wb_init);
module_exit(isp_wb_exit);
MODULE_LICENSE("GPL");
