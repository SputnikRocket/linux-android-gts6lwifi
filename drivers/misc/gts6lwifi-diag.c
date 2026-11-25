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
 * - At every orderly restart or power-off, as Samsung's kernels do, the
 *   kernel log is written to Samsung's "debug" partition in sec_log_buf
 *   format, because a restart is a hard reset that loses memory. It goes to
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
 * Panics restart with a warm reset while the log buffer is in use, in case
 * that keeps memory.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/blkdev.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/string.h>
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

/*
 * Ask the bootloader for recovery now, the way qcom-pon.c does at a restart,
 * so that even a forced reset (below) ends up in TWRP.
 */
static void set_recovery_reason(void)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct regmap *regmap;
	u32 base;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,pm8998-pon");
	if (!np) {
		pr_err("no PON node, cannot ask for recovery\n");
		return;
	}

	ret = of_property_read_u32(np, "reg", &base);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (ret || !pdev) {
		pr_err("PON device not found (%d), cannot ask for recovery\n", ret);
		goto out;
	}

	regmap = pdev->dev.parent ? dev_get_regmap(pdev->dev.parent, NULL) : NULL;
	if (!regmap) {
		pr_err("no PMIC regmap, cannot ask for recovery\n");
		goto out;
	}

	ret = regmap_update_bits(regmap, base + PON_SOFT_RB_SPARE, PON_REASON_MASK,
				 FIELD_PREP(PON_REASON_MASK, PON_REASON_RECOVERY));
	if (ret)
		pr_err("cannot write the PON restart reason: %d\n", ret);
	else
		pr_info("restart reason set to recovery\n");
out:
	if (pdev)
		put_device(&pdev->dev);
}

static int match_debug_partition(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);

	return bdev_is_partition(bdev) && bdev->bd_meta_info &&
	       !strcmp(bdev->bd_meta_info->volname, DEBUG_PART_NAME);
}

/* Write the kernel log to Samsung's debug partition */
static void save_log_to_debug_partition(void)
{
	struct kmsg_dump_iter iter;
	struct sec_log_buf *klog;
	struct file *file;
	struct device *dev;
	size_t len = 0;
	ssize_t written;
	loff_t pos;
	int ret;

	dev = class_find_device(&block_class, NULL, NULL, match_debug_partition);
	if (!dev) {
		pr_err("no \"%s\" partition: storage did not come up, log not saved\n",
		       DEBUG_PART_NAME);
		return;
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

	pos = DEBUG_PART_KLOG_OFFSET;
	written = kernel_write(file, klog, DEBUG_PART_KLOG_SIZE, &pos);
	ret = vfs_fsync(file, 0);
	fput(file);

	if (written != DEBUG_PART_KLOG_SIZE || ret)
		pr_err("writing the log to %s failed: %zd, %d\n", dev_name(dev),
		       written, ret);
	else
		pr_info("saved %zu bytes of kernel log\n", len);
out_free:
	vfree(klog);
out_dev:
	put_device(dev);
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
static int save_log_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	pr_info("%s (%s): saving the kernel log\n",
		action == SYS_RESTART ? "restart" : "power off",
		data ? (char *)data : "");
	save_log_to_debug_partition();

	return NOTIFY_DONE;
}

static struct notifier_block save_log_nb = {
	.notifier_call = save_log_notify,
	/* Before the others, while everything still works */
	.priority = INT_MAX,
};

static int __init gts6lwifi_diag_init(void)
{
	sec_log_init();
	register_reboot_notifier(&save_log_nb);

	if (reboot_after) {
		pr_info("restarting into recovery in %u s\n", reboot_after);
		schedule_delayed_work(&reboot_work,
				      (unsigned long)reboot_after * HZ);
	}

	return 0;
}
/* Early, to keep as much as possible of a boot that stops early */
core_initcall(gts6lwifi_diag_init);
