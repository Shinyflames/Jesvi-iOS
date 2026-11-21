/*
 * The Galaxy S20's sensors (x1slte, SM-G980F, Exynos 990): the accelerometer and gyroscope (LSM6DSO), the magnetometer
 * (AK09918-class, "ssp-mag-type 1"), the barometer (LPS22HH), the front light and proximity sensor (TMD4907) all sit
 * behind one sensor hub, the MCU inside the Broadcom BCM4775 B1 GNSS chip ("ssp,BCM4773"), which the AP reaches over
 * one SPI bus and five pins. On Android the daemon lhd powers it, loads its firmware through the chip's ROM and carries
 * Samsung's sensor protocol (SSP); here this file does all of it. A port of Scion's drivers/input/ssp/hakios.ssp.c (the
 * Galaxy S9's BCM4775 B0, working there since 3 November) with the S20's SoC side: no CHUB here, the hub's SPI is a
 * plain PERIC0 USI. Only with boot-arg shub=N; nothing is touched without it.
 *
 *   shub=1   a look, reading only: the PMIC's LDO21M (vdd_mcu_1p8), CMU_TOP's and CMU_PERIC0's clocks of USI14, the
 *            USI's mode, and the hub's pins as found
 *   shub=2   ... the hub powered: LDO21M on if off (bsd/dev/redwood_acpm_pmic.c, read back), 20 ms; the SPI (USI14 of
 *            PERIC0) clocked and set up, its pins to function 2; AP_SLEEP high; NSTDBY low 20 ms then high (the chip out
 *            of standby: it starts in its ROM); MCU_REQ / MCU_RESP tried once; what the chip has to say read
 *   shub=3   ... the ROM: the autobaud burst, the transport layer's sync, the version (RPC 0x00 -> 0x03)
 *   shub=4   ... the firmware: fw/bcm_sensorhub_a1.bin of the small image (proposals/sensors/mk-fw-sensors.py out of
 *            Samsung's r_os/bbd_patch_file_picasso.h: 388 writes, 355888 bytes) written record by record (RPC 0x05,
 *            reliable), started at its PatchAddress 0x00800161 (RPC 0x2E), synced again, asked its version
 *   shub=5   ... SSP: "sensor init done", WHOAMI (0x55), the AP's type and revision (0xB6), the sensors' formation
 *            (0xF1: accel 6, gyro 6, mag 5, the dtbo's), the glass type (0xEC: 0), what it found (0xF4), the
 *            magnetometer's PDC matrix (0xFD, the dtbo's 27 bytes), its firmware revision, the accelerometer's range
 *   shub=6   ... the sensors on (shub_sensors=), their samples into /dev/redwood-sensors for as long as the phone runs
 *   shub_sensors=<mask>  Samsung's sensor numbers (sensor_list.h: 0 accel, 1 gyro, 4 magnetometer, 5 pressure,
 *            7 proximity, 9 light; default 0x2b3 = those six), only those the hub found
 *   shub_ms=<n>          their period in ms (default 60; pressure 1000; light shub_light_ms=250 while the display is on;
 *            proximity on native HID demand at shub_prox_ms=200; both off
 *            while the display is off)
 *   shub_div=<n>         USI14's divider, 0-15 (default 15: 400 MHz / 16 = 25 MHz IP clock, SCLK a quarter: 6.25 MHz;
 *            Samsung's spi-max-frequency is 25 MHz); 255 leaves it as found
 *   shub_ap_rev=<n>      the AP revision told to the hub (default 21: the dtbo's dtbo-hw_rev 0x15; Samsung sends the
 *            board's revision, current_hw_rev)
 *   shub_keep=N          0: the hub back into standby when the steps end; 1 (default): left running when they all
 *            went through, standby when one failed; 2: left powered even after a failure (for a look at its pins)
 *   shub_log=1           every SPI frame's first bytes in the log (the first 40 are always logged)
 *   shub_hello=1         wait for MCU_RESP before every exchange, as Samsung's driver does (default: MCU_REQ raised,
 *            1 ms, then the transfer, which is what worked on the S9)
 *   shub_req_rpc=<n>, shub_req_len=0   the RPC that carries SSP (default 0x20, with a u16 length): Scion's, as on the S9
 *
 * What is where (the phone's dtbo device/dts/dtbo.1.dtb = ExtremeXT's exynos990-x1slte_eur_open_21.dts, exynos9830.dts,
 * cal-if/exynos9830/cmucal-*.c, pinctrl-exynos-arm64.c; NOTES.md has the line numbers):
 *   SPI          spi_14 = spi@10650000, "samsung,exynos-spi", 64-byte FIFO, mode 3 (spi-cpol, spi-cpha), 25 MHz max,
 *                clocks GATE_PERIC0_TOP1_QCH_USI14_USI and DOUT_CLK_PERIC0_USI14_USI; its USI mode word usi_14 =
 *                usi@10421044 ("spi", dtbo fragment@67; 2 = SPI, usi_v2.c SPI_SW_CONF)
 *   clocks       CMU_PERIC0 0x10400000: USER mux USI14 +0x690 (bit 4: CLKCMU_PERIC0_IP, 400 MHz; else the 26 MHz
 *                oscillator), DIV USI14 +0x1820 (ratio [3:0]), gates RSTNSYNC_CLK_PERIC0_USI14_USI +0x20D8,
 *                TOP1 IPCLK_5 +0x2084, TOP1 PCLK_5 +0x20A4, Q-channel USI14_USI +0x3058. CMU_TOP 0x1A330000:
 *                GATE_CLKCMU_PERIC0_IP +0x20D0, DIV +0x18C8, MUX +0x10D0 (read only)
 *   pins         gpp3 (PERIC0 0x10430000 +0x60): -6 -5 -4 the bus (spi14-bus), -7 nSS (spi14-cs-func), all function 2,
 *                no pull. gpp3-2/-3 are the S2MPB02 torch's bit-banged bus: every pin write goes through
 *                redwood_gpio_func() under the pinctrl lock
 *                MCU_REQ gpb0-5 (PERIC1 0x10730000 +0xE0), output. MCU_RESP gpm19-0 (CMGP 0x15C30000 +0x260), input
 *                HOST_REQ gpa1-7 (ALIVE 0x15850000 +0x20), input, pull-down (the dtbo: function EINT, pud 1); polled here
 *                AP_SLEEP gpa0-5 ("ssp-batch-wake-irq", ALIVE +0x00), output high while the AP runs
 *                NSTDBY gpp6-3 ("ssp-gps-pwr-en", PERIC1 +0x20), output; lhd's GpioNStdbyPath
 *                SHUB_INT gpa4-0: the gyroscope's time stamps for the camera, unused
 *   supply       vdd_mcu_1p8 = S2MPS19 LDO21M (1.8 V, not always-on), lhd's GpioNPowerPath (mcu_power, ssp_sysfs.c)
 *
 * The protocol, layer by layer, is bsd/dev/redwood_bcm_shub.h (Scion's, with the S20's sample sizes).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_exynos_spi.h>
#include <sys/redwood_sensors.h>
#include <sys/redwood_dbgport.h>
#include <sys/systm.h>
#include <sys/vnode.h>          /* IO_NDELAY */
#include <miscfs/devfs/devfs.h>
#include "redwood_bcm_shub.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern int redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value);
extern int redwood_pmic_ldo_on(unsigned ldo, char *report, size_t report_size);

#define say redwood_dsay

/* ---- where ---------------------------------------------------------------------------------------------------- */

#define CMU_TOP_PA              0x1A330000UL
#define  TOP_PERIC0_IP_MUX      0x10D0
#define  TOP_PERIC0_IP_DIV      0x18C8
#define  TOP_PERIC0_IP_GATE     0x20D0
#define CMU_PERIC0_PA           0x10400000UL
#define  USI14_USER_MUX         0x0690  /* bit 4: CLKCMU_PERIC0_IP; else OSCCLK */
#define  USI14_DIV              0x1820
#define  USI14_GATE_RSTNSYNC    0x20D8
#define  USI14_GATE_IPCLK       0x2084  /* PERIC0_TOP1_IPCLK_5 */
#define  USI14_GATE_PCLK        0x20A4  /* PERIC0_TOP1_PCLK_5 */
#define  USI14_QCH              0x3058
#define SPI14_PA                0x10650000UL
#define USI14_MODE_PA           0x10421044UL
#define CMGP_PA                 0x15C30000U

#define BANK_GPP3               0x060U  /* PERIC0 */
#define  PIN_SPI_FIRST          4       /* gpp3-4..-7 */
#define BANK_GPP6               0x020U  /* PERIC1 */
#define  PIN_NSTDBY             3
#define BANK_GPB0               0x0E0U  /* PERIC1 */
#define  PIN_MCU_REQ            5
#define BANK_GPM19              0x260U  /* CMGP */
#define  PIN_MCU_RESP           0
#define BANK_GPA0               0x000U  /* ALIVE */
#define  PIN_AP_SLEEP           5
#define BANK_GPA1               0x020U
#define  PIN_HOST_REQ           7
#define BANK_GPA4               0x080U
#define  PIN_SHUB_INT           0

#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)
#define PMIC_TYPE_PMIC          1
#define LDO21_CTRL              0x5A
#define LDO_ON                  0xC0

/* the dtbo's numbers for the hub (the last overlay of each wins: fragment@133 positions, @139 the matrix) */
#define ACC_POSITION            6
#define MAG_POSITION            5
static uint32_t acc_position = ACC_POSITION, mag_position = MAG_POSITION;   /* boot-args shub_acc_pos, shub_mag_pos */
#define AP_TYPE                 0       /* fragment@91 ssp-ap-type */
#define GLASS_TYPE              0
static const uint8_t mag_pdc[27] = {
	0x99, 0x52, 0x04, 0xef, 0xf2, 0xe0, 0x00, 0xcf, 0xd3, 0x37, 0x05, 0xbd, 0xcf, 0xca,
	0xff, 0x47, 0xff, 0x56, 0xa0, 0x5b, 0xbe, 0xfc, 0x04, 0x0f, 0x0a, 0xbe, 0xbc,
};
#define SSP_AP_INFORMATION      0xB6    /* [ap_type][ap_rev] */
#define SSP_GLASS_TYPE          0xEC    /* [type] */
#define SSP_MAG_STATIC_MATRIX   0xFD    /* the AKM's PDC matrix, 27 bytes */
#define PATCH_ENTRY             0x00800161u     /* <PatchAddress=> of r_os/bbd_patch_file_picasso.h */
#define PATCH_FILE              "bcm_sensorhub_a1.bin"

/* timings (bcm_gps_spi.c, ssp_*.c, lhd as the Tab Active3's notes describe it; Scion's) */
#define HELLO_POLLS             100
#define NSTDBY_LOW_MS           20
#define AFTER_POWER_MS          50
#define AUTOBAUD_BYTES          36
#define AUTOBAUD_MS             100
#define SYNC_TRIES              6
#define SYNC_EVERY_MS           100
#define AUTOBAUD_TRIES          5
#define VERSION_MS              250
#define VERSION_TRIES           5
#define RELIABLE_MS             500
#define RELIABLE_TRIES          10
#define START_MS                40
#define SSP_MS                  400
#define INIT_DONE_MS            1000
#define ACK_EVERY               100

static struct redwood_exynos_spi bus = {
	.name = "shub-spi",
	.regs_pa = SPI14_PA,
	.usi_mode_pa = USI14_MODE_PA,
	.cmu_pa = CMU_PERIC0_PA, .cmu_size = 0x4000,
	.gates = { USI14_GATE_RSTNSYNC, USI14_GATE_IPCLK, USI14_GATE_PCLK }, .ngates = 3,
	.qch = USI14_QCH, .div = USI14_DIV, .mux = USI14_USER_MUX,
	.div_ratio = 15, .ipclk_mhz = 400,
	.pins_pa = 0,                           /* set by pins_spi(), under the pinctrl lock */
	.fifo_bytes = 64,
	.mode = 3,                              /* CPOL 1, CPHA 1 */
};

static vm_offset_t cmu_top, cmu_peric0;
static volatile uint8_t *gpp3, *gpp6, *gpb0, *gpm19, *gpa0, *gpa1, *gpa4;
static uint32_t steps, keep = 1, log_frames, div_ratio = 15, ap_rev = 21;
/* 2026-11-13 (user: the sensor by the camera flashes every second, the phone laggy): the motion sensors at 60 ms
 * (~16 Hz, plenty for rotation) instead of 20; shub_ms=20 gives that back. */
static uint32_t want_sensors = 0x2b3, period_ms = 60, req_rpc = SHUB_RPC_SENSOR_REQ, req_len = 1;
/* Light stays continuous while awake. The visible IR emitter needs explicit
 * native proximity demand; periodic unsolicited bursts are never necessary. */
static uint32_t prox_ms = 200, light_ms = 250;     /* light 4x a second: auto-brightness reacts in ~1.5 s (A, 2026-11-13) */
static volatile bool powered;
static const char *state = "off";

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t interval;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &interval);
	return mach_absolute_time() + interval;
}

static uint64_t
now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* ---- pins (all through bsd/dev/redwood_bbi2c.c's locked helpers: the banks are shared) ------------------------- */

static uint32_t
pin_con(volatile uint8_t *bank, unsigned pin)
{
	return (*(volatile uint32_t *)(bank + REDWOOD_GPIO_CON) >> (4 * pin)) & 0xf;
}

static bool
host_req(void)
{
	return redwood_gpio_get(gpa1, PIN_HOST_REQ) != 0;
}

static bool
mcu_resp(void)
{
	return redwood_gpio_get(gpm19, PIN_MCU_RESP) != 0;
}

static void
mcu_req(bool high)
{
	redwood_gpio_set(gpb0, PIN_MCU_REQ, high);
}

static void
look_pins(const char *when)
{
	say("shub: %s: gpp3 CON %08x PUD %08x (SPI on -4..-7: CON [31:16] 2222); MCU_REQ gpb0-5 function %x level %u; "
	    "MCU_RESP gpm19-0 function %x level %u\n", when, *(volatile uint32_t *)(gpp3 + REDWOOD_GPIO_CON),
	    *(volatile uint32_t *)(gpp3 + REDWOOD_GPIO_PUD), pin_con(gpb0, PIN_MCU_REQ), redwood_gpio_get(gpb0, PIN_MCU_REQ),
	    pin_con(gpm19, PIN_MCU_RESP), redwood_gpio_get(gpm19, PIN_MCU_RESP));
	say("shub: %s: HOST_REQ gpa1-7 function %x level %u; AP_SLEEP gpa0-5 function %x level %u; SHUB_INT gpa4-0 function "
	    "%x level %u; NSTDBY gpp6-3 function %x level %u\n", when, pin_con(gpa1, PIN_HOST_REQ), redwood_gpio_get(gpa1,
	    PIN_HOST_REQ), pin_con(gpa0, PIN_AP_SLEEP), redwood_gpio_get(gpa0, PIN_AP_SLEEP), pin_con(gpa4, PIN_SHUB_INT),
	    redwood_gpio_get(gpa4, PIN_SHUB_INT), pin_con(gpp6, PIN_NSTDBY), redwood_gpio_get(gpp6, PIN_NSTDBY));
}

/* the SPI's four pins to the controller (function 2, no pull), one at a time under the lock */
static void
pins_spi(void)
{
	for (unsigned p = PIN_SPI_FIRST; p < PIN_SPI_FIRST + 4; p++) {
		redwood_gpio_pull(gpp3, p, REDWOOD_GPIO_PULL_NONE);
		redwood_gpio_func(gpp3, p, 2);
	}
}

/* ---- step 1: what is on ------------------------------------------------------------------------------------------ */

static bool
peric0_ip_clocked(void)
{
	uint32_t g = rd(cmu_top, TOP_PERIC0_IP_GATE);

	return !(g & GATE_MANUAL) || (g & GATE_ON);
}

static bool
look(void)
{
	uint8_t ldo = 0;
	int e = redwood_pmic_read(PMIC_TYPE_PMIC, LDO21_CTRL, &ldo);

	say("shub: 1: the hub's supply, LDO21M (vdd_mcu_1p8): CTRL %02x%s\n", ldo, e ? " (not read: ACPM)" :
	    (ldo & LDO_ON) == LDO_ON ? " (on)" : " (off)");
	say("shub: 1: CMU_TOP's CLKCMU_PERIC0_IP: mux %08x divider %08x gate %08x\n", rd(cmu_top, TOP_PERIC0_IP_MUX),
	    rd(cmu_top, TOP_PERIC0_IP_DIV), rd(cmu_top, TOP_PERIC0_IP_GATE));
	say("shub: 1: CMU_PERIC0 USI14: user mux %08x (%s), divider %08x, gates %08x %08x %08x, Q-channel %08x\n",
	    rd(cmu_peric0, USI14_USER_MUX), (rd(cmu_peric0, USI14_USER_MUX) >> 4) & 1 ? "CLKCMU_PERIC0_IP" : "the oscillator",
	    rd(cmu_peric0, USI14_DIV), rd(cmu_peric0, USI14_GATE_RSTNSYNC), rd(cmu_peric0, USI14_GATE_IPCLK),
	    rd(cmu_peric0, USI14_GATE_PCLK), rd(cmu_peric0, USI14_QCH));
	if (!peric0_ip_clocked()) {
		say("shub: 1: CMU_TOP's clock for PERIC0's IPs is gated: stopping (nothing of the SPI is touched)\n");
		return false;
	}
	bus.ipclk_mhz = (rd(cmu_peric0, USI14_USER_MUX) >> 4) & 1 ? 400 : 26;
	if (!redwood_exynos_spi_map(&bus, say)) {
		return false;
	}
	look_pins("1");
	return true;
}

/* ---- step 2: power and the SPI ---------------------------------------------------------------------------------- */

static void
power_off(const char *why)
{
	if (!powered) {
		return;
	}
	redwood_gpio_set(gpp6, PIN_NSTDBY, false);
	mcu_req(false);
	powered = false;
	state = "off";
	say("shub: the hub in standby again (NSTDBY low): %s\n", why);
}

static bool
power_on(void)
{
	char line[160];

	if (redwood_pmic_ldo_on(21, line, sizeof(line)) != 0) {
		say("shub: 2: %s: stopping\n", line);
		return false;
	}
	say("shub: 2: %s\n", line);
	sleep_ms(20);                           /* set_mcu_power(): msleep(20) */
	bus.div_ratio = div_ratio;
	redwood_exynos_spi_power(&bus);
	pins_spi();
	redwood_exynos_spi_reset(&bus);
	say("shub: 2: SPI set up: USI14 divider %08x (ratio %u: %u MHz IP clock, SCLK %u kHz), USI mode %x\n",
	    rd(cmu_peric0, USI14_DIV), (rd(cmu_peric0, USI14_DIV) & 0xf) + 1,
	    bus.ipclk_mhz / ((rd(cmu_peric0, USI14_DIV) & 0xf) + 1),
	    bus.ipclk_mhz * 1000 / 4 / ((rd(cmu_peric0, USI14_DIV) & 0xf) + 1), *(volatile uint32_t *)bus.usi);
	redwood_gpio_pull(gpm19, PIN_MCU_RESP, REDWOOD_GPIO_PULL_NONE);
	redwood_gpio_func(gpm19, PIN_MCU_RESP, REDWOOD_GPIO_INPUT);
	mcu_req(false);
	redwood_gpio_pull(gpa1, PIN_HOST_REQ, 0x1);     /* pull-down, the dtbo's pud 1 */
	redwood_gpio_func(gpa1, PIN_HOST_REQ, REDWOOD_GPIO_INPUT);
	redwood_gpio_set(gpa0, PIN_AP_SLEEP, true);
	redwood_gpio_set(gpp6, PIN_NSTDBY, false);
	powered = true;
	state = "powered";
	sleep_ms(NSTDBY_LOW_MS);
	redwood_gpio_set(gpp6, PIN_NSTDBY, true);
	sleep_ms(AFTER_POWER_MS);
	look_pins("2");
	return true;
}

/* ---- SSI ------------------------------------------------------------------------------------------------------- */

static uint8_t txf[SHUB_SSI_FRAME + 8], rxf[SHUB_SSI_FRAME + 8];
static struct shub_tl_rx tlrx;
static uint8_t last_status;
static uint32_t frames_logged, hello_fails, spi_errors, fc_retries;
static uint64_t bytes_in, bytes_out;

static void handle_packet(const uint8_t *raw, size_t len);
static bool pay_ack(void);

static void
log_frame(const char *dir, const uint8_t *p, size_t n)
{
	char line[3 * 24 + 1];
	size_t k = 0;

	if (frames_logged >= 40 && log_frames == 0) {
		return;
	}
	frames_logged++;
	for (size_t i = 0; i < n && i < 24; i++) {
		k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[i]);
	}
	line[k] = 0;
	say("shub: spi %s %zu: %s%s\n", dir, n, line, n > 24 ? "..." : "");
}

static bool
xfer(uint32_t len)
{
	uint32_t e;

	redwood_exynos_spi_select(&bus, true);
	e = redwood_exynos_spi_transfer(&bus, txf, rxf, len);
	redwood_exynos_spi_select(&bus, false);
	if (e != 0) {
		spi_errors++;
		return false;
	}
	return true;
}

static void
feed(const uint8_t *p, size_t n)
{
	bytes_in += n;
	for (size_t i = 0; i < n; i++) {
		if (shub_tl_byte(&tlrx, p[i]) == SHUB_TL_PACKET) {
			handle_packet(tlrx.buf, tlrx.len);
		}
	}
}

/*
 * MCU_RESP is not waited for unless shub_hello=1 (Scion's S9 finding, kept for the S20 until its gpm19-0 is seen to
 * move: step 2 logs it): on the S9 it reads 0 whatever the chip does (pulled up or down, the
 * hub running or not), while everything else works when MCU_REQ is raised and the transfer follows a millisecond
 * later (3 November, phone 2). The console's "nohello 0|1" changes it on a running phone.
 */
static uint32_t no_hello = 1;

static bool
hello(void)
{
	mcu_req(true);
	if (no_hello) {
		delay(1000);
		return true;
	}
	for (int count = 0; !mcu_resp(); count++) {
		if (count > HELLO_POLLS) {
			mcu_req(false);
			hello_fails++;
			return false;
		}
		sleep_ms(1);
		if (count % 20 == 19) {
			mcu_req(false);
			delay(1000);
			mcu_req(true);
			delay(1000);
		}
	}
	return true;
}

/* one read frame: what the chip has, into the transport layer */
static bool
ssi_rx(void)
{
	uint16_t len, len2;

	memset(txf, 0, 3);
	txf[0] = SHUB_SSI_RX;
	if (!xfer(3)) {
		return false;
	}
	last_status = rxf[0];
	len = shub_le16(rxf + 1);
	if (len == 0) {
		len = 254;      /* bcm_ssi_rx(): read anyway */
	}
	if (len > SHUB_SSI_FRAME - 3) {
		len = SHUB_SSI_FRAME - 3;
	}
	memset(txf, 0, (size_t)len + 3);
	txf[0] = SHUB_SSI_RX;
	if (!xfer((uint32_t)len + 3)) {
		return false;
	}
	last_status = rxf[0];
	len2 = shub_le16(rxf + 1);
	if (len2 < len) {
		len = len2;
	}
	log_frame("rx", rxf, (size_t)len + 3);
	feed(rxf + 3, len);
	return true;
}

/* one write frame of m bytes (and what the chip sends back meanwhile), resent until the chip took them all */
static bool
ssi_tx(const uint8_t *p, size_t m)
{
	uint8_t pending[SHUB_SSI_PAYLOAD];
	size_t left = m;

	if (m > sizeof(pending)) {
		return false;
	}
	memcpy(pending, p, m);
	if (shub_ssi_room(last_status) != 0 && m > shub_ssi_room(last_status)) {
		delay(1000);    /* bcm_rxtx_work_func(): the chip's buffer is short of room; a millisecond, then anyway */
	}
	for (int tries = 0; tries < 3 && left > 0; tries++) {
		uint16_t nread, taken;

		txf[0] = SHUB_SSI_TX;
		txf[1] = (uint8_t)left;
		txf[2] = (uint8_t)(left >> 8);
		memcpy(txf + 3, pending, left);
		txf[3 + left] = 0;
		txf[4 + left] = 0;
		if (!xfer((uint32_t)left + 5)) {
			return false;
		}
		log_frame("tx", txf, left + 5);
		last_status = rxf[0];
		nread = shub_le16(rxf + 1);
		taken = shub_le16(rxf + 3 + left);
		if (nread > 0) {
			feed(rxf + 3, nread < left ? nread : left);
		}
		bytes_out += taken <= left ? taken : 0;
		if (taken >= left) {
			left = 0;
			break;
		}
		fc_retries++;
		memmove(pending, pending + taken, left - taken);
		left -= taken;
	}
	return left == 0;
}

/* an exchange: the chip woken, everything it has read, `p` sent (if any), MCU_REQ dropped */
static bool
exchange(const uint8_t *p, size_t n)
{
	bool ok = true;
	int frames = 0;

	if (!hello()) {
		return false;
	}
	while (ok && host_req() && frames++ < 64) {
		ok = ssi_rx() && pay_ack();
	}
	for (size_t at = 0; ok && at < n;) {
		size_t chunk = n - at > SHUB_SSI_PAYLOAD - 8 ? SHUB_SSI_PAYLOAD - 8 : n - at;

		ok = ssi_tx(p + at, chunk) && pay_ack();
		at += chunk;
		while (ok && host_req() && frames++ < 64) {
			ok = ssi_rx() && pay_ack();
		}
	}
	mcu_req(false);
	return ok;
}

/*
 * read what the chip has for `ms`, or until `done()`: HOST_REQ looked at every idle_us (half a millisecond while the
 * firmware loads and answers are awaited; 4 ms once the sensors run, where samples come every 20 ms or slower)
 */
static uint32_t idle_us = 500;
/* Open GNSS clients need timely patch/command replies even with the display off.
 * The SPI owner reads this without shub_lock; open/close serialize the count. */
static uint32_t gnss_poll_users;
extern volatile int redwood_display_is_on;     /* (redwood_exynos_acpm.c) */

static void
pump(uint32_t ms, bool (*done)(void))
{
	uint64_t deadline = deadline_ms(ms);

	do {
		if (done != NULL && done()) {
			return;
		}
		if (host_req()) {
			(void)exchange(NULL, 0);
		} else if (idle_us >= 1000 && !redwood_display_is_on &&
		    __atomic_load_n(&gnss_poll_users, __ATOMIC_RELAXED) == 0) {
			/* (B, 2026-11-07) display off: 100 ms between looks, not 4 (250 wakeups a second kept the big cores up;
			 * the samples wait in the hub). An interrupt on HOST_REQ (gpa1-7 EINT) is the real fix. */
			delay_for_interval(100000, NSEC_PER_USEC);
		} else if (idle_us >= 1000) {
			delay_for_interval(idle_us, NSEC_PER_USEC);
		} else {
			delay(idle_us);
		}
	} while (mach_absolute_time() < deadline);
}

/* ---- the transport layer ----------------------------------------------------------------------------------------- */

static struct {
	uint8_t tx_seq, tx_rel;                 /* the next to send */
	uint8_t rx_seq, rx_rel;                 /* the last received */
	bool rx_rel_any;
	uint32_t unacked;                       /* payload packets received and not acknowledged yet */
	uint8_t rel_acked;                      /* the peer's last RELIABLE_ACK */
	bool rel_ack_seen, rel_nacked;
	bool synced;
	uint32_t packets, bad, internal, dup, out_of_order, rpcs, unknown_rpcs;
} tl;

static struct {
	bool seen;
	uint32_t asic, rom, patch;
} hubver;

static uint8_t sbuf[2 * SHUB_TL_MAX + 64];

static bool
send_packet(uint16_t flags, const uint8_t *payload, uint16_t size, bool reliable)
{
	struct shub_pkt p = { .seq = tl.tx_seq, .flags = flags, .payload = payload, .size = size };
	size_t n;

	if (!(flags & SHUB_F_INTERNAL)) {
		if (tl.unacked > 0) {
			p.flags |= SHUB_F_ACK;
			p.detail[0] = tl.rx_seq;
			tl.unacked = 0;
		}
		if (reliable) {
			p.flags |= SHUB_F_RELIABLE;
			p.detail[1] = tl.tx_rel;
		}
	}
	n = shub_tl_build(sbuf, sizeof(sbuf), &p);
	if (n == 0) {
		return false;
	}
	tl.tx_seq++;
	return exchange(sbuf, n);
}

/*
 * An acknowledgement owed: noted while a frame is being read (the frame's buffer is still being parsed), sent by
 * exchange() once the read is done. A later one replaces an earlier one (they acknowledge the latest).
 */
static struct {
	bool want;
	uint16_t flags;
	uint8_t rel;
} owed;

static void
send_ack(uint16_t flags, uint8_t rel)
{
	owed.want = true;
	owed.flags = flags;
	owed.rel = rel;
}

static bool
pay_ack(void)
{
	struct shub_pkt p = { .seq = tl.tx_seq, .flags = (uint16_t)(SHUB_F_ACK | owed.flags) };
	uint8_t b[32];
	size_t n;

	if (!owed.want) {
		return true;
	}
	owed.want = false;
	p.detail[0] = tl.rx_seq;
	p.detail[2] = owed.rel;
	p.detail[3] = owed.rel;
	tl.unacked = 0;
	n = shub_tl_build(b, sizeof(b), &p);
	if (n == 0) {
		return false;
	}
	tl.tx_seq++;
	return ssi_tx(b, n);
}

static void deliver_rpcs(const uint8_t *p, size_t n);
static void gnss_post(uint8_t kind, uint8_t flags, uint16_t id, const uint8_t *d, uint16_t len);
#define GNSS_REC_RPC            'R'     /* an RPC from the hub that is not SSP: [id][data] */
#define GNSS_REC_SSP            'S'     /* the answer to a raw SSP request: id = the command, flags 1 = answered */
#define GNSS_REC_SENT           'A'     /* a raw RPC sent: id = the RPC, flags 1 = acknowledged (or sent, if plain) */

static void
handle_internal(const struct shub_pkt *p)
{
	tl.internal++;
	if (p->size >= 8 && p->payload[0] == SHUB_TLI_SEQ_RESPONSE) {
		tl.tx_seq = (uint8_t)(p->payload[2] + 1);
		tl.tx_rel = (uint8_t)(p->payload[4] + 1);
		tl.rx_seq = p->payload[5];
		tl.rx_rel = (uint8_t)(p->payload[6] - 1);
		tl.rx_rel_any = true;
		tl.synced = true;
		say("shub: sync answered: %02x %02x %02x %02x %02x %02x %02x %02x (next seq %u, next reliable %u)\n",
		    p->payload[0], p->payload[1], p->payload[2], p->payload[3], p->payload[4], p->payload[5], p->payload[6],
		    p->payload[7], tl.tx_seq, tl.tx_rel);
	} else if (tl.internal <= 8) {
		say("shub: an internal packet, type %u, %u bytes\n", p->size ? p->payload[0] : 0xff, p->size);
	}
}

static void
handle_packet(const uint8_t *raw, size_t len)
{
	struct shub_pkt p;
	int r = shub_tl_parse(raw, len, &p);

	if (r != SHUB_TL_OK) {
		if (tl.bad++ < 8) {
			say("shub: a packet refused (%s, %zu bytes)\n", r == SHUB_TL_CRC ? "CRC" : r == SHUB_TL_LENGTH ? "length" :
			    "short", len);
		}
		return;
	}
	tl.packets++;
	if (p.flags & SHUB_F_RELIABLE_ACK) {
		tl.rel_acked = p.detail[2];
		tl.rel_ack_seen = true;
	}
	if (p.flags & SHUB_F_RELIABLE_NACK) {
		tl.rel_nacked = true;
	}
	if (p.flags & SHUB_F_INTERNAL) {
		handle_internal(&p);
		return;
	}
	tl.rx_seq = p.seq;
	if (p.flags & SHUB_F_RELIABLE) {
		uint8_t rel = p.detail[1];

		if (tl.rx_rel_any && rel == tl.rx_rel) {
			tl.dup++;
			send_ack(SHUB_F_RELIABLE_ACK, rel);
			return;
		}
		if (tl.rx_rel_any && rel != (uint8_t)(tl.rx_rel + 1)) {
			tl.out_of_order++;
			send_ack(SHUB_F_RELIABLE_NACK, tl.rx_rel);
			return;
		}
		tl.rx_rel = rel;
		tl.rx_rel_any = true;
		send_ack(SHUB_F_RELIABLE_ACK, rel);
	} else if (p.size > 0 && ++tl.unacked >= ACK_EVERY) {
		send_ack(0, 0);
	}
	if (p.size > 0) {
		deliver_rpcs(p.payload, p.size);
	}
}

/* the transport layer's sync: "sequence request" until answered */
static bool
synced(void)
{
	return tl.synced;
}

static bool
autobaud_and_sync(const char *who)
{
	uint8_t burst[AUTOBAUD_BYTES];

	memset(burst, 0x80, sizeof(burst));
	tl.synced = false;
	for (int a = 0; a < AUTOBAUD_TRIES; a++) {
		if (!exchange(burst, sizeof(burst))) {
			say("shub: %s: the autobaud burst not sent (MCU_RESP %s)\n", who, hello_fails ? "never came" : "came");
		}
		pump(AUTOBAUD_MS, NULL);
		for (int s = 1; s <= SYNC_TRIES; s++) {
			uint8_t req[2] = { SHUB_TLI_SEQ_REQUEST, (uint8_t)s };

			(void)send_packet(SHUB_F_INTERNAL | SHUB_F_IGNORE_SEQID, req, 2, false);
			pump(SYNC_EVERY_MS, synced);
			if (tl.synced) {
				return true;
			}
		}
		say("shub: %s: no answer to the sync (try %d of %d; %llu bytes read, %u packets, %u refused, %llu garbage; "
		    "status %02x, HOST_REQ %u, MCU_RESP %u)\n", who, a + 1, AUTOBAUD_TRIES, bytes_in, tl.packets, tl.bad,
		    (unsigned long long)tlrx.garbage, last_status, host_req(), mcu_resp());
	}
	return false;
}

/* a reliable packet, sent again until acknowledged */
static bool
send_reliable(const uint8_t *payload, uint16_t size)
{
	uint8_t rel = tl.tx_rel;

	for (int t = 0; t < RELIABLE_TRIES; t++) {
		uint64_t deadline = deadline_ms(RELIABLE_MS);

		tl.rel_ack_seen = false;
		tl.rel_nacked = false;
		tl.tx_rel = rel;
		if (!send_packet(0, payload, size, true)) {
			continue;
		}
		while (mach_absolute_time() < deadline) {
			if (tl.rel_ack_seen && tl.rel_acked == rel) {
				tl.tx_rel = (uint8_t)(rel + 1);
				return true;
			}
			if (tl.rel_nacked) {
				break;
			}
			if (host_req()) {
				(void)exchange(NULL, 0);
			} else {
				delay(200);
			}
		}
	}
	tl.tx_rel = (uint8_t)(rel + 1);
	return false;
}

/* ---- RPCs ---------------------------------------------------------------------------------------------------- */

static struct shub_ssp_rx ssprx;
static void ssp_stream(void);

static void
text_rpc(const char *what, const uint8_t *d, uint16_t n)
{
	char text[160];
	size_t k = 0;

	for (uint16_t i = 0; i < n && k + 1 < sizeof(text); i++) {
		text[k++] = d[i] >= 0x20 && d[i] < 0x7f ? (char)d[i] : '.';
	}
	text[k] = 0;
	say("shub: hub %s: %s\n", what, text);
}

static void
deliver_rpcs(const uint8_t *p, size_t n)
{
	size_t at = 0;
	uint16_t id, len;
	const uint8_t *d;
	int r;

	while ((r = shub_rpc_next(p, n, &at, &id, &d, &len)) == 1) {
		tl.rpcs++;
		if (id != SHUB_RPC_SENSOR_RSP) {
			gnss_post(GNSS_REC_RPC, 0, id, d, len);  /* everything but SSP, for /dev/redwood-gnss's readers */
		}
		switch (id) {
		case SHUB_RPC_VERSION:
			if (len >= 12) {
				hubver.asic = shub_le32(d);
				hubver.rom = shub_le32(d + 4);
				hubver.patch = shub_le32(d + 8);
				hubver.seen = true;
			}
			break;
		case SHUB_RPC_SENSOR_RSP:
			if (len >= 2 && shub_le16(d) <= len - 2) {
				shub_ssp_rx_add(&ssprx, d + 2, shub_le16(d));
				ssp_stream();
			}
			break;
		case SHUB_RPC_LOG:
		case SHUB_RPC_LOG2:
			if (len >= 2) {
				text_rpc("log", d + 2, (uint16_t)(len - 2));
			}
			break;
		case SHUB_RPC_EXCEPTION:
		case SHUB_RPC_EXCEPTION_TEXT:
			text_rpc("EXCEPTION", d, len);
			break;
		default:
			if (tl.unknown_rpcs++ < 16) {
				say("shub: RPC %x, %u bytes: %02x %02x %02x %02x\n", id, len, len > 0 ? d[0] : 0, len > 1 ? d[1] : 0,
				    len > 2 ? d[2] : 0, len > 3 ? d[3] : 0);
			}
			break;
		}
	}
	if (r < 0 && tl.bad++ < 8) {
		say("shub: a packet's RPCs cut short (%zu bytes)\n", n);
	}
}

static bool
version_seen(void)
{
	return hubver.seen;
}

static bool
ask_version(const char *who)
{
	static const uint8_t req[2] = { SHUB_RPC_VERSION_REQ, 0 };

	hubver.seen = false;
	for (int t = 0; t < VERSION_TRIES; t++) {
		if (!send_reliable(req, sizeof(req))) {
			say("shub: %s: the version request not acknowledged (try %d)\n", who, t + 1);
		}
		pump(VERSION_MS, version_seen);
		if (hubver.seen) {
			say("shub: %s: ASIC %08x, ROM %08x, patch level %08x\n", who, hubver.asic, hubver.rom, hubver.patch);
			return true;
		}
	}
	say("shub: %s: no version\n", who);
	return false;
}

/* ---- step 4: the patch --------------------------------------------------------------------------------------------- */

static bool
load_patch(void)
{
	const uint8_t *data, *patch = NULL;
	uint64_t size = 0;
	struct shub_patch_info info;
	size_t at = 0;
	uint32_t addr, sent = 0;
	uint16_t n;
	uint8_t pay[SHUB_TL_MAX];
	uint64_t t0 = now_ns();

	for (int tries = 0; tries < 20 && patch == NULL; tries++) {
		patch = redwood_image_file("fw", PATCH_FILE, &size, NULL);
		if (patch == NULL) {
			sleep_ms(500);
		}
	}
	if (patch == NULL) {
		say("shub: 4: no fw/%s in the small image (proposals/sensors/mk-fw-sensors.py makes ../fw-sensors/, "
		    "userland/build.sh copies it)\n", PATCH_FILE);
		return false;
	}
	if (!shub_patch_check(patch, (size_t)size, &info)) {
		say("shub: 4: fw/%s (%llu bytes) is not a patch of writes only\n", PATCH_FILE, size);
		return false;
	}
	say("shub: 4: fw/%s: %u writes, %u bytes, %08x-%08x; sending\n", PATCH_FILE, info.records, info.bytes, info.lo,
	    info.hi);
	state = "loading";
	while (shub_patch_next(patch, (size_t)size, &at, &addr, &data, &n) == 1) {
		uint8_t body[4 + 2 + SHUB_PATCH_MAX_RECORD];
		size_t k;

		shub_put32(body, addr);
		body[4] = (uint8_t)n;
		body[5] = (uint8_t)(n >> 8);
		memcpy(body + 6, data, n);
		k = shub_rpc_put(pay, SHUB_RPC_WRITE_MEM, body, (uint16_t)(6 + n));
		if (!send_reliable(pay, (uint16_t)k)) {
			say("shub: 4: write %u of %u (%08x, %u bytes) not acknowledged; stopping\n", sent + 1, info.records, addr,
			    n);
			return false;
		}
		sent++;
		if (sent % 100 == 0) {
			say("shub: 4: %u of %u written\n", sent, info.records);
		}
	}
	say("shub: 4: all %u written in %llu ms; starting it at %08x\n", sent, (now_ns() - t0) / NSEC_PER_MSEC, PATCH_ENTRY);
	{
		uint8_t entry[4];

		shub_put32(entry, PATCH_ENTRY);
		at = shub_rpc_put(pay, SHUB_RPC_START_APP, entry, 4);
		(void)send_packet(0, pay, (uint16_t)at, false);
	}
	sleep_ms(START_MS);
	memset(&tl, 0, sizeof(tl));
	memset(&tlrx, 0, sizeof(tlrx));
	if (!autobaud_and_sync("4 (the application)")) {
		return false;
	}
	return ask_version("4 (the application)");
}

/* ---- SSP ------------------------------------------------------------------------------------------------------- */

static struct {
	uint32_t init_done, frames, frame_errors, unexpected;
	bool answered;
	uint16_t answer_opts;
	uint8_t answer[2048];           /* large for /dev/redwood-gnss's raw SSP reads */
	uint16_t answer_len;
	uint64_t found;                 /* SENSOR_SCANNING's bitmap */
	uint32_t fw_rev;
	uint8_t accel_range;
	uint64_t enabled;
} ssp;

static LCK_GRP_DECLARE(shub_lck_grp, "redwood_ssp");
static LCK_MTX_DECLARE(shub_lock, &shub_lck_grp);       /* last[], the queue: the hub's thread and the readers */

static struct {
	uint8_t data[64];        /* the largest sample: pocket mode, 62 bytes */
	uint8_t size;
	uint64_t count, hub_time, at_ns;
} last[SSP_SENSORS];

static void post_sample(const struct shub_sample *s);

static void
on_sample(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	lck_mtx_lock(&shub_lock);
	memcpy(last[s->sensor].data, s->data, s->size);
	last[s->sensor].size = s->size;
	last[s->sensor].count++;
	last[s->sensor].hub_time = s->time;
	last[s->sensor].at_ns = now_ns();
	lck_mtx_unlock(&shub_lock);
	post_sample(s);
}

/* The hub's own debug text (print_mcu_debug): ~8 lines a second in normal use (482 a minute on 2026-11-13, most
 * of the kernel log). Only with shub_text=1; counted either way (c->debug). */
static uint32_t shub_text;

static void
on_text(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	if (shub_text) {
		text_rpc("says", p, (uint16_t)(n > 150 ? 150 : n));
	}
}

static void
ssp_stream(void)
{
	uint16_t opts;
	const uint8_t *d;
	size_t n, used;
	int k;

	while ((k = shub_ssp_rx_peek(&ssprx, &opts, &d, &n, &used)) != SSP_RX_NONE) {
		if (k == SSP_RX_DATA) {
			struct shub_frame_counts c = { 0 };
			int r = shub_ssp_frame(d, n, &c, NULL, on_sample, on_text);

			ssp.frames++;
			ssp.init_done += c.init_done;
			if (r != SSP_FRAME_OK && ssp.frame_errors++ < 8) {
				size_t at = c.stopped_at;

				char hex[3 * 64 + 1];
				size_t k = 0;

				for (size_t i = 0; i < n && i < 64; i++) {
					k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02x ", d[i]);
				}
				hex[k] = 0;
				say("shub: a data frame of %zu bytes stopped at byte %zu (%s): %s\n", n, at,
				    r == SSP_FRAME_SHORT ? "cut short" : "a record or sensor it does not know", hex);
			}
		} else {
			ssp.answered = true;
			ssp.answer_opts = opts;
			ssp.answer_len = (uint16_t)(n < sizeof(ssp.answer) ? n : sizeof(ssp.answer));
			memcpy(ssp.answer, d, ssp.answer_len);
			ssprx.want_read = 0;
		}
		shub_ssp_rx_consume(&ssprx, used);
	}
}

static bool
ssp_answered(void)
{
	return ssp.answered;
}

static bool
ssp_send(const uint8_t *p, uint16_t n)
{
	uint8_t pay[16 + 2 + 64];
	uint8_t body[2 + 64];
	size_t k;

	if (n > 64) {
		return false;
	}
	if (req_len) {
		body[0] = (uint8_t)n;
		body[1] = (uint8_t)(n >> 8);
		memcpy(body + 2, p, n);
		k = shub_rpc_put(pay, (uint16_t)req_rpc, body, (uint16_t)(n + 2));
	} else {
		k = shub_rpc_put(pay, (uint16_t)req_rpc, p, n);
	}
	return send_packet(0, pay, (uint16_t)k, false);
}

/* one SSP transaction (ssp_i2c.c's ssp_spi_sync): a READ of `len` bytes into buf, a WRITE of `len` from it, or 0 */
static bool
ssp_cmd(uint8_t cmd, uint16_t type, uint32_t arg, uint8_t *buf, uint16_t len)
{
	uint8_t h[9];

	shub_ssp_header(h, cmd, len, type, arg);
	ssp.answered = false;
	ssprx.want_read = type == SSP_AP2HUB_READ ? len : 0;
	if (!ssp_send(h, sizeof(h))) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	pump(SSP_MS, ssp_answered);
	if (!ssp.answered || ssp.answer_opts != type) {
		say("shub: SSP %02x: %s (options %x)\n", cmd, ssp.answered ? "another answer" : "no answer",
		    ssp.answered ? ssp.answer_opts : 0);
		ssprx.want_read = 0;
		return false;
	}
	if (type == SSP_AP2HUB_READ) {
		memcpy(buf, ssp.answer, len < ssp.answer_len ? len : ssp.answer_len);
		return true;
	}
	return ssp_send(buf, len);
}

static bool
init_seen(void)
{
	return ssp.init_done > 0;
}

/* the AP's time to the hub: the header and its 8 bytes sent as one, no go-ahead awaited (bbd_send_timestamp()) */
static void
send_time(void)
{
	uint8_t m[9 + 8];
	uint64_t ns = now_ns();

	shub_ssp_header(m, SSP_CURRENT_TIMESTAMP, 8, SSP_AP2HUB_WRITE, 0);
	for (int i = 0; i < 8; i++) {
		m[9 + i] = (uint8_t)(ns >> (8 * i));
	}
	(void)ssp_send(m, sizeof(m));
}


/* initialize_mcu() of the S20's ssp_dev.c, the parts that matter without Android's files (no calibration from /efs) */
static bool
ssp_init(void)
{
	uint8_t b[32] = { 0 };

	pump(INIT_DONE_MS, init_seen);
	say("shub: 5: %s\n", ssp.init_done ? "the hub's sensors are set up (record 0x0A)" :
	    "no \"sensor init done\" in 1 s (Samsung's driver goes on too)");
	for (int t = 0; t < 3; t++) {          /* get_chipid(): three tries, 5 ms apart */
		if (ssp_cmd(SSP_WHOAMI, SSP_AP2HUB_READ, 0, b, 1) && b[0] == SSP_WHOAMI_ANSWER) {
			break;
		}
		sleep_ms(5);
	}
	if (b[0] != SSP_WHOAMI_ANSWER) {
		say("shub: 5: WHOAMI: %02x (not 0x55): stopping\n", b[0]);
		return false;
	}
	say("shub: 5: WHOAMI 0x55\n");
	b[0] = AP_TYPE;
	b[1] = (uint8_t)ap_rev;
	if (!ssp_cmd(SSP_AP_INFORMATION, SSP_AP2HUB_WRITE, 0, b, 2)) {
		say("shub: 5: the AP's information not taken\n");
	}
	b[0] = (uint8_t)acc_position;
	b[1] = (uint8_t)acc_position;
	b[2] = (uint8_t)mag_position;
	if (!ssp_cmd(SSP_SENSOR_FORMATION, SSP_AP2HUB_WRITE, 0, b, 3)) {
		say("shub: 5: the formation not taken\n");
	}
	b[0] = GLASS_TYPE;
	if (!ssp_cmd(SSP_GLASS_TYPE, SSP_AP2HUB_WRITE, 0, b, 1)) {
		say("shub: 5: the glass type not taken\n");
	}
	memset(b, 0, sizeof(b));
	if (ssp_cmd(SSP_SENSOR_SCANNING, SSP_AP2HUB_READ, 0, b, 8)) {
		ssp.found = (uint64_t)shub_le32(b) | (uint64_t)shub_le32(b + 4) << 32;
	}
	if (ssp.found & (1ull << SSP_MAG)) {
		memcpy(b, mag_pdc, sizeof(mag_pdc));
		if (!ssp_cmd(SSP_MAG_STATIC_MATRIX, SSP_AP2HUB_WRITE, 0, b, sizeof(mag_pdc))) {
			say("shub: 5: the magnetometer's matrix not taken\n");
		}
	}
	if (ssp_cmd(SSP_FIRMWARE_REV, SSP_AP2HUB_READ, 0, b, 4)) {
		ssp.fw_rev = shub_le32(b);
	}
	if (ssp_cmd(SSP_GET_ACCEL_RANGE, SSP_AP2HUB_READ, 0, b, 1)) {
		ssp.accel_range = b[0];
	}
	say("shub: 5: the sensors found: %016llx; firmware revision %u; accelerometer range %u\n", ssp.found, ssp.fw_rev,
	    ssp.accel_range);
	send_time();
	return ssp.found != 0;
}

static bool
sensor_on(uint8_t id, uint32_t ms)
{
	uint8_t b[10];

	b[0] = id;
	shub_put32(b + 1, ms);
	shub_put32(b + 5, 0);           /* no batching: every sample as it comes */
	b[9] = 0;
	if (!ssp_cmd(SSP_SENSOR_ADD, SSP_AP2HUB_WRITE, 0, b, sizeof(b))) {
		return false;
	}
	ssp.enabled |= 1ull << id;
	return true;
}

static uint32_t prox_init = 1;
static void
proximity_setup(void)
{
	uint8_t b[8] = {0};
	if (!prox_init || !(ssp.found & (1ull << SSP_PROXIMITY))) return;
	if (!ssp_cmd(0x47, SSP_AP2HUB_READ, 0, b, sizeof(b))) return;
	bool empty = true;
	for (unsigned i = 0; i < sizeof(b); i++) if (b[i]) empty = false;
	if (!empty) return; /* preserve any calibration already supplied by firmware */
	/* Samsung ssp.h's AMS autocal defaults and ssp_i2c.c's 8-byte,
	 * big-endian threshold write. No factory/NVM calibration is overwritten. */
	const uint16_t thresholds[] = {2400, 1600, 16368, 1000};
	for (unsigned i = 0; i < 4; i++) { b[2 * i] = thresholds[i] >> 8; b[2 * i + 1] = thresholds[i] & 255; }
	say("shub: proximity empty thresholds: Samsung defaults %s\n",
	    ssp_cmd(0xf2, SSP_AP2HUB_WRITE, 0, b, sizeof(b)) ? "sent" : "NOT taken");
}

/* Queue diagnostic transactions to the SPI owner; never race an SSP command
 * from the sysctl thread. Raw proximity is off unless explicitly requested. */
static int prox_request = -1;
static bool prox_raw_wanted;
static uint32_t sensor_power_state;
static uint16_t prox_thresholds[4];
static bool prox_thresholds_valid;
static uint8_t prox_info[18];
static bool prox_info_valid;

static void
dbg_shub_prox(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n > 1 || (n && v[0] > 2)) {
		snprintf(out, cap, "shub.prox [0|1|2]: raw off/on; 2 applies Samsung's missing-calibration fallback\n"); return;
	}
	if (n) __atomic_store_n(&prox_request, (int)v[0], __ATOMIC_RELAXED);
	lck_mtx_lock(&shub_lock);
	uint32_t power = __atomic_load_n(&sensor_power_state, __ATOMIC_RELAXED);
	snprintf(out, cap, "display %d light %u prox %u raw %u demand %#x; proximity samples %llu, state %u adc %u; raw samples %llu adc %u; thresholds %s %u %u %u %u; chip %s %#x drive %#x persist %#x pulse %#x gain %#x time %#x pulse_len %#x offset %02x%02x%s\n",
	    redwood_display_is_on, !!(power & (1u << SSP_LIGHT)), !!(power & (1u << SSP_PROXIMITY)),
	    !!(power & (1u << SSP_PROXIMITY_RAW)), redwood_sensors_proximity_mode(),
	    last[SSP_PROXIMITY].count, last[SSP_PROXIMITY].data[0], shub_le16(last[SSP_PROXIMITY].data + 1),
	    last[SSP_PROXIMITY_RAW].count, shub_le16(last[SSP_PROXIMITY_RAW].data),
	    prox_thresholds_valid ? "read" : "unavailable", prox_thresholds[0], prox_thresholds[1],
	    prox_thresholds[2], prox_thresholds[3], prox_info_valid ? "read" : "unavailable", prox_info[0],
	    prox_info[9], prox_info[10], prox_info[11], prox_info[12], prox_info[13], prox_info[14],
	    prox_info[17], prox_info[16], n ? "; request queued" : "");
	lck_mtx_unlock(&shub_lock);
}

static void
proximity_diagnostic(void)
{
	int request = __atomic_exchange_n(&prox_request, -1, __ATOMIC_RELAXED);
	uint8_t b[8] = {0};
	if (request < 0) return;
	prox_raw_wanted = request != 0;
	if (request) {
		if (request == 2) {
			/* Samsung set_prox_cal_to_ssp(): two zero s32s when the
			 * NVM calibration file is absent. Volatile hub state only. */
			say("shub: proximity missing-calibration fallback %s\n",
			    ssp_cmd(0xcf, SSP_AP2HUB_WRITE, 0, b, sizeof(b)) ? "sent" : "NOT taken");
			(void)ssp_cmd(0x58, SSP_AP2HUB_WRITE, 0, b, 4); /* default call_min */
		}
		/* Samsung factory/proximity.c get_proximity_all_threshold(): read8,
		 * little endian. Read calibration, do not replace factory values. */
		bool ok = ssp_cmd(0x47, SSP_AP2HUB_READ, 0, b, sizeof(b));
		lck_mtx_lock(&shub_lock);
		prox_thresholds_valid = ok;
		if (ok) for (unsigned i = 0; i < 4; i++) prox_thresholds[i] = shub_le16(b + 2 * i);
		lck_mtx_unlock(&shub_lock);
		uint8_t info[18] = {0};
		ok = ssp_cmd(0x4c, SSP_AP2HUB_READ, 0, info, sizeof(info));
		lck_mtx_lock(&shub_lock);
		prox_info_valid = ok;
		if (ok) memcpy(prox_info, info, sizeof(info));
		lck_mtx_unlock(&shub_lock);
	}
}

/* ---- into /dev/redwood-sensors ------------------------------------------------------------------------------ */

/* a sample in sys/redwood_sensors.h's form; false when it is not one user space is given */
static bool
convert(const struct shub_sample *s, struct redwood_sensor_sample *o)
{
	const uint8_t *d = s->data;

	memset(o, 0, sizeof(*o));
	o->time = mach_absolute_time();
	o->hub_ns = s->time;
	switch (s->sensor) {
	case SSP_ACCEL:
		o->type = REDWOOD_SENSOR_ACCEL;
		o->v[0] = (int16_t)shub_le16(d);
		o->v[1] = (int16_t)shub_le16(d + 2);
		o->v[2] = (int16_t)shub_le16(d + 4);
		o->scale = ssp.accel_range ? ssp.accel_range : 8;
		return true;
	case SSP_GYRO:
		o->type = REDWOOD_SENSOR_GYRO;
		o->v[0] = (int32_t)shub_le32(d);
		o->v[1] = (int32_t)shub_le32(d + 4);
		o->v[2] = (int32_t)shub_le32(d + 8);
		return true;
	case SSP_MAG:
		o->type = REDWOOD_SENSOR_MAG;
		o->v[0] = (int16_t)shub_le16(d);
		o->v[1] = (int16_t)shub_le16(d + 2);
		o->v[2] = (int16_t)shub_le16(d + 4);
		o->v[3] = d[6];
		return true;
	case SSP_PRESSURE:
		o->type = REDWOOD_SENSOR_PRESSURE;
		o->v[0] = (int32_t)shub_le32(d);      /* 4096 to a hPa (Scion's S9: 4252216 = 1038.1 hPa) */
		o->v[1] = (int16_t)shub_le16(d + 4);
		return true;
	case SSP_LIGHT:
		o->type = REDWOOD_SENSOR_LIGHT;
		o->v[0] = (int32_t)shub_le32(d);      /* lux (CONFIG_SENSORS_SSP_LIGHT_REPORT_LUX) */
		o->v[1] = (int32_t)shub_le32(d + 4);  /* colour temperature */
		o->v[2] = REDWOOD_LIGHT_FRONT;
		o->v[3] = shub_le16(d + 14);          /* the w channel */
		return true;
	case SSP_PROXIMITY:
		o->type = REDWOOD_SENSOR_PROXIMITY;
		o->v[0] = d[0];                         /* 1: something near */
		o->v[1] = shub_le16(d + 1);
		return true;
	default:
		return false;
	}
}

static void
post_sample(const struct shub_sample *s)
{
	struct redwood_sensor_sample o;

	if (convert(s, &o)) {
		redwood_sensors_post(&o);
	}
}

/* ---- the thread ---------------------------------------------------------------------------------------------- */

static void
report(void)
{
	say("shub: hub %s; ROM %08x/%08x, application's version %08x; found %016llx, on %016llx; %u data frames (%u bad), "
	    "%llu bytes in, %llu out, %u SPI errors, %d readers\n", state, hubver.asic, hubver.rom, hubver.patch, ssp.found,
	    ssp.enabled, ssp.frames, ssp.frame_errors, bytes_in, bytes_out, spi_errors, redwood_sensors_readers());
	lck_mtx_lock(&shub_lock);
	for (uint8_t id = 0; id < SSP_SENSORS; id++) {
		const uint8_t *d = last[id].data;

		if (last[id].count == 0) {
			continue;
		}
		switch (id) {
		case SSP_ACCEL:
			say("shub:   accelerometer x %d y %d z %d (%llu samples)\n", (int16_t)shub_le16(d),
			    (int16_t)shub_le16(d + 2), (int16_t)shub_le16(d + 4), last[id].count);
			break;
		case SSP_GYRO:
			say("shub:   gyroscope x %d y %d z %d (%llu)\n", (int32_t)shub_le32(d), (int32_t)shub_le32(d + 4),
			    (int32_t)shub_le32(d + 8), last[id].count);
			break;
		case SSP_MAG:
			say("shub:   magnetometer x %d y %d z %d accuracy %u (%llu)\n", (int16_t)shub_le16(d),
			    (int16_t)shub_le16(d + 2), (int16_t)shub_le16(d + 4), d[6], last[id].count);
			break;
		case SSP_PRESSURE:
			say("shub:   barometer %d (/4096 hPa) temperature %d (%llu)\n", (int32_t)shub_le32(d),
			    (int16_t)shub_le16(d + 4), last[id].count);
			break;
		case SSP_LIGHT:
			say("shub:   light %u lux cct %d r %u g %u b %u w %u gain %u time %u (%llu)\n", shub_le32(d),
			    (int32_t)shub_le32(d + 4), shub_le16(d + 8), shub_le16(d + 10), shub_le16(d + 12), shub_le16(d + 14),
			    shub_le16(d + 16), shub_le16(d + 18), last[id].count);
			break;
		case SSP_PROXIMITY:
			say("shub:   proximity %s adc %u (%llu)\n", d[0] ? "NEAR" : "far", shub_le16(d + 1), last[id].count);
			break;
		default:
			say("shub:   sensor %u: %u bytes, %llu samples\n", id, last[id].size, last[id].count);
			break;
		}
	}
	lck_mtx_unlock(&shub_lock);
}

/*
 * /dev/redwood-gnss: the hub's link for user space, for the GNSS bring-up (A, 2026-11-13; ported from Scion's
 * /dev/hakios-gnss, 9b886e99, their docs/gnss.md; their gnsstool speaks the same records). Root only. Made at shub=6; requests are carried out by the hub thread between its reads.
 *
 * write(): one request per call, little-endian:
 *   'R' [u16 id][data]       an RPC to the hub in a reliable packet (data up to 1000 bytes); answered by an 'A' record
 *   'P' [u16 id][data]       the same in a plain packet
 *   'S' [u8 cmd][u16 options][u32 arg][u16 len][data]   one SSP transaction (ssp_cmd): options 0 READs len bytes
 *                            (up to 2000) and answers with an 'S' record of them; 1 WRITEs data (up to 64 bytes)
 *   A write while a request is outstanding fails with EBUSY.
 * read(): a stream of records [u8 kind][u8 flags][u16 id][u16 len][len bytes]: 'R' every RPC from the hub except SSP's
 * (0x21) as it arrives, 'A' and 'S' the outcome of requests. Records that do not fit the 64 KB ring are dropped.
 */
#define GNSS_DEV_NAME           "redwood-gnss"
#define GNSS_RING               (64 * 1024)
#define GNSS_REQ_MAX            (1 + 2 + 1000)
static uint8_t gnss_ring[GNSS_RING];
static uint32_t gnss_head, gnss_tail, gnss_lost;
static int gnss_readers;
static bool gnss_waiting;
static uint8_t gnss_req[GNSS_REQ_MAX];
static uint16_t gnss_req_len;
static volatile bool gnss_req_pending;  /* written, for the hub thread */
static volatile bool gnss_req_busy;     /* from a write's start to the hub thread's end of it */
static volatile bool gnss_live;         /* the hub thread is in run(), where requests are taken */

static void
gnss_post(uint8_t kind, uint8_t flags, uint16_t id, const uint8_t *d, uint16_t len)
{
	uint8_t h[6] = { kind, flags, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)len, (uint8_t)(len >> 8) };

	lck_mtx_lock(&shub_lock);
	if (gnss_readers > 0) {
		if (GNSS_RING - (gnss_head - gnss_tail) < sizeof(h) + len) {
			gnss_lost++;
		} else {
			for (unsigned i = 0; i < sizeof(h); i++) {
				gnss_ring[gnss_head++ % GNSS_RING] = h[i];
			}
			for (uint16_t i = 0; i < len; i++) {
				gnss_ring[gnss_head++ % GNSS_RING] = d[i];
			}
			if (gnss_waiting) {
				gnss_waiting = false;
				wakeup(&gnss_head);
			}
		}
	}
	lck_mtx_unlock(&shub_lock);
}

static bool
gnss_pending(void)
{
	return gnss_req_pending;
}

/* On the hub thread: carry out the request user space wrote */
static void
gnss_do(void)
{
	static uint8_t pay[SHUB_TL_MAX];
	const uint8_t *r = gnss_req;
	uint16_t n = gnss_req_len;

	if (n >= 3 && (r[0] == 'R' || r[0] == 'P')) {
		uint16_t id = (uint16_t)(r[1] | r[2] << 8);
		size_t k = shub_rpc_put(pay, id, r + 3, (uint16_t)(n - 3));
		bool ok = r[0] == 'R' ? send_reliable(pay, (uint16_t)k) : send_packet(0, pay, (uint16_t)k, false);

		gnss_post(GNSS_REC_SENT, ok, id, NULL, 0);
	} else if (n >= 10 && r[0] == 'S') {
		uint8_t cmd = r[1];
		uint16_t opts = (uint16_t)(r[2] | r[3] << 8), len = (uint16_t)(r[8] | r[9] << 8);
		uint32_t arg = shub_le32(r + 4);
		static uint8_t buf[2048];
		bool ok;

		if (SSP_TYPE(opts) == SSP_AP2HUB_READ) {
			len = len > 2000 ? 2000 : len;
			memset(buf, 0, len);
			ok = ssp_cmd(cmd, opts, arg, buf, len);
			gnss_post(GNSS_REC_SSP, ok, cmd, buf, ok ? len : 0);
		} else {
			len = len > 64 ? 64 : len;
			len = len > n - 10 ? (uint16_t)(n - 10) : len;
			memcpy(buf, r + 10, len);
			ok = ssp_cmd(cmd, opts, arg, buf, len);
			gnss_post(GNSS_REC_SSP, ok, cmd, NULL, 0);
		}
	} else {
		gnss_post(GNSS_REC_SENT, 0, 0xffff, NULL, 0);
	}
	gnss_req_pending = false;
	gnss_req_busy = false;
}

static int
gnss_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&shub_lock);
	gnss_readers++;
	__atomic_store_n(&gnss_poll_users, (uint32_t)gnss_readers, __ATOMIC_RELAXED);
	lck_mtx_unlock(&shub_lock);
	return 0;
}

static int
gnss_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&shub_lock);
	if (gnss_readers > 0 && --gnss_readers == 0) {
		gnss_tail = gnss_head;
	}
	__atomic_store_n(&gnss_poll_users, (uint32_t)gnss_readers, __ATOMIC_RELAXED);
	lck_mtx_unlock(&shub_lock);
	return 0;
}

static int
gnss_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t chunk[512];
	int error = 0;

	lck_mtx_lock(&shub_lock);
	while (gnss_head == gnss_tail) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&shub_lock);
			return EWOULDBLOCK;
		}
		gnss_waiting = true;
		error = msleep(&gnss_head, &shub_lock, PZERO | PCATCH, GNSS_DEV_NAME, NULL);
		if (error != 0) {
			lck_mtx_unlock(&shub_lock);
			return error;
		}
	}
	while (gnss_head != gnss_tail && uio_resid(uio) > 0) {
		uint32_t k = 0;

		while (k < sizeof(chunk) && gnss_head != gnss_tail && (user_ssize_t)k < uio_resid(uio)) {
			chunk[k++] = gnss_ring[gnss_tail++ % GNSS_RING];
		}
		lck_mtx_unlock(&shub_lock);
		error = uiomove((caddr_t)chunk, (int)k, uio);
		lck_mtx_lock(&shub_lock);
		if (error != 0) {
			break;
		}
	}
	lck_mtx_unlock(&shub_lock);
	return error;
}

static int
gnss_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t n = uio_resid(uio);
	int error;

	if (n < 1 || n > GNSS_REQ_MAX) {
		return EINVAL;
	}
	lck_mtx_lock(&shub_lock);
	if (gnss_req_busy || !gnss_live) {
		lck_mtx_unlock(&shub_lock);
		return EBUSY;
	}
	gnss_req_busy = true;
	lck_mtx_unlock(&shub_lock);
	error = uiomove((caddr_t)gnss_req, (int)n, uio);
	if (error == 0) {
		gnss_req_len = (uint16_t)n;
		gnss_req_pending = true;
	} else {
		gnss_req_busy = false;
	}
	return error;
}

static const struct cdevsw gnss_cdevsw = {
	.d_open = gnss_open,
	.d_close = gnss_close,
	.d_read = gnss_read,
	.d_write = gnss_write,
	.d_ioctl = eno_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0,
};

static void
make_gnss_device(void)
{
	int major = cdevsw_add(-1, &gnss_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, GNSS_DEV_NAME) == NULL) {
		say("shub: /dev/%s could not be made\n", GNSS_DEV_NAME);
		return;
	}
	say("shub: /dev/%s made\n", GNSS_DEV_NAME);
}

static void
run(void)
{
	/* light and pressure once a second (auto-brightness needs no more); proximity, when asked for, 200 ms */
	static const struct { uint8_t id; uint32_t ms; } slow[] = { { SSP_LIGHT, 1000 }, { SSP_PROXIMITY, 0 /* prox_ms */ },
		                                                   { SSP_PRESSURE, 1000 } };
	uint32_t frames_at_time = 0;
	uint64_t report_at = now_ns();

	proximity_setup();
	if ((want_sensors & (1u << SSP_LIGHT)) && (ssp.found & (1ull << SSP_LIGHT))) redwood_als_publish();
	for (uint8_t id = 0; id < SSP_SENSORS && id < 32; id++) {
		uint32_t ms = period_ms;
		/* These are exclusively controlled below; no boot-time IR flash. */
		if (id == SSP_LIGHT || id == SSP_PROXIMITY || id == SSP_PROXIMITY_RAW) continue;

		if (!(want_sensors & (1u << id)) || !(ssp.found & (1ull << id))) {
			continue;
		}
		for (unsigned i = 0; i < sizeof(slow) / sizeof(slow[0]); i++) {
			if (slow[i].id == id) {
				ms = slow[i].ms ? slow[i].ms : prox_ms;
			}
		}
		say("shub: 6: sensor %u on, every %u ms: %s\n", id, ms, sensor_on(id, ms) ? "taken" : "NOT taken");
	}
	gnss_live = true;
	state = "running";
	idle_us = 4000;
	while (powered) {
		proximity_diagnostic();
		{       /* All emitter/power changes stay on the SPI owner thread. */
			static const uint8_t ids[] = { SSP_LIGHT, SSP_PROXIMITY, SSP_PROXIMITY_RAW };
			for (unsigned i = 0; i < sizeof(ids); i++) {
				uint8_t id = ids[i];
				bool allowed = id == SSP_PROXIMITY_RAW ? (ssp.found & (1ull << SSP_PROXIMITY)) != 0 :
				    (want_sensors & (1u << id)) && (ssp.found & (1ull << id));
				bool demand = id == SSP_LIGHT || (id == SSP_PROXIMITY_RAW ? prox_raw_wanted :
				    redwood_sensors_proximity_mode() != 0);
				bool want = allowed && redwood_display_is_on && demand;
				uint32_t ms = id == SSP_LIGHT ? light_ms : id == SSP_PROXIMITY_RAW ? 200 : prox_ms;
				bool on = (ssp.enabled & (1ull << id)) != 0;
				if (on == want) {
					continue;
				}
				if (want) {
					(void)sensor_on(id, ms);
				} else {
					uint8_t b[5];
					b[0] = id;
					shub_put32(b + 1, ms);
					if (ssp_cmd(SSP_SENSOR_REMOVE, SSP_AP2HUB_WRITE, 0, b, 5)) {
						ssp.enabled &= ~(1ull << id);
					}
				}
			}
			__atomic_store_n(&sensor_power_state, (uint32_t)ssp.enabled, __ATOMIC_RELAXED);
		}
		if (gnss_req_pending) {
			gnss_do();
		}
		pump(100, gnss_pending);
		if (ssp.frames - frames_at_time >= 3000) {
			frames_at_time = ssp.frames;
			send_time();
		}
		if (now_ns() - report_at >= 30 * NSEC_PER_SEC) {
			report_at = now_ns();
			report();
		}
	}
}

/* steps 2 to `steps`; false where one stopped */
static bool
from_step2(void)
{
	if (!power_on()) {
		return false;
	}
	if (!hello()) {
		say("shub: 2: MCU_RESP did not answer MCU_REQ in 100 ms (HOST_REQ %u)\n", host_req());
	} else {
		say("shub: 2: MCU_RESP %s; HOST_REQ %u\n", no_hello ? "not waited for (shub_hello=1 waits)" : "answered",
		    host_req());
		mcu_req(false);
	}
	(void)exchange(NULL, 0);
	say("shub: 2: %llu bytes read from the chip, status %02x\n", bytes_in, last_status);
	if (steps < 3) {
		return true;
	}
	state = "ROM";
	memset(&tl, 0, sizeof(tl));
	memset(&tlrx, 0, sizeof(tlrx));
	if (!autobaud_and_sync("3") || !ask_version("3")) {
		return false;
	}
	if (steps < 4 || !load_patch()) {
		return steps < 4;
	}
	state = "application";
	if (steps < 5 || !ssp_init()) {
		return steps < 5;
	}
	state = "SSP";
	if (steps >= 6) {
		run();
	}
	return true;
}

static void
shub_thread(void *param, __unused wait_result_t wr)
{
	bool ok;

	steps = (uint32_t)(uintptr_t)param;
	say("shub: the S20's sensor hub, %u steps\n", steps);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	cmu_peric0 = ml_io_map(CMU_PERIC0_PA, 0x4000);
	gpp3 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP3);
	gpp6 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, BANK_GPP6);
	gpb0 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, BANK_GPB0);
	gpm19 = redwood_gpio_bank(CMGP_PA, BANK_GPM19);
	gpa0 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA0);
	gpa1 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA1);
	gpa4 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA4);
	if (!cmu_top || !cmu_peric0 || !gpp3 || !gpp6 || !gpb0 || !gpm19 || !gpa0 || !gpa1 || !gpa4) {
		say("shub: a block could not be mapped\n");
		return;
	}
	if (!look() || steps < 2) {
		say("shub: done after step 1\n");
		return;
	}
	ok = from_step2();
	if (keep == 0 || (!ok && keep < 2)) {
		power_off(ok ? "the steps are done (shub_keep=0)" : "a step failed");
	}
	say("shub: done: %llu bytes read, %llu sent, %u packets, %u refused, %u SPI errors, %u flow-control retries, %u "
	    "times MCU_RESP did not come\n", bytes_in, bytes_out, tl.packets, tl.bad, spi_errors, fc_retries, hello_fails);
	report();
}

void
redwood_ssp_init(void)
{
	uint32_t n = 0, h = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("shub", &n, sizeof(n)) || n == 0) {
		return;
	}
	n = n > 6 ? 6 : n;
	PE_parse_boot_argn("shub_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("shub_log", &log_frames, sizeof(log_frames));
	PE_parse_boot_argn("shub_text", &shub_text, sizeof(shub_text));
	PE_parse_boot_argn("shub_prox_ms", &prox_ms, sizeof(prox_ms));
	prox_ms = prox_ms < 100 ? 100 : prox_ms;
	PE_parse_boot_argn("shub_light_ms", &light_ms, sizeof(light_ms));
	/* Keep samples inside the automatic controller's three-second freshness window. */
	light_ms = light_ms < 100 ? 100 : light_ms > 1000 ? 1000 : light_ms;
	PE_parse_boot_argn("shub_div", &div_ratio, sizeof(div_ratio));
	PE_parse_boot_argn("shub_ap_rev", &ap_rev, sizeof(ap_rev));
	PE_parse_boot_argn("shub_sensors", &want_sensors, sizeof(want_sensors));
	PE_parse_boot_argn("shub_prox_init", &prox_init, sizeof(prox_init));
	PE_parse_boot_argn("shub_ms", &period_ms, sizeof(period_ms));
	PE_parse_boot_argn("shub_req_rpc", &req_rpc, sizeof(req_rpc));
	PE_parse_boot_argn("shub_req_len", &req_len, sizeof(req_len));
	/* the hub's mounting of the accelerometer/gyroscope and the magnetometer (the dtbo's ssp-acc-position and
	 * ssp-mag-position): the S20's 6 and 5 by default; the S20+ (y2s, SM-G985F) has acc 7, mag 5 */
	PE_parse_boot_argn("shub_acc_pos", &acc_position, sizeof(acc_position));
	PE_parse_boot_argn("shub_mag_pos", &mag_position, sizeof(mag_position));
	acc_position = acc_position > 7 ? ACC_POSITION : acc_position;
	mag_position = mag_position > 7 ? MAG_POSITION : mag_position;
	if (PE_parse_boot_argn("shub_hello", &h, sizeof(h))) {
		no_hello = h == 0;
	}
	period_ms = period_ms < 5 ? 5 : period_ms;
	if (n >= 6) {
		redwood_sensors_device();       /* now, so that backboardd finds it when it starts */
		make_gnss_device();             /* /dev/redwood-gnss: raw RPCs and SSP for user space (GNSS bring-up) */
		redwood_dbgport_add("shub.prox", "[0|1]: cached proximity readings; optional raw diagnostic sampling", dbg_shub_prox);
	}
	if (kernel_thread_start(shub_thread, (void *)(uintptr_t)n, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * The run's end, a panic's way out: the hub into standby (NSTDBY low, MCU_REQ low; no SPI, no sleeping). The supply
 * (LDO21M) is left as it is: the next boot's step 1 says what it found.
 */
void
redwood_ssp_quiesce(void)
{
	if (powered && gpp6 != NULL) {
		powered = false;
		redwood_gpio_set(gpp6, PIN_NSTDBY, false);
		redwood_gpio_set(gpb0, PIN_MCU_REQ, false);
		redwood_dsay("shub: at the run's end: the hub in standby (NSTDBY low)\n");
	}
}
