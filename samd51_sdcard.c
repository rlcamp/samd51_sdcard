/* TODO: something is not working quite right here when running off a non-48 MHz clock */
#include "samd51_sdcard.h"

#if __has_include(<samd51.h>)
/* newer cmsis-atmel from upstream */
#include <samd51.h>
#else
/* older cmsis-atmel from adafruit */
#include <samd.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <limits.h>

#define BAUD_RATE_SLOW 250000
#define BAUD_RATE_FAST 24000000ULL
/* note this baud rate is too fast for DMA to keep up if doing two-way DRE-triggered DMA */

static_assert(((48000000ULL / (2U * BAUD_RATE_FAST) - 1U) + 1U) * (2U * BAUD_RATE_FAST) == 48000000ULL,
              "baud rate not possible");

#define IDMA_SPI_WRITE 2

/* do not use SECTION_DMAC_DESCRIPTOR because the linker script does not define hsram */
__attribute__((weak, aligned(16))) DmacDescriptor dmac_descriptors[8] = { 0 }, dmac_writeback[8] = { 0 };

extern void yield(void);
__attribute((weak)) void yield(void) { }

size_t card_overhead_numerator = 0, card_overhead_denominator = 0;

static void spi_dma_init(void) {
    /* if dma has not yet been initted... */
    if (!DMAC->BASEADDR.bit.BASEADDR) {
        /* init ahb clock for dmac */
        MCLK->AHBMASK.bit.DMAC_ = 1;

        DMAC->CTRL.bit.DMAENABLE = 0;
        DMAC->CTRL.bit.SWRST = 1;

        DMAC->BASEADDR.bit.BASEADDR = (unsigned long)dmac_descriptors;
        DMAC->WRBADDR.bit.WRBADDR = (unsigned long)dmac_writeback;

        /* re-enable dmac */
        DMAC->CTRL.reg = (DMAC_CTRL_Type) { .bit = { .DMAENABLE = 1, .LVLEN0 = 1, .LVLEN1 = 1, .LVLEN2 = 1, .LVLEN3 = 1 } }.reg;
    }

    /* reset channel */
    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 0;
    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.SWRST = 1;

    /* clear sw trigger */
    DMAC->SWTRIGCTRL.reg &= ~(1 << IDMA_SPI_WRITE);

    NVIC_EnableIRQ(DMAC_2_IRQn);
    NVIC_SetPriority(DMAC_2_IRQn, (1 << __NVIC_PRIO_BITS) - 1);
    static_assert(2 == IDMA_SPI_WRITE, "dmac channel isr mismatch");

    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.reg = (DMAC_CHCTRLA_Type) { .bit = {
        .RUNSTDBY = 1,
        .TRIGSRC = 0x07, /* trigger when sercom1 is ready to send a new byte/word */
        .TRIGACT = DMAC_CHCTRLA_TRIGACT_BURST_Val, /* one burst per trigger */
        .BURSTLEN = DMAC_CHCTRLA_BURSTLEN_SINGLE_Val /* one burst = one beat */
    }}.reg;
}

static void cs_high(void) {
    PORT->Group[0].OUTSET.reg = 1U << 14;
}

static void cs_low(void) {
    PORT->Group[0].OUTCLR.reg = 1U << 14;
}

static void spi_disable(void) {
    /* prior to disabling the SERCOM, make sure the CLK pin doesn't float up */
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 0 } };
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 0 } };

    SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

static void spi_enable(void) {
    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

    /* return control of the CLK pin to the SERCOM */
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
}

static void spi_change_baud_rate(unsigned long baudrate) {
    spi_disable();

    SERCOM1->SPI.BAUD.reg = 48000000ULL / (2U * baudrate) - 1U;

    spi_enable();
}

void spi_sd_shutdown(void) {
    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 0;

    SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

    NVIC_DisableIRQ(DMAC_2_IRQn);
    NVIC_ClearPendingIRQ(DMAC_2_IRQn);

    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);

    MCLK->APBAMASK.bit.SERCOM1_ = 0;

    /* deinit cs pin */
    PORT->Group[0].PINCFG[14].reg = 0;
    PORT->Group[0].DIRCLR.reg = 1U << 14;
    PORT->Group[0].OUTCLR.reg = 1U << 14;

    /* deinit other pins */
    PORT->Group[0].PINCFG[17].reg = 0;
    PORT->Group[0].DIRCLR.reg = 1U << 17;
    PORT->Group[1].PINCFG[23].reg = 0;
    PORT->Group[1].DIRCLR.reg = 1U << 23;
}

static void spi_init(unsigned long baudrate) {
    /* configure pin PA14 (silkscreen pin "D4" on the feather m4) as output for cs pin */
    PORT->Group[0].OUTSET.reg = 1U << 14;
    PORT->Group[0].PINCFG[14].reg = 0;
    PORT->Group[0].DIRSET.reg = 1U << 14;

    /* configure pin PA17 ("SCK" on feather m4) to use functionality C (sercom1 pad 1), drive
     strength 0, for sck, AND make sure it stays low when we momentarily disable PMUXEN */
    PORT->Group[0].OUTCLR.reg = 1U << 17;
    PORT->Group[0].DIRSET.reg = 1U << 17;
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
    PORT->Group[0].PMUX[17 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB23 ("MO" on feather m4) to use functionality C (sercom1 pad 3), drive strength 0, for mosi */
    PORT->Group[1].OUTSET.reg = 1U << 23;
    PORT->Group[1].DIRSET.reg = 1U << 23;
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
    PORT->Group[1].PMUX[23 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB22 ("MI" on feather m4) to use functionality C (sercom1 pad 2), input enabled, for miso */
    PORT->Group[1].PINCFG[22] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .INEN = 1 } };
    PORT->Group[1].PMUX[22 >> 1].bit.PMUXE = 0x2;

    /* clear all interrupts */
    NVIC_ClearPendingIRQ(SERCOM1_1_IRQn);

    MCLK->APBAMASK.reg |= MCLK_APBAMASK_SERCOM1;

    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].reg = (GCLK_PCHCTRL_Type) { .bit = {
        /* if GCLK0 is not 48 MHz, assume GCLK1 is */
        .GEN = GCLK_GENCTRL_SRC_DFLL_Val == GCLK->GENCTRL[0].bit.SRC ? GCLK_PCHCTRL_GEN_GCLK0_Val : GCLK_PCHCTRL_GEN_GCLK1_Val,
        .CHEN = 1
    }}.reg;
    while (!GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);

    /* reset spi peripheral */
    SERCOM1->SPI.CTRLA.bit.SWRST = 1;
    while (SERCOM1->SPI.CTRLA.bit.SWRST || SERCOM1->SPI.SYNCBUSY.bit.SWRST);

    SERCOM1->SPI.CTRLA = (SERCOM_SPI_CTRLA_Type) { .bit = {
        .MODE = 0x3, /* spi peripheral is in master mode */
        .DOPO = 0x2, /* clock is sercom pad 1, MOSI is pad 3 */
        .DIPO = 0x2, /* MISO is sercom pad 2 */
        .CPOL = 0, /* sck is low when idle */
        .CPHA = 0,
        .DORD = 0, /* msb first */
        .RUNSTDBY = 1
    }};

    SERCOM1->SPI.CTRLB = (SERCOM_SPI_CTRLB_Type) { .bit = {
        .RXEN = 0, /* spi receive is not enabled until needed */
        .MSSEN = 0, /* no hardware cs control */
        .CHSIZE = 0 /* eight bit characters */
    }};
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.CTRLC.bit.DATA32B = 1;

    SERCOM1->SPI.BAUD.reg = 48000000ULL / (2U * baudrate) - 1U;

    spi_dma_init();

    /* enable spi peripheral */
    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

static char writing_a_block, must_flush_write;
static unsigned char card_write_response;

static void spi_send_nonblocking_start(const void * buf, const size_t count) {
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = count / 4,
        .SRCADDR.reg = ((size_t)buf) + count,
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = { .bit = {
            .VALID = 1,
            .BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val,
            .SRCINC = 1,
            .DSTINC = 0, /* write to the same register every time */
            .BEATSIZE = DMAC_BTCTRL_BEATSIZE_WORD_Val, /* transfer 32 bits per beat */
        }}
    };

    /* clear pending interrupt from before */
    DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* enable interrupt on write completion */
    DMAC->Channel[IDMA_SPI_WRITE].CHINTENSET.bit.TCMPL = 1;

    /* reset the crc */
    DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0 }.reg;
    DMAC->CRCCHKSUM.reg = 0;
    DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0x20 + IDMA_SPI_WRITE }.reg;

    /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
    __DSB();

    /* setting this starts the transaction */
    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;
}

__attribute((always_inline)) inline
static uint8_t spi_receive_one_byte_with_rx_enabled(void) {
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.DATA.bit.DATA = 0xff;
    card_overhead_numerator++;
    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    return SERCOM1->SPI.DATA.bit.DATA;
}

static_assert(2 == IDMA_SPI_WRITE, "dmac channel isr mismatch");
void DMAC_2_Handler(void) {
    if (!(DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.bit.TCMPL)) return;
    DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* grab the CRC that the DMAC calculated on the outgoing 512 bytes... */
    while (DMAC->CRCSTATUS.bit.CRCBUSY);
    const uint16_t crc = DMAC->CRCCHKSUM.reg;

    /* need to wait on TXC before updating the LENGTH register */
    while (!SERCOM1->SPI.INTFLAG.bit.TXC);

    /* tell the SERCOM that the next transaction will be two bytes */
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 2 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    /* blocking send of crc. card expects high byte of CRC first, samd51 sends low byte of DATA first */
    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = __builtin_bswap16(crc);
    card_overhead_numerator += 2;

    /* wait for crc to complete sending before enabling rx */
    while (!SERCOM1->SPI.INTFLAG.bit.TXC);

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    /* blocking receive of one byte */
    SERCOM1->SPI.DATA.bit.DATA = 0xFF;
    card_overhead_numerator++;
    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    card_write_response = SERCOM1->SPI.DATA.bit.DATA;

    writing_a_block = 0;

    /* arm an321 page 22 fairly strongly suggests this is necessary prior to exception
     return (or will be on future processors) when the first thing the processor wants
     to do afterward depends on state changed in the handler */
    __DSB();
}

static void wait_for_card_ready(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* loop until card pulls MISO high for a full byte worth of clocks */
    while (spi_receive_one_byte_with_rx_enabled() != 0xff);
}

static int wait_for_card_ready_with_timeout(unsigned count) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* loop until card pulls MISO high for a full byte worth of clocks */
    while (--count)
        if (0xFF == spi_receive_one_byte_with_rx_enabled()) return 0;
    return -1;
}

static int spi_sd_flush_write_block(void) {
    if (!must_flush_write) return 0;

    while (*(volatile char *)&writing_a_block) yield();
    /* ensure we don't prefetch the below value before the above condition becomes true */
    __DMB();

    const uint16_t response = card_write_response;

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    /* loop until card pulls MISO high for a full word worth of clocks */
    do {
        SERCOM1->SPI.DATA.bit.DATA = 0xFFFFFFFF;
        card_overhead_numerator += 4;
        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    } while (SERCOM1->SPI.DATA.bit.DATA != 0xFFFFFFFF);

    /* assuming we spun on WFE in one of the above, re-raise the event register explicitly,
     so that caller does not have to assume calling this function may have cleared it */
    __SEV();

    must_flush_write = 0;

    return response != 0xE5 ? -1 : 0;
}

static void spi_sd_start_writing_a_block(const void * buf) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = 0xfc;
    card_overhead_numerator++;

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    must_flush_write = 1;
    writing_a_block = 1;
    spi_send_nonblocking_start(buf, 512);
    card_overhead_numerator += 512;
    card_overhead_denominator += 512;
}

static void spi_send(const void * buf, const size_t size) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    const size_t whole_words = size / 4, rem = size % 4;

    if (whole_words) {
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        for (size_t iword = 0; iword < whole_words; iword++) {
            while (!SERCOM1->SPI.INTFLAG.bit.DRE);
            SERCOM1->SPI.DATA.bit.DATA = ((const uint32_t *)buf)[iword];
        }

        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    }

    if (rem) {
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = rem }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        const char * in = ((const char *)buf) + whole_words * 4;
        SERCOM1->SPI.DATA.bit.DATA = (3 == rem ? in[0] | in[1] << 8U | in[2] << 16U :
                                      2 == rem ? in[0] | in[1] << 8U : in[0]);
        /* when sending one byte in 32 bit mode we apparently need to wait for TXC, not DRE */
        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    }
    card_overhead_numerator += size;
}

static uint32_t spi_receive_uint32be(void) {
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;
    card_overhead_numerator += 4;
    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    const uint32_t bits = SERCOM1->SPI.DATA.bit.DATA;

    return __builtin_bswap32(bits);
}

static uint8_t r1_response(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    uint8_t result, attempts = 0;
    while (0xFF == (result = spi_receive_one_byte_with_rx_enabled()) && attempts++ < 8);
    return result;
}

static unsigned char crc7_left_shifted(const unsigned char * restrict const message, const size_t length) {
    const unsigned char polynomial = 0b10001001;
    unsigned char crc = 0;

    for (size_t ibyte = 0; ibyte < length; ibyte++) {
        crc ^= message[ibyte];

        for (size_t ibit = 0; ibit < 8; ibit++)
            crc = (crc & 0x80u) ? (crc << 1) ^ (polynomial << 1) : (crc << 1);
    }

    return crc & 0xfe;
}

static uint8_t command_and_r1_response(const uint8_t cmd, const uint32_t arg) {
    unsigned char msg[6] = { cmd | 0x40, arg >> 24, arg >> 16, arg >> 8, arg, 0x01 };
    msg[5] |= crc7_left_shifted(msg, 5);

    spi_send(msg, 6);

    /* CMD12 wants an extra byte prior to the response */
    if (12 == cmd) spi_send((unsigned char[1]) { 0xff }, 1);

    return r1_response();
}

int spi_sd_init(void) {
    card_overhead_numerator = 0;
    card_overhead_denominator = 0;
    /* spin for the recommended 1 ms, assuming each loop iteration takes at least 2 cycles */
    /* TODO: replace with some other guarantee that it has been > 1 ms since powerup */
    for (size_t idelay = 0; idelay < F_CPU / 2048; idelay++) asm volatile("" :::);

    spi_init(BAUD_RATE_SLOW);

    /* clear miso */
    cs_low();
    spi_send((unsigned char[1]) { 0xff }, 1);

    cs_high();

    /* and then clock out at least 74 cycles at 100-400 kBd with cs pin held high */
    spi_send((unsigned char[10]) { [0 ... 9] = 0xFF }, 10);

    /* cmd0 */
    for (size_t ipass = 0;; ipass++) {
        cs_low();

        /* send cmd0 */
        const uint8_t cmd0_r1_response = command_and_r1_response(0, 0);
        if (-1 == wait_for_card_ready_with_timeout(65536)) return -1;

        cs_high();

        if (0x01 == cmd0_r1_response) break;

        /* if card likely not present, give up */
        if (1024 == ipass && 255 == cmd0_r1_response) return -1;
    }

    /* cmd8 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(8, 0x1AA);

        if (0x1 != r1_response) {
            cs_high();
            continue;
        }

        const uint32_t response = spi_receive_uint32be();
        cs_high();

        if (0x1AA == response) break;
    }

    /* cmd55, then acmd41 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t cmd55_r1_response = command_and_r1_response(55, 0);
        cs_high();

        if (cmd55_r1_response > 1) continue;

        cs_low();
        wait_for_card_ready();

        const uint8_t acmd41_r1_response = command_and_r1_response(41, 1U << 30);
        cs_high();

        if (!acmd41_r1_response) break;
    }

    spi_change_baud_rate(BAUD_RATE_FAST);

    /* cmd58 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(58, 0);

        if (r1_response > 1) {
            cs_high();
            continue;
        }

        const unsigned int ocr = spi_receive_uint32be();
        (void)ocr;
        cs_high();

        break;
    }

    /* cmd16 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(16, 512);
        cs_high();

        if (r1_response <= 1) break;
    }

#if 1
    /* cmd59 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(59, 1);

        cs_high();

        if (r1_response <= 1) break;
    }
#endif

    return 0;
}

static int spi_sd_write_blocks_start(unsigned long long block_address) {
    cs_low();
    wait_for_card_ready();

    const uint8_t response = command_and_r1_response(25, block_address);
    if (response != 0) {
        cs_high();
        return -1;
    }

    /* extra byte prior to data packet */
    spi_send((unsigned char[1]) { 0xff }, 1);

    return 0;
}

static void spi_sd_write_blocks_end(void) {
    /* send stop tran token */
    spi_send((unsigned char[2]) { 0xfd, 0xff }, 2);

    wait_for_card_ready();

    cs_high();
}

static unsigned long long next_write_block_address = ULLONG_MAX;
static const void * known_safe_nonblocking_src = NULL;
static const void * known_pre_erase = NULL;
static unsigned long known_pre_erase_blocks = 0;

static void spi_sd_write_pre_erase(unsigned long blocks) {
    while (1) {
        cs_low();
        wait_for_card_ready();

         const uint8_t cmd55_r1_response = command_and_r1_response(55, 0);

         cs_high();

         if (cmd55_r1_response > 1) continue;

         cs_low();
        wait_for_card_ready();

         const uint8_t acmd23_r1_response = command_and_r1_response(23, blocks);

         cs_high();

         if (!acmd23_r1_response) break;
    }
}

static int spi_sd_finish_multiblock_write_and_leave_enabled(void) {
    if (next_write_block_address != ULLONG_MAX) {
    /* this will block, but will internally call yield() and __WFI() */
        if (-1 == spi_sd_flush_write_block()) return -1;

        spi_sd_write_blocks_end();
        next_write_block_address = ULLONG_MAX;
    }
    else spi_enable();

    return 0;
}

void spi_sd_mark_pointer_for_pre_erase(const void * p, const unsigned long blocks) {
    known_pre_erase = p;
    known_pre_erase_blocks = blocks;
}

void spi_sd_mark_pointer_for_non_blocking_write(const void * p) {
    known_safe_nonblocking_src = p;
}

int spi_sd_start_writing_next_block(const void * buf, const unsigned long long block_address) {
    if (next_write_block_address != ULLONG_MAX) {
        /* this will block, but will internally call yield() and __WFI() */
        if (-1 == spi_sd_flush_write_block()) return -1;
    }
    else spi_enable();

    if (next_write_block_address != block_address) {
        if (ULLONG_MAX != next_write_block_address)
            spi_sd_write_blocks_end();

        if (buf == known_pre_erase) {
            known_pre_erase = NULL;
            spi_sd_write_pre_erase(known_pre_erase_blocks);
        }

        if (-1 == spi_sd_write_blocks_start(block_address))
            return -1;
    }

    spi_sd_start_writing_a_block(buf);
    next_write_block_address = block_address + 1;

    if (buf == known_safe_nonblocking_src)
        known_safe_nonblocking_src = NULL;
    else
        if (-1 == spi_sd_flush_write_block()) return -1;

    return 0;
}

int spi_sd_read_block(void * buf, unsigned long long block_address) {
    if (1 == spi_sd_finish_multiblock_write_and_leave_enabled()) return -1;

    /* TODO: use cmd18 and optimize for case when called with consecutive block addresses */

    cs_low();
    wait_for_card_ready();

    /* send cmd17 and enable rx */
    if (command_and_r1_response(17, block_address) != 0) return -1;

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    uint8_t result;
    /* this can loop for a while */
    while (0xFF == (result = spi_receive_one_byte_with_rx_enabled()));

    /* when we break out of the above loop, we've read the Data Token byte */
    if (0xFE != result) {
        cs_high();
        return -1;
    }

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    uint32_t * restrict const block = (uint32_t *)buf;
    /* loop over 4-byte words in the 512-byte block */
    for (size_t iword = 0; iword < 128; iword++) {
        SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;
        card_overhead_numerator += 4;
        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
        block[iword] = SERCOM1->SPI.DATA.bit.DATA;
    }

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 2 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    /* read and discard two crc bytes */
    SERCOM1->SPI.DATA.bit.DATA = 0xFFFF;
    card_overhead_numerator += 2;
    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    const uint16_t crc_swapped = SERCOM1->SPI.DATA.bit.DATA;
    (void)crc_swapped;

    cs_high();

    return 0;
}

int spi_sd_flush_and_sleep(void) {
    if (-1 == spi_sd_finish_multiblock_write_and_leave_enabled()) return -1;

    spi_disable();

    return 0;
}
