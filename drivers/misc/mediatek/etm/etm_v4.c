#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/proc_fs.h>
#include "etm_v4.h"
#include "etm_register_v4.h"
#include "etb_register_v4.h"

#ifdef CONFIG_ARM64
#define TRACE_RANGE_START 0x0000000000100000	/* default trace range */
#define TRACE_RANGE_END   0xffffffffffffffff	/* default trace range */
#else
#define TRACE_RANGE_START 0xbf000000	/* default trace range */
#define TRACE_RANGE_END   0xd0000000	/* default trace range */
#endif
#define TIMEOUT 1000000
#define ETB_TIMESTAMP 1
#define ETB_CYCLE_ACCURATE 0
#define CS_TP_PORTSIZE 16
/* T32 is 0x2001, we can apply 0x1 is fine */
#define CS_FORMATMODE 0x11	/* Enable Continuous formatter and FLUSHIN */
#define ETM_DEBUG 0
/* #define ETM_INIT_SAMPLE_CODE 1 */

#if ETM_DEBUG
#define ETM_PRINT pr_err
#else
#define ETM_PRINT(...)
#endif

/* use either ETR_SRAM, ETR_DRAM, or ETB, if undefine just by IC version */
/* #define ETR_DRAM */
/* #define ETR_SRAM */
#define ETR_BUFF_SIZE 0x800
/* #define ETR_SRAM_PHYS_BASE (0x00100000 + 0xF800) */
/* #define ETR_SRAM_VIRT_BASE (INTER_SRAM + 0xF800) */

enum {
	TRACE_STATE_STOP = 0,		/* trace stopped */
	TRACE_STATE_TRACING ,		/* tracing */
	TRACE_STATE_UNFORMATTING,	/* unformatting frame */
	TRACE_STATE_UNFORMATTED,	/* frame unformatted */
	TRACE_STATE_SYNCING,		/* syncing to trace head */
	TRACE_STATE_PARSING,		/* decoding packet */
};

struct etm_info {
	int enable;
	int is_ptm;
	const int *pwr_down;
	u32 etmtsr;
	u32 etmtcr;
	u32 trcidr0;
	u32 trcidr2;
};

struct etm_trace_context_t {
	int nr_etm_regs;
	void __iomem **etm_regs;
	void __iomem *etb_regs;
	void __iomem *funnel_regs;
	void __iomem *dem_regs;
	unsigned long etr_virt;
	unsigned long etr_phys;
	unsigned long etr_len;
	int use_etr;
	int etb_total_buf_size;
	int enable_data_trace;
	unsigned long  trace_range_start, trace_range_end;
	struct etm_info *etm_info;
	int etm_idx;
	int state;
	struct mutex mutex;
};

static struct etm_trace_context_t tracer;
static unsigned int *last_etm_buffer;
static unsigned int last_etm_size;

DEFINE_PER_CPU(int, trace_pwr_down);

#define DBGRST_ALL (tracer.dem_regs + 0x028)
#define DBGBUSCLK_EN (tracer.dem_regs + 0x02C)
#define DBGSYSCLK_EN (tracer.dem_regs + 0x030)
#define AHBAP_EN (tracer.dem_regs + 0x040)
#define DEM_UNLOCK (tracer.dem_regs + 0xFB0)
#define DEM_UNLOCK_MAGIC 0xC5ACCE55
#define AHB_EN (1 << 0)
#define POWER_ON_RESET (0 << 0)
#define SYSCLK_EN (1 << 0)
#define BUSCLK_EN (1 << 0)

/**
 * read from ETB register
 * @param ctx trace context
 * @param x register offset
 * @return value read from the register
 */
static unsigned int etb_readl(const struct etm_trace_context_t *ctx, int x)
{
	return __raw_readl(ctx->etb_regs + x);
}

/**
 * write to ETB register
 * @param ctx trace context
 * @param v value to be written to the register
 * @param x register offset
 * @return value written to the register
 */
static void etb_writel(const struct etm_trace_context_t *ctx, unsigned int v, int x)
{
	__raw_writel(v, ctx->etb_regs + x);
}

/**
 * check whether ETB supports lock
 * @param ctx trace context
 * @return 1:supports lock, 0:doesn't
 */
static int etb_supports_lock(const struct etm_trace_context_t *ctx)
{
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETBLS &0x%lx=0x%x\n", (vmalloc_to_pfn(ctx->etb_regs)<<12) + ETBLS, etb_readl(ctx, ETBLS));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
	return etb_readl(ctx, ETBLS) & 0x1;
}

/**
 * check whether ETB registers are locked
 * @param ctx trace context
 * @return 1:locked, 0:aren't
 */
static int etb_is_locked(const struct etm_trace_context_t *ctx)
{
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETBLS &0x%lx=0x%x\n", (vmalloc_to_pfn(ctx->etb_regs)<<12) + ETBLS, etb_readl(ctx, ETBLS));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
	return etb_readl(ctx, ETBLS) & 0x2;
}

/**
 * disable further write access to ETB registers
 * @param ctx trace context
 */
static void etb_lock(const struct etm_trace_context_t *ctx)
{
	if (etb_supports_lock(ctx)) {
		do {
			etb_writel(ctx, 0, ETBLA);
		} while (unlikely(!etb_is_locked(ctx)));
	} else {
		pr_warn("ETB does not support lock\n");
	}
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETBLA &0x%lx=0x%x\n", (vmalloc_to_pfn(ctx->etb_regs)<<12) + ETBLA, etb_readl(ctx, ETBLA));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

/**
 * enable further write access to ETB registers
 * @param ctx trace context
 */
static void etb_unlock(const struct etm_trace_context_t *ctx)
{
	if (etb_supports_lock(ctx)) {
		do {
			etb_writel(ctx, ETBLA_UNLOCK_MAGIC, ETBLA);
		} while (unlikely(etb_is_locked(ctx)));
	} else {
		pr_warn("ETB does not support lock\n");
	}
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETBLA &0x%lx=0x%x\n", (vmalloc_to_pfn(ctx->etb_regs)<<12) + ETBLA, etb_readl(ctx, ETBLA));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

static unsigned long etb_get_data_length(const struct etm_trace_context_t *t)
{
	unsigned int v;
	unsigned long rp, wp;

	v = etb_readl(t, ETBSTS);
	rp = etb_readl(t, ETBRRP);
	wp = etb_readl(t, ETBRWP);

	if (t->use_etr) {
		rp |= (unsigned long)etb_readl(t, TMCRRPHI) << 32;
		wp |= (unsigned long)etb_readl(t, TMCRWPHI) << 32;
	}

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETB status = 0x%x, rp = 0x%lx, wp = 0x%lx\n", v, rp, wp);

	if (v & 1) {
		/* full */
		return t->etb_total_buf_size;
	}
	if (t->use_etr) {
		if (wp == 0) {
			/* The trace is never started yet. Return 0 */
			return 0;
		} else
			return (wp - tracer.etr_phys) / 4;
	} else
		return wp / 4;
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

static int etb_open(struct inode *inode, struct file *file)
{
	if (!tracer.etb_regs)
		return -ENODEV;

	file->private_data = &tracer;

	return nonseekable_open(inode, file);
}

static ssize_t etb_read(struct file *file, char __user *data, size_t len, loff_t *ppos)
{
	int i;
	struct etm_trace_context_t *t = file->private_data;
	unsigned long first = 0, buffer_end = 0;
	u32 *buf;
	unsigned long wpos;
	unsigned int skip, total, length = 0;
	long wlength;
	loff_t pos = *ppos;

	mutex_lock(&t->mutex);
	etb_unlock(t);

	if (t->state == TRACE_STATE_TRACING) {
		length = 0;
		pr_err("[ETM LOG] Need to stop trace\n");
		goto out;
	}

	total = etb_get_data_length(t);

	/* we assume the following is always true
	   because ETM produce log so fast so that
	   the buffer is always full in circular mode */
	if (total == t->etb_total_buf_size) {
		first = etb_readl(t, ETBRWP);
		if (t->use_etr)
			first = (first - t->etr_phys) / 4;
		else
			first /= 4;
	}

	if (pos > total * 4) {
		/* skip = 0; */
		/* wpos = total; */
		goto out;
	} else {
		skip = (int)pos % 4;
		wpos = (int)pos / 4;
	}

	total -= wpos;
	first = (first + wpos) % t->etb_total_buf_size;
	if (!t->use_etr) {
		/* if it's ETB, we set RRP properly to read data */
		etb_writel(t, first * 4, ETBRRP);
	}

	wlength = min(total, DIV_ROUND_UP(skip + (int)len, 4));
	length = min(total * 4 - skip, (unsigned int)len);
	if (wlength == 0)
		goto out;

	buf = vmalloc(wlength * 4);

	ETM_PRINT("[ETM LOG] ETB read %u bytes to %lld from %ld words at %lx\n",
		length, pos, wlength, first);
	ETM_PRINT("[ETM LOG] ETB buffer length: 0x%lx\n", (total + wpos) * 4);
	ETM_PRINT("[ETM LOG] ETB status reg: 0x%x\n", etb_readl(t, ETBSTS));

	if (t->use_etr) {
		/*
		 * XXX: ETBRRP cannot wrap around correctly on ETR.
		 *	  The workaround is to read the buffer from WTBRWP directly.
		 */

		ETM_PRINT("[ETM LOG] ETR virt = 0x%lx, phys = 0x%lx\n", t->etr_virt, t->etr_phys);

		/* translate first and buffer_end from phys to virt */
		first *= 4;
		first += t->etr_virt;
		buffer_end = t->etr_virt + (t->etr_len * 4);
		ETM_PRINT("[ETM LOG] first(virt) = 0x%lx\n\n", first);

		for (i = 0; i < wlength; i++) {
			buf[i] = *((unsigned int *)(first));
			first += 4;
			if (first >= buffer_end)
				first = t->etr_virt;
		}
	} else {
		for (i = 0; i < wlength; i++)
			buf[i] = etb_readl(t, ETBRRD);
	}

	length -= copy_to_user(data, (u8 *)buf + skip, length);
	vfree(buf);
	*ppos = pos + length;

out:
	etb_lock(t);
	mutex_unlock(&t->mutex);

	return length;
}

static const struct file_operations etb_file_ops = {
	.owner = THIS_MODULE,
	.read = etb_read,
	.open = etb_open,
};

static struct miscdevice etb_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "etb",
	.fops = &etb_file_ops
};

static struct miscdevice etm_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "etm",
};

static ssize_t last_etm_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
	return simple_read_from_buffer(buf, len, offset, last_etm_buffer, last_etm_size);
}

static const struct file_operations last_etm_file_ops = {
	.owner = THIS_MODULE,
	.read = last_etm_read,
	.llseek = default_llseek,
};

static unsigned int __init dump_last_etb(void)
{
	struct etm_trace_context_t *t = &tracer;
	u32 first = 0, buffer_end = 0;
	unsigned int length, total, i;

	total = etb_get_data_length(t);
	length = total * 4;
	ETM_PRINT("[ETM LOG] ETB read %u bytes from %u words at %u\n", length, total, first);
	if (total == 0)
		goto out;

	/* we assume the following is always true
	   because ETM produce log so fast so that
	   the buffer is always full in circular mode */
	if (total == t->etb_total_buf_size)
		first = etb_readl(t, ETBRWP);

	if (!t->use_etr) {
		/* if it's ETB, we set RRP properly to read data */
		etb_writel(t, first, ETBRRP);
	}

	last_etm_buffer = vmalloc(length);
	if (!last_etm_buffer) {
		ETM_PRINT("[ETM LOG] Cannot allocate last_etm buffer\n");
		goto out;
	}

	last_etm_size = length;
	ETM_PRINT("[ETM LOG] ETB status reg: 0x%x\n", etb_readl(t, ETBSTS));

	if (t->use_etr) {
		/*
		 * XXX: ETBRRP cannot wrap around correctly on ETR.
		 *	  The workaround is to read the buffer from WTBRWP directly.
		 */

		/* translate first and buffer_end from phys to virt */
		buffer_end = etb_readl(t, TMCDBALO) + (ETR_BUFF_SIZE * 4);
		ETM_PRINT("[ETM LOG] first(virt) = 0x%x\n\n", first);

		for (i = 0; i < total; i++) {
			last_etm_buffer[i] = etb_readl(t, ETBRRD);
			first += 4;
			if (first >= buffer_end) {
				first = etb_readl(t, TMCDBALO);
				etb_writel(t, first, ETBRRP);
			}
		}
	} else {
		for (i = 0; i < total; i++)
			last_etm_buffer[i] = etb_readl(t, ETBRRD);
	}
/*
	for (i = 0; i < total; i++) {
		ETM_PRINT("[ETM LOG]0x%08x ", last_etm_buffer[i]);
		if (i && !(i % 6))
			ETM_PRINT("\n");
	}

	ETM_PRINT("[ETM LOG]read done\n");
*/
out:
	return length;
}

/**
 * read from ETM register
 * @param ctx trace context
 * @param n ETM index
 * @param x register offset
 * @return value read from the register
 */
static unsigned int etm_readl(const struct etm_trace_context_t *ctx, int n, int x)
{
	return __raw_readl(ctx->etm_regs[n] + x);
}

#if 0
/**
 * write to ETM register
 * @param ctx trace context
 * @param n ETM index
 * @param v value to be written to the register
 * @param x register offset
 * @return value written to the register
 */
static unsigned int etm_writel(const struct etm_trace_context_t *ctx, int n, unsigned int v, int x)
{
	return __raw_writel(v, ctx->etm_regs[n] + x);
}
#endif

static void cs_cpu_write(void __iomem *addr_base, u32 offset, u32 wdata)
{
	/* TINFO="Write addr %h, with data %h", addr_base+offset, wdata */
	__raw_writel(wdata, addr_base + offset);
}

static void cs_cpu_write_64(void __iomem *addr_base, unsigned long offset, unsigned long wdata)
{
#ifdef CONFIG_ARM64
	/* TINFO="Write addr %h, with data %h", addr_base+offset, wdata */
	__raw_writeq(wdata, addr_base + offset);
#else
	__raw_writel(wdata, addr_base + offset);
	__raw_writel(0x0, addr_base + offset + 0x4);
#endif
}

static u32 cs_cpu_read(const void __iomem *addr_base, u32 offset)
{
	u32 actual;

	/* TINFO="Read addr %h, with data %h", addr_base+offset, actual */
	actual = __raw_readl(addr_base + offset);
	return actual;
}

#if ETM_DEBUG
static unsigned long long cs_cpu_read_64(const void __iomem *addr_base, unsigned long offset)
{
	/* TINFO="Read addr %h, with data %h", addr_base+offset, actual */
	u64 actual;
#ifdef CONFIG_ARM64

	actual = readq(addr_base + offset);
#else
	unsigned long long high_actual;

	actual = __raw_readl(addr_base + offset);
	high_actual = __raw_readl(addr_base + offset + 0x4);
	actual |= (high_actual<<32);
#endif
	return actual;
}
#endif

#define SW_LOCK_IMPLEMENTED 0x1
#define SW_LOCK_LOCKED      0x2
#define OS_LOCK_BIT3        (0x1 << 3)
#define OS_LOCK_BIT0        (0x1 << 0)
#define OS_LOCK_LOCKED      0x2
#define OS_LOCK_LOCK        0x1

static void cs_cpu_lock(void __iomem *addr_base)
{
	u32 result;

	result = cs_cpu_read(addr_base, ETMLSR) & 0x3;
	/* if software lock locked? */
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMLSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(addr_base)<<12)+ETMLSR, cs_cpu_read(addr_base, ETMLSR));
	switch (result) {
	case SW_LOCK_IMPLEMENTED | SW_LOCK_LOCKED:
		ETM_PRINT("[ETM LOG]addr @ %p already locked\n", addr_base);
		break;
	case SW_LOCK_IMPLEMENTED:
		ETM_PRINT("[ETM LOG]addr @ %p implemented SW lock but not locked\n", addr_base);
		cs_cpu_write(addr_base, ETMLAR, 0x0);
		ETM_PRINT("[ETM LOG] ETMLAR  &0x%lx=0x%x\n",
			(vmalloc_to_pfn(addr_base)<<12) + ETMLAR, cs_cpu_read(addr_base, ETMLAR));
		break;
	default:
		ETM_PRINT("[ETM LOG]addr @ %p doesn't have SW lock\n", addr_base);
		break;
	}
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

static void cs_cpu_oslock(void __iomem *addr_base)
{
	u32 result, oslm;

	result = cs_cpu_read(addr_base, ETMOSLSR);
	oslm = ((result & OS_LOCK_BIT3) >> 2) | (result & OS_LOCK_BIT0);
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMOSLSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(addr_base)<<12) + ETMOSLSR, cs_cpu_read(addr_base, ETMOSLSR));
	if (!oslm)
		ETM_PRINT("[ETM LOG]addr @ %p doens't have OS lock\n", addr_base);
	else if (result & OS_LOCK_LOCKED)
		ETM_PRINT("[ETM LOG]addr @ %p already locked\n", addr_base);
	else {
		ETM_PRINT("[ETM LOG]addr @ %p implemented OS lock but not locked\n", addr_base);
		cs_cpu_write(addr_base, ETMOSLAR, OS_LOCK_LOCK);
		ETM_PRINT("[ETM LOG] ETMOSLAR  &0x%lx=0x%x\n",
			(vmalloc_to_pfn(addr_base)<<12) + ETMOSLAR, cs_cpu_read(addr_base, ETMOSLAR));
	}
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}
static void cs_cpu_unlock(void __iomem *addr_base)
{
	u32 result;

	result = cs_cpu_read(addr_base, ETMLSR) & 0x3;
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMLSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(addr_base)<<12) + ETMLSR, cs_cpu_read(addr_base, ETMLSR));
	/* if software lock locked? */
	switch (result) {
	case SW_LOCK_IMPLEMENTED | SW_LOCK_LOCKED:
		ETM_PRINT("[ETM LOG]addr @ %p locked\n", addr_base);
		cs_cpu_write(addr_base, ETMLAR, 0xC5ACCE55);
		ETM_PRINT("[ETM LOG] ETMLAR  &0x%lx=0x%x\n",
			(vmalloc_to_pfn(addr_base)<<12) + ETMLAR, cs_cpu_read(addr_base, ETMLAR));
		break;
	case SW_LOCK_IMPLEMENTED:
		ETM_PRINT("[ETM LOG]addr @ %p implemented SW already unlocked\n", addr_base);
		break;
	default:
		ETM_PRINT("[ETM LOG]addr @ %p doesn't have SW lock\n", addr_base);
		break;
	}
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

static void cs_cpu_osunlock(void __iomem *addr_base)
{
	u32 result, oslm;

	result = cs_cpu_read(addr_base, ETMOSLSR);
	oslm = ((result & OS_LOCK_BIT3) >> 2) | (result & OS_LOCK_BIT0);
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMOSLSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(addr_base)<<12)+ETMOSLSR, cs_cpu_read(addr_base, ETMOSLSR));
	if (!oslm)
		ETM_PRINT("[ETM LOG]addr @ %p doens't have OS lock\n", addr_base);
	else if (result & OS_LOCK_LOCKED) {
		ETM_PRINT("[ETM LOG]addr @ %p OS locked\n", addr_base);
		cs_cpu_write(addr_base, ETMOSLAR, ~OS_LOCK_LOCK);
		ETM_PRINT("[ETM LOG] ETMOSLAR  &0x%lx=0x%x\n",
			(vmalloc_to_pfn(addr_base)<<12)+ETMOSLAR, cs_cpu_read(addr_base, ETMOSLAR));
	} else
		ETM_PRINT("[ETM LOG]addr @ %p implemented OS lock but not locked\n", addr_base);
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

#define PCR_ENABLE      0x1
#define TSR_IDLE        0x1
#define TSR_PMSTABLE    0x2
static void cs_cpu_etm_enable(void __iomem *ptm_addr_base)
{
	u32 result;
	u32 counter = 0;

	if ((cs_cpu_read(ptm_addr_base, ETMPCR) & PCR_ENABLE)) {
		ETM_PRINT("[ETM LOG] Already enabled\n");
		return;
	}

	/* Set ETMPCR to enable */
	cs_cpu_write(ptm_addr_base, ETMPCR, PCR_ENABLE);

	/* Wait for TSR_IDLE / TSR_PMSTABLE to be set */
	do {
		result = cs_cpu_read(ptm_addr_base, ETMTSR);
		counter++;
	} while (counter < TIMEOUT && (result & (TSR_IDLE | TSR_PMSTABLE)));

	if (counter >= TIMEOUT)
		ETM_PRINT("[ETM LOG]%s, %p timeout, result = 0x%x\n", __func__, ptm_addr_base, result);

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMPCR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMPCR, cs_cpu_read(ptm_addr_base, ETMPCR));
	ETM_PRINT("[ETM LOG] ETMTSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTSR, cs_cpu_read(ptm_addr_base, ETMTSR));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

void cs_cpu_etm_disable(void __iomem *ptm_addr_base)
{
	u32 result;
	u32 counter = 0;

	if ((cs_cpu_read(ptm_addr_base, ETMPCR) & PCR_ENABLE) == 0) {
		ETM_PRINT("[ETM LOG] Already disabled\n");
		return;
	}

	/* Set ETMPCR to disable */
	cs_cpu_write(ptm_addr_base, ETMPCR, ~PCR_ENABLE);

	/* Wait for TSR_IDLE / TSR_PMSTABLE to be set */
	do {
		result = cs_cpu_read(ptm_addr_base, ETMTSR);
		counter++;
	} while (counter < TIMEOUT && !(result & (TSR_IDLE | TSR_PMSTABLE)));

	if (counter >= TIMEOUT)
		ETM_PRINT("[ETM LOG]%s, %p timeout, result = 0x%x\n", __func__, ptm_addr_base, result);

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETMPCR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMPCR, cs_cpu_read(ptm_addr_base, ETMPCR));
	ETM_PRINT("[ETM LOG] ETMTSR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTSR, cs_cpu_read(ptm_addr_base, ETMTSR));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

void cs_cpu_funnel_setup(void)
{
	u32 funnel_ports, i;

	funnel_ports = 0;
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	for (i = 0; i < tracer.nr_etm_regs; i++) {
		if (tracer.etm_info[i].enable) {
			funnel_ports = funnel_ports | (1 << i);
			ETM_PRINT("[ETM LOG] funnel_ports=0x%x, tracer.nr_etm_regs=0x%x\n",
				funnel_ports, tracer.nr_etm_regs);
		}
	}

	cs_cpu_write(tracer.funnel_regs, 0x000, funnel_ports);
	ETM_PRINT("[ETM LOG] funnel_ports &0x%lx=0x%x\n",
		(vmalloc_to_pfn(tracer.funnel_regs)<<12), cs_cpu_read(tracer.funnel_regs, 0x000));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
#if 0
	/* Adjust priorities so that ITM has highest */
	cs_cpu_write(tracer.funnel_regs, 0x004, 0x00FAC0D1);
#endif
}

void cs_cpu_etb_setup(void)
{
	/* Formatter and Flush Control Register - Enable Continuous formatter and FLUSHIN */
	cs_cpu_write(tracer.etb_regs, ETBFFCR, CS_FORMATMODE);
	/* Configure ETB control (set TraceCapture) */
	cs_cpu_write(tracer.etb_regs, ETBCTL, 0x01);
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	ETM_PRINT("[ETM LOG] ETBFFCR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(tracer.etb_regs)<<12)+ETBFFCR, cs_cpu_read(tracer.etb_regs, ETBFFCR));
	ETM_PRINT("[ETM LOG] ETBCTL  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(tracer.etb_regs)<<12)+ETBCTL, cs_cpu_read(tracer.etb_regs, ETBCTL));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

#define RS_ARC_GROUP    (0x5 << 16)
#define RS_SELECT(x)    (0x1 << x)
#define SS_STATUS_EN    (0x1 << 9)
#define EVENT_SELECT(x) (0x1 << x)
#define IN_SELECT(x)    (0x1 << x)
#define EX_SELECT(x)    (0x1 << (x + 16))
#define CCCI_SUPPORT    (0x1 << 7)
#define TSSIZE          (0x1F << 24)
#define CONFIG_TS       (0x1 << 11)
#define CONFIG_CCI      (0x1 << 4)
#define SYNCPR          (0x1 << 25)
#define SSSTATUS        (0x1 << 9)
#define EXLEVEL_NS      (0x1 << 12)
#define EXLEVEL_S       (0x1 << 8)
#ifdef ETM_INIT_SAMPLE_CODE
void cs_cpu_etm_sample_setup(void __iomem *ptm_addr_base, int core)
{
	u32 result, config;

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	/*
	 * Set up to trace memory range defined by ARC1
	 * SAC1&2 (ARC1)
	 */
#if 0
	cs_cpu_write(ptm_addr_base, ETMACVR1, tracer.trace_range_start);
	cs_cpu_write(ptm_addr_base, ETMACVR2, tracer.trace_range_end);
	/* TATR1&2 */
	/* Don't have to set in ETMv4 */
	/* Select ARC1 on resource select register 2 (0 / 1 has special meaning) */
	/* cs_cpu_write(ptm_addr_base, ETMRSCTLR2, RS_ARC_GROUP | RS_SELECT(0)); */
	cs_cpu_write(ptm_addr_base, ETMRSCTLR2, 0x1<<16 | 0xff);
	/* Set ARC1 as Include in ETMVIIECTLR */
	cs_cpu_write(ptm_addr_base, ETMVIIECTLR, IN_SELECT(0));
	/* Set ETMVICTLR */
	/* set SSSTATUS = 1 and EVENT select 1 (always true) */
	/* cs_cpu_write(ptm_addr_base, ETMVICTLR, SS_STATUS_EN | EVENT_SELECT(1)); */
	cs_cpu_write(ptm_addr_base, ETMVICTLR, SS_STATUS_EN | 0x4);
	/* cs_cpu_write(ptm_addr_base, ETMVICTLR, SS_STATUS_EN | 0x1); */
#endif
	cs_cpu_write(ptm_addr_base, ETMSR, 0x18c1);	/* TRCCONFIGR */
	cs_cpu_write(ptm_addr_base, ETMTEEVR, 0x0);	/* TRCEVENTCTL0R */
	cs_cpu_write(ptm_addr_base, ETMTECR1, 0x0);	/* TRCEVENTCTL1R */
	cs_cpu_write(ptm_addr_base, ETMFFLR, 0x0);	/* TRCSTALLCTLR */
	cs_cpu_write(ptm_addr_base, ETMVDCR1, 0xc);	/* TRCSYNCPR */
	cs_cpu_write(ptm_addr_base, ETMTRID, 0x0);	/* TRCTRACEIDR */
	cs_cpu_write(ptm_addr_base, ETMVDEVR, 0x0);	/* TRCTSCTLR */
	cs_cpu_write(ptm_addr_base, ETMVICTLR, 0x201);	/* TRCVICTLR */
	cs_cpu_write(ptm_addr_base, ETMVIIECTLR, 0x0);	/* TRCVIIECTLR */
	cs_cpu_write(ptm_addr_base, ETMVISSCTLR, 0x0);	/* TRCVISSCTLR */
	config = 0;
	result = cs_cpu_read(ptm_addr_base, ETMIDR0);
	if (result & TSSIZE) {
#if ETB_TIMESTAMP
		/* Enable timestamp */
		config |= CONFIG_TS;
#endif
	} else
		ETM_PRINT("[ETM LOG]addr @ %p doesn't support global timestamp\n", ptm_addr_base);

	if (result & CCCI_SUPPORT) {
#if ETB_CYCLE_ACCURATE
		/* Enable cycle accurate */
		/* Currently we don't use Cycle count */
		config |= CONFIG_CCI;
#endif
	}
	/* Write config */
	cs_cpu_write(ptm_addr_base, ETMCONFIG, config);

	/* set TraceID for each core */
	/* start with cpu 0 = 2, cpu1 = 4, ... */
	cs_cpu_write(ptm_addr_base, ETMTRID, core * 2 + 2);

	/* Set up synchronization frequency */
	result = cs_cpu_read(ptm_addr_base, ETMIDR3);
	if (!(result & SYNCPR)) {
		/* TRCSYNCPR is RW */
		/* Trace synchronization request every 256 bytes */
		cs_cpu_write(ptm_addr_base, ETMSYNCPR, 0x8);
	}
	ETM_PRINT("[ETM LOG] ETMACVR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACVR1, cs_cpu_read(ptm_addr_base, ETMACVR1));
	ETM_PRINT("[ETM LOG] ETMACVR2  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACVR2, cs_cpu_read(ptm_addr_base, ETMACVR2));
	ETM_PRINT("[ETM LOG] ETMRSCTLR2  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMRSCTLR2, cs_cpu_read(ptm_addr_base, ETMRSCTLR2));
	ETM_PRINT("[ETM LOG] ETMVIIECTLR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVIIECTLR, cs_cpu_read(ptm_addr_base, ETMVIIECTLR));
	ETM_PRINT("[ETM LOG] ETMVICTLR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVICTLR, cs_cpu_read(ptm_addr_base, ETMVICTLR));
	ETM_PRINT("[ETM LOG] ETMIDR0  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR0, cs_cpu_read(ptm_addr_base, ETMIDR0));
	ETM_PRINT("[ETM LOG] ETMIDR1  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR1, cs_cpu_read(ptm_addr_base, ETMIDR1));
	ETM_PRINT("[ETM LOG] ETMIDR2  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR2, cs_cpu_read(ptm_addr_base, ETMIDR2));
	ETM_PRINT("[ETM LOG] ETMIDR3  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR3, cs_cpu_read(ptm_addr_base, ETMIDR3));
	ETM_PRINT("[ETM LOG] ETMIDR4  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR4, cs_cpu_read(ptm_addr_base, ETMIDR4));
	ETM_PRINT("[ETM LOG] ETMIDR5  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR5, cs_cpu_read(ptm_addr_base, ETMIDR5));
	ETM_PRINT("[ETM LOG] ETMCONFIG  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMCONFIG, cs_cpu_read(ptm_addr_base, ETMCONFIG));
	ETM_PRINT("[ETM LOG] ETMTRID  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTRID, cs_cpu_read(ptm_addr_base, ETMTRID));
	ETM_PRINT("[ETM LOG] ETMSYNCPR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMSYNCPR, cs_cpu_read(ptm_addr_base, ETMSYNCPR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMSR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMSR, cs_cpu_read(ptm_addr_base, ETMSR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMTEEVR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTEEVR, cs_cpu_read(ptm_addr_base, ETMTEEVR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMTECR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTECR1, cs_cpu_read(ptm_addr_base, ETMTECR1));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMFFLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMFFLR, cs_cpu_read(ptm_addr_base, ETMFFLR));

	ETM_PRINT("[ETM LOG] [SAMPLE]ETMVDCR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVDCR1, cs_cpu_read(ptm_addr_base, ETMVDCR1));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMTRID &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTRID, cs_cpu_read(ptm_addr_base, ETMTRID));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMVDEVR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVDEVR, cs_cpu_read(ptm_addr_base, ETMVDEVR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMVICTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVICTLR, cs_cpu_read(ptm_addr_base, ETMVICTLR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMVIIECTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVIIECTLR, cs_cpu_read(ptm_addr_base, ETMVIIECTLR));
	ETM_PRINT("[ETM LOG] [SAMPLE]ETMVISSCTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVISSCTLR, cs_cpu_read(ptm_addr_base, ETMVISSCTLR));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);

	/* Init ETM */
	/* cs_cpu_etm_enable(ptm_addr_base); */
}
#else
static void cs_cpu_etm_setup(void __iomem *ptm_addr_base, int core)
{
	u32 result, config;

	ETM_PRINT("[ETM LOG] %s\n", __func__);

	/* Since we use Inclue/Exclude to trigger Trace Unit(View Inst), Since
	 * Include/Exclude function already define address range, so we don't
	 * need ViewInst EVENT to config address range anymore. Thus we use
	 * resource 1 to make event always return TRUE so that trace result
	 * will not be affect by ViewInst EVENT and and become precise tracing.
	 */

	/*
	 * 1. Set up Address comparason range
	 */

	cs_cpu_write_64(ptm_addr_base, ETMACVR1, tracer.trace_range_start);
	cs_cpu_write_64(ptm_addr_base, ETMACVR2, tracer.trace_range_end);

	/*
	 * 2. make trace unit can perform a comparison in NSecure or Secure.
	 */
	cs_cpu_write(ptm_addr_base, ETMACTR1, 0x0);
	cs_cpu_write(ptm_addr_base, ETMACTR2, 0x0);

	/*
	 * 3. Select address comparator pair 0 as include address range.
	 */
	cs_cpu_write(ptm_addr_base, ETMVIIECTLR, 0x1);

	/* 4. Since we only use includ/exclude function to filter address range, we have to
	 *     ensure following condition (return true means indicates that all instructions are included)
	 *     1. start/stop logic always return true
	 *     2. ViewInt event always reture true
	 *     3. Include/exclude function return true or false depend on instruction address comparason result.
	 *     4. To disable the exception level filter
	 *   4.1 Select resource 1 which return true all the time as ViewInst's event.
	 *   4.2 configure start/stop logic in start state and clear start/stop point for return true all the time.
	 *   4.3 To disable the exception level filter
	 *   4.4 disable trace event.
	 */
	cs_cpu_write(ptm_addr_base, ETMVICTLR, 0x201);
	cs_cpu_write(ptm_addr_base, ETMVISSCTLR, 0x0);  /* clear start/stop */

	cs_cpu_write(ptm_addr_base, ETMTEEVR, 0x0);  /* TRCEVENTCTL0R */
	cs_cpu_write(ptm_addr_base, ETMTECR1, 0x0);  /* TRCEVENTCTL1R */

	cs_cpu_write(ptm_addr_base, ETMVDEVR, 0x0);
	config = 0;
	result = cs_cpu_read(ptm_addr_base, ETMIDR0);
	if (result & TSSIZE) {
#if ETB_TIMESTAMP
		/* Enable timestamp */
		config |= CONFIG_TS;
#endif
	} else
		ETM_PRINT("[ETM LOG]addr @ %p doesn't support global timestamp\n", ptm_addr_base);

	if (result & CCCI_SUPPORT) {
#if ETB_CYCLE_ACCURATE
		/* Enable cycle accurate */
		/* Currently we don't use Cycle count */
		config |= CONFIG_CCI;
#endif
	}

	/* Write config */
	cs_cpu_write(ptm_addr_base, ETMCONFIG, config);

	/* set TraceID for each core */
	/* start with cpu 0 = 2, cpu1 = 4, ... */
	cs_cpu_write(ptm_addr_base, ETMTRID, core * 2 + 2);

	/* Set up synchronization frequency */
	result = cs_cpu_read(ptm_addr_base, ETMIDR3);
	if (!(result & SYNCPR)) {
		/* TRCSYNCPR is RW */
		/* Trace synchronization request every 256 bytes */
		cs_cpu_write(ptm_addr_base, ETMSYNCPR, 0x8);
	}
	ETM_PRINT("[ETM LOG] ETMACVR1 &0x%lx=0x%llx\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACVR1, cs_cpu_read_64(ptm_addr_base, ETMACVR1));
	ETM_PRINT("[ETM LOG] ETMACVR2  &0x%lx=0x%llx\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACVR2, cs_cpu_read_64(ptm_addr_base, ETMACVR2));
	ETM_PRINT("[ETM LOG] ETMRSCTLR2  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMRSCTLR2, cs_cpu_read(ptm_addr_base, ETMRSCTLR2));
	ETM_PRINT("[ETM LOG] ETMVIIECTLR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVIIECTLR, cs_cpu_read(ptm_addr_base, ETMVIIECTLR));
	ETM_PRINT("[ETM LOG] ETMVICTLR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVICTLR, cs_cpu_read(ptm_addr_base, ETMVICTLR));
	ETM_PRINT("[ETM LOG] ETMIDR0  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR0, cs_cpu_read(ptm_addr_base, ETMIDR0));
	ETM_PRINT("[ETM LOG] ETMIDR1  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR1, cs_cpu_read(ptm_addr_base, ETMIDR1));
	ETM_PRINT("[ETM LOG] ETMIDR2  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR2, cs_cpu_read(ptm_addr_base, ETMIDR2));
	ETM_PRINT("[ETM LOG] ETMIDR3  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR3, cs_cpu_read(ptm_addr_base, ETMIDR3));
	ETM_PRINT("[ETM LOG] ETMIDR4  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR4, cs_cpu_read(ptm_addr_base, ETMIDR4));
	ETM_PRINT("[ETM LOG] ETMIDR5  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMIDR5, cs_cpu_read(ptm_addr_base, ETMIDR5));
	ETM_PRINT("[ETM LOG] ETMCONFIG  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMCONFIG, cs_cpu_read(ptm_addr_base, ETMCONFIG));
	ETM_PRINT("[ETM LOG] ETMTRID  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTRID, cs_cpu_read(ptm_addr_base, ETMTRID));
	ETM_PRINT("[ETM LOG] ETMSYNCPR  &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMSYNCPR, cs_cpu_read(ptm_addr_base, ETMSYNCPR));
	ETM_PRINT("[ETM LOG] ETMSR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMSR, cs_cpu_read(ptm_addr_base, ETMSR));
	ETM_PRINT("[ETM LOG] ETMTEEVR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTEEVR, cs_cpu_read(ptm_addr_base, ETMTEEVR));
	ETM_PRINT("[ETM LOG] ETMTECR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTECR1, cs_cpu_read(ptm_addr_base, ETMTECR1));
	ETM_PRINT("[ETM LOG] ETMFFLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMFFLR, cs_cpu_read(ptm_addr_base, ETMFFLR));
	ETM_PRINT("[ETM LOG] ETMVDCR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVDCR1, cs_cpu_read(ptm_addr_base, ETMVDCR1));
	ETM_PRINT("[ETM LOG] ETMTRID &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMTRID, cs_cpu_read(ptm_addr_base, ETMTRID));
	ETM_PRINT("[ETM LOG] ETMVDEVR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVDEVR, cs_cpu_read(ptm_addr_base, ETMVDEVR));
	ETM_PRINT("[ETM LOG] ETMVICTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVICTLR, cs_cpu_read(ptm_addr_base, ETMVICTLR));
	ETM_PRINT("[ETM LOG] ETMVIIECTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVIIECTLR, cs_cpu_read(ptm_addr_base, ETMVIIECTLR));
	ETM_PRINT("[ETM LOG] ETMVISSCTLR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMVISSCTLR, cs_cpu_read(ptm_addr_base, ETMVISSCTLR));
	ETM_PRINT("[ETM LOG] ETMACTR1 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACTR1, cs_cpu_read(ptm_addr_base, ETMACTR1));
	ETM_PRINT("[ETM LOG] ETMACTR2 &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMACTR2, cs_cpu_read(ptm_addr_base, ETMACTR2));
	ETM_PRINT("[ETM LOG] ETMOSLSR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMOSLSR, cs_cpu_read(ptm_addr_base, ETMOSLSR));
	ETM_PRINT("[ETM LOG] ETMLSR &0x%lx=0x%x\n",
		(vmalloc_to_pfn(ptm_addr_base)<<12)+ETMLSR, cs_cpu_read(ptm_addr_base, ETMLSR));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);

	/* Init ETM */
	/* cs_cpu_etm_enable(ptm_addr_base); */
}
#endif

static void trace_start(void)
{
	int i;
	int pwr_down;

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	if (tracer.state == TRACE_STATE_TRACING) {
		ETM_PRINT("[ETM LOG] ETM trace is already running\n");
		return;
	}

	get_online_cpus();

	mutex_lock(&tracer.mutex);

	__raw_writel(DEM_UNLOCK_MAGIC, DEM_UNLOCK);

	etb_unlock(&tracer);

	cs_cpu_unlock(tracer.funnel_regs);
	/* cs_cpu_unlock(tracer.etb_regs); */

	cs_cpu_funnel_setup();
	cs_cpu_etb_setup();

	for (i = 0; i < tracer.nr_etm_regs; i++) {
		if (tracer.etm_info[i].pwr_down == NULL)
			pwr_down = 0;
		else
			pwr_down = *(tracer.etm_info[i].pwr_down);

		if (!pwr_down && tracer.etm_info[i].enable && cpu_online(i)) {
			cs_cpu_unlock(tracer.etm_regs[i]);
			cs_cpu_osunlock(tracer.etm_regs[i]);

			/* Disable TMs so that they can be set up safely */
			cs_cpu_etm_disable(tracer.etm_regs[i]);

			/* Set up TMs */
#ifdef ETM_INIT_SAMPLE_CODE
			cs_cpu_etm_sample_setup(tracer.etm_regs[i], i);
#else
			cs_cpu_etm_setup(tracer.etm_regs[i], i);
#endif
			/* update the ETMTSR and ETMCONFIG*/
			tracer.etm_info[i].trcidr0 = etm_readl(&tracer, i, ETMIDR0);
			tracer.etm_info[i].trcidr2 = etm_readl(&tracer, i, ETMIDR2);

			/* Set up CoreSightTraceID */
			/* cs_cpu_write(tracer.etm_regs[i], 0x200, i + 1); */

			/* Enable TMs now everything has been set up */
			cs_cpu_etm_enable(tracer.etm_regs[i]);
		}
	}

	/* AHBAP_EN to enable master port, then ETR could write the trace to bus */
	__raw_writel(AHB_EN, AHBAP_EN);
	/* Avoid DBG_sys being reset */
	/* ETB is reset by power-on, not watch-dog */
	__raw_writel(POWER_ON_RESET, DBGRST_ALL);
	__raw_writel(BUSCLK_EN, DBGBUSCLK_EN);
	__raw_writel(SYSCLK_EN, DBGSYSCLK_EN);

	ETM_PRINT("[ETM LOG] DEM_UNLOCK &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0xFB0), __raw_readl(DEM_UNLOCK));
	ETM_PRINT("[ETM LOG] DBGRST_ALL &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x028), __raw_readl(DBGRST_ALL));
	ETM_PRINT("[ETM LOG] DBGBUSCLK_EN &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x02c), __raw_readl(DBGBUSCLK_EN));
	ETM_PRINT("[ETM LOG] DBGSYSCLK_EN &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x030), __raw_readl(DBGSYSCLK_EN));

	tracer.state = TRACE_STATE_TRACING;

	etb_lock(&tracer);
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
	mutex_unlock(&tracer.mutex);

	put_online_cpus();
}

static void trace_stop(void)
{
	int i;
	int pwr_down;

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	if (tracer.state == TRACE_STATE_STOP) {
		ETM_PRINT("[ETM LOG] ETM trace is already stop!\n");
		return;
	}

	get_online_cpus();

	mutex_lock(&tracer.mutex);

	etb_unlock(&tracer);

	for (i = 0; i < tracer.nr_etm_regs; i++) {
		if (tracer.etm_info[i].pwr_down == NULL)
			pwr_down = 0;
		else
			pwr_down = *(tracer.etm_info[i].pwr_down);

		if (!pwr_down && tracer.etm_info[i].enable && cpu_online(i)) {
			/* "Trace program done" */
			/* "Disable trace components" */
			cs_cpu_etm_disable(tracer.etm_regs[i]);

			/* power down */
			/* cs_cpu_write(tracer.etm_regs[i], 0x0, 0x1); */
		}
	}

	/* Disable ETB capture (ETB_CTL bit0 = 0x0) */
	cs_cpu_write(tracer.etb_regs, ETBCTL, 0x0);
	/* Reset ETB RAM Read Data Pointer (ETB_RRP = 0x0) */
	/* no need to reset RRP */
#if 0
	cs_cpu_write(tracer.etb_regs, 0x14, 0x0);
#endif
	ETM_PRINT("[ETM LOG] ETBCTL  &0x%lx=0x%x\n",
		vmalloc_to_pfn(tracer.etb_regs)+ETBCTL, cs_cpu_read(tracer.etb_regs, ETBCTL));

	tracer.state = TRACE_STATE_STOP;

	etb_lock(&tracer);
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
	mutex_unlock(&tracer.mutex);

	put_online_cpus();
}

/*
 * trace_start_by_cpus: Restart traces of the given CPUs.
 * @mask: cpu mask
 * @init_etb: a flag to re-initialize ETB, funnel, ... etc
 */
void trace_start_by_cpus(const struct cpumask *mask, int init_etb)
{
	int i;

	if (!mask)
		return;

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	if (init_etb) {
		__raw_writel(DEM_UNLOCK_MAGIC, DEM_UNLOCK);

		cs_cpu_unlock(tracer.funnel_regs);
		/* cs_cpu_unlock(tracer.etb_regs); */
		etb_unlock(&tracer);

		cs_cpu_funnel_setup();

		/* Disable ETB capture (ETB_CTL bit0 = 0x0) */
		/* For wdt reset */
		cs_cpu_write(tracer.etb_regs, ETBCTL, 0x0);

		if (tracer.use_etr) {
			/* Set up ETR memory buffer address */
			etb_writel(&tracer, tracer.etr_phys, TMCDBALO);
			etb_writel(&tracer, tracer.etr_phys >> 32, TMCDBAHI);
			/* Set up ETR memory buffer size */
			etb_writel(&tracer, tracer.etr_len, TMCRSZ);
		}

		cs_cpu_etb_setup();
	}

	for (i = 0; i < tracer.nr_etm_regs; i++) {
		if (cpumask_test_cpu(i, mask) && tracer.etm_info[i].enable && cpu_online(i)) {
			cs_cpu_unlock(tracer.etm_regs[i]);
			cs_cpu_osunlock(tracer.etm_regs[i]);

			/* Disable PTMs so that they can be set up safely */
			cs_cpu_etm_disable(tracer.etm_regs[i]);

			/* Set up PTMs */
#ifdef ETM_INIT_SAMPLE_CODE
			cs_cpu_etm_sample_setup(tracer.etm_regs[i], i);
#else
			cs_cpu_etm_setup(tracer.etm_regs[i], i);
#endif

			/* Set up CoreSightTraceID */
			/* cs_cpu_write(tracer.etm_regs[i], 0x200, i + 1); */

			/* Enable PTMs now everything has been set up */
			cs_cpu_etm_enable(tracer.etm_regs[i]);
		}
	}

	if (init_etb) {
		/* enable master port such that ETR could write the trace to bus */
		__raw_writel(AHB_EN, AHBAP_EN);
		/* Avoid DBG_sys being reset */
		__raw_writel(POWER_ON_RESET, DBGRST_ALL);
		__raw_writel(BUSCLK_EN, DBGBUSCLK_EN);
		__raw_writel(SYSCLK_EN, DBGSYSCLK_EN);

		etb_lock(&tracer);
	}
	ETM_PRINT("[ETM LOG] DEM_UNLOCK &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0xFB0), __raw_readl(DEM_UNLOCK));
	ETM_PRINT("[ETM LOG] DBGRST_ALL &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x028), __raw_readl(DBGRST_ALL));
	ETM_PRINT("[ETM LOG] DBGBUSCLK_EN &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x02c), __raw_readl(DBGBUSCLK_EN));
	ETM_PRINT("[ETM LOG] DBGSYSCLK_EN &0x%lx=0x%x\n",
		((vmalloc_to_pfn(tracer.dem_regs)<<12) + 0x030), __raw_readl(DBGSYSCLK_EN));
	ETM_PRINT("[ETM LOG] %s Done\n", __func__);
}

static ssize_t run_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] run_show show tracer.state 0x%x\n", tracer.state);
	return snprintf(buf, PAGE_SIZE, "%x\n", tracer.state);
}

static ssize_t run_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int value;
	/* char *data; */

	if (unlikely(kstrtouint(buf, 10, &value) != 0))
		return -EINVAL;
	ETM_PRINT("[ETM LOG] run_show store value 0x%x\n", value);
#if 0
	if (value == 1 || value == 0) {
		ETM_PRINT("[ETM LOG] Let AEE Skip run ETM/stop ETM\n");
		return n;
	}
#endif

	if (value == 1) {
		trace_start();
		ETM_PRINT("[ETM LOG] start() return\n");
	} else if (value == 0) {
		trace_stop();
		ETM_PRINT("[ETM LOG] stop() return\n");
		/*
		data = vmalloc(8192);
		if (data) {
		    etb_read_test(data, 8192, 0);
		    vfree(data);
		}
		else {
		    ETM_PRINT("%s can't read ETB data\n", __func__);
		}
		*/
	} else
		return -EINVAL;

	return n;
}
DEVICE_ATTR(run, 0644, run_show, run_store);

#define TMCReady    (0x1 << 2)
static  ssize_t etb_length_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	int v, etb_length;

	etb_length = 0;
	ETM_PRINT("[ETM LOG] etb_length_show\n");
	v = etb_readl(&tracer, ETBSTS);
	if (v & TMCReady) {
		etb_length = etb_get_data_length(&tracer);
		ETM_PRINT("[ETM LOG] etb_length 0x%x\n", etb_length);
		return sprintf(buf, "%08x\n", etb_length);
	}
	ETM_PRINT("[ETM LOG] Need to stop trace before get length, etb_length 0x%x\n", etb_length);
	return sprintf(buf, "Need to stop trace before get length\n");
}

static ssize_t etb_length_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] etb_length_store\n");
	/* do nothing */
	return n;
}
DEVICE_ATTR(etb_length, 0644, etb_length_show, etb_length_store);

static ssize_t trace_data_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] trace_data_show\n");
	return sprintf(buf, "%08x\n", tracer.enable_data_trace);
}

static ssize_t trace_data_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int value;

	ETM_PRINT("[ETM LOG] trace_data_store\n");

	if (unlikely(kstrtouint(buf, 10, &value) != 0))
		return -EINVAL;

	if (tracer.state == TRACE_STATE_TRACING) {
		pr_err("[ETM LOG] ETM trace is running. Stop first before changing setting\n");
		return n;
	}

	mutex_lock(&tracer.mutex);

	if (value == 1)
		tracer.enable_data_trace = 1;
	else
		tracer.enable_data_trace = 0;

	mutex_unlock(&tracer.mutex);

	return n;
}
DEVICE_ATTR(trace_data, 0644, trace_data_show, trace_data_store);

static ssize_t trace_range_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] trace_range_show\n");
	return sprintf(buf, "%lx %lx\n", tracer.trace_range_start, tracer.trace_range_end);
}

static ssize_t trace_range_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned long range_start, range_end;

	ETM_PRINT("[ETM LOG] trace_range_store\n");
	if (sscanf(buf, "%lx %lx", &range_start, &range_end) != 2)
		return -EINVAL;

	if (tracer.state == TRACE_STATE_TRACING) {
		pr_err("[ETM LOG] ETM trace is running. Stop first before changing setting\n");
		return n;
	}

	mutex_lock(&tracer.mutex);

	tracer.trace_range_start = range_start;
	tracer.trace_range_end = range_end;

	mutex_unlock(&tracer.mutex);

	return n;
}
DEVICE_ATTR(trace_range, 0644, trace_range_show, trace_range_store);

static ssize_t etm_online_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	unsigned int i;

	ETM_PRINT("[ETM LOG] etm_online_show\n");
	for (i = 0; i < tracer.nr_etm_regs; i++)
		sprintf(buf, "%sETM_%d is %s\n", buf, i, (tracer.etm_info[i].enable) ? "Enabled" : "Disabled");

	return strlen(buf);
}

static ssize_t etm_online_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int ret;
	unsigned int num = 0;
	unsigned char str[10];

	ETM_PRINT("[ETM LOG] etm_online_store\n");
	ret = sscanf(buf, "%s %d", str, &num);

	if (tracer.state == TRACE_STATE_TRACING) {
		pr_err("[ETM LOG] ETM trace is running. Stop first before changing setting\n");
		return n;
	}

	mutex_lock(&tracer.mutex);

	if (!strncmp(str, "ENABLE", strlen("ENABLE")))
		tracer.etm_info[num].enable = 1;
	else if (!strncmp(str, "DISABLE", strlen("DISABLE")))
		tracer.etm_info[num].enable = 0;
	else
		pr_err("Input is not correct\n");

	mutex_unlock(&tracer.mutex);

	return n;
}
DEVICE_ATTR(etm_online, 0644, etm_online_show, etm_online_store);

static ssize_t nr_etm_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] nr_etm_show\n");
	return sprintf(buf, "%d\n", tracer.nr_etm_regs);
}

static ssize_t nr_etm_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] nr_etm_store\n");
	return n;
}
DEVICE_ATTR(nr_etm, 0644, nr_etm_show, nr_etm_store);

static ssize_t etm_tcr_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] %s\n", __func__);
	if (tracer.state == TRACE_STATE_TRACING)
		return sprintf(buf, "ETM trace is running. Stop first before changing setting\n");

	return sprintf(buf, "0x%08x\n", etm_readl(&tracer, tracer.etm_idx, ETMCONFIG));
}

static ssize_t etm_tcr_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] etm_tcr_store\n");
	return n;
}
DEVICE_ATTR(etm_tcr, 0644, etm_tcr_show, etm_tcr_store);

static ssize_t etm_idr0_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] etm_idr0_show\n");

	return sprintf(buf, "0x%08x\n", tracer.etm_info[tracer.etm_idx].trcidr0);
}

static ssize_t etm_idr0_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] etm_idr0_store\n");
	return n;
}
DEVICE_ATTR(etm_idr0, 0644, etm_idr0_show, etm_idr0_store);

static ssize_t etm_idr2_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] etm_idr2_show\n");
	return sprintf(buf, "0x%08x\n", tracer.etm_info[tracer.etm_idx].trcidr2);
}

static ssize_t etm_idr2_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] etm_idr2_store\n");
	return n;
}
DEVICE_ATTR(etm_idr2, 0644, etm_idr2_show, etm_idr2_store);

static ssize_t etm_lock_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] etm_lock_show\n");
	return 0;
}

static ssize_t etm_lock_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int value;
	int i;

	ETM_PRINT("[ETM LOG] etm_lock_store\n");
	if (unlikely(kstrtouint(buf, 10, &value) != 0))
		return -EINVAL;

	if (tracer.state == TRACE_STATE_TRACING) {
		pr_err("[ETM LOG] ETM trace is running. Stop first before changing setting\n");
		return n;
	}

	if (value == 1) { /* lock */
		for (i = 0; i < tracer.nr_etm_regs; i++) {
			if (cpumask_test_cpu(i, cpu_online_mask) && tracer.etm_info[i].enable) {
				cs_cpu_lock(tracer.etm_regs[i]);
				cs_cpu_oslock(tracer.etm_regs[i]);
			}
		}
	} else if (value == 0) { /* unlock */
		for (i = 0; i < tracer.nr_etm_regs; i++) {
			if (cpumask_test_cpu(i, cpu_online_mask) && tracer.etm_info[i].enable) {
				cs_cpu_unlock(tracer.etm_regs[i]);
				cs_cpu_osunlock(tracer.etm_regs[i]);
			}
		}
	}
	for (i = 0; i < num_possible_cpus(); i++) {
		ETM_PRINT("[ETM LOG][%s] ETMOSLSR &0x%lx=0x%x\n", __func__,
			(vmalloc_to_pfn(tracer.etm_regs[i])<<12)+ETMOSLSR, cs_cpu_read(tracer.etm_regs[i], ETMOSLSR));
		ETM_PRINT("[ETM LOG][%s] ETMLSR &0x%lx=0x%x\n", __func__,
			(vmalloc_to_pfn(tracer.etm_regs[i])<<12)+ETMLSR, cs_cpu_read(tracer.etm_regs[i], ETMLSR));
	}
	return n;
}
DEVICE_ATTR(etm_lock, 0644, etm_lock_show, etm_lock_store);

static ssize_t is_ptm_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] is_ptm_show\n");
	return sprintf(buf, "%d\n", (tracer.etm_info[tracer.etm_idx].is_ptm) ? 1 : 0);
}

static ssize_t is_ptm_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	ETM_PRINT("[ETM LOG] is_ptm_store\n");
	return n;
}
DEVICE_ATTR(is_ptm, 0644, is_ptm_show, is_ptm_store);

static ssize_t index_show(struct device *kobj, struct device_attribute *attr, char *buf)
{
	ETM_PRINT("[ETM LOG] index_show\n");
	return sprintf(buf, "%d\n", tracer.etm_idx);
}

static ssize_t index_store(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int value;
	int ret;

	ETM_PRINT("[ETM LOG] index_store\n");
	if (unlikely(kstrtouint(buf, 10, &value) != 0))
		return -EINVAL;

	mutex_lock(&tracer.mutex);

	if (value >= tracer.nr_etm_regs)
		ret = -EINVAL;
	else {
		tracer.etm_idx = value;
		ret = n;
	}

	mutex_unlock(&tracer.mutex);

	return ret;
}
DEVICE_ATTR(index, 0644,  index_show, index_store);

static int create_files(void)
{
	int ret = device_create_file(etm_device.this_device, &dev_attr_run);

	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etb_length);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_trace_data);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_trace_range);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etm_online);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_nr_etm);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etm_tcr);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etm_idr0);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etm_idr2);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_etm_lock);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_is_ptm);
	if (unlikely(ret != 0))
		return ret;

	ret = device_create_file(etm_device.this_device, &dev_attr_index);
	if (unlikely(ret != 0))
		return ret;

	return 0;
}

static void remove_files(void)
{
	device_remove_file(etm_device.this_device, &dev_attr_run);

	device_remove_file(etm_device.this_device, &dev_attr_etb_length);

	device_remove_file(etm_device.this_device, &dev_attr_trace_data);

	device_remove_file(etm_device.this_device, &dev_attr_trace_range);

	device_remove_file(etm_device.this_device, &dev_attr_etm_online);

	device_remove_file(etm_device.this_device, &dev_attr_nr_etm);

	device_remove_file(etm_device.this_device, &dev_attr_etm_tcr);

	device_remove_file(etm_device.this_device, &dev_attr_etm_idr0);

	device_remove_file(etm_device.this_device, &dev_attr_etm_idr2);

	device_remove_file(etm_device.this_device, &dev_attr_etm_lock);
}

static int __init etm_probe(struct platform_device *pdev)
{
	int ret = 0, i;

	ETM_PRINT("[ETM LOG] etm_probe\n");

	mutex_lock(&tracer.mutex);

	of_property_read_u32(pdev->dev.of_node, "num", &tracer.nr_etm_regs);

	ETM_PRINT("[ETM LOG]get num from DT = %d\n", tracer.nr_etm_regs);
	tracer.nr_etm_regs = num_possible_cpus();
	ETM_PRINT("[ETM LOG]get num = %d\n", tracer.nr_etm_regs);

	tracer.etm_regs = kmalloc_array(tracer.nr_etm_regs, sizeof(void *), GFP_KERNEL);
	if (!tracer.etm_regs) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < tracer.nr_etm_regs; i++) {
		tracer.etm_regs[i] = of_iomap(pdev->dev.of_node, i);
		ETM_PRINT("[ETM LOG]etm %d @ 0x%p\n", i+1, tracer.etm_regs[i]);
	}

	tracer.etm_info = kmalloc_array(tracer.nr_etm_regs, sizeof(struct etm_info), GFP_KERNEL);
	if (!tracer.etm_info) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < tracer.nr_etm_regs; i++) {
		memset(&(tracer.etm_info[i]), 0, sizeof(struct etm_info));
		tracer.etm_info[i].enable = 1;
		tracer.etm_info[i].is_ptm = 0;
		tracer.etm_info[i].pwr_down = &(per_cpu(trace_pwr_down, i));
	}

	ret = misc_register(&etm_device);
	if (unlikely(ret != 0)) {
		pr_err("[ETM LOG] Fail to register etm device\n");
		goto out;
	}

	ret = create_files();
	if (unlikely(ret != 0)) {
		pr_err("[ETM LOG] Fail to create device files\n");
		goto deregister;
	}

out:
	mutex_unlock(&tracer.mutex);
	return ret;

deregister:
	misc_deregister(&etm_device);
	mutex_unlock(&tracer.mutex);
	return ret;
}

static void etm_shutdown(struct platform_device *pdev)
{
	ETM_PRINT("[ETM LOG][ETM LOG] etm_shutdown\n");
}

static const struct of_device_id etm_of_ids[] __initconst = {
	{   .compatible = "mediatek,mt8173-etm", },
	{}
};

static struct platform_driver etm_driver = {
	.probe = etm_probe,
	.shutdown = etm_shutdown,
	.driver = {
		.name = "etm",
		.of_match_table = etm_of_ids,
	},
};

#if defined(ETR_DRAM)
struct platform_device etr_alloc_buffer;
#endif

static int __init etb_probe(struct platform_device *pdev)
{
	void __iomem *etb_base, *etr_base;
	struct proc_dir_entry *entry;
#if defined(ETR_DRAM)
	void *buff;
	dma_addr_t dma_handle;
#endif

	ETM_PRINT("[ETM LOG] %s\n", __func__);
	mutex_lock(&tracer.mutex);

	etb_base = of_iomap(pdev->dev.of_node, 0);
	if (!etb_base) {
		ETM_PRINT("[ETM LOG][ETM LOG]can't of_iomap for etb!!\n");
		return -ENOMEM;
	}
	ETM_PRINT("[ETM LOG][ETM LOG]of_iomap for etb @ 0x%p\n", etb_base);

	etr_base = of_iomap(pdev->dev.of_node, 1);
	if (!etr_base) {
		ETM_PRINT("[ETM LOG][ETM LOG]can't of_iomap for etr!!\n");
		return -ENOMEM;
	}
	ETM_PRINT("[ETM LOG][ETM LOG]of_iomap for etr @ 0x%p\n", etr_base);

	tracer.funnel_regs = of_iomap(pdev->dev.of_node, 2);
	if (!tracer.funnel_regs) {
		ETM_PRINT("[ETM LOG][ETM LOG]can't of_iomap for funnel!!\n");
		return -ENOMEM;
	}
	ETM_PRINT("[ETM LOG][ETM LOG]of_iomap for funnel @ 0x%p\n", tracer.funnel_regs);

	tracer.dem_regs = of_iomap(pdev->dev.of_node, 3);
	if (!tracer.dem_regs) {
		ETM_PRINT("[ETM LOG][ETM LOG]can't of_iomap for dem!!\n");
		return -ENOMEM;
	}
	ETM_PRINT("[ETM LOG][ETM LOG]of_iomap for dem @ 0x%p\n", tracer.dem_regs);

#if defined(ETR_DRAM)
	/* DRAM */
	etr_alloc_buffer.dev.coherent_dma_mask = DMA_BIT_MASK(32);
	buff = dma_alloc_coherent(&etr_alloc_buffer.dev, ETR_BUFF_SIZE * sizeof(int), &dma_handle, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;
	memset(buff, 0, ETR_BUFF_SIZE * sizeof(int));
	tracer.etr_virt = (unsigned long)buff;
	tracer.etr_phys = dma_handle;
	tracer.etr_len = ETR_BUFF_SIZE;
	tracer.use_etr = 1;
	tracer.etb_regs = etr_base;
#elif defined(ETR_SRAM)
	/* SRAM */
	buff = ioremap(0x0010F800, ETR_BUFF_SIZE * 4);
	tracer.etr_virt = (u32)buff;
	tracer.etr_phys = (dma_addr_t)ETR_SRAM_PHYS_BASE;
	tracer.etr_len = ETR_BUFF_SIZE;
	tracer.use_etr = 1;
	tracer.etb_regs = etr_base;
#else
	/* ETB */
	tracer.etr_len = 0x800;
	tracer.use_etr = 0;
	tracer.etb_regs = etb_base;
#endif

	if (unlikely(misc_register(&etb_device) != 0))
		pr_err("[ETM LOG]Fail to register etb device\n");

	/* AHBAP_EN to enable master port, then ETR could write the trace to bus */
	__raw_writel(DEM_UNLOCK_MAGIC, DEM_UNLOCK);
	__raw_writel(AHB_EN, AHBAP_EN);
	__raw_writel(POWER_ON_RESET, DBGRST_ALL);
	__raw_writel(BUSCLK_EN, DBGBUSCLK_EN);
	__raw_writel(SYSCLK_EN, DBGSYSCLK_EN);
	etb_unlock(&tracer);

	/* Disable ETB capture (ETB_CTL bit0 = 0x0) */
	/* For wdt reset */
	cs_cpu_write(tracer.etb_regs, ETBCTL, 0x0);

	/* size is in 32-bit words */
	tracer.etb_total_buf_size = tracer.etr_len;
	tracer.state = TRACE_STATE_STOP;

	if (dump_last_etb()) {
		entry = proc_create("last_etm", 0444, NULL, &last_etm_file_ops);
		if (!entry) {
			ETM_PRINT("[ETM LOG] last_etm: failed to create proc entry\n");
			return 0;
		}
	}

	if (tracer.use_etr) {
		ETM_PRINT("[ETM LOG]ETR virt = 0x%lx, phys = 0x%lx\n", tracer.etr_virt, tracer.etr_phys);
		/* Set up ETR memory buffer address */
		etb_writel(&tracer, tracer.etr_phys, TMCDBALO);
		etb_writel(&tracer, tracer.etr_phys >> 32, TMCDBAHI);
		/* Set up ETR memory buffer size */
		/* etr_len is word-count, 1 means 4 bytes */
		etb_writel(&tracer, (tracer.etr_len), TMCRSZ);
	}

	mutex_unlock(&tracer.mutex);
	ETM_PRINT("[ETM LOG][ETM LOG] ETBCTL &0x%lx=0x%x\n",
		(vmalloc_to_pfn(tracer.etb_regs)<<12)+ETBCTL, cs_cpu_read(tracer.etb_regs+ETBCTL, 0x0));
	ETM_PRINT("[ETM LOG][ETM LOG] %s Done\n", __func__);

	return 0;
}

static void etb_shutdown(struct platform_device *pdev)
{
	ETM_PRINT("[ETM LOG][ETM LOG] etb_shutdown\n");
}

static const struct of_device_id etb_of_ids[] __initconst = {
	{   .compatible = "mediatek,mt8173-etb", },
	{}
};

static struct platform_driver etb_driver = {
	.probe = etb_probe,
	.shutdown = etb_shutdown,
	.driver = {
		.name = "etb",
		.of_match_table = etb_of_ids,
	},
};

void trace_start_dormant(void)
{
	int cpu;

	if (tracer.state == TRACE_STATE_TRACING)
		trace_start_by_cpus(cpumask_of(0), 1);

	/*
	 * XXX: This function is called just before entering the suspend mode.
	 *	  The Linux kernel is already freeze.
	 *	  So it is safe to do the trick to access the per-cpu variable directly.
	 */
	for (cpu = 1; cpu < num_possible_cpus(); cpu++)
		per_cpu(trace_pwr_down, cpu) = 1;
}

static int restart_trace(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	unsigned long cpu;
	int *pwr_down;

	cpu = (unsigned long)hcpu;
	action = action & 0xf;
	switch (action) {
	case CPU_STARTING:
		switch (cpu) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			pwr_down = &get_cpu_var(trace_pwr_down);
			if (*pwr_down) {
				if (tracer.state == TRACE_STATE_TRACING)
					trace_start_by_cpus(cpumask_of(cpu), 0);
			}
			*pwr_down = 0;
			put_cpu_var(trace_pwr_down);
			break;
		default:
			break;
		}
		break;

	case CPU_DYING:
		switch (cpu) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			per_cpu(trace_pwr_down, cpu) = 1;
			break;
		default:
			break;
		}
			break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block pftracer_notifier __cpuinitdata = {
	.notifier_call = restart_trace,
};

/**
 * driver initialization entry point
 */
static int __init etm_init(void)
{
	int i, err;

	memset(&tracer, 0, sizeof(struct etm_trace_context_t));
	mutex_init(&tracer.mutex);
	tracer.trace_range_start = TRACE_RANGE_START;
	tracer.trace_range_end = TRACE_RANGE_END;

	for (i = 0; i < num_possible_cpus(); i++) {
		per_cpu(trace_pwr_down, i) = 0;
		/* etm_driver_data[i].pwr_down = &(per_cpu(trace_pwr_down, i)); */
	}

	register_cpu_notifier(&pftracer_notifier);

	err = platform_driver_register(&etm_driver);
	if (err)
		return err;

	err = platform_driver_register(&etb_driver);
	if (err)
		return err;

	return 0;
}
module_init(etm_init);

/**
 * driver exit point
 */
static void __exit etm_exit(void)
{
	kfree(tracer.etm_info);
	kfree(tracer.etm_regs);
	if (last_etm_buffer)
		vfree(last_etm_buffer);
	remove_files();

	if (misc_deregister(&etm_device))
		pr_err("[ETM LOG]Fail to deregister etm_device\n");
	if (misc_deregister(&etb_device))
		pr_err("[ETM LOG]Fail to deregister etb_device\n");
}
module_exit(etm_exit);
