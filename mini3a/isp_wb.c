/*
 * isp_wb.c — RK3568 ISP21 AWB 数字增益的用户态控制接口(sysfs) v5
 *
 * 寄存器(内核 isp_params_v21.c isp_awbgain_config 的直译):
 *   0xfdff0138 GAIN0_G  = (gb << 16) | gr     ISP21_AWB_GAIN0_G
 *   0xfdff013c GAIN0_RB = (b  << 16) | r      ISP21_AWB_GAIN0_RB
 *   GAIN1/2 同构(0x140/144, 0x148/14c)——三组一并写,只写 GAIN0 部分路径不生效
 *   使能 + 锁存:CIF_ISP_CTRL(0xfdff0000)
 *     BIT7 AWB_ENA;BIT9 CFG_UPD(帧边界锁存);BIT8 CFG_UPD_PERMANENT(持续更新)
 *
 * v5 安全语义:
 *   insmod 只做 ioremap,不碰寄存器。原因:ISP 断流后驱动 runtime suspend
 *   会关时钟,此时 readl/writel = 总线挂死(2026-10-10 实测挂死全系统)。
 *   首次 sysfs 写入才访问硬件——使用流程保证那一刻 ISP 流是开着的。
 *
 * 用法:
 *   insmod isp_wb.ko          (在 ISP 断流状态安全)
 *   ... 开流跑 mini3a ...
 *   echo 0x180 > /sys/module/isp_wb/parameters/gain_r   (流中,即写即生效)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>

#define ISP_BASE        0xfdff0000UL
#define ISP_SIZE        0x10000UL
#define R_ISP_CTRL      0x0000
#define R_AWB_GAIN0_G   0x0138
#define R_AWB_GAIN0_RB  0x013c
#define R_AWB_GAIN1_G   0x0140
#define R_AWB_GAIN1_RB  0x0144
#define R_AWB_GAIN2_G   0x0148
#define R_AWB_GAIN2_RB  0x014c
#define B_AWB_ENA       BIT(7)
#define B_CFG_UPD       BIT(9)   /* 影子寄存器锁存脉冲(rkisp.c:1536 同款) */
#define B_CFG_PERM      BIT(8)   /* 持续更新模式(rkisp.c:1619 同款) */

static void __iomem *base;
static u32 gain_r = 0x100, gain_gr = 0x100, gain_gb = 0x100, gain_b = 0x100;
static int hw_touched;           /* 首次 sysfs 写后置 1(exit 时才安全回滚 ENA) */

static void apply_gains(void)
{
	u32 g, rb, ctrl;

	g  = ((gain_gb & 0xffff) << 16) | (gain_gr & 0xffff);
	rb = ((gain_b  & 0xffff) << 16) | (gain_r  & 0xffff);
	writel(g,  base + R_AWB_GAIN0_G);
	writel(rb, base + R_AWB_GAIN0_RB);
	writel(g,  base + R_AWB_GAIN1_G);
	writel(rb, base + R_AWB_GAIN1_RB);
	writel(g,  base + R_AWB_GAIN2_G);
	writel(rb, base + R_AWB_GAIN2_RB);

	ctrl = readl(base + R_ISP_CTRL);
	ctrl |= B_AWB_ENA | B_CFG_UPD | B_CFG_PERM;
	writel(ctrl, base + R_ISP_CTRL);
	hw_touched = 1;
	pr_info("isp_wb: gains r=0x%x gr=0x%x gb=0x%x b=0x%x (ctrl=0x%x)\n",
		gain_r, gain_gr, gain_gb, gain_b, ctrl);
}

static int param_set_gain(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_uint(val, kp);
	if (ret == 0 && base) {          /* base 非 NULL 即已 insmod;时钟活性由使用流程保证 */
		apply_gains();
	}
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
MODULE_PARM_DESC(gain_r,  "AWB red gain, u16 fixpoint 0x100=1.0");
MODULE_PARM_DESC(gain_gr, "AWB green-red gain");
MODULE_PARM_DESC(gain_gb, "AWB green-blue gain");
MODULE_PARM_DESC(gain_b,  "AWB blue gain");

static int __init isp_wb_init(void)
{
	base = ioremap(ISP_BASE, ISP_SIZE);
	if (!base)
		return -ENOMEM;
	/* v5:init 不碰寄存器(断流后时钟已关,碰 = 总线挂死)。
	 * 真正的访问延迟到首次 sysfs 写入(那时流开着)。 */
	pr_info("isp_wb: loaded, register access deferred to first sysfs write\n");
	return 0;
}

static void __exit isp_wb_exit(void)
{
	if (base) {
		if (hw_touched) {            /* 只有真碰过硬件才回滚(否则又会挂) */
			u32 ctrl = readl(base + R_ISP_CTRL);
			writel(ctrl & ~B_AWB_ENA, base + R_ISP_CTRL);
		}
		iounmap(base);
	}
	pr_info("isp_wb: unloaded\n");
}
module_init(isp_wb_init);
module_exit(isp_wb_exit);
MODULE_LICENSE("GPL");
