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
#include <stdio.h>

#define IDMA_SPI_WRITE 2
#define IDMA_SPI_READ 1

/* do not use SECTION_DMAC_DESCRIPTOR because the linker script does not define hsram */
__attribute__((weak, aligned(16))) DmacDescriptor dmac_descriptors[8] = { 0 }, dmac_writeback[8] = { 0 };

extern void yield(void);
__attribute((weak)) void yield(void) { }

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

    /* reset channel */
    DMAC->Channel[IDMA_SPI_READ].CHCTRLA.bit.ENABLE = 0;
    DMAC->Channel[IDMA_SPI_READ].CHCTRLA.bit.SWRST = 1;

    /* clear sw trigger */
    DMAC->SWTRIGCTRL.reg &= ~(1 << IDMA_SPI_READ);

    DMAC->Channel[IDMA_SPI_READ].CHCTRLA.reg = (DMAC_CHCTRLA_Type) { .bit = {
        .RUNSTDBY = 1,
        .TRIGSRC = 0x06, /* trigger when sercom1 has received */
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
    /* disable rx */
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* put back in one-byte mode */
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

    /* return control of the CLK pin to the SERCOM */
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
}

void spi_sd_shutdown(void) {
    DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 0;
    DMAC->Channel[IDMA_SPI_READ].CHCTRLA.bit.ENABLE = 0;

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

static void spi_init() {
    /* configure pin PA14 (silkscreen pin "D4" on the feather m4) as output for cs pin */
    PORT->Group[0].OUTSET.reg = 1U << 14;
    PORT->Group[0].PINCFG[14].reg = 0;
    PORT->Group[0].DIRSET.reg = 1U << 14;

    /* configure pin PA17 ("SCK" on feather m4) to use functionality C (sercom1 pad 1), drive
     strength 0, for sck, AND make sure it stays low when we momentarily disable PMUXEN */
    PORT->Group[0].OUTCLR.reg = 1U << 17;
    PORT->Group[0].DIRSET.reg = 1U << 17;
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
    PORT->Group[0].PMUX[17 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB23 ("MO" on feather m4) to use functionality C (sercom1 pad 3), drive strength 0, for mosi */
    PORT->Group[1].OUTSET.reg = 1U << 23;
    PORT->Group[1].DIRSET.reg = 1U << 23;
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
    PORT->Group[1].PMUX[23 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB22 ("MI" on feather m4) to use functionality C (sercom1 pad 2), input enabled, for miso */
    PORT->Group[1].PINCFG[22] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .INEN = 1 } };
    PORT->Group[1].PMUX[22 >> 1].bit.PMUXE = 0x2;

    /* clear all interrupts */
    NVIC_ClearPendingIRQ(SERCOM1_1_IRQn);

    MCLK->APBAMASK.bit.SERCOM1_ = 1;

    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].reg = (GCLK_PCHCTRL_Type) { .bit = {
        /* unconditionally use GCLK0 as the clock ref */
        .GEN = GCLK_PCHCTRL_GEN_GCLK0_Val,
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

    /* this results in 400 kBd at 120 MHz, lower at lower */
    SERCOM1->SPI.BAUD.reg = 149;

    spi_dma_init();

    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    /* enable spi peripheral */
    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

__attribute((always_inline)) inline
static uint8_t spi_receive_one_byte_with_rx_enabled(void) {
    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = 0xff;

    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    return SERCOM1->SPI.DATA.bit.DATA;
}

static_assert(2 == IDMA_SPI_WRITE, "dmac channel isr mismatch");
void DMAC_2_Handler(void) {
    /* note we don't clear the interrupt flag, we just disable the interrupt. this allows
     the main thread to see that the interrupt has fired, while still waking from sleep
     without needing sevonpend */
    if (DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.bit.TCMPL)
        DMAC->Channel[IDMA_SPI_WRITE].CHINTENCLR.reg = (DMAC_CHINTENCLR_Type) { .bit.TCMPL = 1 }.reg;
}

static void wait_for_card_ready(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;

    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    if (0xffffffff != SERCOM1->SPI.DATA.bit.DATA)
        do {
            while (!SERCOM1->SPI.INTFLAG.bit.DRE);
            SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;

            while (!SERCOM1->SPI.INTFLAG.bit.RXC) { __SEV(); yield(); };
        } while (SERCOM1->SPI.DATA.bit.DATA != 0xffffffff);

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);
}

static void spi_send(const void * buf, const size_t size) {
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
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = (3 == rem ? in[0] | in[1] << 8U | in[2] << 16U :
                                      2 == rem ? in[0] | in[1] << 8U : in[0]);
        /* when sending one byte in 32 bit mode we apparently need to wait for TXC, not DRE */
        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    }

    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);
}

static uint32_t spi_receive_uint32be(void) {
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;

    while (!SERCOM1->SPI.INTFLAG.bit.RXC);
    const uint32_t bits = SERCOM1->SPI.DATA.bit.DATA;

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
    SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
    while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    return __builtin_bswap32(bits);
}

static uint8_t r1_response(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    uint8_t result, attempts = 0;
    /* sd and mmc agree on max 8 attempts, mmc requires at least two attempts */
    while (0xFF == (result = spi_receive_one_byte_with_rx_enabled()) && attempts++ < 8);

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);

    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

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

static void send_command_with_crc7(const uint8_t cmd, const uint32_t arg) {
    unsigned char msg[6] = { cmd | 0x40, arg >> 24, arg >> 16, arg >> 8, arg, 0x01 };
    msg[5] |= crc7_left_shifted(msg, 5);

    spi_send(msg, 6);
}

static uint8_t command_and_r1_response(const uint8_t cmd, const uint32_t arg) {
    send_command_with_crc7(cmd, arg);
    return r1_response();
}

void spi_sd_restore_baud_rate(void) {
    /* use mclk/4 as the baud rate */
    SERCOM1->SPI.BAUD.reg = 1;
}

int spi_sd_init(unsigned baud_rate_reduction) {
    /* NOTE: we need to not call this until it has been about 1 ms since power was applied */
    spi_init();

    /* clear miso */
    cs_low();
    spi_send((unsigned char[1]) { 0xff }, 1);
    cs_high();

    /* and then clock out at least 74 cycles at 100-400 kBd with cs pin held high */
    spi_send((unsigned char[10]) { [0 ... 9] = 0xFF }, 10);

    /* cmd0, software reset. TODO: is looping this even necessary? */
    for (size_t ipass = 0;; ipass++) {
        /* if card likely not present, give up */
        if (ipass > 1024) {
            spi_disable();
            return -1;
        }
        cs_low();

        /* send cmd0 */
        const uint8_t cmd0_r1_response = command_and_r1_response(0, 0);
        cs_high();

        if (0x01 == cmd0_r1_response) break;

        /* give other stuff a chance to run if we are looping */
        __SEV(); yield();
    }

    /* cmd8, check voltage range and test pattern */
    for (size_t ipass = 0;; ipass++) {
        if (ipass > 3) {
            /* TODO: find out how many times we should try this before giving up */
            spi_disable();
            return -1;
        }
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

    /* cmd59, re-enable crc feature, which is disabled by cmd0 */
    cs_low();
    wait_for_card_ready();
    if (command_and_r1_response(59, 1) > 1) {
        cs_high();
        spi_disable();
        return -1;
    }
    cs_high();

    /* cmd55, then acmd41, init. must loop this until the response is 0 */
    for (size_t ipass = 0;; ipass++) {
        if (ipass > 2500) {
            /* each pass takes about 20 bytes minimum, give up after one second at 400 kBd */
            spi_disable();
            return -1;
        }
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

    /* now bump the baud rate up to the max allowable speed (mclk/4) */
    spi_disable();
    SERCOM1->SPI.BAUD.reg = 1 + baud_rate_reduction;
    spi_enable();

    /* TODO: if any of the following fail, restart the procedure with a lower baud rate */
    do {
        /* cmd58, read ocr register */
        cs_low();
        wait_for_card_ready();
        if (command_and_r1_response(58, 0) > 1) break;

        const unsigned int ocr = spi_receive_uint32be();
        (void)ocr;
        cs_high();

        /* cmd16, set block length to 512 */
        cs_low();
        wait_for_card_ready();
        if (command_and_r1_response(16, 512) > 1) break;
        cs_high();

        /* we get here on overall success of this function */
        cs_high();
        spi_disable();
        return 0;
    } while(0);

    /* we get here on failure */
    cs_high();
    spi_disable();
    return -1;
}

int spi_sd_write_blocks_start(unsigned long long block_address) {
    spi_enable();
    cs_low();
    wait_for_card_ready();

    const uint8_t response = command_and_r1_response(25, block_address);
    if (response != 0) {
        cs_high();
        spi_disable();
        return -1;
    }

    /* extra byte prior to data packet */
    spi_send((unsigned char[1]) { 0xff }, 1);

    return 0;
}

void spi_sd_write_blocks_end(void) {
    /* send stop tran token */
    spi_send((unsigned char[2]) { 0xfd, 0xff }, 2);

    wait_for_card_ready();

    cs_high();
    spi_disable();
}

int spi_sd_write_pre_erase(unsigned long blocks) {
    spi_enable();
    cs_low();
    wait_for_card_ready();

    const uint8_t cmd55_r1_response = command_and_r1_response(55, 0);
    cs_high();

    if (cmd55_r1_response > 1) {
        spi_disable();
        return -1;
    }

    cs_low();
    wait_for_card_ready();

    const uint8_t acmd23_r1_response = command_and_r1_response(23, blocks);

    cs_high();
    spi_disable();

    return acmd23_r1_response ? -1 : 0;
}

int spi_sd_write_some_blocks(const void * buf, const unsigned long blocks) {
    for (size_t iblock = 0; iblock < blocks; iblock++) {
        const unsigned char * block = buf ? (void *)((unsigned char *)buf + 512 * iblock) : NULL;

        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = 0xfc;

        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        static const uint32_t zero_word = 0;
        *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_WRITE) = (DmacDescriptor) {
            .BTCNT.reg = 512 / 4,
            .SRCADDR.reg = block ? ((size_t)block) + 512 : (size_t)&zero_word,
            .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
            .BTCTRL = { .bit = {
                .VALID = 1,
                .BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val,
                .SRCINC = block ? 1 : 0,
                .DSTINC = 0, /* write to the same register every time */
                .BEATSIZE = DMAC_BTCTRL_BEATSIZE_WORD_Val, /* transfer 32 bits per beat */
            }}
        };

        /* clear pending interrupt from before */
        DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;

        /* enable interrupt on write completion */
        DMAC->Channel[IDMA_SPI_WRITE].CHINTENSET.reg = (DMAC_CHINTENSET_Type) { .bit.TCMPL = 1 }.reg;

        /* reset the crc */
        DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0 }.reg;
        DMAC->CRCCHKSUM.reg = 0;
        DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0x20 + IDMA_SPI_WRITE }.reg;

        /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
        __DSB();

        /* setting this starts the transaction */
        DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;

        while (!DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.bit.TCMPL) yield();
        DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;

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

        /* wait for crc to complete sending before enabling rx */
        while (!SERCOM1->SPI.INTFLAG.bit.TXC);

        SERCOM1->SPI.CTRLB.bit.RXEN = 1;
        while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        /* blocking receive of one byte */
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = 0xFF;

        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
        const unsigned char response = SERCOM1->SPI.DATA.bit.DATA & 0b11111;

        /* this leaves sercom in rx disabled, one byte mode */
        wait_for_card_ready();

        if (0b00101 != response) {
            if (0b01011 == response)
                dprintf(2, "%s: bad crc\r\n", __func__);
            else
                dprintf(2, "%s: error 0x%x\r\n", __func__, response);

            cs_high();
            spi_disable();
            return -1;
        }
    }

    return 0;
}

int spi_sd_write_blocks(const void * buf, const unsigned long blocks, const unsigned long long block_address) {
    if (-1 == spi_sd_write_blocks_start(block_address) ||
        -1 == spi_sd_write_some_blocks(buf, blocks))
        return -1;

    spi_sd_write_blocks_end();

    return 0;
}

int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address) {
    spi_enable();
    cs_low();
    wait_for_card_ready();

    /* send cmd17 or cmd18 */
    if (command_and_r1_response(blocks > 1 ? 18 : 17, block_address) != 0) {
        cs_high();
        spi_disable();
        return -1;
    }

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* clock out the response in 1 + 512 + 2 byte blocks */
    for (size_t iblock = 0; iblock < blocks; iblock++) {
        uint8_t result;
        /* this can loop for a while */
        while (0xFF == (result = spi_receive_one_byte_with_rx_enabled()));

        /* when we break out of the above loop, we've read the Data Token byte */
        if (0xFE != result) {
            while (!SERCOM1->SPI.INTFLAG.bit.TXC);
            SERCOM1->SPI.CTRLB.bit.RXEN = 0;
            while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

            cs_high();
            spi_disable();
            return -1;
        }

        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 0 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        uint32_t * restrict const block = ((uint32_t *)buf) + 128 * iblock;

        *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_READ) = (DmacDescriptor) {
            .BTCNT.reg = 512 / 4,
            .SRCADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
            .DSTADDR.reg = ((size_t)block) + 512,
            .BTCTRL = { .bit = {
                .VALID = 1,
                .BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val,
                .SRCINC = 0,
                .DSTINC = 1, /* write to the same register every time */
                .BEATSIZE = DMAC_BTCTRL_BEATSIZE_WORD_Val, /* transfer 32 bits per beat */
            }}
        };

        /* clear pending interrupt from before */
        DMAC->Channel[IDMA_SPI_READ].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;

        DMAC->Channel[IDMA_SPI_READ].CHINTENCLR.reg = (DMAC_CHINTENCLR_Type) { .bit.TCMPL = 1 }.reg;

        /* reset the crc */
        DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0 }.reg;
        DMAC->CRCCHKSUM.reg = 0;
        DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0x20 + IDMA_SPI_READ }.reg;

        static const uint32_t dummy = 0xffffffff;
        *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_WRITE) = (DmacDescriptor) {
            .BTCNT.reg = 512 / 4,
            .SRCADDR.reg = (size_t)&dummy,
            .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
            .BTCTRL = { .bit = {
                .VALID = 1,
                .BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val,
                .SRCINC = 0,
                .DSTINC = 0, /* write to the same register every time */
                .BEATSIZE = DMAC_BTCTRL_BEATSIZE_WORD_Val, /* transfer 32 bits per beat */
            }}
        };

        /* clear pending interrupt from before */
        DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;

        /* enable interrupt on write completion */
        DMAC->Channel[IDMA_SPI_WRITE].CHINTENSET.reg = (DMAC_CHINTENSET_Type) { .bit.TCMPL = 1 }.reg;

        /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
        __DSB();

        /* setting this starts the transaction */
        DMAC->Channel[IDMA_SPI_READ].CHCTRLA.bit.ENABLE = 1;
        DMAC->Channel[IDMA_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;

        /* yield/sleep here until dma write transaction finishes */
        while (!(DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.bit.TCMPL)) yield();
        DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;

        /* busy loop for the last little bit until the read transaction finishes */
        while (!(DMAC->Channel[IDMA_SPI_READ].CHINTFLAG.bit.TCMPL));
        DMAC->Channel[IDMA_SPI_READ].CHINTFLAG.reg = (DMAC_CHINTFLAG_Type) { .bit.TCMPL = 1 }.reg;
        DMAC->Channel[IDMA_SPI_READ].CHINTENCLR.reg = (DMAC_CHINTENCLR_Type) { .bit.TCMPL = 1 }.reg;

        /* grab the CRC that the DMAC calculated on the outgoing 512 bytes... */
        while (DMAC->CRCSTATUS.bit.CRCBUSY);
        const uint16_t crc = DMAC->CRCCHKSUM.reg;

        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 2 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        /* read and discard two crc bytes */
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = 0xFFFF;

        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
        const uint16_t crc_swapped = SERCOM1->SPI.DATA.bit.DATA;
        const uint16_t crc_received = __builtin_bswap16(crc_swapped);

        while (!SERCOM1->SPI.INTFLAG.bit.TXC);
        SERCOM1->SPI.LENGTH.reg = (SERCOM_SPI_LENGTH_Type) { .bit.LENEN = 1, .bit.LEN = 1 }.reg;
        while (SERCOM1->SPI.SYNCBUSY.bit.LENGTH);

        if (crc_received != crc) {
            SERCOM1->SPI.CTRLB.bit.RXEN = 0;
            while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

            cs_high();
            spi_disable();
            dprintf(2, "%s: bad crc\r\n", __func__);
            return -1;
        }
    }

    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* if we sent cmd18, send cmd12 to stop */
    if (blocks > 1) {
        send_command_with_crc7(12, 0);

        /* CMD12 wants an extra byte prior to the response */
        spi_send((unsigned char[1]) { 0xff }, 1);

        (void)r1_response();
        wait_for_card_ready();
    }

    cs_high();
    spi_disable();

    return 0;
}
