// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot diagnostics for the Samsung Galaxy Tab S6 Wi-Fi (gts6lwifi, SM-T860)
 *
 * The tablet has no serial port within reach, and while the mainline port
 * is brought up neither its display nor its USB can be relied on, so a boot
 * that goes wrong leaves no trace. This keeps one where Samsung's own
 * kernels, and so TWRP, can show it:
 *
 * - The kernel log is copied into Samsung's log buffer in reserved memory,
 *   in the format of Samsung's kernels (drivers/samsung/debug/sec_log_buf.c
 *   in the SM-T860 sources). The bootloader passes its location on the
 *   command line as "sec_log=<size>@<address>". After a reset that keeps
 *   memory, the next Samsung kernel shows it in /proc/last_kmsg, after the
 *   logs of earlier boots.
 *
 * - gts6lwifi_diag.reboot_after=<seconds> restarts into recovery, with a
 *   warm reset that keeps memory, that many seconds into the boot. Getting
 *   to TWRP that way also shows that the kernel was still running.
 *
 * Panics also restart with a warm reset while the log buffer is in use, so
 * their log survives too.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/console.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>
#include <linux/string.h>
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

	/* A panic should keep the log as well */
	panic_reboot_mode = REBOOT_WARM;

	register_console(&sec_log_console);
	pr_info("keeping the kernel log in sec_log=%#llx@%pa, from offset %u\n",
		sec_log_size, &sec_log_base, sec_log_idx % sec_log_buf_size);

	return 0;
}

static unsigned int reboot_after;
module_param(reboot_after, uint, 0444);
MODULE_PARM_DESC(reboot_after,
		 "Restart into recovery with a warm reset this many seconds into the boot (0: never)");

#define REBOOT_FALLBACK_SEC	15

static struct hrtimer reboot_fallback_timer;

/* The orderly restart is stuck, probably in a driver's shutdown */
static enum hrtimer_restart reboot_fallback(struct hrtimer *timer)
{
	pr_emerg("restart stuck for %d s, forcing it\n", REBOOT_FALLBACK_SEC);
	emergency_restart();

	return HRTIMER_NORESTART;
}

static void reboot_work_fn(struct work_struct *work)
{
	pr_emerg("%u s into the boot, restarting into recovery (gts6lwifi_diag.reboot_after)\n",
		 reboot_after);

	/* Keep memory, and so the log, across the reset */
	reboot_mode = REBOOT_WARM;

	hrtimer_setup(&reboot_fallback_timer, reboot_fallback, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	hrtimer_start(&reboot_fallback_timer,
		      ktime_set(REBOOT_FALLBACK_SEC, 0), HRTIMER_MODE_REL);

	kernel_restart("recovery");
}

static DECLARE_DELAYED_WORK(reboot_work, reboot_work_fn);

static int __init gts6lwifi_diag_init(void)
{
	sec_log_init();

	if (reboot_after) {
		pr_info("restarting into recovery in %u s\n", reboot_after);
		schedule_delayed_work(&reboot_work, (unsigned long)reboot_after * HZ);
	}

	return 0;
}
/* Early, to keep as much as possible of a boot that stops early */
core_initcall(gts6lwifi_diag_init);
