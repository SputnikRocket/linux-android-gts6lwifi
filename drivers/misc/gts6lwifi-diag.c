// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot diagnostics for the Samsung Galaxy Tab S6 Wi-Fi (gts6lwifi, SM-T860)
 *
 * The tablet has no serial port within reach, and while the mainline port
 * is brought up neither its display nor its USB can be relied on, so a boot
 * that goes wrong leaves no trace. This keeps one where TWRP can get at it:
 *
 * - The kernel log is copied into Samsung's log buffer in reserved memory,
 *   in the format of Samsung's kernels (drivers/samsung/debug/sec_log_buf.c
 *   in the SM-T860 sources). The bootloader passes its location on the
 *   command line as "sec_log=<size>@<address>", and the next Samsung kernel
 *   shows it in /proc/last_kmsg, as long as memory survives the reset.
 *
 * - At every orderly restart or power-off, as Samsung's kernels do, and
 *   every gts6lwifi_diag.save_every=<seconds> if set, the kernel log is
 *   written to Samsung's "debug" partition in sec_log_buf format, because a
 *   restart is a hard reset that loses memory. It is written with bios, not
 *   through a file: the task restarting the tablet is often Android's init,
 *   which SELinux does not let write to a block device. It goes to
 *   the slot for the kernel log of off-mode charging
 *   (6 MiB in, 2 MiB long), which Samsung's kernel only writes with
 *   CONFIG_SEC_LOG_STORE_LPM_KMSG, off in its config: the bootloader copies
 *   the RAM log buffer over the main kernel log slot (1 MiB in) at every
 *   boot, before TWRP could read what was there.
 *
 * - gts6lwifi_diag.reboot_after=<seconds> restarts into recovery that many
 *   seconds into the boot, saving the log on the way. Getting to TWRP that
 *   way also shows that the kernel was still running.
 *
 * - For when not even the log can be saved: at the same restarts, a status
 *   word goes into the value of the PMIC's RTC alarm, which the bootloader
 *   prints at the next boot ("[SAPA] ALARM=<decimal>", in TWRP's
 *   /proc/last_kmsg): one bit per device below, set when it has a driver
 *   bound, and a few more. The top three bits are 101 (hex a or b), which
 *   keeps the alarm decades away.
 *
 * - gts6lwifi_diag.te_probe=1 logs, a few times during the boot, whether
 *   the display panel's tear effect (TE) signal reaches the display engine,
 *   from the interface blocks' tear check counters (see te_probe_intf()).
 *
 * Panics restart with a warm reset while the log buffer is in use, in case
 * that keeps memory.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bio.h>
#include <linux/bitfield.h>
#include <linux/blkdev.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

/* Samsung's struct sec_log_buf */
struct sec_log_buf {
	u32 boot_cnt;
	u32 magic;
	u32 idx;	/* total bytes ever written: the next write goes at idx % size */
	u32 prev_idx;
	char buf[];
};

#define SEC_LOG_MAGIC		0x4d474f4c	/* "LOGM" */

/*
 * Samsung's debug partition: SEC_DEBUG_RESET_LPM_KLOG_OFFSET/SIZE in
 * include/linux/samsung/debug/sec_debug_partition_type.h of the SM-T860
 * sources
 */
#define DEBUG_PART_NAME		"debug"
#define DEBUG_PART_KLOG_OFFSET	(6 * SZ_1M)
#define DEBUG_PART_KLOG_SIZE	SZ_2M

/* PON restart reason, as qcom-pon.c writes it on PMIC gen2 */
#define PON_SOFT_RB_SPARE	0x8f
#define PON_REASON_MASK		GENMASK(7, 1)
#define PON_REASON_RECOVERY	0x1

/* RTC alarm value, 4 bytes little endian (rtc-pm8xxx.c, pm8941_regs) */
#define RTC_ALARM_RW		0x140

static phys_addr_t sec_log_base __initdata;
static unsigned long long sec_log_size __initdata;

static struct sec_log_buf *sec_log;
static u32 sec_log_buf_size;
static u32 sec_log_idx;

static int __init sec_log_setup(char *str)
{
	unsigned long long size, base;
	char *end;

	size = memparse(str, &end);
	if (!size || !is_power_of_2(size) || *end != '@' ||
	    kstrtoull(end + 1, 16, &base)) {
		pr_warn("ignoring malformed sec_log=%s\n", str);
		return 0;
	}

	sec_log_size = size;
	sec_log_base = base;

	return 0;
}
early_param("sec_log", sec_log_setup);

static void sec_log_write(struct console *con, const char *s,
			  unsigned int count)
{
	while (count) {
		u32 pos = sec_log_idx % sec_log_buf_size;
		u32 len = min(count, sec_log_buf_size - pos);

		memcpy(&sec_log->buf[pos], s, len);
		sec_log_idx += len;
		s += len;
		count -= len;
	}

	WRITE_ONCE(sec_log->idx, sec_log_idx);
}

static struct console sec_log_console = {
	.name	= "sec_log",
	.write	= sec_log_write,
	/* Enabled whatever console= says, with the whole log so far */
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

static int __init sec_log_init(void)
{
	phys_addr_t end;
	u32 start;

	if (!sec_log_size) {
		pr_info("no sec_log= on the command line, not keeping the log\n");
		return 0;
	}

	end = sec_log_base + sec_log_size - 1;

	/*
	 * Only ever write to memory set aside for it: the range must be
	 * reserved with no-map in the device tree, so the kernel itself
	 * never uses it.
	 */
	if (sec_log_size <= sizeof(*sec_log) || sec_log_size > SZ_64M ||
	    end < sec_log_base ||
	    !memblock_is_region_memory(sec_log_base, sec_log_size) ||
	    memblock_is_map_memory(sec_log_base) ||
	    memblock_is_map_memory(end)) {
		pr_warn("sec_log=%#llx@%pa is not in reserved no-map memory, not using it\n",
			sec_log_size, &sec_log_base);
		return 0;
	}

	/* Write-combined: writes reach memory without cache maintenance */
	sec_log = memremap(sec_log_base, sec_log_size, MEMREMAP_WC);
	if (!sec_log) {
		pr_err("cannot map sec_log=%#llx@%pa\n", sec_log_size,
		       &sec_log_base);
		return 0;
	}

	sec_log_buf_size = sec_log_size - offsetof(struct sec_log_buf, buf);

	if (READ_ONCE(sec_log->magic) != SEC_LOG_MAGIC) {
		sec_log->idx = 0;
		sec_log->prev_idx = 0;
		WRITE_ONCE(sec_log->magic, SEC_LOG_MAGIC);
	}
	sec_log_idx = READ_ONCE(sec_log->idx);
	start = sec_log_idx % sec_log_buf_size;

	/* A panic should keep the log as well */
	panic_reboot_mode = REBOOT_WARM;

	register_console(&sec_log_console);
	pr_info("keeping the kernel log in sec_log=%#llx@%pa, from offset %u\n",
		sec_log_size, &sec_log_base, start);

	return 0;
}

static unsigned int reboot_after;
module_param(reboot_after, uint, 0444);
MODULE_PARM_DESC(reboot_after,
		 "Restart into recovery this many seconds into the boot, saving the log on the way (0: never)");

/* The regmap and base address of the first PMIC peripheral with @compat */
static struct regmap *pmic_peripheral(const char *compat, u32 *base)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct regmap *regmap = NULL;

	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np)
		return NULL;

	pdev = of_find_device_by_node(np);
	if (pdev && pdev->dev.parent && !of_property_read_u32(np, "reg", base))
		regmap = dev_get_regmap(pdev->dev.parent, NULL);
	of_node_put(np);
	if (pdev)
		put_device(&pdev->dev);

	return regmap;
}

/*
 * Ask the bootloader for recovery now, the way qcom-pon.c does at a restart,
 * so that even a forced reset (below) ends up in TWRP.
 */
static void set_recovery_reason(void)
{
	struct regmap *regmap;
	u32 base;
	int ret;

	regmap = pmic_peripheral("qcom,pm8998-pon", &base);
	if (!regmap) {
		pr_err("no PON device, cannot ask for recovery\n");
		return;
	}

	ret = regmap_update_bits(regmap, base + PON_SOFT_RB_SPARE, PON_REASON_MASK,
				 FIELD_PREP(PON_REASON_MASK, PON_REASON_RECOVERY));
	if (ret)
		pr_err("cannot write the PON restart reason: %d\n", ret);
	else
		pr_info("restart reason set to recovery\n");
}

static int match_debug_partition(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);

	return bdev_is_partition(bdev) && bdev->bd_meta_info &&
	       !strcmp(bdev->bd_meta_info->volname, DEBUG_PART_NAME);
}

#define SAVE_CHUNK		SZ_64K

/* Write the log buffer to the slot with bios, then flush the write cache */
static int write_klog(struct block_device *bdev, void *buf)
{
	unsigned int off;
	int ret;

	for (off = 0; off < DEBUG_PART_KLOG_SIZE; off += SAVE_CHUNK) {
		struct bio *bio;

		bio = bio_alloc(bdev, SAVE_CHUNK / PAGE_SIZE,
				REQ_OP_WRITE | REQ_SYNC, GFP_KERNEL);
		bio->bi_iter.bi_sector = (DEBUG_PART_KLOG_OFFSET + off) >> SECTOR_SHIFT;
		if (!bio_add_vmalloc(bio, buf + off, SAVE_CHUNK)) {
			bio_put(bio);
			return -EIO;
		}

		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
	}

	return blkdev_issue_flush(bdev);
}

enum save_result {
	SAVE_NO_PARTITION,
	SAVE_FAILED,
	SAVE_DONE,
};

/* Write the kernel log to Samsung's debug partition */
static enum save_result save_log_to_debug_partition(void)
{
	enum save_result result = SAVE_FAILED;
	struct kmsg_dump_iter iter;
	struct sec_log_buf *klog;
	struct file *file;
	struct device *dev;
	size_t len = 0;
	int ret;

	dev = class_find_device(&block_class, NULL, NULL, match_debug_partition);
	if (!dev) {
		pr_err("no \"%s\" partition: storage did not come up, log not saved\n",
		       DEBUG_PART_NAME);
		return SAVE_NO_PARTITION;
	}

	if (bdev_nr_bytes(dev_to_bdev(dev)) <
	    DEBUG_PART_KLOG_OFFSET + DEBUG_PART_KLOG_SIZE) {
		pr_err("\"%s\" partition is too small, log not saved\n",
		       DEBUG_PART_NAME);
		goto out_dev;
	}

	klog = vzalloc(DEBUG_PART_KLOG_SIZE);
	if (!klog)
		goto out_dev;

	pr_info("saving the kernel log to %s (\"%s\")\n", dev_name(dev),
		DEBUG_PART_NAME);

	/* The newest part of the log that fits */
	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, false, klog->buf,
			     DEBUG_PART_KLOG_SIZE - sizeof(*klog), &len);
	klog->magic = SEC_LOG_MAGIC;
	klog->idx = len;

	file = bdev_file_open_by_dev(dev->devt, BLK_OPEN_READ | BLK_OPEN_WRITE,
				     NULL, NULL);
	if (IS_ERR(file)) {
		pr_err("cannot open %s: %pe\n", dev_name(dev), file);
		goto out_free;
	}

	ret = write_klog(file_bdev(file), klog);
	fput(file);

	if (ret)
		pr_err("writing the log to %s failed: %d\n", dev_name(dev), ret);
	else {
		pr_info("saved %zu bytes of kernel log\n", len);
		result = SAVE_DONE;
	}
out_free:
	vfree(klog);
out_dev:
	put_device(dev);

	return result;
}

/* Status bits: devices, by compatible, that should have a driver bound */
static const char * const status_devices[] = {
	"qcom,cmd-db",				/*  0 */
	"qcom,rpmh-rsc",			/*  1 */
	"qcom,sm8150-rpmh-clk",			/*  2 */
	"qcom,sm8150-rpmhpd",			/*  3 */
	"qcom,pm8150-rpmh-regulators",		/*  4 */
	"qcom,pm8150l-rpmh-regulators",		/*  5 */
	"qcom,gcc-sm8150",			/*  6 */
	"qcom,sm8150-aggre1-noc",		/*  7 */
	"qcom,sm8150-mc-virt",			/*  8 */
	"qcom,sm8150-gem-noc",			/*  9 */
	"qcom,sm8150-smmu-500",			/* 10 */
	"qcom,sm8150-pinctrl",			/* 11 */
	"qcom,sm8150-aoss-qmp",			/* 12 */
	"qcom,sm8150-qmp-ufs-phy",		/* 13 */
	"qcom,sm8150-ufshc",			/* 14 */
	"qcom,sm8150-usb-hs-phy",		/* 15 */
	"qcom,sm8150-dwc3",			/* 16 */
	"snps,dwc3",				/* 17 */
	"qcom,sm8150-dispcc",			/* 18 */
	"qcom,sm8150-mdss",			/* 19 */
	"qcom,sm8150-dpu",			/* 20 */
	"qcom,sm8150-dsi-ctrl",			/* 21 */
	"qcom,sm8150-sdhci",			/* 22 */
};

#define STATUS_MUIC		BIT(23)	/* SM5705 MUIC (I2C) bound */
#define STATUS_DISK		BIT(24)	/* an sd* disk (UFS LUN) exists */
#define STATUS_PARTITION	BIT(25)	/* the debug partition exists */
#define STATUS_SAVED		BIT(26)	/* the log was saved there */
#define STATUS_RUNNING		BIT(27)	/* init is running: root was mounted */
#define STATUS_PROBING		BIT(28)	/* a platform device is still in probe */
#define STATUS_MAGIC		(0x5U << 29)

static bool compatible_bound(const char *compat)
{
	struct platform_device *pdev;
	struct device_node *np;
	bool bound = false;

	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np)
		return false;

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (pdev) {
		bound = device_is_bound(&pdev->dev);
		put_device(&pdev->dev);
	}

	return bound;
}

static bool muic_bound(void)
{
	struct i2c_client *client;
	struct device_node *np;
	bool bound = false;

	np = of_find_compatible_node(NULL, NULL, "siliconmitus,sm5705-muic");
	if (!np)
		return false;

	client = of_find_i2c_device_by_node(np);
	of_node_put(np);
	if (client) {
		bound = device_is_bound(&client->dev);
		put_device(&client->dev);
	}

	return bound;
}

/* really_probe() holds the device lock: a held lock now means a stuck probe */
static int check_probing(struct device *dev, void *data)
{
	if (device_trylock(dev)) {
		device_unlock(dev);
		return 0;
	}

	pr_warn("%s is busy, probably stuck in probe\n", dev_name(dev));
	*(bool *)data = true;

	return 0;
}

static int match_sd_disk(struct device *dev, const void *data)
{
	return !bdev_is_partition(dev_to_bdev(dev)) &&
	       !strncmp(dev_name(dev), "sd", 2);
}

static u32 boot_status(enum save_result saved)
{
	struct device *disk;
	u32 status = STATUS_MAGIC;
	bool probing = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(status_devices); i++)
		if (compatible_bound(status_devices[i]))
			status |= BIT(i);

	if (muic_bound())
		status |= STATUS_MUIC;

	disk = class_find_device(&block_class, NULL, NULL, match_sd_disk);
	if (disk) {
		status |= STATUS_DISK;
		put_device(disk);
	}

	if (saved != SAVE_NO_PARTITION)
		status |= STATUS_PARTITION;
	if (saved == SAVE_DONE)
		status |= STATUS_SAVED;
	if (system_state == SYSTEM_RUNNING)
		status |= STATUS_RUNNING;

	bus_for_each_dev(&platform_bus_type, NULL, &probing, check_probing);
	if (probing)
		status |= STATUS_PROBING;

	return status;
}

/* Leave @status in the RTC alarm value, which the bootloader prints */
static void report_status(u32 status)
{
	struct regmap *regmap;
	u8 val[4];
	u32 base;
	int ret;

	pr_info("boot status %#010x\n", status);

	regmap = pmic_peripheral("qcom,pm8941-rtc", &base);
	if (!regmap) {
		pr_err("no RTC device, cannot report the status\n");
		return;
	}

	put_unaligned_le32(status, val);
	ret = regmap_bulk_write(regmap, base + RTC_ALARM_RW, val, sizeof(val));
	if (ret)
		pr_err("cannot write the RTC alarm: %d\n", ret);
}

#define REBOOT_FALLBACK_SEC	45

static struct hrtimer reboot_fallback_timer;

/* Saving the log or the orderly restart is stuck: force the reset */
static enum hrtimer_restart reboot_fallback(struct hrtimer *timer)
{
	pr_emerg("restart stuck for %d s, forcing it\n", REBOOT_FALLBACK_SEC);
	emergency_restart();

	return HRTIMER_NORESTART;
}

static void reboot_work_fn(struct work_struct *work)
{
	pr_emerg("%u s into the boot: restarting into recovery (gts6lwifi_diag.reboot_after)\n",
		 reboot_after);

	set_recovery_reason();

	hrtimer_setup(&reboot_fallback_timer, reboot_fallback, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	hrtimer_start(&reboot_fallback_timer,
		      ktime_set(REBOOT_FALLBACK_SEC, 0), HRTIMER_MODE_REL);

	/* Keep memory too, if the reset allows it; the notifier saves the log */
	reboot_mode = REBOOT_WARM;
	kernel_restart("recovery");
}

static DECLARE_DELAYED_WORK(reboot_work, reboot_work_fn);

/* Save the log at every orderly restart and power-off, like sec_log_store() */
static unsigned int save_every;
module_param(save_every, uint, 0444);
MODULE_PARM_DESC(save_every,
		 "Also save the log and status this often, in seconds (0: only at restart)");

static DEFINE_MUTEX(save_lock);

static void save_log_and_status(void)
{
	mutex_lock(&save_lock);
	report_status(boot_status(save_log_to_debug_partition()));
	mutex_unlock(&save_lock);
}

static void save_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(save_work, save_work_fn);

/* For a boot that hangs or is reset by force: the last save survives */
static void save_work_fn(struct work_struct *work)
{
	save_log_and_status();
	schedule_delayed_work(&save_work, (unsigned long)save_every * HZ);
}

static int save_log_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	pr_info("%s (%s): saving the kernel log\n",
		action == SYS_RESTART ? "restart" : "power off",
		data ? (char *)data : "");
	cancel_delayed_work_sync(&save_work);
	save_log_and_status();

	return NOTIFY_DONE;
}

static struct notifier_block save_log_nb = {
	.notifier_call = save_log_notify,
	/* Before the others, while everything still works */
	.priority = INT_MAX,
};

/*
 * A command mode panel pulses TE once a frame, and each pulse resets the
 * read pointer line counter of the tear check feeding it to the vertical
 * display size (vsync_init_val). From DPU 5.0 (SM8150) on, the tear check
 * is in the interface (INTF) blocks, not the pingpong ones. Without TE the
 * counter runs on, at the rate of the configured refresh, up to
 * sync_cfg_height (twice the frame) before it wraps, which also halves the
 * frame rate. So a counter that goes past one frame's lines means no TE.
 */
static bool te_probe;
module_param(te_probe, bool, 0444);
MODULE_PARM_DESC(te_probe,
		 "Log whether the display panel's TE signal reaches the display engine");

#define DPU_INTF_TEAR_MDP_VSYNC_SEL	0x280
#define DPU_INTF_TEAR_TEAR_CHECK_EN	0x284
#define DPU_INTF_TEAR_SYNC_CONFIG_HEIGHT 0x28c
#define DPU_INTF_TEAR_VSYNC_INIT_VAL	0x294
#define DPU_INTF_TEAR_INT_COUNT_VAL	0x298
#define DPU_INTF_TEAR_AUTOREFRESH_CONFIG 0x2b4

/* INTF_1 and INTF_2 (DSI0 and DSI1) in SM8150's DPU */
static const u32 te_probe_intf_offsets[] = { 0x6a800, 0x6b000 };
static const unsigned int te_probe_at_s[] = { 3, 6, 12, 25, 45 };
static unsigned int te_probe_step;
static void __iomem *te_probe_mdp;

static void te_probe_intf(unsigned int index)
{
	void __iomem *intf = te_probe_mdp + te_probe_intf_offsets[index];
	u32 first, last, val, line, max_line = 0, min_line = U16_MAX;
	unsigned int i, wraps = 0, prev = 0;
	ktime_t start;
	s64 us;

	/* Bit 31 enables it, the low bits are the frame count */
	pr_info("te_probe: intf%u: autorefresh %#x\n", index + 1,
		readl(intf + DPU_INTF_TEAR_AUTOREFRESH_CONFIG));

	if (!readl(intf + DPU_INTF_TEAR_TEAR_CHECK_EN)) {
		pr_info("te_probe: intf%u: tear check off\n", index + 1);
		return;
	}

	start = ktime_get();
	first = readl(intf + DPU_INTF_TEAR_INT_COUNT_VAL);
	for (i = 0; i < 2000; i++) {		/* 100 ms */
		val = readl(intf + DPU_INTF_TEAR_INT_COUNT_VAL);
		line = FIELD_GET(GENMASK(15, 0), val);
		max_line = max(max_line, line);
		min_line = min(min_line, line);
		if (i && line < prev)
			wraps++;
		prev = line;
		udelay(50);
	}
	last = readl(intf + DPU_INTF_TEAR_INT_COUNT_VAL);
	us = ktime_us_delta(ktime_get(), start);

	pr_info("te_probe: intf%u: %u frames and %u counter resets in %lld us; line counter %u..%u (init %u, height %u, vsync source %u)\n",
		index + 1,
		(u16)(FIELD_GET(GENMASK(31, 16), last) - FIELD_GET(GENMASK(31, 16), first)),
		wraps, us, min_line, max_line,
		(u32)FIELD_GET(GENMASK(15, 0), readl(intf + DPU_INTF_TEAR_VSYNC_INIT_VAL)),
		(u32)FIELD_GET(GENMASK(15, 0), readl(intf + DPU_INTF_TEAR_SYNC_CONFIG_HEIGHT)),
		readl(intf + DPU_INTF_TEAR_MDP_VSYNC_SEL) & 0xf);
}

static void te_probe_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(te_probe_work, te_probe_work_fn);

static void te_probe_work_fn(struct work_struct *work)
{
	struct device_node *np;
	struct platform_device *pdev;
	unsigned int i;

	np = of_find_compatible_node(NULL, NULL, "qcom,sm8150-dpu");
	pdev = np ? of_find_device_by_node(np) : NULL;
	if (!pdev || !device_is_bound(&pdev->dev)) {
		pr_info("te_probe: no display engine\n");
		goto next;
	}

	if (!te_probe_mdp)
		te_probe_mdp = of_iomap(np, 0);

	/* Its registers are only there while it is powered */
	if (!te_probe_mdp || pm_runtime_get_if_active(&pdev->dev) <= 0) {
		pr_info("te_probe: display engine not powered\n");
		goto next;
	}
	for (i = 0; i < ARRAY_SIZE(te_probe_intf_offsets); i++)
		te_probe_intf(i);
	pm_runtime_put(&pdev->dev);

next:
	if (pdev)
		put_device(&pdev->dev);
	of_node_put(np);

	if (++te_probe_step < ARRAY_SIZE(te_probe_at_s))
		schedule_delayed_work(&te_probe_work,
				      (te_probe_at_s[te_probe_step] -
				       te_probe_at_s[te_probe_step - 1]) * HZ);
}

static int __init gts6lwifi_diag_init(void)
{
	sec_log_init();
	register_reboot_notifier(&save_log_nb);

	if (reboot_after) {
		pr_info("restarting into recovery in %u s\n", reboot_after);
		schedule_delayed_work(&reboot_work,
				      (unsigned long)reboot_after * HZ);
	}

	if (save_every) {
		pr_info("saving the log every %u s\n", save_every);
		schedule_delayed_work(&save_work, (unsigned long)save_every * HZ);
	}

	if (te_probe)
		schedule_delayed_work(&te_probe_work, te_probe_at_s[0] * HZ);

	return 0;
}
/* Early, to keep as much as possible of a boot that stops early */
core_initcall(gts6lwifi_diag_init);
