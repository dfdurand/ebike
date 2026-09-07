/*
 * eBike tracker — boot diagnostics implementation.
 */

#include "diag.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(diag, CONFIG_EBIKE_LOG_LEVEL);

static uint32_t reset_cause;
static char reset_str[48];

/* ---------- Crash capture (fatal error) ---------- */

/* Sentinel telling a real crash apart from RAM noise on the first power-on. */
#define CRASH_MAGIC 0x0DCA5AEDu

struct crash_record {
	uint32_t magic;
	unsigned int reason;
	uint32_t pc;
	uint32_t lr;
	char thread[16];
};

/* Uninitialised RAM: Zephyr touches neither .bss nor .data here, so the
 * contents survive a warm reboot (the fatal handler's NVIC reset does not
 * clear SRAM). Written by the handler below, read back on the next boot. */
static __noinit struct crash_record crash_rec;

static char crash_str[64];

/* ---------- Boot marker: was RAM retained or lost? ----------
 *
 * A reboot with an EMPTY crash record (so the fatal handler never ran) and a
 * reset cause of "0x0" (TF-M consumes RESETREAS before the NS application) is
 * indistinguishable between a power cut, a brownout and a reset from the
 * secure world — three causes with opposite fixes.
 *
 * This marker resolves that without relying on hwinfo: SRAM survives EVERY
 * warm reset (fatal handler, pin reset, watchdog, TF-M fault) but NOT a power
 * loss. A sentinel that is never consumed is therefore enough:
 *   magic present at boot -> RAM retained -> warm reset
 *   magic absent          -> RAM cleared  -> power loss
 *
 * The counter also gives the number of reboots since the device was last
 * powered, which points at the supply or the secure world whenever it climbs
 * while the crash record stays empty.
 */

#define BOOT_MAGIC 0x0DCB0075u

struct boot_record {
	uint32_t magic;
	uint32_t count; /* boots since the last power-on */
};

static __noinit struct boot_record boot_rec;

static bool boot_warm;
static char boot_str[24];

/* Replaces the NCS fatal handler (RESET_ON_FATAL_ERROR is disabled in
 * prj.conf): store reason, PC and thread BEFORE rebooting, so the next boot
 * can report them through the MQTT status. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *name;

	crash_rec.magic = CRASH_MAGIC;
	crash_rec.reason = reason;
#if defined(CONFIG_CPU_CORTEX_M)
	crash_rec.pc = (esf != NULL) ? esf->basic.pc : 0;
	crash_rec.lr = (esf != NULL) ? esf->basic.lr : 0;
#else
	crash_rec.pc = 0;
	crash_rec.lr = 0;
#endif

	name = k_thread_name_get(k_current_get());
	if (name != NULL && name[0] != '\0') {
		strncpy(crash_rec.thread, name, sizeof(crash_rec.thread) - 1);
		crash_rec.thread[sizeof(crash_rec.thread) - 1] = '\0';
	} else {
		crash_rec.thread[0] = '\0';
	}

	/* Flush deferred logs (Zephyr already dumped the fault to the console)
	 * then reboot; the retained RAM is preserved. */
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);

	CODE_UNREACHABLE;
}

static void log_boot(void);

/* Append a token to reset_str, prefixed with '+' when text is already there. */
static void append_cause(const char *tok)
{
	size_t used = strlen(reset_str);

	if (used > 0 && used < sizeof(reset_str) - 1) {
		strncat(reset_str, "+", sizeof(reset_str) - used - 1);
		used++;
	}
	strncat(reset_str, tok, sizeof(reset_str) - used - 1);
}

int diag_init(void)
{
	uint32_t cause = 0;
	int err;

	/* Boot marker FIRST: it is the only information distinguishing a power
	 * cut from a warm reset when RESETREAS is unavailable. The sentinel is
	 * never cleared — only a power loss makes it disappear. */
	boot_warm = (boot_rec.magic == BOOT_MAGIC);
	if (boot_warm) {
		boot_rec.count++;
	} else {
		boot_rec.magic = BOOT_MAGIC;
		boot_rec.count = 1;
	}
	snprintf(boot_str, sizeof(boot_str), "%s %u",
		 boot_warm ? "warm" : "cold", boot_rec.count);

	/* Crash from the previous boot, captured by the fatal handler. Read
	 * before anything else, independently of hwinfo. */
	if (crash_rec.magic == CRASH_MAGIC) {
		snprintf(crash_str, sizeof(crash_str),
			 "r%u pc=0x%08x lr=0x%08x %s",
			 crash_rec.reason, crash_rec.pc, crash_rec.lr,
			 crash_rec.thread[0] != '\0' ? crash_rec.thread : "?");
		LOG_ERR("Crash on the previous boot: %s", crash_str);
		crash_rec.magic = 0; /* consumed: do not report it twice */
	} else {
		crash_str[0] = '\0';
	}

	err = hwinfo_get_reset_cause(&cause);
	if (err) {
		LOG_WRN("Reset cause unavailable (hwinfo: %d)", err);
		strcpy(reset_str, "n/a");
		/* The boot marker does not depend on hwinfo, and this is
		 * exactly the case where it carries all the information. */
		log_boot();
		return err;
	}

	/* Clear it, otherwise the bits accumulate from one reset to the next
	 * and the following boot would report the union of all past causes
	 * instead of its own. */
	(void)hwinfo_clear_reset_cause();

	reset_cause = cause;
	reset_str[0] = '\0';

	if (cause & RESET_POR) {
		append_cause("por");
	}
	if (cause & RESET_PIN) {
		append_cause("pin");
	}
	if (cause & RESET_BROWNOUT) {
		append_cause("brownout");
	}
	if (cause & RESET_SOFTWARE) {
		append_cause("software");
	}
	if (cause & RESET_WATCHDOG) {
		append_cause("watchdog");
	}
	if (cause & RESET_CPU_LOCKUP) {
		append_cause("lockup");
	}
	if (cause & RESET_LOW_POWER_WAKE) {
		append_cause("lpwake");
	}
	if (cause & RESET_DEBUG) {
		append_cause("debug");
	}
	if (reset_str[0] == '\0') {
		snprintf(reset_str, sizeof(reset_str), "0x%x", cause);
	}

	/* brownout, watchdog and lockup mean something went wrong (supply or
	 * hang) and deserve a warning; por, pin and software are normal. */
	if (cause & (RESET_BROWNOUT | RESET_WATCHDOG | RESET_CPU_LOCKUP)) {
		LOG_WRN("Reset cause: %s (0x%x)", reset_str, cause);
	} else {
		LOG_INF("Reset cause: %s (0x%x)", reset_str, cause);
	}

	log_boot();

	return 0;
}

static void log_boot(void)
{
	if (!boot_warm) {
		LOG_INF("Boot: %s — RAM cleared (power-on)", boot_str);
		return;
	}

	/* Warm reboot with no application crash: neither the supply nor our
	 * fatal handler is responsible. That leaves the watchdog and the
	 * secure world, and saying so plainly is the whole point. */
	if (crash_str[0] == '\0') {
		LOG_WRN("Boot: %s — warm reset WITHOUT an application crash "
			"(watchdog or secure world; this is NOT a power loss)",
			boot_str);
	} else {
		LOG_WRN("Boot: %s — warm reset following the crash above",
			boot_str);
	}
}

bool diag_boot_is_warm(void)
{
	return boot_warm;
}

uint32_t diag_boot_count(void)
{
	return boot_rec.count;
}

const char *diag_boot_str(void)
{
	return boot_str;
}

uint32_t diag_reset_cause(void)
{
	return reset_cause;
}

const char *diag_reset_cause_str(void)
{
	return reset_str;
}

const char *diag_crash_str(void)
{
	return crash_str;
}
