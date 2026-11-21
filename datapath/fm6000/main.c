/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nosd-fm6000 -- the datapath daemon for the Intel FM6000 "Alta".
 *
 * SPDX note aside, the first thing to say about this file is what it does NOT
 * do: it does not forward packets, it does not bring ports up, and it does not
 * program a forwarding table. The 7150S-52 port is at M1. Saying so here, in
 * the daemon's own banner and in the capabilities it reports, is deliberate --
 * a datapath that claims more than it does produces a switch that accepts
 * configuration and silently drops it on the floor.
 *
 * What it does today:
 *   - finds the chip, maps BAR0, and says whether it is really there
 *   - serves the switch-api socket, so the CLI works against this board and
 *     reports the truth about it
 *   - watches for the chip leaving the PCIe bus, and says so once, loudly
 *
 * That last one is not a placeholder for real work. On this silicon an
 * illegal access takes the endpoint off the bus while the link stays up, and
 * the host then sees stalls and RCU warnings somewhere unrelated, minutes
 * later, naming nothing. Something that watches for it and reports it at the
 * moment it happens is worth more than the next three features.
 *
 * There is no SDK under this. Every register this daemon will ever touch goes
 * through pci.c, and every address it uses is in regs.h with a note saying
 * where the address came from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>

static void kmsg(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, msg, strlen(msg));
		write(fd, "\n", 1);
		close(fd);
	}
	fprintf(stderr, "nosd-fm6000: KMSG: %s\n", msg);
}

#include "bist.h"
#include "boot.h"
#include "esched.h"
#include "dma.h"
#include "ffu.h"
#include "glort.h"
#include "l2f.h"
#include "lbs.h"
#include "memfill.h"
#include "parser.h"
#include "pci.h"
#include "pcie_init.h"
#include "portmap.h"
#include "regs.h"
#include "saf.h"
#include "sbus.h"
#include "serdes.h"
#include "spico.h"
#include "ssched.h"
#include "sock.h"
#include "tap.h"

/*
 * How long to wait for the chip before failing.
 *
 * Long enough that the platform HAL releasing the ASIC from SCD reset is never
 * a race, short enough that a genuinely broken image fails its A/B trial while
 * somebody is still watching.
 */
#define FM_DEFAULT_WAIT_SECS 30

static volatile int running = 1;

/*
 * Pipeline init: the blocks that boot_cold() leaves zeroed and that the
 * forwarding path needs before frames can move.
 *
 * These are idempotent -- the same end state on every call -- so running them
 * on a chip that has already been through this once (e.g. nosd restarted) is
 * harmless. A nosd restart that skipped this and left the pipeline unconfigured
 * would be worse than one that re-ran 400 writes.
 */
static void pipeline_init(struct fm6000 *d)
{
	uint32_t fc_before, fc_after;
	unsigned n;
	int rv;

	fc_before = fm_fatal_count(d);
	kmsg("nosd-fm6000: pipeline_init entry");
	fprintf(stderr, "nosd-fm6000: pipeline_init entry: alive=%d fc=%u\n",
		fm_alive(d), fc_before);

	/*
	 * ESCHED is NOT written here. Writing the ESCHED block (0x002000-0x003fff)
	 * via PCIe while this daemon also holds the LBUS (SCD BAR1) mapping causes
	 * a chip fault that hangs the kernel. ESCHED is handled by the short-lived
	 * fm6000-esched-init tool, which holds only PCIe resource0 (no LBUS) and
	 * calls _exit(0) immediately after writing. Run it between fm6000-pcie-init
	 * and (re)starting this daemon.
	 */

	/*
	 * SAF init runs before fm_spico_load (see call site below) while
	 * BIST-established ECC is still valid. SPICO's ~39 soft resets corrupt
	 * SAF ECC, making fm_saf_init writes fatal here. Skip it.
	 */
	kmsg("nosd-fm6000: SAF init: skipped (written pre-SPICO)");
	fc_before = fm_fatal_count(d);

	/*
	 * Pre-initialise parser SRAM ECC with a zero-fill burst before
	 * writing the actual per-port seeds. The step-10 fill writes these
	 * banks early in the sequence; the ~235 resets from later banks clear
	 * all SRAM globally (soft self-resets preserve config registers, not
	 * SRAM); BIST does not cover these parser banks. After step 10+11 the
	 * ECC at 0x108200 is therefore uninitialised.
	 *
	 * A read from uninitialised ECC causes a HARD chip fault (needs SCD
	 * reset pulse -- measured 2026-10-13). A write from the management
	 * path also faults because the ECC RMW first reads the cluster and
	 * hits the bad ECC. The burst path (fm_mem_fill, check_writes off)
	 * matches how the step-10 fill initialised these banks and is the
	 * safe way to establish valid ECC before per-word writes follow.
	 *
	 * TICK_CFG=2 must be active before any write to parser SRAM; without
	 * it writes cause a hard chip fault (FATAL_COUNT 4→0) rather than the
	 * recoverable ECC soft-reset. It is written in boot.c before step 10's
	 * fill and must survive through BIST and esched init. Re-assert it
	 * here as a safeguard, and read it back first so the log can show
	 * whether the boot left it set (if 0x2 appears it was fine; if 0x0
	 * appears the fault was TICK_CFG dropping and this write fixes it).
	 */
	{
		uint32_t tick_cfg_val = 0;
		int pre_rv;

		(void)fm_rd(d, FM6000_SSCHED_TICK_CFG, &tick_cfg_val);
		/*
		 * TICK_CFG is 0x0 here: esched_init clears it (measured 2026-10-13)
		 * and writing 0x00f010 after the ring is running + ESCHED configured
		 * kills the chip (fatal write when ring is live, measured).
		 *
		 * Do NOT write TICK_CFG here. The ring clock domain is already running
		 * (kept by the FM6000 hardware once started); TICK_CFG=0 at this point
		 * is the hardware clearing the one-shot trigger after use. The parser
		 * SRAM pre-fill below goes through the burst path (fm_mem_fill,
		 * check_writes=0), which does not trigger ECC read-modify-write, so it
		 * initialises ECC without needing TICK_CFG=2 as an arming condition.
		 */
		fprintf(stderr, "nosd-fm6000: TICK_CFG at pipeline_init: 0x%x alive=%d\n",
			tick_cfg_val, fm_alive(d));

		kmsg("nosd-fm6000: parser pre-fill 0x100200 start");
		pre_rv = fm_mem_fill(d, 0x100200u, 14336u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x100200: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");
		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: parser pre-fill resets after: %u alive=%d\n",
			fc_after > fc_before ? fc_after - fc_before : 0,
			fm_alive(d));
		fm_clear_offbus(d);
		fc_before = fc_after;

		pre_rv = fm_mem_fill(d, 0x108000u, 152u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x108000: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");

		pre_rv = fm_mem_fill(d, 0x108200u, 304u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x108200: %s alive=%d\n",
			pre_rv == FM_OK ? "ok" : "failed", fm_alive(d));
		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: parser pre-fill resets: %u\n",
			fc_after > fc_before ? fc_after - fc_before : 0);
		fc_before = fc_after;
		fm_clear_offbus(d);
		{
			uint32_t ps = 0;
			char lbuf[80];
			(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
			snprintf(lbuf, sizeof(lbuf),
				"nosd-fm6000: parser pre-fill done: pin_strap=0x%x alive=%d",
				ps, fm_alive(d));
			kmsg(lbuf);
		}
	}

	kmsg("nosd-fm6000: parser fields init start");
	rv = fm_parser_fields_init(d, &n);
	fc_after = fm_fatal_count(d);
	if (rv == FM_EOFFBUS) {
		fprintf(stderr,
			"nosd-fm6000: parser fields init: ECC pass (resets: %u); retrying\n",
			fc_after > fc_before ? fc_after - fc_before : 0);
		fc_before = fc_after;
		fm_clear_offbus(d);
		rv = fm_parser_fields_init(d, &n);
		fc_after = fm_fatal_count(d);
	}
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: parser fields init failed (%d); resets during: %u\n",
			rv, fc_after > fc_before ? fc_after - fc_before : 0);
	else
		fprintf(stderr, "nosd-fm6000: parser fields: %u writes ok; resets during: %u\n",
			n, fc_after > fc_before ? fc_after - fc_before : 0);
	fc_before = fc_after;
	{
		uint32_t ps = 0;
		char lbuf[80];
		(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
		snprintf(lbuf, sizeof(lbuf),
			"nosd-fm6000: parser fields done: pin_strap=0x%x alive=%d",
			ps, fm_alive(d));
		kmsg(lbuf);
	}

	/* CM watermarks (cmwm) and CM rest (cmrest) written in ssched.c
	 * post-settle block (TICK_CFG=2, IC=0).  Calling them here at
	 * TICK_CFG=0 with IC=1 kills the chip: the CM monitor catches
	 * SRAM mid-write → ECC fault. */
	/*
	 * Pre-fill LBS CAM before fm_lbs_init.
	 *
	 * LBS_CAM at 0x014000 is a 76-entry CAM. Like the CM-block SRAMs,
	 * its ECC is corrupted by SPICO's soft resets and must be initialized
	 * via the burst path before fm_lbs_init's fm_wr calls.
	 */
	{
		int pre_rv;

		pre_rv = fm_mem_fill(d, 0x014000u, 76u, 0u);  /* LBS_CAM: 76 ports */
		fc_after = fm_fatal_count(d);
		fm_clear_offbus(d);
		{
			uint32_t ps = 0;
			char lbuf[80];
			(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
			snprintf(lbuf, sizeof(lbuf),
				"nosd-fm6000: LBS pre-fill: rv=%d pin_strap=0x%x alive=%d resets=%u",
				pre_rv, ps, fm_alive(d),
				fc_after > fc_before ? fc_after - fc_before : 0);
			kmsg(lbuf);
		}
		fc_before = fc_after;
	}

	kmsg("nosd-fm6000: LBS start");

	rv = fm_lbs_init(d, &n);
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: LBS init failed (%d)\n", rv);
	else
		fprintf(stderr, "nosd-fm6000: LBS: %u writes ok\n", n);
	{
		uint32_t ps = 0;
		char lbuf[80];
		(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
		snprintf(lbuf, sizeof(lbuf),
			"nosd-fm6000: LBS done: pin_strap=0x%x alive=%d",
			ps, fm_alive(d));
		kmsg(lbuf);
	}
	/*
	 * Pre-fill GLORT SRAM before fm_glort_init.
	 *
	 * fm_spico_load's ~39 chip soft self-resets corrupt GLORT_CAM (0x00e000)
	 * and GLORT_RAM (0x00e800) ECC. The first fm_wr inside fm_glort_init hits
	 * bad ECC in the chip's internal read-modify-write → hard chip fault, chip
	 * goes off the local bus. Established by install80 (2026-10-22): all phases
	 * through LBS show pin_strap=0x208; GLORT done shows pin_strap=0x0.
	 *
	 * The burst path (fm_mem_fill, check_writes=0) writes directly without ECC
	 * RMW, establishing valid ECC for value zero. Sizes from memfill.c banks[].
	 */
	kmsg("nosd-fm6000: GLORT pre-fill start");
	{
		int pre_rv;

		pre_rv = fm_mem_fill(d, 0x00e000u, 1024u, 0u);
		fprintf(stderr, "nosd-fm6000: GLORT_CAM pre-fill 0xe000: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");

		pre_rv = fm_mem_fill(d, 0x00e800u, 2048u, 0u);
		fprintf(stderr, "nosd-fm6000: GLORT_RAM pre-fill 0xe800: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");

		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: GLORT pre-fill resets: %u alive=%d\n",
			fc_after > fc_before ? fc_after - fc_before : 0,
			fm_alive(d));
		fm_clear_offbus(d);
		fc_before = fc_after;
		{
			uint32_t ps = 0;
			char lbuf[80];
			(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
			snprintf(lbuf, sizeof(lbuf),
				"nosd-fm6000: GLORT pre-fill done: pin_strap=0x%x alive=%d",
				ps, fm_alive(d));
			kmsg(lbuf);
		}
	}
	kmsg("nosd-fm6000: GLORT start");

	rv = fm_glort_init(d);
	{
		uint32_t glort_cam0 = 0xdeadbeef;
		uint32_t gt1 = 0xdeadbeef, gt3 = 0xdeadbeef;
		char lbuf[200];
		uint32_t ps = 0;
		/*
		 * GLORT_CAM[0] readback: verifies direct writes to 0xe000 landed.
		 * Also read GLORT_TABLE[1] and [3] -- the injection targets for et1
		 * and et2 respectively.  If these read 0 the forwarding table writes
		 * failed and DGLORT=1/3 injection would route nowhere.
		 *
		 * NOT reading MCAST_DEST_TABLE (0x240004/0x240005): that read caused
		 * a hard chip fault on 2026-10-22 (pin_strap 0x208→0x0), removed.
		 */
		if (rv == FM_OK) {
			(void)fm_rd(d, 0x00e000u, &glort_cam0);
			(void)fm_rd(d, 0x00014001u, &gt1);
			(void)fm_rd(d, 0x00014003u, &gt3);
		}
		(void)fm_rd(d, FM6000_PIN_STRAP, &ps);
		snprintf(lbuf, sizeof(lbuf),
			"nosd-fm6000: GLORT done: rv=%d pin_strap=0x%x alive=%d cam0=0x%x gt1=0x%08x gt3=0x%08x",
			rv, ps, fm_alive(d), glort_cam0, gt1, gt3);
		kmsg(lbuf);
		if (rv != FM_OK)
			fprintf(stderr, "nosd-fm6000: GLORT init failed (%d)\n", rv);
		else
			fprintf(stderr,
				"nosd-fm6000: GLORT: ok; CAM[0]=0x%08x "
				"TABLE[1]=0x%08x(et1,exp 0x0036ffc9) "
				"TABLE[3]=0x%08x(et2,exp 0x0035ffca)\n",
				glort_cam0, gt1, gt3);
	}
	/* FFU init is NOT called here: pipeline_init runs before AER is disabled
	 * (fm_pcie_serr_disable / fm_pcie_disable_root_aer_cmd at "install209").
	 * A chip self-reset during the 14,549-entry FFU bulk write causes a PCIe
	 * CTO, which fires AER while the mfence stalls, deadlocking the kernel.
	 * FFU is instead called in the pre-DMA block (after install209). */
	kmsg("nosd-fm6000: pipeline_init done");
}

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/*
 * Forwarding state: DMA engine + TAP file descriptors for et1/et2.
 *
 * Only et1 and et2 are wired up -- they are the ports with cables in the lab.
 * Adding more ports is a matter of creating more TAPs and extending the table.
 */
#define FWD_NPORTS 2

struct fwd_port {
	int       tap_fd;
	uint16_t  glort;       /* ingress SGLORT (RX port identification) */
	uint16_t  tx_dglort;   /* inject DGLORT (TX destination) */
	const char *name;
};

struct fwd_state {
	struct fm_dma    dma;
	struct fwd_port  ports[FWD_NPORTS];
};

static struct fwd_state g_fwd;

static unsigned long g_rx_total = 0;

/* Called from DMA RX poll for each received frame. */
static void rx_deliver(void *ctx, const uint8_t *frame, uint16_t len)
{
	static unsigned long rx_unknown = 0;
	unsigned long rx_total;
	uint16_t sglort;
	int i;

	(void)ctx;

	/* Frame is DMAC(6) SMAC(6) F64(8) EType(2) Payload...
	 * Strip the F64 tag before delivering to the TAP.
	 * Minimum viable frame: 14 bytes of header + F64 + EType = 22 bytes. */
	if (len < 22)
		return;

	rx_total = ++g_rx_total;
	sglort = fm_f64_sglort(frame, len);

	/* Log first 8 frames with hex dump; every 100th unknown for diagnosis. */
	if (rx_total <= 8 || (rx_unknown > 0 && rx_unknown % 100 == 0)) {
		uint16_t dump = len < 24 ? len : 24;
		uint16_t k;
		fprintf(stderr, "nosd-fm6000: DMA RX #%lu: sglort=%u len=%u bytes[0..%u]:",
			rx_total, sglort, len, dump - 1);
		for (k = 0; k < dump; k++)
			fprintf(stderr, " %02x", frame[k]);
		fprintf(stderr, "\n");
	}

	for (i = 0; i < FWD_NPORTS; i++) {
		if (g_fwd.ports[i].glort != sglort)
			continue;
		if (g_fwd.ports[i].tap_fd < 0)
			return;
		/* Build frame without the F64 tag: DMAC(6) SMAC(6) EType... */
		{
			uint8_t stripped[4096];
			uint16_t stripped_len = len - FM6000_F64_LEN;

			if (stripped_len > sizeof(stripped))
				return;
			memcpy(stripped, frame, 12);                    /* DMAC+SMAC */
			memcpy(stripped + 12, frame + 20, len - 20);   /* EType+payload */
			fm_tap_write(g_fwd.ports[i].tap_fd, stripped, stripped_len);
		}
		return;
	}
	/* Frame from an unknown SGLORT -- not a TAP port, ignore. */
	rx_unknown++;
}

/*
 * Read FATAL_COUNT via a raw LBUS read (fm_rd_hazardous) that never touches
 * bus-2 Config Space.  fm_fatal_count() → fm_rd() → fm_check_offbus() reads
 * 0000:02:00.0/config (CF8/CFC) when LBUS returns 0xFFFF; on AMD RS780, that
 * Config Read stalls the CPU indefinitely when the FM6000 link is down.
 * Returns 0xFFFFFFFF when the chip is in FATAL (LBUS all-ones).
 */
static inline uint32_t fc_read_safe(struct fm6000 *d)
{
	uint32_t v;
	if (fm_rd_hazardous(d, FM6000_FATAL_COUNT, &v) != FM_OK)
		return 0xFFFFFFFFu;
	return v;
}

static void *fwd_thread(void *arg)
{
	struct fwd_state *fwd = arg;
	uint8_t txbuf[4096];
	struct timeval tv;
	fd_set rfds;
	int maxfd, i, n;
	time_t last_log = 0;
	unsigned long rx_poll_count = 0;
	unsigned long tx_sent = 0, tx_ringfull = 0;
	uint32_t last_fc = fc_read_safe(fwd->dma.d);
	unsigned long poll_since_fc_check = 0;

	for (;;) {
		/* Poll DMA RX (non-blocking) */
		rx_poll_count++;
		poll_since_fc_check++;
		fm_dma_rx_poll(&fwd->dma, 16, rx_deliver, NULL);
		fm_dma_tx_reclaim(&fwd->dma);

		/* Check for chip self-resets every ~100ms (100 poll iterations).
		 * A soft self-reset clears DMA ring base address registers to 0;
		 * without reprogram the FM6000 DMA-writes received frames to
		 * physical address 0 (the x86 IDT), corrupting the kernel.
		 * Re-programming the rings after a reset restores correct addresses. */
		if (poll_since_fc_check >= 100) {
			uint32_t fc = fc_read_safe(fwd->dma.d);
			poll_since_fc_check = 0;
			if (fc != last_fc) {
				char hb[128];
				snprintf(hb, sizeof(hb),
					"nosd-fm6000: RESET during DMA: fc %u->%u rx_polls=%lu -- awaiting stability",
					last_fc, fc, rx_poll_count);
				kmsg(hb);
				last_fc = fc;

				/* Wait for FATAL_COUNT to stabilise (2 s clean window)
				 * before restoring Bus Master or reprogramming DMA.
				 *
				 * Use fc_read_safe() (fm_rd_hazardous) not fm_fatal_count():
				 * fm_fatal_count → fm_rd → fm_check_offbus reads bus-2
				 * Config Space via CF8/CFC, which stalls the CPU indefinitely
				 * on AMD RS780 when the FM6000 link is down (FATAL). */
				{
					uint32_t prev_fc = fc_read_safe(fwd->dma.d);
					int stbl = 0, j;

					fm_clear_offbus(fwd->dma.d);
					for (j = 0; j < 40 && stbl < 4; j++) {
						struct timespec ts = { 0, 500000000L };
						uint32_t cur_fc;

						nanosleep(&ts, NULL);
						cur_fc = fc_read_safe(fwd->dma.d);
						if (cur_fc == prev_fc &&
						    cur_fc != 0xFFFFFFFFu)
							stbl++;
						else {
							stbl = 0;
							prev_fc = cur_fc;
							last_fc = cur_fc;
						}
					}
				}

				/* Restore Memory Space + Bus Master.
				 * pciehp's probe() no longer does this (DLLSCE disabled
				 * in fm6000_kdrv after the first stable probe).
				 * fm_pcie_enable_portio waits for Link Active before
				 * writing, so this is safe even if stability is marginal. */
				fm_pcie_enable_portio(2, 0, 0);

				/* After SSCHED MASTER_RESET, the PCIe link bounces.
				 * DL_Init runs while BusMaster=0 → FM6000 advertises
				 * 0 PD (Posted-Data) flow-control credits.  With 0 PD
				 * credits the RS780 cannot forward any Memory Write
				 * TLPs to the FM6000 (BAR0 or BAR1).  The first BAR0
				 * write in pipeline_init() stalls the CPU indefinitely
				 * in the RS780 posted-write buffer.
				 *
				 * fm_pcie_enable_portio() restores BusMaster=1.
				 * Calling fm_pcie_retrain_link() triggers a new DL_Init
				 * while DMAEn=1 in PCI_CORE_CTRL_1, so the FM6000
				 * allocates its DMA receive buffer and advertises
				 * nonzero PD credits.  BAR0 writes are then safe.
				 * [install248] */
				{
					struct timespec ts_retrain = { 0, 600000000L };
					char rb[96];
					int rv_retrain = fm_pcie_retrain_link();
					snprintf(rb, sizeof rb,
						"nosd-fm6000: RESET reprogram: retrain rv=%d [install248]",
						rv_retrain);
					kmsg(rb);
					nanosleep(&ts_retrain, NULL);
				}

				/* Re-initialize pipeline SRAM and FFU BEFORE starting
				 * DMA.  After a chip self-reset the DMA engine is
				 * stopped; fm_dma_reprogram's CMD_TX/CMD_RX restart it.
				 * If we start DMA while FFU is still empty, the first
				 * frame the fabric forwards hits no FFU rule and the
				 * chip self-resets again, creating an infinite loop.
				 * Run pipeline_init + ffu_init while DMA is stopped,
				 * then let fm_dma_reprogram start the engines safely. */
				{
					struct fm6000 *d = fwd->dma.d;
					int rv_fill4k, rv_fill256, rv_l2f, rv_ffu;
					char rp_buf[96];

					/* install260: after HARD FATAL + retrain, SCD BAR1
					 * (LBUS) and FM6000 BAR0 are both cleared (the FM6000
					 * endpoint reset cleared its BARs).  fm_pcie_retrain_link
					 * alone does NOT restore them — only a sysfs rescan
					 * forces the kernel to re-enumerate and write BAR0+Command.
					 * Without this, fm_mem_fill / pipeline_init / ffu_init
					 * all return rv=-2 (LBUS dead) indefinitely. */
					{
						int rfd260;
						char b260[128];
						int rv260_link;

						rfd260 = open("/sys/class/pci_bus/0000:02/rescan",
						              O_WRONLY);
						if (rfd260 >= 0) {
							(void)write(rfd260, "1\n", 2);
							close(rfd260);
							kmsg("nosd-fm6000: RESET reprogram: install260 bus=2 rescan done");
						} else {
							kmsg("nosd-fm6000: RESET reprogram: install260 WARNING bus=2 rescan open failed");
						}
						nap_ms(300);

						fm_detach_pcie(d);
						if (fm_attach_pcie(d) == FM_OK)
							kmsg("nosd-fm6000: RESET reprogram: install260 BAR0 remapped");
						else
							kmsg("nosd-fm6000: RESET reprogram: install260 WARNING BAR0 remap failed");

						/* Write PCI_DMA_CFG=0x35 NOW that LBUS is live again,
						 * then link_disable → DL_Init → nonzero PD credits. */
						(void)fm_wr(d, FM6000_PCI_DMA_CFG, 0x35u);
						kmsg("nosd-fm6000: RESET reprogram: install260 PCI_DMA_CFG=0x35 written");

						(void)fm_upstream_link_disable(1);
						usleep(150000);
						(void)fm_upstream_link_disable(0);
						rv260_link = fm_wait_link_active(10000);
						snprintf(b260, sizeof b260,
						         "nosd-fm6000: RESET reprogram: install260 DLLLA rv=%d",
						         rv260_link);
						kmsg(b260);

						rfd260 = open("/sys/class/pci_bus/0000:02/rescan",
						              O_WRONLY);
						if (rfd260 >= 0) {
							(void)write(rfd260, "1\n", 2);
							close(rfd260);
							kmsg("nosd-fm6000: RESET reprogram: install260 rescan2 done");
						} else {
							kmsg("nosd-fm6000: RESET reprogram: install260 WARNING rescan2 open failed");
						}
						nap_ms(300);

						fm_detach_pcie(d);
						if (fm_attach_pcie(d) == FM_OK)
							kmsg("nosd-fm6000: RESET reprogram: install260 BAR0 remapped2");
						else
							kmsg("nosd-fm6000: RESET reprogram: install260 WARNING BAR0 remap2 failed");
					}

					/*
					 * install270: ring_init + BIST + freelists + esched.
					 *
					 * install269's fm_boot_cold() crashed the system because
					 * its internal PCIe SerDes init fires 57 SPICO FATALs,
					 * each of which briefly takes the FM6000 off-bus.  The
					 * RS780 PCIe root port then stalls any pending non-posted
					 * transactions (Config Space reads via CF8/CFC, and /dev/mem
					 * Memory Reads via BAR0/BAR1) until the link recovers,
					 * freezing the x86 CPU indefinitely (PCIe CTO → NMI → panic).
					 *
					 * SerDes re-init is NOT needed after a chip autonomous
					 * self-reset: the PCIe link was already trained by the
					 * initial boot, and install260's DL_Init + rescan above
					 * re-establishes it.  What IS needed is:
					 *   1. SSCHED ring_init — ring probe fires ~160 FATALs,
					 *      circulates the IC flip-flop (persists through reset),
					 *      and makes LBUS register reads return valid data
					 *      (pin_strap=0x208).
					 *   2. BAR0 re-link (install270b) — ring_init's FATALs
					 *      bounce the PCIe link; a second DL_Init + rescan
					 *      restores d->pcie_regs before dma_reprogram uses it.
					 *   3. BIST — restores ESCHED/SAF ECC; must run with LBUS
					 *      alive (ring running) so the march engine can write.
					 *   4. fm_boot_freelists — SPICO wiped the freelist SRAM;
					 *      tokens must be re-injected for the SSCHED to schedule.
					 *   5. fm_esched_set_cfg2 — configures the CPU-port DRR
					 *      queue; needed so the first CPU-bound frame can exit
					 *      the egress scheduler without a FATAL.
					 *
					 * ring_init fires ~160 FATALs internally (the SSCHED probe
					 * phase).  BAR0 is detached before ring_init and must be
					 * re-attached after (install270b).  d->regs (LBUS via SCD
					 * BAR1) is unaffected — it stays valid through all FATALs.
					 * [install270]
					 */
					/* 270a: detach BAR0 so ring_init's FATALs cannot stall
					 * any thread via a dangling pcie_regs pointer.  Only
					 * fwd_thread uses pcie_regs (DMA functions), so the detach
					 * is safe from this call site.  d->pcie_bar_phys is
					 * preserved — fm_attach_pcie() will remap from it. */
					fm_detach_pcie(d);
					kmsg("nosd-fm6000: RESET reprogram: install270 BAR0 detached before ring_init");

					/* 270b: SCD reset pulse + ring_init.
					 *
					 * alive_wait [install272] confirmed: after a DMA FATAL the
					 * chip's internal RESET_N (from the DMA watchdog) clears
					 * SSCHED state but does NOT re-run SPICO ROM.  pin_strap
					 * stays 0x0 forever — 120 polls (120s) all returned alive=0.
					 *
					 * SPICO ROM only runs when the SCD hardware drives an
					 * EXTERNAL RESET_N via the RESET_N pin on the FM6000.
					 * fm_scd_reset_pulse() issues exactly that.  After the pulse,
					 * SPICO ROM runs and initialises the SSCHED ring; pin_strap
					 * returns 0x208 within ~100ms of the pulse (same path as
					 * boot.c:241 on initial cold boot).
					 *
					 * After the pulse the FM6000 PCIe endpoint also resets and
					 * re-trains (BAR values cleared by the chip; install270c's
					 * rescan re-enables them).  d->pcie_regs is already NULL
					 * (detached by install270a) so no BAR0 access races.
					 * [install273]
					 */
					{
						/* SCD reset pulse → SPICO ROM → ring alive */
						{
							int rv_pulse = fm_scd_reset_pulse(d);
							char pb[128];
							snprintf(pb, sizeof pb,
								"nosd-fm6000: RESET reprogram: scd_reset_pulse rv=%d [install273]",
								rv_pulse);
							kmsg(pb);
						}
						nap_ms(500); /* SPICO ROM start + initial PCIe re-train.
						 *
						 * install275 tried 10s but that did not help: ring_init
						 * with flags=0 does fm_mem_fill(0x118800) which zeroes
						 * SPICO ROM's ring memory and kills the ring every time
						 * regardless of how long we wait.
						 *
						 * The fix [install276] is to call ring_init with the
						 * same flags as the cold-boot path: NEXT_CHAIN |
						 * NO_SWEEPER.  NEXT_CHAIN skips the initial 0x118800
						 * fill, so SPICO ROM's ring stays alive.  The settle
						 * phase's ~163 ECC-init resets do not clear pin_strap,
						 * so fm_alive passes throughout.  After settle, the
						 * 0x118800 fill runs on a stopped chip (safe). */
						fm_clear_offbus(d);

						/* alive_wait: should be alive within 1–2 polls;
						 * keep up to 10s in case SPICO needs slightly more. */
						{
							char awb[128];
							for (int aw = 0; aw < 10; aw++) {
								fm_clear_offbus(d);
								int alive273 = fm_alive(d);
								snprintf(awb, sizeof awb,
									"nosd-fm6000: RESET reprogram: alive_wait aw=%d alive=%d [install273]",
									aw, alive273);
								kmsg(awb);
								if (alive273 == 1)
									break;
								nap_ms(1000);
							}
						}

						/* Post-pulse DLLLA wait [install274].
						 *
						 * ring_init's fm_sbus_start (first fm_wr) uses
						 * check_writes → fm_check_offbus reads PCIe vendor
						 * ID via /sys/.../config.  After the SCD reset pulse
						 * the FM6000 PCIe endpoint resets and re-trains; during
						 * re-training the vendor ID reads 0xFFFF → offbus=1 →
						 * ring_init exits in 0.3s with rv=-2.
						 *
						 * LBUS (pin_strap) becomes 0x208 within 500ms (as
						 * alive_wait confirms), but PCIe DLLLA takes longer.
						 * Waiting for DLLLA ensures the vendor ID is valid
						 * before ring_init's check_writes run. [install274]
						 */
						{
							int rv_link = fm_wait_link_active(10000);
							char lb[128];
							snprintf(lb, sizeof lb,
								"nosd-fm6000: RESET reprogram: post-pulse DLLLA rv=%d [install274]",
								rv_link);
							kmsg(lb);
							fm_clear_offbus(d);
						}

						/*
						 * install283: PLL setup before ring_init (boot.c step 6
						 * pattern), then ring_init with NEXT_SKIP_PLL.
						 *
						 * Root cause chain (install278-282):
						 * - RESET_N (SCD pulse) clears PLL_CTRL_0/1 to 0.
						 * - PLL_CTRL_0=0 powers down the PLL block entirely.
						 * - SPICO ROM re-runs after RESET_N and releases JSS from
						 *   SOFT_RESET (makes JSS active, starts SBus).
						 * - Writing PLL_CTRL_0=GOLDEN while JSS is active triggers
						 *   a HARD chip FATAL with internal RESET_N (not merely
						 *   an SSCHED SRAM wipe). After this internal RESET_N,
						 *   PLL_CTRL_0 is cleared to 0 again. This repeats: the
						 *   fm_rd_hazardous + 300ms wait in install282 never sees
						 *   pll != 0x0 because the FATAL keeps resetting the chip.
						 *
						 * Fix (install283): mirror boot.c steps 6-7 exactly.
						 * Before ring_init: hold JSS in SOFT_RESET, then write
						 * PLL_CTRL_0=GOLDEN safely (JSS not active, no FATAL),
						 * wait for PLL lock, then release JSS from SOFT_RESET.
						 * Then call ring_init with NEXT_SKIP_PLL so its NEXT_CHAIN
						 * does not re-write PLL_CTRL_0 (already set).
						 * boot.c: step6 (PLL, JSS held) -> step7 (release JSS)
						 *         -> ring_init.  We reproduce that here.
						 * [install283, 2026-11-21]
						 */
						{
							char spll[128];

							/* Hold JSS in SOFT_RESET (also holds SBus). */
							(void)fm_wr(d, FM6000_SOFT_RESET,
								    FM6000_SOFT_RESET_JSS);
							kmsg("nosd-fm6000: RESET reprogram: JSS held in SOFT_RESET [install283]");

							/* Write PLL_CTRL_0=GOLDEN (safe: JSS not active). */
							(void)fm_wr(d, FM6000_PLL_CTRL_0,
								    FM6000_PLL_CTRL_0_GOLDEN);
							(void)fm_wr(d, FM6000_PLL_CTRL_1,
								    FM6000_PLL_CTRL_1_GOLDEN);
							(void)fm_wr(d, FM6000_DLL_CTRL,
								    FM6000_DLL_CTRL_ENABLE);

							/* Wait for PLL lock (fm_rd_hazardous, 300ms). */
							{
								uint32_t pll283 = 0;
								unsigned w283;
								for (w283 = 0;
								     w283 < FM6000_PLL_LOCK_MAX_MS;
								     w283++) {
									if (fm_rd_hazardous(d,
									    FM6000_PLL_STATUS,
									    &pll283) == FM_OK &&
									    (pll283 & FM6000_PLL_STATUS_LOCKED_ALL) ==
									    FM6000_PLL_STATUS_LOCKED_ALL)
										break;
									usleep(1000);
								}
								snprintf(spll, sizeof spll,
									"nosd-fm6000: RESET reprogram: pre-ring PLL waited=%u pll=0x%08x [install283]",
									w283, pll283);
								kmsg(spll);
							}

							/* Release JSS from SOFT_RESET. */
							(void)fm_wr(d, FM6000_SOFT_RESET, 0);
							kmsg("nosd-fm6000: RESET reprogram: JSS released from SOFT_RESET [install283]");
						}

						{
							struct fm_ssched_report srep283 = {0};
							int circulating283 = 0;
							char sri[128];
							int rv_ri = FM_EOFFBUS;

							for (int ring_try = 0;
							     ring_try < 5 &&
							     rv_ri < 0 && rv_ri != FM_ERR;
							     ring_try++) {
								fm_clear_offbus(d);
								memset(&srep283, 0, sizeof srep283);
								circulating283 = 0;
								rv_ri = fm_ssched_ring_init(d,
									FM_SSCHED_NEXT_CHAIN |
									FM_SSCHED_NO_SWEEPER |
									FM_SSCHED_NEXT_SKIP_PLL,
									&circulating283, &srep283);
								snprintf(sri, sizeof sri,
									"nosd-fm6000: RESET reprogram: ring_init try=%d rv=%d circ=%d lf=0x%08x [install283]",
									ring_try, rv_ri, circulating283,
									srep283.last_fatal);
								kmsg(sri);
								if (rv_ri < 0)
									nap_ms(2000);
							}
						}
					}

					/* 270c: re-link PCIe after ring_init's FATALs.
					 * Same DL_Init + rescan + remap as install260 above. */
					{
						int rfd270;
						char b270[128];
						int rv270_link;

						rfd270 = open("/sys/class/pci_bus/0000:02/rescan", O_WRONLY);
						if (rfd270 >= 0) {
							(void)write(rfd270, "1\n", 2);
							close(rfd270);
							kmsg("nosd-fm6000: RESET reprogram: install270c rescan1 done");
						} else {
							kmsg("nosd-fm6000: RESET reprogram: install270c WARNING rescan1 failed");
						}
						nap_ms(300);

						if (fm_attach_pcie(d) == FM_OK)
							kmsg("nosd-fm6000: RESET reprogram: install270c BAR0 remapped");
						else
							kmsg("nosd-fm6000: RESET reprogram: install270c WARNING BAR0 remap failed");

						(void)fm_upstream_link_disable(1);
						usleep(150000);
						(void)fm_upstream_link_disable(0);
						rv270_link = fm_wait_link_active(10000);
						snprintf(b270, sizeof b270,
						         "nosd-fm6000: RESET reprogram: install270c DLLLA rv=%d",
						         rv270_link);
						kmsg(b270);

						rfd270 = open("/sys/class/pci_bus/0000:02/rescan", O_WRONLY);
						if (rfd270 >= 0) {
							(void)write(rfd270, "1\n", 2);
							close(rfd270);
							kmsg("nosd-fm6000: RESET reprogram: install270c rescan2 done");
						} else {
							kmsg("nosd-fm6000: RESET reprogram: install270c WARNING rescan2 failed");
						}
						nap_ms(300);

						fm_detach_pcie(d);
						if (fm_attach_pcie(d) == FM_OK)
							kmsg("nosd-fm6000: RESET reprogram: install270c BAR0 remapped2");
						else
							kmsg("nosd-fm6000: RESET reprogram: install270c WARNING BAR0 remap2 failed");

						(void)fm_wr(d, FM6000_PCI_DMA_CFG, 0x35u);
						kmsg("nosd-fm6000: RESET reprogram: install270c PCI_DMA_CFG=0x35");
					}

					/* 270d: esched only (bist+freelists moved to
					 * before ring_init by install277 above) */
					{
						char bc270[128];
						int rv_es = fm_esched_set_cfg2(d, FM_ESCHED_CFG2_SKIP_FP_PORTS);
						snprintf(bc270, sizeof bc270,
							"nosd-fm6000: RESET reprogram: esched_cfg2 rv=%d [install270]",
							rv_es);
						kmsg(bc270);
					}

					rv_fill4k = fm_mem_fill(d, 0x180000u, 16384u, 0u);
					rv_fill256 = fm_mem_fill(d, 0x1a0000u, 4096u, 0u);
					snprintf(rp_buf, sizeof rp_buf,
						"nosd-fm6000: RESET reprogram: L2F pre-fill rv4k=%d rv256=%d",
						rv_fill4k, rv_fill256);
					kmsg(rp_buf);
					rv_l2f = fm_l2f_init(d);
					snprintf(rp_buf, sizeof rp_buf,
						"nosd-fm6000: RESET reprogram: l2f_init rv=%d", rv_l2f);
					kmsg(rp_buf);
					pipeline_init(d);
					kmsg("nosd-fm6000: RESET reprogram: pipeline_init done");
					rv_ffu = fm_ffu_init(d);
					snprintf(rp_buf, sizeof rp_buf,
						"nosd-fm6000: RESET reprogram: ffu_init rv=%d", rv_ffu);
					kmsg(rp_buf);
				}

				fm_dma_reprogram(&fwd->dma);

					/*
					 * install268: refresh last_fc after reprogram.
					 *
					 * The stability loop exits when fc stabilises at any
					 * non-0xFFFFFFFF value.  If LBUS returned 0 during SPICO
					 * initialisation, it exits with last_fc=0.  After the
					 * chip fully recovers (SPICO done, PCIe retrain, rescan),
					 * the chip's actual FATAL_COUNT is N>0.  The first
					 * CHECK_PERIOD then sees N != 0 and fires a false RESET.
					 * Re-reading fc here (after LBUS is alive and DMA is
					 * reprogrammed) sets last_fc to the chip's actual count,
					 * preventing false RESET detection.  [install268]
					 */
					last_fc = fc_read_safe(fwd->dma.d);
				}
		}

		/* Log TX/RX activity every 1 second (diagnostic interval) */
		{
			time_t now = time(NULL);
			if (now - last_log >= 1) {
				last_log = now;
				/* Dump first 4 TX descriptor STATUS bytes for diagnosis */
				{
					const struct fm_dma_ring *r = &fwd->dma.tx;
					uint32_t j;
					/* fc_read_safe: fm_fatal_count → fm_check_offbus reads bus-2
					 * config via CF8/CFC, which stalls on AMD RS780 when FM6000
					 * link is down.  Use hazardous path: MMIO only. */
					uint32_t fc = fc_read_safe(fwd->dma.d);
					fprintf(stderr,
						"nosd-fm6000: fwd heartbeat: rx_polls=%lu rx_frames=%lu tx_sent=%lu tx_ringfull=%lu tx_head=%u tx_tail=%u fc=%u\n",
						rx_poll_count, g_rx_total, tx_sent, tx_ringfull,
						r->head & r->mask, r->tail & r->mask, fc);
					fprintf(stderr, "nosd-fm6000: TX desc STATUS[0..7]:");
					for (j = 0; j < 8 && j < r->size; j++)
						fprintf(stderr, " %02x",
							*(const uint8_t *)(r->desc + (size_t)j * FM6000_DESC_STRIDE + FM6000_DESC_STATUS));
					fprintf(stderr, "\n");

					/* Dump RX descriptor STATUS bytes */
					{
						const struct fm_dma_ring *rx = &fwd->dma.rx;
						fprintf(stderr, "nosd-fm6000: RX desc STATUS[0..7]:");
						for (j = 0; j < 8 && j < rx->size; j++)
							fprintf(stderr, " %02x",
								*(const uint8_t *)(rx->desc + (size_t)j * FM6000_DESC_STRIDE + FM6000_DESC_STATUS));
						fprintf(stderr, "\n");
					}

					/* kmsg heartbeat: proves the kernel is alive from the console */
					{
						char hb[160];
						snprintf(hb, sizeof(hb),
							"nosd-fm6000: fwd heartbeat: rx_polls=%lu rx_frames=%lu tx_sent=%lu fc=%u",
							rx_poll_count, g_rx_total, tx_sent, fc);
						kmsg(hb);
					}
					/* install266: bar_check (BAR0 DMA_CFG/STATUS reads) REMOVED.
					 * The read triggered a PCIe Completion Timeout (CTO) on the
					 * RS780 → non-correctable MCE → kernel panic at [161.9s].
					 * CTO happened because the FM6000 DMA block stalls non-posted
					 * transactions (BAR0 reads) when the DMA TX engine has not yet
					 * consumed any frames.  FATAL_COUNT was unchanged (chip alive),
					 * confirming the 0xffffffff was from CTO not chip self-reset.
					 * FATALs are detected via fc_read_safe (LBUS, not BAR0). */
					kmsg("nosd-fm6000: DMA266 bar_check skipped [install266]");
				}
			}
		}

		/* Wait up to 1ms for TAP readability */
		FD_ZERO(&rfds);
		maxfd = -1;
		for (i = 0; i < FWD_NPORTS; i++) {
			if (fwd->ports[i].tap_fd >= 0) {
				FD_SET(fwd->ports[i].tap_fd, &rfds);
				if (fwd->ports[i].tap_fd > maxfd)
					maxfd = fwd->ports[i].tap_fd;
			}
		}
		if (maxfd < 0) {
			usleep(1000);
			continue;
		}
		tv.tv_sec = 0; tv.tv_usec = 1000;
		n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (n <= 0)
			continue;

		for (i = 0; i < FWD_NPORTS; i++) {
			ssize_t len;
			int fd = fwd->ports[i].tap_fd;

			if (fd < 0 || !FD_ISSET(fd, &rfds))
				continue;
			len = fm_tap_read(fd, txbuf, sizeof(txbuf));
			if (len < 14)
				continue;
			if (fm_dma_tx(&fwd->dma, txbuf, (uint16_t)len,
				      FM6000_CPU_GLORT, fwd->ports[i].tx_dglort) == 0)
				tx_sent++;
			else
				tx_ringfull++;
		}
	}
	return NULL;
}

/*
 * Bring up the two front-panel ports that have cables in the lab.
 *
 * fm_lane_enable() runs the full SerDes init: SBus device reset, polarity,
 * rate select, PLL lock, TX/RX enable, EPL_CFG_A/B, per-lane SERDES regs,
 * and the TX equalizer.  Without this the physical ports never emit signal
 * and no frames flow regardless of the DMA state.
 */
static void bring_up_ports(struct fm6000 *d)
{
	static const int portlist[] = { 1, 2 };
	static const char *portnames[] = { "et1", "et2" };
	int saved_check;
	size_t i;

	kmsg("nosd-fm6000: bring_up_ports entry");

	/*
	 * Read EPL SERDES_CFG_LO before Si5338 runs, as a baseline.
	 * If it reads back a non-zero value after we write it below, EPL is
	 * clocked and registers are latching.  If it always reads 0, EPL has
	 * no clock and writes are not taking effect.
	 */
	{
		uint32_t cfg_lo = 0xdeadbeef;
		char sbuf[96];

		(void)fm_rd(d, FM6000_EPL_LANE(14, 0) + FM6000_EPL_SERDES_CFG_LO, &cfg_lo);
		snprintf(sbuf, sizeof(sbuf),
			"nosd-fm6000: pre-si5338: EPL14 SERDES_CFG_LO=0x%08x "
			"pll_status=0x%x",
			cfg_lo, 0u);
		{
			uint32_t pst = 0;
			(void)fm_rd(d, FM6000_PLL_STATUS, &pst);
			snprintf(sbuf, sizeof(sbuf),
				"nosd-fm6000: pre-si5338: EPL14 SERDES_CFG_LO=0x%08x "
				"pll_status=0x%08x",
				cfg_lo, pst);
		}
		kmsg(sbuf);
	}

	/*
	 * Si5338 NOTE (install84, 2026-10-22): PLLs are already locked (0xf) and
	 * EPL14 is clocked (non-zero readback) BEFORE fm6000-si5338 config runs.
	 * Running the config kills the PLLs (drops to 0, never recovers in 5s).
	 * The Si5338 NVM retains its EOS-programmed output from the previous boot.
	 * Skip the config program; the hardware is already in the correct state.
	 */
	kmsg("nosd-fm6000: Si5338 config skipped (NVM already correct)");

	/*
	 * Check PLL state before touching SerDes. Do NOT rewrite PLL_CTRL_0/1
	 * here: writing them while the chip is running resets the PLLs (measured
	 * install82, 2026-10-22 -- pll_status 0xf→0 after rewrite). The boot
	 * sequence left PLLs locked (pll_status=0xf confirmed at post_pipeline);
	 * a rewrite here undoes that work.
	 *
	 * Only write DLL_CTRL if DLLs are not already locked (bits 3:2 of
	 * PLL_STATUS). PLLs themselves (bits 1:0) auto-lock without intervention.
	 */
	{
		uint32_t pll_st = 0, cfg_lo = 0;
		char pbuf[160];

		(void)fm_rd(d, FM6000_PLL_STATUS, &pll_st);
		snprintf(pbuf, sizeof(pbuf),
			"nosd-fm6000: PLL_STATUS pre-serdes: 0x%08x", pll_st);
		kmsg(pbuf);

		if ((pll_st & FM6000_PLL_STATUS_LOCKED_ALL) != FM6000_PLL_STATUS_LOCKED_ALL) {
			/*
			 * PLLs or DLLs not locked. Write DLL_CTRL to enable DLLs
			 * (bits 3:2). Do NOT touch PLL_CTRL_0/1 -- only write if
			 * DLLs specifically are absent.
			 */
			(void)fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE);
			kmsg("nosd-fm6000: DLL_CTRL written (DLLs not locked)");
			nap_ms(100);
			(void)fm_rd(d, FM6000_PLL_STATUS, &pll_st);
			snprintf(pbuf, sizeof(pbuf),
				"nosd-fm6000: PLL_STATUS after DLL_CTRL: 0x%08x", pll_st);
			kmsg(pbuf);
		}

		/*
		 * Write SERDES_CFG_LO for EPL14 lane0 then immediately read it back.
		 * If EPL is clocked, the readback returns the written value.
		 * If EPL has no clock, the readback returns 0 (write not latched).
		 */
		(void)fm_wr(d, FM6000_EPL_LANE(14, 0) + FM6000_EPL_SERDES_CFG_LO,
			    FM6000_EPL_SERDES_CFG_LO_10G);
		(void)fm_rd(d, FM6000_EPL_LANE(14, 0) + FM6000_EPL_SERDES_CFG_LO,
			    &cfg_lo);
		snprintf(pbuf, sizeof(pbuf),
			"nosd-fm6000: EPL14 SERDES_CFG_LO after write: "
			"readback=0x%08x (want=0x%08x, 0=no_epl_clock)",
			cfg_lo, FM6000_EPL_SERDES_CFG_LO_10G);
		kmsg(pbuf);
	}

	/*
	 * Re-initialise the SBus master and log its state for diagnostics.
	 * fm_sbus_start() is a no-op when SBUS_CFG is already 0 (already running).
	 */
	{
		int sr;
		char buf[128];

		sr = fm_sbus_start(d);
		snprintf(buf, sizeof(buf),
			"nosd-fm6000: fm_sbus_start rv=%d", sr);
		kmsg(buf);

		{
			int p1 = fm_sbus_present(d, 0x49u); /* et1 EPL14 lane0 */
			int p2 = fm_sbus_present(d, 0x45u); /* et2 EPL16 lane0 */
			snprintf(buf, sizeof(buf),
				"nosd-fm6000: sbus_present: 0x49(et1)=%s 0x45(et2)=%s",
				p1 ? "OK" : "ABSENT", p2 ? "OK" : "ABSENT");
			kmsg(buf);
		}
	}

	/*
	 * Disable the per-write fm_check_offbus() sysfs reads for the duration.
	 * Each SBus write would otherwise open /sys/.../config, which acquires
	 * a kernel PCIe lock.  The DMA fwd_thread is already running at this
	 * point, and AER events from in-flight DMA traffic can hold that lock,
	 * producing a deadlock that freezes the kernel.
	 *
	 * The local bus reads 0x00000000 (not 0xffffffff) when the chip is
	 * off-bus, so fm_rd_byte never triggers the sysfs path either.
	 * fm_alive() is the right post-init check if we need one.
	 */
	saved_check = d->check_writes;
	d->check_writes = 0;

	for (i = 0; i < sizeof(portlist) / sizeof(portlist[0]); i++) {
		const struct fm_port *p = fm_port_lookup(portlist[i]);
		struct fm_lane_report rep;
		int rv;

		if (!p) {
			fprintf(stderr,
				"nosd-fm6000: port %d (%s) not in SerDes table\n",
				portlist[i], portnames[i]);
			continue;
		}
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "nosd-fm6000: SerDes %s start", portnames[i]);
			kmsg(buf);
		}
		rv = fm_lane_enable(d, p, &rep);
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "nosd-fm6000: SerDes %s %s ls=0x%08x ps=0x%08x",
				portnames[i], rv == FM_OK ? "up" : "FAILED",
				rep.lane_status, rep.port_status);
			kmsg(buf);
			/* On failure: log which step timed out or errored. */
			if (rv != FM_OK) {
				int si;
				for (si = 1; si < FM_LANE__COUNT; si++) {
					if (rep.step[si].rv != 0 && rep.step[si].rv != FM_ENOADDR &&
					    rep.step[si].what != NULL) {
						char b2[128];
						snprintf(b2, sizeof(b2),
							"nosd-fm6000: SerDes %s step %d rv=%d: %s",
							portnames[i], si, rep.step[si].rv,
							rep.step[si].what);
						kmsg(b2);
						break;
					}
				}
			}
		}
		fprintf(stderr,
			"nosd-fm6000: %s (port %d, EPL %d lane %d): SerDes %s\n",
			portnames[i], portlist[i], p->epl, p->lane,
			rv == FM_OK ? "up" : "FAILED");
		if (rv == FM_OK) {
			/*
			 * Clear PARSER_FLAGS_DOWN. The init seed marks every port
			 * link-down; leaving it set after SerDes comes up causes
			 * the chip to treat the port as down in the forwarding path.
			 */
			int pr = fm_parser_port_link_up(d, portlist[i]);
			fprintf(stderr,
				"nosd-fm6000: %s parser seed link-up: %s\n",
				portnames[i], pr == FM_OK ? "ok" : "FAILED");
		}
	}

	d->check_writes = saved_check;
	kmsg("nosd-fm6000: bring_up_ports done");
}

static int start_forwarding(struct fm6000 *d)
{
	pthread_t tid;
	int i;

	if (d->pcie_regs == NULL) {
		fprintf(stderr, "nosd-fm6000: PCIe BAR0 not mapped; skipping DMA\n");
		return -1;
	}

	/*
	 * Pre-DMA FATAL_COUNT stability wait — two-phase approach.
	 *
	 * SPICO loading bounces the FM6000 PCIe endpoint (one hard reset that
	 * wipes fc back to 0).  With pcie_ports=native, pciehp processes each
	 * link bounce as a hot-plug cycle and fires a Config Space write to the
	 * FM6000 ~7 s after the link comes back.  FM6000 does not respond to
	 * Config TLPs until fm_pcie_serdes_init() re-trains the link.  On AMD
	 * RS780, a Config TLP with no response stalls the CPU in the CF8/CFC
	 * spin-loop indefinitely (no CTO).
	 *
	 * Old approach: long stability wait (up to 90 s), THEN SerDes re-init.
	 * Problem: pciehp fires at ~7 s into the stability wait → kernel freeze.
	 *
	 * Fix (install79c): two-phase approach.
	 *   Phase 1: poll via LBUS hazardous reads until fc first leaves
	 *            UINT32_MAX (chip returned from SPICO reset).  Exit immediately.
	 *   Phase 2: call SerDes re-init right away.  Link training takes ~5 s,
	 *            making Config Space accessible BEFORE pciehp's ~7 s timer.
	 *   Phase 3: full stability wait (10 stable × 500 ms) now that Config
	 *            Space works; pciehp events complete safely.
	 *
	 * All LBUS polls use fm_rd_hazardous() (SCD bus=0, never Config Space).
	 */
	{
		uint32_t fc = 0xFFFFFFFFu, fc2;
		int n_stable = 0;
		int i;
		char stab_msg[128];
		char scd_slot[FM_SLOT_LEN];
		int saved_banks_ready = d->banks_ready;
		int saved_boot_done   = d->boot_done;
		int saved_sched_ready = d->sched_ready;

		snprintf(scd_slot, sizeof scd_slot, "%s", d->slot);
		fm_clear_offbus(d);

		/* Phase 1: wait for chip to return from SPICO reset. */
		kmsg("nosd-fm6000: pre-DMA: waiting for FATAL_COUNT stability");
		for (i = 0; i < 60; i++) {
			struct timespec ts = { 0, 500000000L };
			char iter_msg[128];
			snprintf(iter_msg, sizeof iter_msg,
				 "nosd-fm6000: pre-DMA: iter %d sleep fc=%u stable=%d",
				 i, fc, n_stable);
			kmsg(iter_msg);
			nanosleep(&ts, NULL);
			snprintf(iter_msg, sizeof iter_msg,
				 "nosd-fm6000: pre-DMA: iter %d awake", i);
			kmsg(iter_msg);
			if (fm_rd_hazardous(d, FM6000_FATAL_COUNT, &fc2) != FM_OK)
				fc2 = 0xFFFFFFFFu;
			snprintf(iter_msg, sizeof iter_msg,
				 "nosd-fm6000: pre-DMA: iter %d fc=%u->%u stable=%d",
				 i, fc, fc2, n_stable);
			kmsg(iter_msg);
			fc = fc2;
			if (fc != 0xFFFFFFFFu)
				break;  /* chip is back; start SerDes re-init now */
		}

		/*
		 * Phase 2: PCIe SerDes re-init post-SPICO.
		 *
		 * install113: Link Disable + fm_pcie_serdes_init post-SPICO.
		 * install110 (no Link Disable) failed because pciehp fired a Config
		 * Write ~7 s after the link came back (CPU freeze).
		 *
		 * install244: drop the fm_upstream_link_disable(1) call.
		 * Root cause found: Link Disable on the RS780 root port causes the
		 * FM6000 to deallocate its PCIe posted-data receive buffer and
		 * advertise 0 PD flow-control credits in the subsequent DL_Init.
		 * With 0 PD credits the RS780 cannot forward Memory Write TLPs to
		 * the FM6000; the CPU hangs on the first dma_wr_nc() call (BAR0
		 * Memory Write stalls in the RS780 posted-write buffer indefinitely).
		 * The pciehp problem that forced install113 to add Link Disable is
		 * now gone: fm6000_kdrv silences pciehp slot events at bind time
		 * ("pciehp + AER + PME IRQ silenced"), so no Config Write races.
		 * fm_pcie_serdes_init (SOFT_RESET_PCIE) alone does NOT reset the
		 * PD credit advertiser — only the Link Disable / Electrical Idle
		 * transition does.  Skipping Link Disable preserves the nonzero PD
		 * credits set during the initial (T+55s) DL_Init.
		 * [install244, 2026-11-16]
		 */
		kmsg("nosd-fm6000: pre-DMA: SerDes re-init (no Link Disable — pciehp silenced) [install244]");

		{
			int rv_si = fm_pcie_serdes_init(d);
			char si_msg[80];
			snprintf(si_msg, sizeof si_msg,
				 "nosd-fm6000: pre-DMA: PCIe SerDes re-init rv=%d [install113]",
				 rv_si);
			kmsg(si_msg);
		}

		/* Release LBUS -- fm_pcie_serdes_init started link training via
		 * PCI_CORE_CTRL_1; SCD slot is disrupted and must not be used. */
		munmap((void *)d->regs, d->bar_bytes);
		close(d->bar_fd);
		d->regs   = NULL;
		d->bar_fd = -1;

		kmsg("nosd-fm6000: pre-DMA: waiting 5s for PCIe endpoint training");
		{
			struct timespec nap5s = { 5, 0 };
			nanosleep(&nap5s, NULL);
		}

		kmsg("nosd-fm6000: pre-DMA: clearing Link Disable [install113]");
		(void)fm_upstream_link_disable(0);

		(void)fm_wait_link_active(15000); /* 15 s for post-clear re-train */
		/*
		 * install203: skip fm_wait_pcie_alive — its internal rescan
		 * write is unsafe.  fm_wait_link_active above already confirmed
		 * DLLLA=1.  Call fm_pcie_enable_portio to confirm the link is
		 * stable and enable Memory Space via a CF8 posted write (no CTO
		 * stall risk).  fm_attach_pcie below uses the cached
		 * pcie_bar_phys to reprogram BAR0 and mmap without sysfs.
		 * [install203, 2026-10-29]
		 */
		{
			int rv203 = fm_pcie_enable_portio(2, 0, 0);
			char b203[128];
			snprintf(b203, sizeof b203,
				 "nosd-fm6000: install203: pre-DMA portio rv=%d"
				 " [install203]", rv203);
			kmsg(b203);
		}
		kmsg("nosd-fm6000: pre-DMA: endpoint alive; reopening LBUS");

		/*
		 * install204: save PCIe BAR cache before fm_reopen_lbus_devmem
		 * wipes the struct with memset(d, 0, sizeof(*d)).  The mapping
		 * (pcie_regs) is NOT munmap'd by the memset — the virtual pages
		 * stay mapped — so restoring the pointer after the reopen is
		 * safe.  [install204, 2026-10-29]
		 */
		{
			volatile uint32_t *saved_pcie_regs   = d->pcie_regs;
			size_t             saved_pcie_bytes   = d->pcie_bar_bytes;
			unsigned long      saved_pcie_phys    = d->pcie_bar_phys;
			int                saved_pcie_bar_fd  = d->pcie_bar_fd;

			if (fm_reopen_lbus_devmem(d, scd_slot) != FM_OK) {
				kmsg("nosd-fm6000: pre-DMA: LBUS reopen failed; aborting");
				return -1;
			}
			d->pcie_regs      = saved_pcie_regs;
			d->pcie_bar_bytes = saved_pcie_bytes;
			d->pcie_bar_phys  = saved_pcie_phys;
			d->pcie_bar_fd    = saved_pcie_bar_fd;
		}
		d->banks_ready = saved_banks_ready;
		d->boot_done   = saved_boot_done;
		d->sched_ready = saved_sched_ready;
		kmsg("nosd-fm6000: pre-DMA: LBUS reopened; attaching PCIe");

		/*
		 * install204: FM6000 self-resets clear its BAR0 register, so the
		 * existing /dev/mem mapping (obtained while the device was in
		 * sysfs) points at the right physical address but the chip is not
		 * decoding it.  Reprogram BAR0 via CF8 portio using the cached
		 * pcie_bar_phys, then keep the mapping — no detach/reattach.
		 *
		 * The detach+reattach path (install83) fails under STRICT_DEVMEM
		 * because after boot.c:820 removes the FM6000 from sysfs the
		 * kernel removes its iomem entry, and /dev/mem rejects mmap of
		 * that range.  install203's cached-path CF8 addresses were also
		 * wrong (bus=0,dev=2 instead of bus=2,dev=0).  Both issues are
		 * fixed here: BAR0 is reprogrammed with the correct CF8 addresses
		 * (0x80020010/14 = bus=2,dev=0,fn=0) and the mapping is kept.
		 * [install204, 2026-10-29]
		 */
		if (d->pcie_bar_phys != 0 && d->pcie_regs != NULL) {
			int rv204 = fm_reprogram_bar_portio(d);
			char b204[128];
			snprintf(b204, sizeof b204,
				 "nosd-fm6000: install204: BAR0 reprogram rv=%d"
				 " lo=0x%08x hi=0x%08x [install204]",
				 rv204,
				 (uint32_t)(d->pcie_bar_phys & 0xFFFFFFF0UL) | 0x4u,
				 (uint32_t)(d->pcie_bar_phys >> 32));
			kmsg(b204);
		} else {
			/* Fallback: BAR not cached or mapping lost; try detach+reattach */
			fm_detach_pcie(d);
			if (fm_attach_pcie(d) != FM_OK) {
				kmsg("nosd-fm6000: pre-DMA: PCIe attach failed; aborting");
				return -1;
			}
		}
		kmsg("nosd-fm6000: pre-DMA: PCIe attached; Bus Master enabled");

		/*
		 * POST-PCIE-INIT TABLE READBACK.
		 *
		 * The PCIe SerDes re-init above triggers link training, which may
		 * cause the FM6000 to soft-reset (FATAL_COUNT bump). A chip reset
		 * wipes all SRAM-based tables including GLORT_TABLE, GLORT_CAM,
		 * and L2F. If they read zero here, we must re-run pipeline_init()
		 * before starting DMA or injection frames will be silently dropped.
		 */
		{
			uint32_t gt1 = 0xdeadbeef, cam0 = 0xdeadbeef;
			uint32_t fc_now = 0;
			char post_msg[200];

			fm_rd_hazardous(d, FM6000_FATAL_COUNT, &fc_now);
			(void)fm_rd(d, 0x00014001u, &gt1);
			(void)fm_rd(d, 0x0000e000u, &cam0);
			snprintf(post_msg, sizeof post_msg,
				"nosd-fm6000: POST_PCIE_INIT: fc=%u TABLE[1]=0x%08x(want=0x0036ffc9) "
				"CAM[0]=0x%08x(want=0x007fffff) alive=%d",
				fc_now, gt1, cam0, fm_alive(d));
			kmsg(post_msg);

			if (gt1 == 0 || gt1 == 0xdeadbeef) {
				kmsg("nosd-fm6000: POST_PCIE_INIT: GLORT_TABLE wiped -- re-running pipeline_init");
				pipeline_init(d);
				kmsg("nosd-fm6000: POST_PCIE_INIT: pipeline_init re-run done");
				/*
				 * Also re-run L2F init since its SRAM would be wiped too.
				 * bring_up_ports and parser link-up are not re-run: parser
				 * SRAM needs the ECC pre-fill burst which fm_mem_fill does
				 * inside pipeline_init(), and the port link state is preserved
				 * by the SerDes (link is still up at the EPL level).
				 */
				{
					int rv2 = fm_l2f_init(d);
					char l2msg[80];
					snprintf(l2msg, sizeof l2msg,
						"nosd-fm6000: POST_PCIE_INIT: L2F re-init rv=%d", rv2);
					kmsg(l2msg);
				}
			}
		}

		/*
		 * Phase 3: full stability wait (10 stable × 500 ms).
		 *
		 * Config Space is now working (link trained).  pciehp events will
		 * complete without hanging.  Use hazardous LBUS reads throughout
		 * to avoid any remaining Config Space risk during this window.
		 * Wait until FATAL_COUNT does not change for 10 consecutive polls.
		 * Cap at 90 s for the pathological case where resets never settle.
		 */
		fc = 0xFFFFFFFFu;
		n_stable = 0;
		fm_clear_offbus(d);
		kmsg("nosd-fm6000: pre-DMA: phase3: full stability wait");
		for (i = 0; i < 180 && n_stable < 10; i++) {
			struct timespec ts = { 0, 50000000L };
			char iter_msg[128];
			snprintf(iter_msg, sizeof iter_msg,
				 "nosd-fm6000: pre-DMA: p3 %d sleep fc=%u stable=%d",
				 i, fc, n_stable);
			kmsg(iter_msg);
			nanosleep(&ts, NULL);
			if (fm_rd_hazardous(d, FM6000_FATAL_COUNT, &fc2) != FM_OK)
				fc2 = 0xFFFFFFFFu;
			snprintf(iter_msg, sizeof iter_msg,
				 "nosd-fm6000: pre-DMA: p3 %d fc=%u->%u stable=%d",
				 i, fc, fc2, n_stable);
			kmsg(iter_msg);
			if (fc2 == fc && fc2 != 0xFFFFFFFFu)
				n_stable++;
			else {
				fc = fc2;
				n_stable = 0;
			}
		}
		snprintf(stab_msg, sizeof stab_msg,
			 "nosd-fm6000: pre-DMA: stability done (fc=%u stable=%d/%d)",
			 fc, n_stable, i);
		kmsg(stab_msg);
	}

	/*
	 * install125: re-run NEXT_CHAIN ring_init with Memory Space disabled.
	 *
	 * Root cause of the TX ring being stuck (TX_RT=0x14, TX_FOUND=0) after
	 * install113's PCIe SerDes re-init:
	 *
	 *   1. Memory Space (PCI_COMMAND bit 1) is enabled on FM6000 when IC=1
	 *      fires.  boot.c established (install59, confirmed by the first TX
	 *      circulation in c5567d2) that enabling PCIe Memory Space before
	 *      IC=1 stalls the TX ring engine: TX_REPLACE_TOKEN sticks at ring
	 *      slot 0 and TX_FOUND never fires, regardless of token state.
	 *
	 *   2. install113's 180+ chip FALTALs (code 0x9a/0x97) flush the
	 *      INIT_TOKEN FIFO.  tx_reload re-writes NEXT_PORT but does NOT
	 *      reset or refill the FIFO.  IC=1 on a RUNNING engine with empty
	 *      FIFO: TX_FOUND=0, fc stable (no FATAL -- engine advances with no
	 *      tokens and no ring activity).
	 *
	 *   3. ESCHED set_cfg2 with TX_FOUND=0 triggers a chip FATAL (measured
	 *      install115).  With SERR# re-enabled, FATAL → ERR_FATAL → SERR# →
	 *      SMI → BIOS SMM → CF8/CFC bus=2 while link bouncing → CTO.
	 *      With SERR# disabled, FATAL → link bounce → kernel CF8/CFC bus=2
	 *      → no SERR# → AMD RS780 cannot generate error completion → stall.
	 *      Both paths deadlock the OS.
	 *
	 * Fix:
	 *   a. Disable SERR# on root ports: suppress SERR# → SMI → SMM path
	 *      for any residual resets during the ring re-init.
	 *   b. Disable Memory Space on FM6000 via CF8/CFC (bus=2 link is stable
	 *      after Phase 3 wait): replicates the pcie_regs==NULL condition
	 *      under which boot.c first achieved TX circulation (c5567d2).
	 *   c. Null d->pcie_regs temporarily: fm_esched_init inside ring_init
	 *      is a no-op when pcie_regs==NULL; ESCHED is restored separately
	 *      after TX has been confirmed circulating.
	 *   d. Run fm_ssched_ring_init(NEXT_CHAIN|NO_SWEEPER): resets the
	 *      INIT_TOKEN FIFO (fm_boot_freelists), re-writes NEXT_PORT scan
	 *      chain, loads 64 fresh tokens, and fires IC=1 on the RESET engine
	 *      (install113 FALTALs reset the IC flip-flop).  With Memory Space
	 *      off, TX circulates.
	 *   e. Restore pcie_regs; re-enable Memory Space after stability wait.
	 *   f. ESCHED set_cfg2 with TX circulating: no FATAL, no SMM path,
	 *      clean DRR_Q+CFG_1+CFG_2 restore.
	 *
	 * SERR# remains disabled until fm_pcie_enable_portio (portio) below,
	 * which re-enables it once the link is confirmed at L0.
	 * [RE 2026-10-27, install125]
	 */
	{
	int circ125 = 0;  /* declared here so ESCHED guard below can read it */

	/*
	 * install134: remove FM6000 from PCIe BEFORE RESET_N to prevent the
	 * pciehp/AER CF8/CFC deadlock that froze install133.
	 *
	 * install133 froze at ESCHED CFG_2 inside boot.c line 863.
	 * Root cause:
	 *   1. After main.c's pre-DMA PCIe SerDes re-init, fm6000_kdrv has an
	 *      active AER handler and pciehp is monitoring the FM6000 slot.
	 *   2. RESET_N drops the PCIe link.  pciehp sees the hot-removal and
	 *      starts polling Config Space; fm6000_kdrv's error_detected fires.
	 *      Both use CF8/CFC (I/O port 0xcf8/0xcfc) on AMD RS780.
	 *   3. boot_cold's fm_pcie_serdes_init fires ~57 SPICO FATALs, each
	 *      bouncing the PCIe link.  pciehp and AER race to do CF8/CFC
	 *      accesses during each bounce.  On AMD RS780 a CF8/CFC read to an
	 *      off-bus device holds the spinlock forever (no CTO for MMIO).
	 *      The kernel deadlocks.
	 *   4. fm_esched_set_cfg2 writes via d->pcie_regs (posted UC MMIO, no
	 *      stall), but the subsequent esched_dbg("done") calls write() (a
	 *      syscall).  With the kernel deadlocked, the syscall never returns.
	 *      Process appears frozen after logging "writing CFG_2".
	 *
	 * Fix:
	 *   a. Remove FM6000 from PCIe BEFORE RESET_N: fm6000_kdrv.remove()
	 *      tears down the AER handler; pciehp stops monitoring the slot.
	 *      No concurrent CF8/CFC during SerDes FALTALs.
	 *   b. fm_detach_pcie(d): fm_attach_pcie is a no-op when pcie_regs !=
	 *      NULL; clearing it ensures boot_cold's fm_attach_pcie re-reads
	 *      the BAR address and reprograms it after the rescan.
	 *   c. boot_cold runs ring_init(NEXT_CHAIN) + SerDes (no concurrent
	 *      CF8/CFC) + its own ESCHED write (boot.c line 863) + stability
	 *      wait.  Do NOT call fm_esched_set_cfg2 again below.
	 *   d. RESET_N wipes pipeline SRAM (parser, LBS, GLORT, L2F).
	 *      Re-run pipeline_init + fm_l2f_init after boot_cold returns.
	 *
	 * TXMP/RXMP root cause (install132): CM_PORT_TXMP_USAGE (0x118800)
	 * and CM_PORT_RXMP_USAGE (0x111000) hold stale non-zero values from
	 * the previous ring run.  RESET_N zeros them (power-on state).
	 * BIST+FREELISTS_ALL does not zero them.
	 * [RE 2026-10-27, install134]
	 */
	{
		struct fm_boot_report brep134;
		int rv_boot;
		char bmsg[192];

		/* a. Remove FM6000 from PCIe to stop pciehp monitoring + AER. */
		{
			int rfd = open("/sys/bus/pci/devices/0000:02:00.0/remove",
				       O_WRONLY);
			if (rfd >= 0) {
				(void)write(rfd, "1\n", 2);
				close(rfd);
				kmsg("nosd-fm6000: ring-reinit: FM6000 removed from PCIe (kills pciehp+AER)");
			} else {
				kmsg("nosd-fm6000: ring-reinit: WARNING: could not remove FM6000 from PCIe");
			}
		}

		/* b. Clear pcie_regs so boot_cold's fm_attach_pcie runs properly. */
		fm_detach_pcie(d);
		nap_ms(200);  /* let fm6000_kdrv.remove() and pciehp settle */

		/* c. RESET_N: zeros TXMP/RXMP; pciehp/AER already gone. */
		{
			int rv_reset = fm_scd_reset_pulse(d);
			snprintf(bmsg, sizeof bmsg,
				"nosd-fm6000: ring-reinit: reset_pulse rv=%d"
				" (zeros TXMP/RXMP; pciehp+AER already removed)",
				rv_reset);
			kmsg(bmsg);
		}
		nap_ms(100);
		fm_clear_offbus(d);

		rv_boot = fm_boot_cold(d, &brep134);
		circ125 = (rv_boot == FM_OK) ? 1 : 0;
		snprintf(bmsg, sizeof bmsg,
			"nosd-fm6000: ring-reinit: fm_boot_cold rv=%d circ=%d",
			rv_boot, circ125);
		kmsg(bmsg);

		/* d. pipeline_init: RESET_N wiped parser/LBS/GLORT/L2F SRAM. */
		if (circ125) {
			/*
			 * boot_cold clears HPIE/DLLSCE/PDCE on the upstream port
			 * (fm_pcie_sltctl_clear_hpie) before its ESCHED writes.
			 * That clearing persists through pipeline_init: no pciehp
			 * events fire on GLORT or other SRAM write FALTALs.
			 * [install137, 2026-10-27]
			 *
			 * install174: clear RS780 error accumulator bits and ENABLE
			 * SERR# before pipeline_init and fm_l2f_init.
			 *
			 * install173 froze immediately after "pipeline_init done"
			 * during fm_l2f_init.  Root cause: L2F SRAM writes trigger
			 * a 1-FATAL ECC soft-reset that briefly bounces the PCIe
			 * link.  The AMD SB700 periodic BIOS SMI timer caught that
			 * bounce; with SERR# disabled the CTO on bus=2 had no error
			 * completion → CPU stalled permanently in SMM.
			 *
			 * Fix: wait for link stability first (link may still be
			 * settling after ring-reinit boot_cold), clear bridge status
			 * to flush any CTOs accumulated during the wait, then enable
			 * SERR# so any subsequent BIOS timer CTO during L2F's brief
			 * FATAL gets an error completion and the SMM handler exits.
			 *
			 * install179: SERR# is re-disabled right after L2F (below).
			 *
			 * install180: moved fm_wait_link_active to before SERR# enable
			 * so the BIOS timer cannot catch a still-settling link with
			 * SERR# already on; added a second bridge-status clear after
			 * the wait to flush CTOs accumulated during wait; removed
			 * fm_pcie_clear_root_aer_errors() which (a) requires SERR#
			 * disabled per its own precondition and (b) is a no-op on
			 * RS780 (no AER extended cap — chain ends VSEC+VC, confirmed
			 * install167) while doing a sysfs pread that acquires kernel
			 * pci_lock, making it vulnerable to BIOS SMI deadlock.
			 * [install174+install180, 2026-10-27]
			 */
			fm_wait_link_active(10000);
			fm_pcie_clear_bridge_status();
			kmsg("nosd-fm6000: ring-reinit: bridge-status cleared [install180]");
			fm_pcie_serr_reenable();
			kmsg("nosd-fm6000: ring-reinit: SERR# enabled pre-l2f [install180]");

			/*
			 * install196: boot.c now pre-fills ALL ECC-sensitive SRAM tables
			 * (parser, LBS_CAM, GLORT, L2F) inside fm_boot_cold() before
			 * fm_esched_set_cfg2().  The fills below (L2F_TABLE_4K/256) are
			 * kept as defence-in-depth for the pipeline_init stage.
			 *
			 * Root cause (install195 freeze): after RESET_N the SRAM ECC is
			 * uninitialised.  fm_esched_set_cfg2()'s CFG_1 writes cause the
			 * ring tokens to access ECC-bad cells → sustained self-resets →
			 * BIOS periodic-timer CF8 fires during a FATAL → BIOS SERR#
			 * cascade → RS780 stalls.  boot.c's pre-fill ensures CFG_1's FATAL
			 * is brief (the normal 1–2 resets, not hundreds), so the cascade
			 * window is too narrow to complete.  [install195/196, 2026-10-28]
			 */
			{
				int rv_fill4k, rv_fill256;
				char fmsg[128];
				/* L2F_TABLE_4K: 4096 entries × 4 words = 16384 words */
				rv_fill4k = fm_mem_fill(d, 0x180000u, 16384u, 0u);
				/* L2F_TABLE_256: ~1024 entries × 4 words = 4096 words */
				rv_fill256 = fm_mem_fill(d, 0x1a0000u, 4096u, 0u);
				snprintf(fmsg, sizeof fmsg,
					"nosd-fm6000: ring-reinit: L2F pre-fill rv4k=%d rv256=%d [install196]",
					rv_fill4k, rv_fill256);
				kmsg(fmsg);
			}
			{
				int rv_l2f = fm_l2f_init(d);
				char l2msg[80];
				snprintf(l2msg, sizeof l2msg,
					"nosd-fm6000: ring-reinit: L2F pre-init rv=%d [install194]",
					rv_l2f);
				kmsg(l2msg);
			}
			kmsg("nosd-fm6000: ring-reinit: re-running pipeline_init (RESET_N wiped SRAM)");
			pipeline_init(d);
			kmsg("nosd-fm6000: ring-reinit: pipeline_init done [install194]");
			/* L2F SRAM already has valid ECC; no second fm_l2f_init needed */
			kmsg("nosd-fm6000: ring-reinit: SERR# stays enabled post-pipeline [install194]");
		}
	}

	/*
	 * install192: removed fm_pcie_serr_disable() and 300ms nanosleep that
	 * install189/190 inserted here.
	 *
	 * install191 showed both were dangerous: the serr_disable CF8 call fired
	 * the SB700 I/O-port SMI trap; the BIOS entered SMM and read bus=2 at
	 * the exact moment a post-L2F ring FATAL bounced the FM6000 link → CTO
	 * → cascading BIOS SERR# handler SMI → RS780 stall.  Every CF8 call in
	 * the post-L2F window carries this ~30% freeze risk.
	 *
	 * Fix: skip all CF8 calls here.  The sysfs rescan below (install184)
	 * uses kernel MMCONFIG — no SB700 I/O-port SMI trap, no bus=2 CTO risk.
	 * SERR# remains at whatever boot_cold(circ=1) left it (0x0107 from
	 * boot.c's serr_reenable before ESCHED).  With SERR# enabled and the
	 * FM6000 UP after L2F, the periodic BIOS timer reads bus=2 → valid
	 * response → no deadlock.
	 * [install192, 2026-10-28]
	 */
	kmsg("nosd-fm6000: install192: post-L2F going direct to sysfs rescan (no CF8)");

	/*
	 * install140: fm_pcie_dev_memspc_enable() removed.
	 * On AMD RS780, a CF8/CFC write to bus=2 with SERR# disabled has no
	 * error completion on CTO -- the outw stalls the CPU indefinitely if
	 * the PCIe link is still settling after ring-reinit boot_cold.
	 * Memory Space + Bus Master are restored safely by
	 * fm_pcie_enable_portio() below: SERR# re-enabled first, LNKSTA
	 * polled to L0, then CF8/CFC with bounded-stall protection.
	 * [install139 freeze, RE 2026-10-27]
	 */

	/*
	 * install134: fm_boot_cold ran ESCHED set_cfg2 internally (boot.c
	 * line 863) as part of its post-ring-init sequence.  Do NOT call it
	 * again here: a redundant ESCHED on an already-circulating ring with
	 * Memory Space potentially off will re-trigger the CF8/CFC freeze.
	 * [install133 lesson]
	 */
	if (!circ125)
		kmsg("nosd-fm6000: ring-reinit: boot_cold failed (circ=0) -- no ESCHED");
	else
		kmsg("nosd-fm6000: ring-reinit: boot_cold ESCHED done (boot.c:863)");
	} /* end circ125 scope */

	/*
	 * install182: remove the second pre-portio bridge_clear (install169).
	 *
	 * install181 showed (markers A-G) that the freeze is inside the
	 * fm_pcie_clear_bridge_status() call at marker E — AFTER markers A-D
	 * and dllla-stable all succeeded.  The BIOS periodic SMI timer fires
	 * every ~3-4s via CF8/CFC to bus=2; with SERR# disabled a CTO on that
	 * bus=2 read (FM6000 link momentarily down) stalls the CPU permanently
	 * in SMM.  The timer appears to fire deterministically at the point
	 * where the timing of the pre-portio bridge_clear aligns with the
	 * ~3-4s interval.
	 *
	 * The pre-portio bridge_clear is redundant: fm_pcie_enable_portio()
	 * already contains its own bridge-status clear (install168, visible in
	 * the log as "portio: bridge status cleared") before the bus=2 write.
	 * That inner clear covers the same status bits and is the last clear
	 * before the write TLP is posted.  Removing this outer clear eliminates
	 * one CF8/CFC exposure window and shifts the timing so the BIOS SMI
	 * timer fires after portio rather than during it.
	 *
	 * install179's bridge_clear (marker A) right after L2F + SERR# disable
	 * is kept: it flushes the fresh CTO latch from ring-reinit activity
	 * and gives a clean slate for portio's own install168 inner clear.
	 * [install182, 2026-10-27]
	 */
	/*
	 * install184: replace CF8/CFC portio with sysfs PCIe rescan +
	 * sysfs command-register write (fm_pcie_enable_command).
	 *
	 * Root cause of portio freezes (install181-183): fm_pcie_enable_portio
	 * uses iopl(3) + outl to 0xCF8 (x86 CF8/CFC legacy PCI config access).
	 * Every outl to 0xCF8 fires an AMD SB700 I/O-port SMI trap.  The BIOS
	 * SMM handler reads bus=2 (FM6000) via CF8/CFC.  The ring-reinit link
	 * bounce (second fm_boot_cold PCIe SerDes re-init) leaves a CTO latch
	 * in the RS780.  Any BIOS SMM bus=2 CF8/CFC read with the latch active
	 * stalls the CPU permanently in SMM (NMI deferred; no exit).
	 *
	 * Fix: the kernel uses MMCONFIG (ECAM) for PCIe config space access —
	 * no 0xCF8 I/O port write, no SB700 I/O-port SMI trap.  fm_boot_cold
	 * already uses this path (rescan sysfs + fm_pcie_enable_command sysfs
	 * pwrite) and it works reliably.  [install184, 2026-10-28]
	 */
	/*
	 * install200: gate the rescan on FATAL_COUNT stability.
	 *
	 * install199 root cause: the rescan write() stalled in SMM.  The ring
	 * still has FATALs after ESCHED completes; each FATAL briefly bounces
	 * the PCIe link.  The BIOS periodic-timer CF8/CFC read to bus=2 (every
	 * ~3.5 s) fires during a bounce → completion timeout → CPU stuck in SMM
	 * permanently.  The pci_scan_bus() call made by the rescan write() runs
	 * into this stall and never returns.  A fixed 2 s nap is not enough:
	 * ring-reinit FATALs can continue for > 2 s after ESCHED.
	 *
	 * Fix: replace the fixed nap with the same FATAL_COUNT stability poll
	 * used in boot.c:851–874 (pre-DMA stability wait).  Read fc every 500 ms;
	 * when it is unchanged for 20 consecutive polls (10 s of silence) the ring
	 * is quiet, all link bounces have died, and a BIOS SMI fired during the
	 * rescan will see a stable FM6000 on bus=2 → SMM exits normally.
	 *
	 * d->pcie_regs is valid here: fm_boot_cold's internal rescan (boot.c:763)
	 * called fm_attach_pcie(d) which maps BAR0; the subsequent device removal
	 * (boot.c:820) removes the kernel bus entry but leaves the BAR0 mmap intact.
	 * [install200, 2026-10-29]
	 */
	kmsg("nosd-fm6000: install200: clearing SLTCTL [install200]");
	fm_pcie_sltctl_clear_hpie();
	{
		int wla200 = fm_wait_link_active(10000);
		char wla_buf[96];
		snprintf(wla_buf, sizeof(wla_buf),
			 "nosd-fm6000: install200: link active rv=%d [install200]",
			 wla200);
		kmsg(wla_buf);
	}
	kmsg("nosd-fm6000: install201: fc stability poll start [install201]");
	{
		uint32_t fc200 = 0, fc200b = 0;
		int n_stable200 = 0;
		char fc_msg[128];
		fm_rd(d, 0x8, &fc200);
		snprintf(fc_msg, sizeof(fc_msg),
			 "nosd-fm6000: install201: initial fc=%u [install201]", fc200);
		kmsg(fc_msg);
		for (int i200 = 0; i200 < 120 && n_stable200 < 20; i200++) {
			struct timespec ts200 = { 0, 500000000L };
			nanosleep(&ts200, NULL);
			fm_rd(d, 0x8, &fc200b);
			if (fc200b == fc200) {
				n_stable200++;
			} else {
				fc200 = fc200b;
				n_stable200 = 0;
			}
		}
		snprintf(fc_msg, sizeof(fc_msg),
			 "nosd-fm6000: install201: fc=%u n_stable=%d [install201]",
			 fc200, n_stable200);
		kmsg(fc_msg);
	}
	/*
	 * install202: skip the sysfs rescan entirely.
	 *
	 * install200/install201 analysis (2026-10-29):
	 *
	 *   The sysfs write() to /sys/class/pci_bus/0000:02/rescan deadlocks the
	 *   CPU in BIOS SMM every time.  The root cause trace from pci.c and
	 *   fm6000_kdrv.c:
	 *
	 *     pci_scan_bus() reads FM6000 config space via MMCONFIG.  On AMD
	 *     RS780, an MMCONFIG read to bus=2 that coincides with a momentary
	 *     link glitch causes a Completion Timeout (CTO).  With SERR# enabled
	 *     the RS780 signals the BIOS SERR# SMI handler.  The SMM handler then
	 *     re-reads bus=2 via CF8/CFC to diagnose the fault.  CF8/CFC reads to
	 *     bus=2 stall the CPU indefinitely if the link is mid-training.  The
	 *     BIOS enters an infinite "retry the CF8 read" loop inside SMM, NMI is
	 *     deferred, and the CPU never returns to OS.  (Empirical: install113–
	 *     install200, all 8 attempts, same freeze at the rescan write.)
	 *
	 *   Memory Space + Bus Master are already ENABLED on the FM6000 hardware.
	 *   boot.c:784 called fm_pcie_enable_command() after Phase 2 boot_cold's
	 *   internal rescan, writing 0x0406 (MemSp + BusMaster + IntDis) to the
	 *   FM6000 Command register.  The subsequent device removal at boot.c:820
	 *   does NOT change the hardware Command register; it only removes the
	 *   kernel's software representation.  d->pcie_regs (mapped via /dev/mem)
	 *   remains valid for the same reason.
	 *
	 *   fm6000_kdrv probe() only silences pciehp, AER, and PME on the parent
	 *   root port; Phase 2's probe() (from the boot.c:763 internal rescan)
	 *   already did that, and remove() is a no-op, so those guards persist.
	 *   The kernel DMA API is not used; fm_dma_init() pins pages via mlock +
	 *   /proc/self/pagemap and reads d->pcie_regs directly.
	 *
	 *   Conclusion: the rescan serves no purpose that boot.c:784 has not
	 *   already fulfilled.  Skipping it avoids the SMM freeze entirely.
	 *
	 * fm_pcie_enable_portio() below confirms DLLLA=1 (link truly active,
	 * belt-and-suspenders after the fc poll above), clears residual bridge
	 * error bits, and re-writes 0x0406 to the FM6000 Command register via a
	 * posted CF8 write (posted = completes immediately from CPU's perspective,
	 * no CTO stall risk — install172 confirmed this).  No sysfs access, no
	 * kernel rescan, no SMM hazard.  [install202, 2026-10-29]
	 */
	/*
	 * install206: skip fm_pcie_enable_portio here.
	 *
	 * install202 called fm_pcie_enable_portio to re-confirm DLLLA and
	 * re-write Command=0x0406.  This CF8 portio write fires the AMD SB700
	 * I/O-port SMI trap; the BIOS SMM handler re-reads bus=2 via CF8/CFC.
	 * When the 2nd boot_cold (ring-reinit) used install205's portio path
	 * (no sysfs rescan), the FM6000 PCIe link had a residual CTO latch from
	 * the RESET_N pulse; the BIOS CF8 read stalls the CPU permanently in SMM.
	 *
	 * The portio call is redundant:
	 *   • DLLLA=1 was already confirmed by install200's fm_wait_link_active.
	 *   • Command=0x0406 (Memory Space + Bus Master + IntDis) was written by
	 *     fm_pcie_enable_portio() inside boot.c's install205 portio path AND
	 *     by fm_attach_pcie()'s cached path CF8 write (bus=2, reg=0x04).
	 *   • Bridge status was cleared by install180's fm_pcie_clear_bridge_status.
	 *
	 * Removing this CF8 call eliminates the SMM freeze.
	 * [install206, 2026-10-29]
	 */
	kmsg("nosd-fm6000: install206: portio skipped — Command+link confirmed by install200/205 [install206]");
	fm_pcie_sltctl_clear_hpie();
	{
		int rv184 = fm_pcie_enable_command(d);
		char buf184[96];
		snprintf(buf184, sizeof(buf184),
			 "nosd-fm6000: install184: enable_command rv=%d [install184]",
			 rv184);
		kmsg(buf184);
	}
	kmsg("nosd-fm6000: install184: MemSp+BM restored via sysfs [install184]");

	/*
	 * install207: disable SERR# and Root AER Command before DMA init.
	 *
	 * DMA init programs FM6000 DMA-engine registers via d->pcie_regs.
	 * If any write triggers a FATAL (PCIe link bounce), two independent
	 * freeze paths open:
	 *
	 *   A. SERR# enabled: FATAL → SERR# → BIOS SMI → SMM reads CF8 to
	 *      bus=2; if the link is still mid-bounce the CF8 read stalls the
	 *      CPU in SMM permanently.
	 *
	 *   B. Root AER Command non-zero: FATAL → AER MSI → kernel aer_isr()
	 *      reads Root Error Source ID, then calls pci_read_config_dword on
	 *      bus=2; on AMD RS780 a config-space read to a bouncing endpoint
	 *      blocks the CPU permanently (no CTO).
	 *
	 * fm_pcie_disable_root_aer_cmd() was called before ESCHED writes in the
	 * ring-reinit path (inside boot.c) and restored afterwards.  The same
	 * disable was supposed to bracket DMA init here but the call was absent.
	 * [install207, 2026-10-29]
	 *
	 * install210: reorder to clear CTO latch and disable SMI BEFORE any CF8
	 * write (fm_pcie_serr_disable / fm_pcie_disable_root_aer_cmd both use
	 * CF8/CFC which fire the AMD SB700 I/O-port SMI trap).
	 *
	 * Root cause of install207–209 stall: fm_pcie_serr_disable() was called
	 * first; its outl(0xCF8) fired the SB700 I/O-port SMI trap; the BIOS
	 * SMM handler read bus=2 (FM6000) via CF8/CFC; a residual CTO latch
	 * from pipeline_init / bring_up_ports link activity stalled the CPU
	 * permanently in SMM (NMI deferred, no exit).
	 *
	 * Fix sequence:
	 *   1. fm_pcie_clear_bridge_status_sysfs() — MMCONFIG only, no CF8,
	 *      no SMI trap, flushes any CTO latch accumulated since install180.
	 *   2. fm_pcie_smi_global_disable() — CF8 to find PMIO base, fires SMI
	 *      once, but link is now stable and CTO latch is clear so BIOS SMM
	 *      reads bus=2 successfully and exits; GBL_SMI_EN then set to 0.
	 *   3. fm_pcie_serr_disable() — CF8 writes now fire I/O-port SMI trap
	 *      but GBL_SMI_EN=0 so no SMM is generated; safe.
	 *   4. fm_pcie_disable_root_aer_cmd() — likewise safe.
	 * [install210, 2026-11-06]
	 */
	fm_pcie_clear_bridge_status_sysfs();
	kmsg("nosd-fm6000: install210: bridge-status cleared (sysfs, no CF8) [install210]");
	if (fm_pcie_smi_global_disable())
		kmsg("nosd-fm6000: install211: SMI disabled; CF8 writes now safe [install211]");
	else
		kmsg("nosd-fm6000: install211: WARNING: SMI NOT disabled (PMIO not found) [install211]");

	/*
	 * install246: third fm_pcie_serdes_init() to get a DL_Init that runs
	 * with PCI_DMA_CFG=0x35 (DMAEn=1 → nonzero PD flow-control credits).
	 *
	 * Root cause of install241 BAR0 write stall (fully confirmed by install245):
	 *
	 * EVERY DL_Init triggered in this path is preceded by a FATAL that
	 * resets FM6000_PCI_DMA_CFG=0:
	 *
	 *   1. ring_reinit's fm_boot_cold: ~31 FAT ALs (freelists) reset all FM6000
	 *      registers.  fm_pcie_serdes_init (#2) writes PCI_DMA_CFG=0x35 then
	 *      PCI_CORE_CTRL_1 → DL_Init with DMAEn=1.  BUT the subsequent port /
	 *      pipeline init causes 1 more FATAL (confirmed: install201 fc=1) which
	 *      resets PCI_DMA_CFG=0.  The chip self-trains from Detect with
	 *      PCI_DMA_CFG=0 → PD credits=0.
	 *
	 *   2. install242 LNKCTL retrain: Recovery triggered another FATAL on
	 *      every attempt (confirmed: fc wraps on each retrain).  install245
	 *      re-wrote PCI_DMA_CFG=0x35 before the retrain, but the retrain's
	 *      FATAL zeroed it again before DL_Init ran.  Writing PCI_DMA_CFG=0x35
	 *      after a FATAL is always too late — PD credits are set at DL_Init
	 *      time and don't change until the next DL_Init.
	 *
	 * Fix: use fm_pcie_serdes_init() (SerDes re-init #3) after all FAT ALs
	 * are done.  Its sequence:
	 *   SOFT_RESET_PCIE cycle  — may cause a synchronous FATAL (clears fast)
	 *   SBus SerDes config writes
	 *   PCI_DMA_CFG = 0x35      — written AFTER any FATAL from SerDes init
	 *   PCI_CORE_CTRL_1 = 0x1c01f → DL_Init with DMAEn=1 → nonzero PD credits
	 *
	 * No subsequent FATAL follows (port/pipeline already done), so PD credits
	 * stay nonzero and BAR0 Memory Writes are accepted by the RS780.
	 * This matches the pcie_init_main.c (tool) flow that works.
	 *
	 * PCI_CORE_CTRL_1 disrupts the SCD slot; LBUS must be released immediately
	 * and reopened with the same recovery used after the second SerDes init
	 * in ring_reinit (munmap → wait 5s → fm_wait_link_active → fm_reopen_lbus).
	 * CF8 Config Writes to bus=2 deadlock after the sysfs rescan (install192),
	 * so fm_pcie_enable_portio(2,...) is intentionally skipped here; Command
	 * and BAR0 written earlier by boot.c:784 persist through SOFT_RESET_PCIE.
	 * [install246, 2026-11-16]
	 */
	{
		char               slot246[FM_SLOT_LEN];
		volatile uint32_t *saved_pcie_regs   = d->pcie_regs;
		size_t             saved_pcie_bytes  = d->pcie_bar_bytes;
		unsigned long      saved_pcie_phys   = d->pcie_bar_phys;
		int                saved_pcie_bar_fd = d->pcie_bar_fd;
		int                saved_banks_ready = d->banks_ready;
		int                saved_boot_done   = d->boot_done;
		int                saved_sched_ready = d->sched_ready;
		int                rv246;
		char               b246[128];

		snprintf(slot246, sizeof(slot246), "%s", d->slot);

		rv246 = fm_pcie_serdes_init(d);
		snprintf(b246, sizeof(b246),
			 "nosd-fm6000: install246: SerDes re-init #3 rv=%d [install246]",
			 rv246);
		kmsg(b246);

		/* PCI_CORE_CTRL_1 disrupted the SCD slot; release LBUS immediately. */
		munmap((void *)d->regs, d->bar_bytes);
		close(d->bar_fd);
		d->regs   = NULL;
		d->bar_fd = -1;

		kmsg("nosd-fm6000: install246: waiting 5s for SCD slot to stabilize [install246]");
		{
			struct timespec nap5 = { 5, 0 };
			nanosleep(&nap5, NULL);
		}

		(void)fm_upstream_link_disable(0);

		{
			int wla246 = fm_wait_link_active(15000);
			char b246w[128];
			snprintf(b246w, sizeof(b246w),
				 "nosd-fm6000: install246: DLLLA rv=%d [install246]",
				 wla246);
			kmsg(b246w);
		}

		/* fm_reopen_lbus_devmem clears d with memset; restore PCIe state after. */
		if (fm_reopen_lbus_devmem(d, slot246) != FM_OK) {
			kmsg("nosd-fm6000: install246: FATAL: cannot reopen LBUS after SerDes #3 [install246]");
			return -1;
		}
		d->pcie_regs      = saved_pcie_regs;
		d->pcie_bar_bytes = saved_pcie_bytes;
		d->pcie_bar_phys  = saved_pcie_phys;
		d->pcie_bar_fd    = saved_pcie_bar_fd;
		d->banks_ready    = saved_banks_ready;
		d->boot_done      = saved_boot_done;
		d->sched_ready    = saved_sched_ready;
		kmsg("nosd-fm6000: install246: LBUS reopened after SerDes #3 [install246]");

		/* FATAL_COUNT stability: up to 20 × 500ms, require 5 consecutive stable. */
		{
			uint32_t fc246a = 0, fc246b = 0;
			int n246 = 0, i246;
			char fb246[128];

			fm_rd(d, 0x8, &fc246a);
			for (i246 = 0; i246 < 20 && n246 < 5; i246++) {
				struct timespec ts = { 0, 500000000L };
				nanosleep(&ts, NULL);
				fm_rd(d, 0x8, &fc246b);
				if (fc246b == fc246a)
					n246++;
				else {
					fc246a = fc246b;
					n246 = 0;
				}
			}
			snprintf(fb246, sizeof(fb246),
				 "nosd-fm6000: install246: post-SerDes-3 fc=0x%08x n_stable=%d [install246]",
				 fc246a, n246);
			kmsg(fb246);
		}
		/* Confirm PCI_DMA_CFG=0x35 from fm_pcie_serdes_init; re-write if another FATAL reset it. */
		{
			uint32_t pdc246 = 0xdeadbeef;
			char b246d[192];

			fm_rd(d, FM6000_PCI_DMA_CFG, &pdc246);
			snprintf(b246d, sizeof(b246d),
				 "nosd-fm6000: install246: PCI_DMA_CFG=0x%05x [install246]",
				 pdc246);
			kmsg(b246d);
			if (pdc246 != 0x35u) {
				kmsg("nosd-fm6000: install246: WARNING PCI_DMA_CFG not 0x35"
				     " — another FATAL reset it; re-writing [install246]");
				fm_wr(d, FM6000_PCI_DMA_CFG, 0x35u);
			}
		}

		/*
		 * install247: sysfs rescan bus=2 + BAR0 remap after SerDes re-init #3.
		 *
		 * Root cause of bar_check: cfg=0xffffffff (confirmed install246):
		 *   fm_pcie_serdes_init's SOFT_RESET_PCIE clears the FM6000's PCIe BAR0
		 *   config register (0xe2000000 → 0x0).  pciehp also removes 0000:02:00.0
		 *   from sysfs when the link bounces.  Without a rescan:
		 *     a. The FM6000's BAR0 decoder no longer claims 0xe2000000 → all
		 *        BAR0 Memory Write TLPs are silently discarded.  DMA_CFG, CMD_TX,
		 *        CMD_RX never reach the DMA engine.
		 *     b. BAR0 Memory Read TLPs return CTO (0xffffffff) — the FM6000 has
		 *        no address decoder active for BAR0.
		 *     c. The DMA engine never starts; rx_frames=0 forever.
		 *
		 * Fix: rescan bus=2 (kernel re-enumerates FM6000, restores BAR0 and
		 * Command config registers via ECAM), then remap d->pcie_regs.
		 * Safe here: SMI is globally disabled (install211), SERR# is still
		 * enabled (install209 has not run), FM6000 is stable (install201 fc poll).
		 * [install247, 2026-11-16]
		 */
		{
			char b247[192];
			int  rfd247;

			rfd247 = open("/sys/class/pci_bus/0000:02/rescan", O_WRONLY);
			if (rfd247 >= 0) {
				(void)write(rfd247, "1\n", 2);
				close(rfd247);
				kmsg("nosd-fm6000: install247: bus=2 rescan done [install247]");
			} else {
				kmsg("nosd-fm6000: install247: WARNING bus=2 rescan open failed [install247]");
			}
			nap_ms(200); /* let fm6000_kdrv probe() settle */

			/* Remap BAR0: SOFT_RESET_PCIE clears the FM6000 BAR0 config register. */
			fm_detach_pcie(d);
			if (fm_attach_pcie(d) == FM_OK) {
				snprintf(b247, sizeof(b247),
					 "nosd-fm6000: install247: BAR0 remapped"
					 " regs=%p phys=0x%016lx [install247]",
					 (void *)d->pcie_regs, d->pcie_bar_phys);
				kmsg(b247);
			} else {
				kmsg("nosd-fm6000: install247: WARNING BAR0 remap failed; using saved mapping [install247]");
				/* Restore stale mapping as fallback; install221 will catch NULL. */
				d->pcie_regs      = saved_pcie_regs;
				d->pcie_bar_bytes = saved_pcie_bytes;
				d->pcie_bar_phys  = saved_pcie_phys;
				d->pcie_bar_fd    = saved_pcie_bar_fd;
			}
		}
	}

	/*
	 * install254: SERR# disabled BEFORE tx_reload.
	 *
	 * install253 diagnosis: tx_reload(SKIP_PLL) runs at ~90s with SERR#
	 * still enabled.  If the ring is STUCK, esched_set_cfg2(IC=0) fires a
	 * chip FATAL, which asserts SERR# → Linux AER handler → CF8 access to
	 * bus=2 stalls waiting for BIOS SMM → kernel deadlock → SSH unreachable
	 * from ~90s to reboot at ~374s.  The deadlock prevents reading dmesg.
	 *
	 * Fix: disable SERR# and AER before tx_reload so any FATAL during the
	 * SSCHED/ESCHED restore is silent.  The original install209 comment
	 * ("before DMA init") still holds — SERR# is never re-enabled after
	 * this point.  Moving the disable earlier is safe and allows the kernel
	 * to stay responsive so the tx_reload result can be read via SSH.
	 * [install254, 2026-11-18]
	 */
	fm_pcie_serr_disable();
	fm_pcie_disable_root_aer_cmd();
	kmsg("nosd-fm6000: install254: SERR#+AER-cmd disabled before tx_reload [install254]");

	/*
	 * install252: SSCHED + ESCHED restore after SerDes #3 (install246).
	 *
	 * install246's fm_pcie_serdes_init() uses SOFT_RESET_PCIE to cycle the
	 * PCIe SerDes.  SOFT_RESET_PCIE is NOT a chip HARD FATAL:
	 *   - FATAL_COUNT (reg 0x8) stays at 1 before and after install246
	 *     (confirmed in install246's post-SerDes-3 fc poll, 2026-11-18).
	 *   - SSCHED SRAM (NEXT_PORT, freelist) is intact from boot_cold's
	 *     ring_init and fm_boot_freelists — a PCIe-subsystem reset does not
	 *     wipe chip SRAM arrays.
	 *   - Chip registers ARE zeroed (SOFT_RESET_PCIE is a register reset):
	 *     PLL_CTRL_0=0, JSS_CFG_8=0, IC=0.  PLL hardware continues running.
	 *
	 * Earlier approaches failed:
	 *
	 *   install249/250 (tx_reload): _need_reinit=1 (PLL_CTRL_0=0) → writes
	 *     PLL_CTRL_0=GOLDEN → PLL re-lock triggers a true HARD FATAL
	 *     (FATAL_COUNT++, SSCHED SRAM wiped, NEXT_PORT=0) → STUCK.
	 *
	 *   install251 (ring_init): probe writes PLL_CTRL_0=GOLDEN → same HARD
	 *     FATAL cascade; without fm_bist_memory_init+freelists first the
	 *     settle phase hit cascading ECC FATALs (code 0xd0) for 1457s and
	 *     never converged.
	 *
	 *   install252 (tx_reload, SKIP_PLL, SBus inside block): SBus 0x45
	 *     JSS clock enable was inside the SKIP_PLL block → JSS synthesiser
	 *     dead → TX_FOUND=0 → STUCK → esched_set_cfg2(IC=0) FATAL → panic.
	 *
	 *   install253 (SBus moved outside SKIP_PLL): TX probe showed STUCK; the
	 *     esched_set_cfg2 FATAL (expected with TX stuck) briefly bounced FM6000
	 *     off-bus.  With SERR# disabled (install254), any BAR0 read that races
	 *     the bounce stalls the CPU permanently (no CTO error completion on
	 *     AMD RS780 when SERR# off).  fm_ffu_init, the first BAR0 access after
	 *     the esched block, could race that ~100-200ms recovery window.
	 *
	 *   install255 (Memory Space disable before IC=1): disabling PCI_COMMAND
	 *     Memory Space bit makes FM6000 return Unsupported Request for ALL
	 *     Memory Space TLPs — including BAR1 (LBUS).  Every NEXT_PORT / token /
	 *     IC=1 write in tx_reload was silently dropped; TX always read as 0x0
	 *     ("STUCK") because the ring was never set up.  Config Space (CF8/CFC)
	 *     is unaffected by the Memory Space bit, so the BIOS SMI during the
	 *     first cold boot could always read bus=2 and not stall — explaining
	 *     why the first boot survived 50 h while the second froze after
	 *     Memory Space was re-enabled and esched's FATAL raced an fm_ffu_init
	 *     BAR0 read.
	 *
	 * Fix (install256): leave Memory Space enabled throughout (LBUS writes
	 * work normally).  After esched_set_cfg2, call fm_wait_link_active(2000)
	 * to block until FM6000's PCIe link is active again after any FATAL the
	 * esched writes may have triggered.  This guarantees fm_ffu_init and
	 * fm_dma_init never race the bounce window.
	 *
	 *   install256 (cold boot): survived esched_set_cfg2 but trial-boot
	 *     watchdog fired at 412s — DMA init never confirmed.  Root cause:
	 *     esched_set_cfg2's FATAL_COUNT stability poll (esched.c:252) used
	 *     fm_rd (BAR0); with SERR# disabled, the first BAR0 read during the
	 *     CFG_1 FATAL stalled the CPU permanently (no error completion on
	 *     AMD RS780 when SERR# off).  install256 cold boot survived by luck
	 *     — the CFG_1 FATAL happened to clear before the fm_rd — but the
	 *     warm-reboot path freezes consistently because FM6000 is in a
	 *     more complex state (IC=1 from prior session).
	 *
	 * Fix (install257): change esched_set_cfg2's FATAL_COUNT poll to use
	 * fm_rd_hazardous (LBUS/BAR1) which always gets an error completion
	 * (returns 0) when FM6000 is transiently off-bus, never stalling.
	 * [install257, 2026-11-20]
	 */
	{
		int rv252, rv_cfg2;
		char b252[128];

		rv252 = fm_ssched_tx_reload(d, FM_SSCHED_TX_RELOAD_SKIP_PLL);
		snprintf(b252, sizeof b252,
			"nosd-fm6000: install256: tx_reload(SKIP_PLL) rv=%d [install256]",
			rv252);
		kmsg(b252);

		/*
		 * install258: pass FM_ESCHED_CFG2_SKIP_FP_PORTS.
		 *
		 * After tx_reload(SKIP_PLL) the SSCHED ring is STUCK (TX_RT=0).
		 * Writing CFG_2 for front-panel ports (port 20+) with ring not
		 * circulating triggers a chip FATAL → PCIe link bounce → kernel
		 * IRQ handler grabs logbuf_lock + stalls on ECAM read with SERR#
		 * disabled → esched_dbg("done") write() syscall never returns.
		 * Skip front-panel CFG_2 here; it will be written in a later
		 * install once ring circulation is confirmed.  CPU-port CFG_2
		 * (port 0) is still written — needed for DMA rx_frames.
		 * [install258, 2026-11-20]
		 */
		rv_cfg2 = fm_esched_set_cfg2(d, FM_ESCHED_CFG2_SKIP_FP_PORTS);
		snprintf(b252, sizeof b252,
			"nosd-fm6000: install258: esched_set_cfg2(SKIP_FP) rv=%d [install258]",
			rv_cfg2);
		kmsg(b252);

		(void)fm_wait_link_active(2000);
		kmsg("nosd-fm6000: install256: link active (or timeout) after esched [install256]");
	}

	/*
	 * install259: link-disable pulse + sysfs rescan to fix PD-credit=0 and
	 * IntDis=0 caused by the ESCHED CFG_1 HARD FATAL in esched_set_cfg2.
	 *
	 * Root cause: the CFG_1 writes in fm_esched_set_cfg2 trigger a HARD FATAL
	 * (confirmed by the FATAL_COUNT stability poll in esched.c:252).  The FATAL
	 * resets the FM6000's PCIe endpoint:
	 *   - PCI_DMA_CFG=0  → DL_Init advertises 0 PD credits → RS780 cannot
	 *     forward BAR0 Memory Write TLPs → DMA rings never programmed
	 *   - Command=0  → IntDis=0 → FM6000 may assert INTA# → kernel IRQ fires
	 *     during the 100ms usleep in program_rings → kernel IRQ handler grabs
	 *     logbuf_lock + stalls on ECAM read with SERR# disabled (no CTO error
	 *     completion on AMD RS780) → system-wide freeze at DMA init [117.4s]
	 *   - BAR0 config register=0  → FM6000 no longer claims 0xe2000000
	 *
	 * Fix:
	 *   1. Write PCI_DMA_CFG=0x35 via LBUS (link-disable does not reset it).
	 *   2. Link-disable pulse: Root Port disables link then re-enables.
	 *      DL_Init fires with PCI_DMA_CFG=0x35 → nonzero PD credits.
	 *   3. Sysfs rescan bus=2: kernel re-enumerates FM6000 and writes
	 *      BAR0=0xe2000000 + Command=0x0406 (IntDis=1, MemSp=1) via ECAM.
	 *      SERR# disabled (install254); FM6000 is responding → no ECAM stall.
	 *   4. fm_attach_pcie: remap d->pcie_regs to restored BAR0.
	 * [install259, 2026-11-20]
	 */
	{
		int rv259_link;
		char b259[128];
		int rfd259;

		/* Step 1: write PCI_DMA_CFG=0x35 via LBUS before link re-enables.
		 * DL_Init picks up this value at link-up time; link-disable does not
		 * reset internal FM6000 registers (only a HARD FATAL does). */
		fm_wr(d, FM6000_PCI_DMA_CFG, 0x35u);
		kmsg("nosd-fm6000: install259: PCI_DMA_CFG=0x35 written via LBUS [install259]");

		/* Step 2: link-disable pulse → new DL_Init with PCI_DMA_CFG=0x35.
		 * Uses CF8 portio on bus=0 (Root Port LNKCTL); never stalls. */
		(void)fm_upstream_link_disable(1);
		kmsg("nosd-fm6000: install259: link_disable(1) [install259]");
		usleep(150000);  /* 150ms hold */
		(void)fm_upstream_link_disable(0);
		kmsg("nosd-fm6000: install259: link_disable(0) released [install259]");

		rv259_link = fm_wait_link_active(10000);
		snprintf(b259, sizeof b259,
			 "nosd-fm6000: install259: DLLLA rv=%d [install259]",
			 rv259_link);
		kmsg(b259);

		/* Step 3: sysfs rescan restores BAR0=0xe2000000 + Command=0x0406
		 * (IntDis=1, MemSp=1, BusMaster=1).  SERR# disabled (install254);
		 * FM6000 is responding → Config TLPs complete normally → no stall. */
		rfd259 = open("/sys/class/pci_bus/0000:02/rescan", O_WRONLY);
		if (rfd259 >= 0) {
			(void)write(rfd259, "1\n", 2);
			close(rfd259);
			kmsg("nosd-fm6000: install259: bus=2 rescan done [install259]");
		} else {
			kmsg("nosd-fm6000: install259: WARNING bus=2 rescan open failed [install259]");
		}
		nap_ms(300); /* let fm6000_kdrv probe() settle */

		/* Step 4: remap d->pcie_regs to restored BAR0. */
		fm_detach_pcie(d);
		if (fm_attach_pcie(d) == FM_OK) {
			char b259r[128];
			snprintf(b259r, sizeof b259r,
				 "nosd-fm6000: install259: BAR0 remapped"
				 " regs=%p phys=0x%016lx [install259]",
				 (void *)d->pcie_regs, d->pcie_bar_phys);
			kmsg(b259r);
		} else {
			kmsg("nosd-fm6000: install259: WARNING BAR0 remap failed [install259]");
		}
	}

	/*
	 * install221: d->pcie_regs validity check before DMA init.
	 *
	 * install259 (above) restored BAR0=0xe2000000 + Command=0x0406 via a
	 * link-disable pulse + sysfs rescan, so pcie_regs is valid here.
	 * The original install221 comment noted "nothing between install184 and
	 * install209 touches it", but the ESCHED CFG_1 HARD FATAL (install258 path)
	 * clears Command and BAR0 AFTER install254 (SERR# disable).  install259
	 * restores both via ECAM (safe with SERR# disabled when FM6000 is up).
	 * [install221, 2026-11-09; updated install259, 2026-11-20]
	 */
	{
		char buf221[256];
		snprintf(buf221, sizeof buf221,
			 "nosd-fm6000: install221: pre-DMA pcie_regs=%p"
			 " bar_phys=0x%016lx (BAR0+Cmd set by install184/192) [install221]",
			 (void *)d->pcie_regs, d->pcie_bar_phys);
		kmsg(buf221);

		if (d->pcie_regs == NULL) {
			int rv_a = fm_attach_pcie(d);
			char buf_a[128];
			snprintf(buf_a, sizeof buf_a,
				 "nosd-fm6000: install221: NULL-regs fallback"
				 " attach rv=%d [install221]",
				 rv_a);
			kmsg(buf_a);
			if (d->pcie_regs == NULL) {
				kmsg("nosd-fm6000: install221: FATAL pcie_regs NULL");
				return -1;
			}
		}
	}

	/* Re-run FFU just before DMA init.  RESET_N (triggered by any chip
	 * self-reset during the FATAL_COUNT stability wait above) wipes FFU SRAM.
	 * pipeline_init() runs earlier (before the stability wait) and is itself
	 * subject to those self-resets.  Running FFU here, after the chip has
	 * been declared stable, ensures the forwarding rules survive into the
	 * DMA-active window without being wiped by a subsequent self-reset. */
	{
		int rv_ffu = fm_ffu_init(d);
		if (rv_ffu != FM_OK) {
			char buf_ffu[128];
			snprintf(buf_ffu, sizeof buf_ffu,
				 "nosd-fm6000: pre-DMA FFU re-init failed rv=%d", rv_ffu);
			kmsg(buf_ffu);
		}
	}
	kmsg("nosd-fm6000: DMA init start");
	if (fm_dma_init(&g_fwd.dma, d) < 0)
		return -1;
	kmsg("nosd-fm6000: DMA init done");
	fm_pcie_smi_global_restore();
	/* install166: restore Root Error Command now that DMA is up and the
	 * FM6000 link is stable (no more training bounces expected). */
	fm_pcie_restore_root_aer_cmd();
	kmsg("nosd-fm6000: DMA: aer-cmd restored [install166]");
	/* install265: do NOT re-enable SERR# after DMA init.
	 *
	 * install262/263: re-enabling SERR# (fm_pcie_serr_reenable, CF8/CFC
	 * write) triggered NMI → BIOS SMM → platform reset → FM6000 PCIe
	 * permanently lost (BAR0 dead at [162s], link never recovered).
	 * The FATAL always happened ~200ms after the SERR# CF8 write.
	 *
	 * install264: added fm_pcie_clear_root_aer_errors() before the SERR#
	 * re-enable, but the CF8 write in fm_pcie_serr_reenable() then blocked
	 * in BIOS SMM forever (no message after AER clear, no forwarding thread).
	 * Conclusion: any CF8-based SERR# enable at this point is unsafe.
	 *
	 * SERR# is not required for forwarding: FATALs are detected via
	 * FATAL_COUNT polling (fc_read_safe / fm_rd_hazardous).  Leave SERR#
	 * in the state install254 set it to (disabled) for the life of the
	 * forwarding session. */
	kmsg("nosd-fm6000: DMA: SERR# re-enable SKIPPED (install265 — CF8 unsafe here)");

	/*
	 * et1 = front-panel port 1, alta 40.
	 *   glort=1:       ingress SGLORT assigned by parser (fm6000_glort_of(40)=1)
	 *   tx_dglort=1:   GLORT_TABLE[1]=0x0036ffc9 → et1; direct SRAM lookup,
	 *                  bypasses GLORT_CAM.  Previous value 0x03ef required the
	 *                  GLORT_CAM, which maps via a content-addressed search that
	 *                  produced no confirmed egress frames (install64).
	 */
	g_fwd.ports[0].name     = "et1";
	g_fwd.ports[0].glort    = 1;
	g_fwd.ports[0].tx_dglort = 1;
	g_fwd.ports[0].tap_fd   = fm_tap_create("et1");

	/*
	 * et2 = front-panel port 2, alta 20.
	 *   glort=2:       ingress SGLORT (fm6000_glort_of(20)=2)
	 *   tx_dglort=3:   GLORT_TABLE[3]=0x0035ffca → et2.  EOS places et2 at
	 *                  GLORT_TABLE index 3, not 2 (index 2 is absent from the
	 *                  captured snapshot).
	 */
	g_fwd.ports[1].name     = "et2";
	g_fwd.ports[1].glort    = 2;
	g_fwd.ports[1].tx_dglort = 3;
	g_fwd.ports[1].tap_fd   = fm_tap_create("et2");

	for (i = 0; i < FWD_NPORTS; i++) {
		if (g_fwd.ports[i].tap_fd < 0)
			fprintf(stderr,
				"nosd-fm6000: WARNING: TAP %s failed to create\n",
				g_fwd.ports[i].name);
	}

	if (pthread_create(&tid, NULL, fwd_thread, &g_fwd) != 0) {
		fprintf(stderr, "nosd-fm6000: could not start forwarding thread\n");
		return -1;
	}
	pthread_detach(tid);
	fprintf(stderr, "nosd-fm6000: packet forwarding active (et1/et2)\n");
	kmsg("nosd-fm6000: forwarding thread started (et1/et2)");
	return 0;
}

static void banner(const struct fm6000 *d)
{
	if (d->xport == FM_XPORT_LBUS)
		printf("nosd-fm6000: Intel FM6000 via SCD %s BAR1, %zu MB window\n",
		       d->slot, d->bar_bytes >> 20);
	else
		printf("nosd-fm6000: Intel FM6000 at %s, BAR0 %zu MB\n",
		       d->slot, d->bar_bytes >> 20);
	printf("nosd-fm6000: forwarding via DMA punt/inject; TAPs et1/et2\n");
	fflush(stdout);
}

static void usage(void)
{
	fprintf(stderr,
"usage: nosd-fm6000 [--slot ADDR] [--scd ADDR] [--socket PATH] [--no-boot]\n"
"\n"
"  --slot A  the FM6000's PCI address. Default: search for 8086:155b.\n"
"  --scd A   the SCD's PCI address, for the local-bus window. Default:\n"
"            search for 3475:0001. The chip is reached over the SCD when it\n"
"            is not on the PCI bus, which on a cold 7150S is always.\n"
"  --wait N  seconds to wait for the chip before failing (default %d).\n"
"            0 fails immediately. The wait exists because the SCD holds the\n"
"            ASIC in reset until the platform HAL releases it, so \"not there\n"
"            yet\" is a normal transient rather than a fault.\n"
"  --no-boot do NOT run the documented cold-boot sequence at start-up.\n"
"            The sequence runs by default, because on a cold board nothing\n"
"            else runs it and an image that boots to a prompt with a dead\n"
"            ASIC is not a shippable image. It is skipped automatically on a\n"
"            chip that has already been booted, so restarting this daemon\n"
"            does not disturb a running dataplane. Use this to hold a chip\n"
"            in its cold state for debugging.\n",
		FM_DEFAULT_WAIT_SECS);
}

int main(int argc, char **argv)
{
	struct fm6000 dev;
	const char *slot = NULL, *sockpath = NULL;
	int no_boot = 0, i, reported_offbus = 0, waited = 0;
	const char *scd_slot = NULL;
	int wait_secs = FM_DEFAULT_WAIT_SECS;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc)
			slot = argv[++i];
		else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
			sockpath = argv[++i];
		else if (strcmp(argv[i], "--no-boot") == 0)
			no_boot = 1;
		else if (strcmp(argv[i], "--scd") == 0 && i + 1 < argc)
			scd_slot = argv[++i];
		else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
			wait_secs = atoi(argv[++i]);
		else {
			usage();
			return 2;
		}
	}

	/*
	 * Wait for the chip, up to a bound, and then give up.
	 *
	 * Both halves of that matter and they pull in opposite directions.
	 *
	 * GIVING UP IS REQUIRED. A datapath that cannot find its chip is exactly
	 * the condition A/B trial-confirm exists to catch: the supervisor sees
	 * nosd fail, the trial boot does not confirm itself healthy, and the
	 * switch rolls back to the slot that worked. A daemon that stayed up
	 * reporting "no chip" would defeat that by looking alive, and a bad
	 * image would be committed.
	 *
	 * WAITING IS ALSO REQUIRED, and this board is why. The FM6000 is held in
	 * reset by the SCD from power-on and does not appear on the PCI bus until
	 * the platform HAL releases it -- so "no chip yet" is a normal transient
	 * at start-up, not a fault, and a daemon that exits on the first look is
	 * racing something that is doing its job.
	 *
	 * Exiting instantly did both wrong at once. Measured on the switch: with
	 * the chip held in reset, `restart: always` respawned this several times
	 * a second, which printed a line each time and made the console unusable
	 * for the very debugging the restarts were evidence for. The exit was
	 * right and the cadence was not.
	 *
	 * So the wait is what bounds the restart rate, rather than a sleep bolted
	 * on before exit: while the chip is legitimately on its way this is not a
	 * failure at all, and once the deadline passes it is one.
	 */
	if (wait_secs > 0)
		fprintf(stderr, "nosd-fm6000: looking for the chip, up to %ds\n",
			wait_secs);
	while (fm_open_auto(&dev, slot, scd_slot) != FM_OK) {
		if (waited >= wait_secs) {
			fprintf(stderr,
"nosd-fm6000: could not reach the chip after %ds, either way.\n"
"nosd-fm6000: no 8086:155b on the PCI bus -- which on a 7150S is normal,\n"
"nosd-fm6000: the FM6000 does not enumerate until it has been configured --\n"
"nosd-fm6000: and no SCD (3475:0001) whose BAR1 would give us the local bus.\n"
"nosd-fm6000: Without one of those two there is no way to address the chip.\n",
				waited);
			return 1;
		}
		nap_ms(1000);
		waited++;
	}
	if (waited > 0)
		fprintf(stderr, "nosd-fm6000: chip appeared after %ds\n", waited);
	if (dev.xport == FM_XPORT_LBUS)
		fprintf(stderr, "nosd-fm6000: reaching the chip over the SCD's "
				"local bus at %s\n", dev.slot);
	banner(&dev);

	/*
	 * Bring the chip up, unless it is already up or we were told not to.
	 *
	 * ⚠ THIS IS THE DEFAULT, AND IT HAS TO BE. On a cold board nothing else
	 * runs Table 4-1: the platform HAL releases the reset and stops there,
	 * because what to do with a released chip is the datapath's business. A
	 * daemon that waited to be asked would ship an image that boots to a
	 * prompt with a dead ASIC and no indication why.
	 *
	 * ⚠ AND IT HAS TO BE CONDITIONAL. `restart: always` means this runs
	 * again every time nosd is restarted, and putting a forwarding chip back
	 * through the sequence would drop the dataplane on what should be a
	 * no-op. fm_boot_already_done() tests the end state rather than
	 * remembering anything, so it is still right after a crash, and right
	 * for a chip somebody else booted.
	 */
	if (!no_boot) {
		int done = fm_boot_already_done(&dev);

		if (done < 0) {
			fprintf(stderr, "nosd-fm6000: cannot read the chip to find "
					"out whether it has been booted; not writing to it\n");
			fm_close(&dev);
			return 1;
		}
		if (done == 1) {
			fprintf(stderr, "nosd-fm6000: the chip is already booted; "
					"leaving it alone\n");
			fm_boot_mark_done(&dev);
			/* Attach PCIe BAR0 for DMA (normally done inside
			 * fm_boot_cold step 9; must be explicit on warm restart). */
			if (fm_attach_pcie(&dev) != FM_OK)
				fprintf(stderr, "nosd-fm6000: PCIe attach failed on warm restart\n");
		} else {
			struct fm_boot_report rep;
			int rv;

			fprintf(stderr, "nosd-fm6000: cold chip -- running the "
					"documented boot sequence\n");
			rv = fm_boot_cold(&dev, &rep);
			fm_boot_report_print(&rep);
			if (rv == FM_EOFFBUS) {
				fprintf(stderr, "nosd-fm6000: the chip left the bus "
						"during boot. Not continuing.\n");
				fm_close(&dev);
				return 1;
			}
			if (rv != FM_OK) {
				fprintf(stderr, "nosd-fm6000: the boot sequence did not "
						"complete. Not continuing.\n");
				fm_close(&dev);
				return 1;
			}
		}
		/* Checkpoint: PLL state immediately after boot_cold / mark_done */
		{
			uint32_t pst=0, pc0=0, sr=0;
			char cb[128];
			fm_rd(&dev, FM6000_PLL_STATUS, &pst);
			fm_rd(&dev, FM6000_PLL_CTRL_0, &pc0);
			fm_rd(&dev, FM6000_SOFT_RESET, &sr);
			snprintf(cb, sizeof(cb),
				"nosd-fm6000: post_boot: pll_status=0x%x pll_ctrl_0=0x%x "
				"soft_reset=0x%x fatal=%u",
				pst, pc0, sr, fm_fatal_count(&dev));
			kmsg(cb);
		}
		/*
		 * Remove FM6000 from PCIe bus before fm_spico_load causes chip resets.
		 *
		 * boot.c step 9 already re-enumerated 0000:02:00.0 (via PCIe rescan
		 * at ~t+47s); pciehp is again monitoring the slot.  fm_spico_load
		 * causes ~39 chip self-resets, each a PCIe link bounce.  pciehp fires
		 * Config Space accesses (pci_restore_state) for each link-up.  On AMD
		 * RS780, a Config TLP with no Completion stalls the CF8/CFC loop
		 * forever.  Removing the device while the chip is on-bus (pll_status=
		 * 0xf, post fm_boot_cold) stops pciehp from monitoring the slot during
		 * fm_spico_load and the subsequent pipeline_init resets.
		 *
		 * The device is re-enumerated in start_forwarding() via an explicit
		 * rescan after SerDes link training completes (when Config Space works).
		 * [2026-10-25, install82]
		 */
		{
			int rfd = open("/sys/bus/pci/devices/0000:02:00.0/remove",
				       O_WRONLY);
			if (rfd >= 0) {
				(void)write(rfd, "1\n", 2);
				close(rfd);
				kmsg("nosd-fm6000: pre_spico: FM6000 removed from PCIe bus");
			} else {
				kmsg("nosd-fm6000: pre_spico: WARNING: could not open remove path");
			}
		}
		/*
		 * Load SPICO firmware after ring is confirmed circulating.
		 *
		 * fm_spico_load reads the 6000-word firmware from the EOS SWI
		 * (~2 s) and downloads it via SBus, causing ~39 chip self-resets.
		 * It calls fm_sbus_start() internally to recover SBUS_CFG after
		 * those resets.  Without SPICO running, the SerDes PLL does not
		 * lock (measured: 9 installs, step 6 FM_ETIMEOUT every time).
		 *
		 * ⚠ The ssched.c note "fibre ports work with zero Intel code"
		 * was inferred from EdgeNOS analysis, not from hardware measurement
		 * on this specific boot sequence.  It has not proven correct here.
		 */
		{
			int srv;
			kmsg("nosd-fm6000: fm_spico_load starting");
			srv = fm_spico_load(&dev);
			{
				char buf[64];
				snprintf(buf, sizeof(buf),
					"nosd-fm6000: fm_spico_load rv=%d", srv);
				kmsg(buf);
			}
		}
		/* Checkpoint: PLL state after SPICO load */
		{
			uint32_t pst=0, pc0=0;
			char cb[96];
			fm_rd(&dev, FM6000_PLL_STATUS, &pst);
			fm_rd(&dev, FM6000_PLL_CTRL_0, &pc0);
			snprintf(cb, sizeof(cb),
				"nosd-fm6000: post_spico: pll_status=0x%x pll_ctrl_0=0x%x "
				"fatal=%u",
				pst, pc0, fm_fatal_count(&dev));
			kmsg(cb);
		}
		pipeline_init(&dev);
		/* Checkpoint: PLL state after pipeline init.
		 * Also read PIN_STRAP (watchdog block, accessible without EPL clock) as
		 * a LBUS canary: 0x208 = LBUS works (reads are reliable); 0x0 = chip is
		 * off the local bus (all register reads return 0). */
		{
			uint32_t pst=0, pc0=0, ps=0;
			char cb[160];
			fm_rd(&dev, FM6000_PLL_STATUS, &pst);
			fm_rd(&dev, FM6000_PLL_CTRL_0, &pc0);
			fm_rd(&dev, FM6000_PIN_STRAP, &ps);
			snprintf(cb, sizeof(cb),
				"nosd-fm6000: post_pipeline: pll_status=0x%x pll_ctrl_0=0x%x "
				"pin_strap=0x%x(want=0x208) fatal=%u",
				pst, pc0, ps, fm_fatal_count(&dev));
			kmsg(cb);
		}

		bring_up_ports(&dev);
		{
			int rv;
			/*
			 * Pre-fill L2F SRAM block at 0x14080 before fm_l2f_init.
			 *
			 * fm_l2f_init writes to two disjoint ranges in 0x14xxx:
			 *   0x14000–0x1404b  (76 words) — covered by the LBS pre-fill above
			 *   0x14080–0x1408b  (12 words) — NOT covered; ECC uninitialized
			 *
			 * An fm_wr to ECC-uninit SRAM triggers an RMW fault → soft chip
			 * reset → PCIe link drop during init.  The LBS pre-fill happens
			 * long before bring_up_ports; SPICO may have issued more resets
			 * since then, so ECC at 0x14080 may be corrupted again.  Fill it
			 * here, immediately before the writes that land there.
			 */
			(void)fm_mem_fill(&dev, 0x014080u, 12u, 0u);
			kmsg("nosd-fm6000: L2F pre-fill 0x14080 done");
			kmsg("nosd-fm6000: L2F init start");
			rv = fm_l2f_init(&dev);
			if (rv != FM_OK)
				fprintf(stderr, "nosd-fm6000: L2F init failed (%d)\n", rv);
			else
				fprintf(stderr, "nosd-fm6000: L2F: ok\n");
			kmsg("nosd-fm6000: L2F init done");
		}
		start_forwarding(&dev);
	}

	if (fm_sock_start(&dev, sockpath) != 0)
		fprintf(stderr, "nosd-fm6000: could not serve %s -- carrying on "
				"without it\n",
			sockpath ? sockpath : NOSAIC_QUERY_SOCKET);

	/*
	 * The watch loop. One second is frequent enough that an operator
	 * watching the log sees the failure while they still remember what they
	 * did, and slow enough to cost nothing.
	 *
	 * It reports the transition ONCE. A chip that has gone off the bus does
	 * not come back without a reset, so repeating the message every second
	 * would bury the one line that says when it happened -- which is the
	 * only piece of information the message carries.
	 */
	while (running) {
		if (!reported_offbus && fm_alive(&dev) == 0) {
			fprintf(stderr,
"nosd-fm6000: ⚠ THE CHIP HAS STOPPED ANSWERING.\n"
"nosd-fm6000: this is what an uncorrectable ECC error on an uninitialised\n"
"nosd-fm6000: bank memory looks like. Over PCIe the endpoint reads all ones;\n"
"nosd-fm6000: over the local bus it reads all zeros. Either way it will not\n"
"nosd-fm6000: come back without a reset PULSE from the SCD.\n"
"nosd-fm6000: %llu reads, %llu writes, %llu refused before this.\n",
				dev.reads, dev.writes, dev.refused);
			reported_offbus = 1;
		}
		nap_ms(1000);
	}

	fm_close(&dev);
	return 0;
}
