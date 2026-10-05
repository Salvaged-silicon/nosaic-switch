/*
 * fm6000-si5338 -- Configure the Si5338 "Quartzy" clock chip on the 7150S-52.
 *
 * The FM6000 SSCHED clock domain is fed by the Si5338 oscillator.  Without
 * correct configuration the FM6000 frame-handler PLL locks to a fallback and
 * the scheduler ring never advances (FOUND never set, FoundTok=0).
 *
 * Hardware path:
 *   SB800 GPIO191 (AcpiMmio 0xFED801BF bit6) enables the Quartzy oscillator.
 *   Si5338 at I2C addr 0x70 on SCD SMBus master 0x8080 bus 1.
 *   SCD BAR0 at /sys/bus/pci/devices/0000:04:00.0/resource0.
 *
 * Usage:
 *   fm6000-si5338 read     -- check lock status (read-only)
 *   fm6000-si5338 config   -- program Rosa-Quartzy (AN428 sequence)
 *
 * Register config data: Rosa-Quartzy-0101.si5338, Arista EOS 4.14.16M.
 * SCD SMBus protocol: derived independently from GPL scd-smbus.h register map.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- SCD layout --------------------------------------------------------- */

#define SCD_RESOURCE  "/sys/bus/pci/devices/0000:04:00.0/resource0"
#define SCD_BAR0_SIZE 0x40000u

/*
 * SCD SMBus master at BAR0+0x8080, bus select 1 = Si5338.
 * (scd-setup.sh: "smbus_master 0x8080 1 2  # psu0 + Si5338 refclk osc")
 * Register offsets within the master block (from GPL scd-smbus.h):
 */
#define SCD_SMBUS_MASTER       0x8080u
#define SCD_SMBUS_SI5338_BUS   1

#define SMBUS_REQ_OFF  0x10u
#define SMBUS_CS_OFF   0x20u
#define SMBUS_RESP_OFF 0x30u

/* CS register */
#define CS_RST  (1u << 31)
#define CS_FE   (1u << 30)
#define CS_FOE  (1u << 13)

/* Response register error bits */
#define RSP_D_MASK   0xffu
#define RSP_ACK_ERR  (1u << 10)
#define RSP_TIMEOUT  (1u <<  9)
#define RSP_CONFLICT (1u <<  8)

static volatile uint32_t *scd_bar0;
static unsigned mb_req, mb_cs, mb_resp;

static void scd_wr32(unsigned off, uint32_t v)
{
    __sync_synchronize();
    scd_bar0[off / 4] = v;
    __sync_synchronize();
}

static uint32_t scd_rd32(unsigned off)
{
    __sync_synchronize();
    return scd_bar0[off / 4];
}

/* ---- SCD SMBus request register ---------------------------------------- */

/*
 * Field layout (from GPL scd-smbus.h):
 *   [7:0]  d   data byte
 *   [13:8] ss  sequence size (first request in transaction only)
 *   [14]   ed  (unused here)
 *   [15]   br  bus reset
 *   [19:18] t  timing (default 1)
 *   [20]   sp  last byte in sequence
 *   [21]   da  data-ack (read continuation)
 *   [22]   dod (unused here, set for writes)
 *   [23]   st  START condition
 *   [27:24] bs bus select
 *   [31:28] ti transaction index
 */
#define MK_REQ(d,ss,t,sp,da,dod,st,bs,ti) \
    ( (uint32_t)((d)  & 0xff)      | \
      (uint32_t)((ss) & 0x3f) << 8 | \
      (uint32_t)((t)  & 0x3)  << 18| \
      (uint32_t)((sp) & 1)    << 20| \
      (uint32_t)((da) & 1)    << 21| \
      (uint32_t)((dod)& 1)    << 22| \
      (uint32_t)((st) & 1)    << 23| \
      (uint32_t)((bs) & 0xf)  << 24| \
      (uint32_t)((ti) & 0xf)  << 28)

#define BS  SCD_SMBUS_SI5338_BUS
#define SI5338_ADDR  0x70u

/* ---- Low-level SMBus master -------------------------------------------- */

static void smbus_reset(void)
{
    uint32_t cs = scd_rd32(mb_cs);
    scd_wr32(mb_cs, cs | CS_RST | CS_FOE);
    usleep(50000);
    scd_wr32(mb_cs, (cs & ~CS_RST) | CS_FOE);
    usleep(10000);
}

static int smbus_wait(void)
{
    for (int i = 0; i < 5000; i++) {
        uint32_t cs = scd_rd32(mb_cs);
        if (cs & CS_FE) {
            scd_wr32(mb_cs, cs);   /* clear fe */
            return 0;
        }
        usleep(100);
    }
    fprintf(stderr, "si5338: smbus timeout cs=0x%08x\n", scd_rd32(mb_cs));
    return -1;
}

static uint32_t smbus_pop_resp(void)
{
    return scd_rd32(mb_resp);
}

/*
 * I2C write:  START addr(W) reg data STOP    (3 bytes, ss=3)
 */
static int si5338_write(uint8_t reg, uint8_t data)
{
    uint32_t cs = scd_rd32(mb_cs);
    if (cs & (CS_FE | 0x3ffu))
        smbus_reset();
    scd_wr32(mb_cs, scd_rd32(mb_cs) | CS_FOE);

    scd_wr32(mb_req, MK_REQ((SI5338_ADDR<<1)|0, 3, 1, 0, 0, 1, 1, BS, 0));
    scd_wr32(mb_req, MK_REQ(reg,  0, 1, 0, 0, 1, 0, BS, 1));
    scd_wr32(mb_req, MK_REQ(data, 0, 1, 1, 0, 1, 0, BS, 2));

    if (smbus_wait() != 0) return -1;

    for (int i = 0; i < 3; i++) {
        uint32_t r = smbus_pop_resp();
        if (r & (RSP_ACK_ERR | RSP_TIMEOUT | RSP_CONFLICT)) {
            fprintf(stderr, "si5338: write r%-3d=0x%02x resp[%d]=0x%08x\n",
                    reg, data, i, r);
            return -1;
        }
    }
    return 0;
}

/*
 * I2C read:  START addr(W) reg  RESTART addr(R) data(NACK) STOP  (ss=4)
 */
static int si5338_read(uint8_t reg)
{
    uint32_t cs = scd_rd32(mb_cs);
    if (cs & (CS_FE | 0x3ffu))
        smbus_reset();
    scd_wr32(mb_cs, scd_rd32(mb_cs) | CS_FOE);

    scd_wr32(mb_req, MK_REQ((SI5338_ADDR<<1)|0, 4, 1, 0, 0, 1, 1, BS, 0));
    scd_wr32(mb_req, MK_REQ(reg,               0, 1, 0, 0, 1, 0, BS, 1));
    scd_wr32(mb_req, MK_REQ((SI5338_ADDR<<1)|1, 0, 1, 0, 0, 1, 1, BS, 2));
    scd_wr32(mb_req, MK_REQ(0,                 0, 1, 1, 0, 0, 0, BS, 3));

    if (smbus_wait() != 0) return -1;

    uint32_t last = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t r = smbus_pop_resp();
        if (r & (RSP_ACK_ERR | RSP_TIMEOUT | RSP_CONFLICT)) {
            fprintf(stderr, "si5338: read r%-3d resp[%d]=0x%08x\n",
                    reg, i, r);
            return -1;
        }
        last = r;
    }
    return (int)(last & RSP_D_MASK);
}

/*
 * AN428 configReg: mask=0 skip, mask=0xFF full write, else read-modify-write.
 */
static int si5338_rmw(int addr, int val, int mask)
{
    if (mask == 0)       return 0;
    if (mask == 0xFF)    return si5338_write(addr, val);
    int cur = si5338_read(addr);
    if (cur < 0)         return cur;
    return si5338_write(addr, (cur & ~mask) | (val & mask));
}

/* ---- AcpiMmio GPIO ------------------------------------------------------ */

static int gpio_enable_quartzy(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "si5338: /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    volatile uint8_t *m = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0xFED80000u);
    close(fd);
    if (m == MAP_FAILED) {
        fprintf(stderr, "si5338: mmap AcpiMmio: %s\n", strerror(errno));
        return -1;
    }
    uint8_t prev = m[0x1bf];
    m[0xdbf] = 0x01;   /* GPIO191: enable output */
    m[0x1bf] = 0x40;   /* GPIO191 = HIGH (Quartzy on) */
    __sync_synchronize();
    fprintf(stderr, "si5338: GPIO191 0x%02x -> 0x%02x\n", prev, (unsigned)m[0x1bf]);
    munmap((void *)m, 0x2000);
    usleep(50000);
    return 0;
}

/* ---- Rosa-Quartzy-0101 config data -------------------------------------- */
/*
 * Extracted from Arista EOS 4.14.16M firmware: Rosa-Quartzy-0101.si5338.
 * Format per AN428: {addr, val, mask}.  mask=0 → skip.
 * Entry {255, 1/0, 0xFF} switches the Si5338 page register.
 * Input frequency: 156.250000 MHz.
 */
static const struct { uint8_t addr, val, mask; } si5338_cfg[] = {
    {   0, 0x00, 0x00 },
    {   1, 0x00, 0x00 },
    {   2, 0x00, 0x00 },
    {   3, 0x00, 0x00 },
    {   4, 0x00, 0x00 },
    {   5, 0x00, 0x00 },
    {   6, 0x08, 0x1D },
    {   7, 0x00, 0x00 },
    {   8, 0x70, 0x00 },
    {   9, 0x0F, 0x00 },
    {  10, 0x00, 0x00 },
    {  11, 0x00, 0x00 },
    {  12, 0x00, 0x00 },
    {  13, 0x00, 0x00 },
    {  14, 0x00, 0x00 },
    {  15, 0x00, 0x00 },
    {  16, 0x00, 0x00 },
    {  17, 0x00, 0x00 },
    {  18, 0x00, 0x00 },
    {  19, 0x00, 0x00 },
    {  20, 0x00, 0x00 },
    {  21, 0x00, 0x00 },
    {  22, 0x00, 0x00 },
    {  23, 0x00, 0x00 },
    {  24, 0x00, 0x00 },
    {  25, 0x00, 0x00 },
    {  26, 0x00, 0x00 },
    {  27, 0x70, 0x80 },
    {  28, 0x03, 0xFF },
    {  29, 0x20, 0xFF },
    {  30, 0xA0, 0xFF },
    {  31, 0xC0, 0xFF },
    {  32, 0xC1, 0xFF },
    {  33, 0xC0, 0xFF },
    {  34, 0xC0, 0xFF },
    {  35, 0x15, 0xFF },
    {  36, 0x04, 0x1F },
    {  37, 0x00, 0x1F },
    {  38, 0x06, 0x1F },
    {  39, 0x01, 0x1F },
    {  40, 0x10, 0xFF },
    {  41, 0x12, 0x7F },
    {  42, 0x37, 0x3F },
    {  43, 0x00, 0x00 },
    {  44, 0x00, 0x00 },
    {  45, 0x00, 0xFF },
    {  46, 0x00, 0xFF },
    {  47, 0x14, 0x3F },
    {  48, 0x33, 0xFF },
    {  49, 0x05, 0xFF },
    {  50, 0xC4, 0xFF },
    {  51, 0x07, 0xFF },
    {  52, 0x10, 0x7F },
    {  53, 0x00, 0xFF },
    {  54, 0x08, 0xFF },
    {  55, 0x00, 0xFF },
    {  56, 0x00, 0xFF },
    {  57, 0x00, 0xFF },
    {  58, 0x00, 0xFF },
    {  59, 0x01, 0xFF },
    {  60, 0x00, 0xFF },
    {  61, 0x00, 0xFF },
    {  62, 0x00, 0x3F },
    {  63, 0x10, 0x7F },
    {  64, 0x00, 0xFF },
    {  65, 0x08, 0xFF },
    {  66, 0x00, 0xFF },
    {  67, 0x00, 0xFF },
    {  68, 0x00, 0xFF },
    {  69, 0x00, 0xFF },
    {  70, 0x01, 0xFF },
    {  71, 0x00, 0xFF },
    {  72, 0x00, 0xFF },
    {  73, 0x00, 0x3F },
    {  74, 0x10, 0x7F },
    {  75, 0x00, 0xFF },
    {  76, 0x08, 0xFF },
    {  77, 0x00, 0xFF },
    {  78, 0x00, 0xFF },
    {  79, 0x00, 0xFF },
    {  80, 0x00, 0xFF },
    {  81, 0x01, 0xFF },
    {  82, 0x00, 0xFF },
    {  83, 0x00, 0xFF },
    {  84, 0x00, 0x3F },
    {  85, 0x10, 0x7F },
    {  86, 0x00, 0xFF },
    {  87, 0x30, 0xFF },
    {  88, 0x00, 0xFF },
    {  89, 0x00, 0xFF },
    {  90, 0x00, 0xFF },
    {  91, 0x00, 0xFF },
    {  92, 0x01, 0xFF },
    {  93, 0x00, 0xFF },
    {  94, 0x00, 0xFF },
    {  95, 0x00, 0x3F },
    {  96, 0x10, 0x00 },
    {  97, 0x00, 0xFF },
    {  98, 0x7B, 0xFF },
    {  99, 0x00, 0xFF },
    { 100, 0x00, 0xFF },
    { 101, 0x00, 0xFF },
    { 102, 0x00, 0xFF },
    { 103, 0x01, 0xFF },
    { 104, 0x00, 0xFF },
    { 105, 0x00, 0xFF },
    { 106, 0x80, 0xBF },
    { 107, 0x00, 0xFF },
    { 108, 0x00, 0x7F },
    { 109, 0x00, 0xFF },
    { 110, 0x40, 0xFF },
    { 111, 0x00, 0xFF },
    { 112, 0x00, 0x7F },
    { 113, 0x00, 0xFF },
    { 114, 0x40, 0xFF },
    { 115, 0x00, 0xFF },
    { 116, 0x80, 0xFF },
    { 117, 0x00, 0xFF },
    { 118, 0x40, 0xFF },
    { 119, 0x00, 0xFF },
    { 120, 0x00, 0xFF },
    { 121, 0x00, 0xFF },
    { 122, 0x40, 0xFF },
    { 123, 0x00, 0xFF },
    { 124, 0x00, 0xFF },
    { 125, 0x00, 0xFF },
    { 126, 0x00, 0xFF },
    { 127, 0x00, 0xFF },
    { 128, 0x00, 0xFF },
    { 129, 0x00, 0x0F },
    { 130, 0x00, 0x0F },
    { 131, 0x00, 0xFF },
    { 132, 0x00, 0xFF },
    { 133, 0x00, 0xFF },
    { 134, 0x00, 0xFF },
    { 135, 0x00, 0xFF },
    { 136, 0x00, 0xFF },
    { 137, 0x00, 0xFF },
    { 138, 0x00, 0xFF },
    { 139, 0x00, 0xFF },
    { 140, 0x00, 0xFF },
    { 141, 0x00, 0xFF },
    { 142, 0x00, 0xFF },
    { 143, 0x00, 0xFF },
    { 144, 0x00, 0xFF },
    { 145, 0x00, 0x00 },
    { 146, 0xFF, 0x00 },
    { 147, 0x00, 0x00 },
    { 148, 0x00, 0x00 },
    { 149, 0x00, 0x00 },
    { 150, 0x00, 0x00 },
    { 151, 0x00, 0x00 },
    { 152, 0x00, 0xFF },
    { 153, 0x00, 0xFF },
    { 154, 0x00, 0xFF },
    { 155, 0x00, 0xFF },
    { 156, 0x00, 0xFF },
    { 157, 0x00, 0xFF },
    { 158, 0x00, 0x0F },
    { 159, 0x00, 0x0F },
    { 160, 0x00, 0xFF },
    { 161, 0x00, 0xFF },
    { 162, 0x00, 0xFF },
    { 163, 0x00, 0xFF },
    { 164, 0x00, 0xFF },
    { 165, 0x00, 0xFF },
    { 166, 0x00, 0xFF },
    { 167, 0x00, 0xFF },
    { 168, 0x00, 0xFF },
    { 169, 0x00, 0xFF },
    { 170, 0x00, 0xFF },
    { 171, 0x00, 0xFF },
    { 172, 0x00, 0xFF },
    { 173, 0x00, 0xFF },
    { 174, 0x00, 0xFF },
    { 175, 0x00, 0xFF },
    { 176, 0x00, 0xFF },
    { 177, 0x00, 0xFF },
    { 178, 0x00, 0xFF },
    { 179, 0x00, 0xFF },
    { 180, 0x00, 0xFF },
    { 181, 0x00, 0x0F },
    { 182, 0x00, 0xFF },
    { 183, 0x00, 0xFF },
    { 184, 0x00, 0xFF },
    { 185, 0x00, 0xFF },
    { 186, 0x00, 0xFF },
    { 187, 0x00, 0xFF },
    { 188, 0x00, 0xFF },
    { 189, 0x00, 0xFF },
    { 190, 0x00, 0xFF },
    { 191, 0x00, 0xFF },
    { 192, 0x00, 0xFF },
    { 193, 0x00, 0xFF },
    { 194, 0x00, 0xFF },
    { 195, 0x00, 0xFF },
    { 196, 0x00, 0xFF },
    { 197, 0x00, 0xFF },
    { 198, 0x00, 0xFF },
    { 199, 0x00, 0xFF },
    { 200, 0x00, 0xFF },
    { 201, 0x00, 0xFF },
    { 202, 0x00, 0xFF },
    { 203, 0x00, 0x0F },
    { 204, 0x00, 0xFF },
    { 205, 0x00, 0xFF },
    { 206, 0x00, 0xFF },
    { 207, 0x00, 0xFF },
    { 208, 0x00, 0xFF },
    { 209, 0x00, 0xFF },
    { 210, 0x00, 0xFF },
    { 211, 0x00, 0xFF },
    { 212, 0x00, 0xFF },
    { 213, 0x00, 0xFF },
    { 214, 0x00, 0xFF },
    { 215, 0x00, 0xFF },
    { 216, 0x00, 0xFF },
    { 217, 0x00, 0xFF },
    { 218, 0x00, 0x00 },
    { 219, 0x00, 0x00 },
    { 220, 0x00, 0x00 },
    { 221, 0x0D, 0x00 },
    { 222, 0x00, 0x00 },
    { 223, 0x00, 0x00 },
    { 224, 0xF4, 0x00 },
    { 225, 0xF0, 0x00 },
    { 226, 0x00, 0x00 },
    { 227, 0x00, 0x00 },
    { 228, 0x00, 0x00 },
    { 229, 0x00, 0x00 },
    { 231, 0x00, 0x00 },
    { 232, 0x00, 0x00 },
    { 233, 0x00, 0x00 },
    { 234, 0x00, 0x00 },
    { 235, 0x00, 0x00 },
    { 236, 0x00, 0x00 },
    { 237, 0x00, 0x00 },
    { 238, 0x14, 0x00 },
    { 239, 0x00, 0x00 },
    { 240, 0x00, 0x00 },
    { 242, 0x00, 0x02 },
    { 243, 0xF0, 0x00 },
    { 244, 0x00, 0x00 },
    { 245, 0x00, 0x00 },
    { 247, 0x00, 0x00 },
    { 248, 0x00, 0x00 },
    { 249, 0xA8, 0x00 },
    { 250, 0x00, 0x00 },
    { 251, 0x84, 0x00 },
    { 252, 0x00, 0x00 },
    { 253, 0x00, 0x00 },
    { 254, 0x00, 0x00 },
    { 255, 0x01, 0xFF },
    {   0, 0x00, 0x00 },
    {   1, 0x00, 0x00 },
    {   2, 0x00, 0x00 },
    {   3, 0x00, 0x00 },
    {   4, 0x00, 0x00 },
    {   5, 0x00, 0x00 },
    {   6, 0x00, 0x00 },
    {   7, 0x00, 0x00 },
    {   8, 0x00, 0x00 },
    {   9, 0x00, 0x00 },
    {  10, 0x00, 0x00 },
    {  11, 0x00, 0x00 },
    {  12, 0x00, 0x00 },
    {  13, 0x00, 0x00 },
    {  14, 0x00, 0x00 },
    {  15, 0x00, 0x00 },
    {  16, 0x00, 0x00 },
    {  17, 0x01, 0x00 },
    {  18, 0x00, 0x00 },
    {  19, 0x00, 0x00 },
    {  20, 0x90, 0x00 },
    {  21, 0x31, 0x00 },
    {  22, 0x00, 0x00 },
    {  23, 0x00, 0x00 },
    {  24, 0x01, 0x00 },
    {  25, 0x00, 0x00 },
    {  26, 0x00, 0x00 },
    {  27, 0x00, 0x00 },
    {  28, 0x00, 0x00 },
    {  29, 0x00, 0x00 },
    {  30, 0x00, 0x00 },
    {  31, 0x00, 0xFF },
    {  32, 0x00, 0xFF },
    {  33, 0x01, 0xFF },
    {  34, 0x00, 0xFF },
    {  35, 0x00, 0xFF },
    {  36, 0x90, 0xFF },
    {  37, 0x31, 0xFF },
    {  38, 0x00, 0xFF },
    {  39, 0x00, 0xFF },
    {  40, 0x01, 0xFF },
    {  41, 0x00, 0xFF },
    {  42, 0x00, 0xFF },
    {  43, 0x00, 0x0F },
    {  44, 0x00, 0x00 },
    {  45, 0x00, 0x00 },
    {  46, 0x00, 0x00 },
    {  47, 0x00, 0xFF },
    {  48, 0x00, 0xFF },
    {  49, 0x01, 0xFF },
    {  50, 0x00, 0xFF },
    {  51, 0x00, 0xFF },
    {  52, 0x90, 0xFF },
    {  53, 0x31, 0xFF },
    {  54, 0x00, 0xFF },
    {  55, 0x00, 0xFF },
    {  56, 0x01, 0xFF },
    {  57, 0x00, 0xFF },
    {  58, 0x00, 0xFF },
    {  59, 0x00, 0x0F },
    {  60, 0x00, 0x00 },
    {  61, 0x00, 0x00 },
    {  62, 0x00, 0x00 },
    {  63, 0x00, 0xFF },
    {  64, 0x00, 0xFF },
    {  65, 0x01, 0xFF },
    {  66, 0x00, 0xFF },
    {  67, 0x00, 0xFF },
    {  68, 0x90, 0xFF },
    {  69, 0x31, 0xFF },
    {  70, 0x00, 0xFF },
    {  71, 0x00, 0xFF },
    {  72, 0x01, 0xFF },
    {  73, 0x00, 0xFF },
    {  74, 0x00, 0xFF },
    {  75, 0x00, 0x0F },
    {  76, 0x00, 0x00 },
    {  77, 0x00, 0x00 },
    {  78, 0x00, 0x00 },
    {  79, 0x00, 0xFF },
    {  80, 0x00, 0xFF },
    {  81, 0x00, 0xFF },
    {  82, 0x00, 0xFF },
    {  83, 0x00, 0xFF },
    {  84, 0x90, 0xFF },
    {  85, 0x31, 0xFF },
    {  86, 0x00, 0xFF },
    {  87, 0x00, 0xFF },
    {  88, 0x01, 0xFF },
    {  89, 0x00, 0xFF },
    {  90, 0x00, 0xFF },
    {  91, 0x00, 0x0F },
    {  92, 0x00, 0x00 },
    {  93, 0x00, 0x00 },
    {  94, 0x00, 0x00 },
    { 255, 0x00, 0xFF },
};

/* ---- SCD BAR0 open ------------------------------------------------------ */

static int scd_open(void)
{
    int fd = open(SCD_RESOURCE, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "si5338: open %s: %s\n", SCD_RESOURCE, strerror(errno));
        return -1;
    }
    scd_bar0 = mmap(NULL, SCD_BAR0_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);
    if (scd_bar0 == MAP_FAILED) {
        fprintf(stderr, "si5338: mmap SCD BAR0: %s\n", strerror(errno));
        return -1;
    }
    mb_req  = SCD_SMBUS_MASTER + SMBUS_REQ_OFF;
    mb_cs   = SCD_SMBUS_MASTER + SMBUS_CS_OFF;
    mb_resp = SCD_SMBUS_MASTER + SMBUS_RESP_OFF;
    fprintf(stderr, "si5338: SCD BAR0 mapped (cs=0x%08x)\n", scd_rd32(mb_cs));
    return 0;
}

/* ---- Commands ----------------------------------------------------------- */

/* Ensure the Si5338 is on register page 0. */
static void set_page0(void)
{
    int p = si5338_read(255);
    if (p > 0)
        si5338_write(255, 0);
}

static int cmd_read(void)
{
    if (scd_open() != 0) return 1;
    smbus_reset();

    int s = si5338_read(218);
    if (s < 0) {
        fprintf(stderr, "si5338: no ACK on SCD SMBus master 0x8080 bus 1\n");
        return 1;
    }
    int r6  = si5338_read(6);
    int r27 = si5338_read(27);
    int r28 = si5338_read(28);
    int r29 = si5338_read(29);
    int r49 = si5338_read(49);
    int r230 = si5338_read(230);
    int r241 = si5338_read(241);
    fprintf(stderr,
        "si5338: reg218=0x%02x  %s  (LOS_CLKIN=%d LOS_FDBK=%d LOL=%d CAL=%d)\n"
        "        r6=0x%02x r27=0x%02x r28=0x%02x r29=0x%02x r49=0x%02x r230=0x%02x r241=0x%02x\n",
        s, (s & 0x15) == 0 ? "LOCKED" : "NOT-LOCKED",
        !!(s & 0x04), !!(s & 0x08), !!(s & 0x10), !!(s & 0x01),
        r6, r27, r28, r29, r49, r230, r241);
    return (s & 0x15) == 0 ? 0 : 2;
}

static int cmd_dump(void)
{
    if (scd_open() != 0) return 1;
    smbus_reset();
    set_page0();
    fprintf(stderr, "si5338: DUMP page0:\n");
    for (int r = 0; r < 256; r++) {
        if ((r & 0xf) == 0) fprintf(stderr, "%02x:", r);
        int v = si5338_read(r);
        fprintf(stderr, " %02x", v < 0 ? 0xff : v);
        if ((r & 0xf) == 0xf) fprintf(stderr, "\n");
    }
    return 0;
}

/*
 * Relock: restart the PLL lock sequence WITHOUT re-streaming config.
 * Use this when the config is already written (Rosa-Quartzy values confirmed)
 * but the PLL is not locked — e.g. after a failed config run left FCAL state
 * inconsistent.
 */
static int cmd_relock(void)
{
    if (scd_open() != 0) return 1;
    smbus_reset();
    set_page0();

    { int v = si5338_read(230); if (v >= 0) si5338_write(230, v | 0x10); }   /* disable outputs */
    { int v = si5338_read(241); if (v >= 0) si5338_write(241, v | 0x80); }   /* pause LOL */
    { int v = si5338_read(49);  if (v >= 0) si5338_write(49, v & ~0x80); }   /* FCAL override OFF */
    si5338_write(246, 0x02);                                                   /* soft reset */
    { int v = si5338_read(241); if (v >= 0) si5338_write(241, v & ~0x80); }   /* restart LOL */
    usleep(25000);

    int locked = 0;
    for (int i = 0; i < 20; i++) {
        int v = si5338_read(218);
        if (v >= 0 && (v & 0x15) == 0) { locked = 1; break; }
        usleep(1000);
    }

    int r218 = si5338_read(218);
    fprintf(stderr, "si5338: relock reg218=0x%02x  PLL %s (AN428 criteria)\n",
            r218, locked ? "LOCKED" : "not locked -- proceeding (ignoreLos=true)");

    /* Always copy FCAL result and enable override (EOS ignoreLos=true behavior). */
    { int lo = si5338_read(235), mm = si5338_read(236), hi = si5338_read(237);
      int o  = si5338_read(47);
      si5338_write(45, lo >= 0 ? lo : 0);
      si5338_write(46, mm >= 0 ? mm : 0);
      si5338_write(47, (o >= 0 ? (o & 0xFC) : 0) | (hi >= 0 ? (hi & 3) : 0));
      fprintf(stderr, "si5338: FCAL: r45=0x%02x r46=0x%02x r47[1:0]=0x%02x (from r235/236/237)\n",
              lo & 0xff, mm & 0xff, hi >= 0 ? (hi & 3) : 0); }
    { int v = si5338_read(49); if (v >= 0) si5338_write(49, v | 0x80); }      /* FCAL override ON */
    { int v = si5338_read(230); if (v >= 0) si5338_write(230, v & ~0x10); }   /* enable outputs */

    return locked ? 0 : 2;
}

static int cmd_config(void)
{
    if (gpio_enable_quartzy() != 0)
        fprintf(stderr, "si5338: GPIO enable failed -- continuing\n");

    if (scd_open() != 0) return 1;
    smbus_reset();

    /* Probe */
    if (si5338_read(0) < 0) {
        fprintf(stderr, "si5338: no ACK -- check wiring / SCD master\n");
        return 1;
    }
    fprintf(stderr, "si5338: device present on bus %d addr 0x%02x\n",
            SCD_SMBUS_SI5338_BUS, SI5338_ADDR);

    /* AN428 preConfig */
    if (si5338_rmw(230, 0x10, 0x10) < 0) return 1;  /* disable outputs */
    if (si5338_rmw(241, 0x80, 0x80) < 0) return 1;  /* pause LOL */

    /* AN428 stream: write all non-skip registers in table order */
    int n = (int)(sizeof(si5338_cfg) / sizeof(si5338_cfg[0]));
    int written = 0;
    for (int i = 0; i < n; i++) {
        int a = si5338_cfg[i].addr;
        int v = si5338_cfg[i].val;
        int m = si5338_cfg[i].mask;
        if (m == 0) continue;
        if (si5338_rmw(a, v, m) < 0) {
            fprintf(stderr, "si5338: stream write failed at reg %d\n", a);
            return 1;
        }
        written++;
    }
    fprintf(stderr, "si5338: %d registers written\n", written);

    /* AN428 postConfig */
    set_page0();   /* ensure page 0 before post-config register writes */

    /* wait for input clock (LOS_CLKIN = reg218 bit2 = 0) */
    for (int i = 0; i < 50; i++) {
        int s = si5338_read(218);
        if (s >= 0 && (s & 0x04) == 0) {
            fprintf(stderr, "si5338: input clock present\n");
            break;
        }
        if (i == 49)
            fprintf(stderr, "si5338: WARNING: LOS_CLKIN set (reg218=0x%02x)\n",
                    si5338_read(218));
        usleep(1000);
    }

    /* AN428 postConfig: disable FCAL override, soft reset, restart LOL. */
    { int v = si5338_read(49); if (v >= 0) si5338_write(49, v & ~0x80); }
    si5338_write(246, 0x02);   /* soft reset (leave set; chip holds it until calibration done) */
    { int v = si5338_read(241); if (v >= 0) si5338_write(241, v & ~0x80); }
    usleep(25000);

    int locked = 0;
    for (int i = 0; i < 20; i++) {
        int s = si5338_read(218);
        if (s >= 0 && (s & 0x15) == 0) { locked = 1; break; }
        usleep(1000);
    }

    int r218 = si5338_read(218);
    fprintf(stderr, "si5338: reg218=0x%02x  PLL %s (ignoreLos=true: continuing)\n",
            r218, locked ? "LOCKED" : "not locked");

    /*
     * Always copy FCAL calibration result and enable FCAL override.
     * EOS uses ignoreLos=True: it always enables FCAL override and
     * output drive regardless of lock status — the Si5338 on this
     * board may show LOS_FDBK without impacting the FM6000 reference.
     */
    { int lo = si5338_read(235), mm = si5338_read(236), hi = si5338_read(237);
      int o  = si5338_read(47);
      si5338_write(45, lo >= 0 ? lo : 0);
      si5338_write(46, mm >= 0 ? mm : 0);
      si5338_write(47, (o >= 0 ? (o & 0xFC) : 0) | (hi >= 0 ? (hi & 3) : 0));
      fprintf(stderr, "si5338: FCAL: r45=0x%02x r46=0x%02x r47[1:0]=0x%02x (from r235/236/237)\n",
              lo & 0xff, mm & 0xff, hi >= 0 ? (hi & 3) : 0); }
    { int v = si5338_read(49); if (v >= 0) si5338_write(49, v | 0x80); }     /* FCAL override ON */
    { int v = si5338_read(230); if (v >= 0) si5338_write(230, v & ~0x10); }  /* enable outputs */
    usleep(2000);

    fprintf(stderr, "si5338: Quartzy configured (PLL %s, outputs enabled)\n",
            locked ? "locked" : "not locked by AN428 criteria but outputs on");
    return 0;   /* always succeed, as EOS does with ignoreLos=true */
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fm6000-si5338 read|dump|config|relock\n");
        return 2;
    }
    if (strcmp(argv[1], "read")   == 0) return cmd_read();
    if (strcmp(argv[1], "dump")   == 0) return cmd_dump();
    if (strcmp(argv[1], "config") == 0) return cmd_config();
    if (strcmp(argv[1], "relock") == 0) return cmd_relock();
    fprintf(stderr, "fm6000-si5338: unknown command '%s'\n", argv[1]);
    return 2;
}
