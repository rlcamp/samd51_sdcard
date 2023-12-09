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

#define BAUD_RATE_SLOW 250000
#define BAUD_RATE_FAST (F_CPU / 2)
/* note this baud rate is too fast for DMA to keep up if doing two-way DRE-triggered DMA */

static_assert(((F_CPU / (2U * BAUD_RATE_FAST) - 1U) + 1U) * (2U * BAUD_RATE_FAST) == F_CPU,
              "baud rate not possible");

#define IDMA_SPI_WRITE 2

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
        .TRIGSRC = 0x0B, /* trigger when sercom3 is ready to send a new byte/word */
        .TRIGACT = DMAC_CHCTRLA_TRIGACT_BURST_Val, /* one burst per trigger */
        .BURSTLEN = DMAC_CHCTRLA_BURSTLEN_SINGLE_Val /* one burst = one beat */
    }}.reg;
}

static void cs_high(void) {
    PORT->Group[0].OUTSET.reg = 1U << 20;
}

static void cs_low(void) {
    PORT->Group[0].OUTCLR.reg = 1U << 20;
}

static void spi_change_baud_rate(unsigned long baudrate) {
    SERCOM3->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM3->SPI.SYNCBUSY.bit.ENABLE);

    SERCOM3->SPI.BAUD.reg = F_CPU / (2U * baudrate) - 1U;

    SERCOM3->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.ENABLE);
}

static void spi_hot_switch_32_bit(unsigned state) {
    /* prior to momentarily disabling the SERCOM, make sure the CLK pin doesn't float up */
    PORT->Group[0].PINCFG[16] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 0 } };

    /* we can only swap 8/32 bit state when the sercom is not enabled */
    SERCOM3->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM3->SPI.SYNCBUSY.bit.ENABLE);

    SERCOM3->SPI.CTRLC.bit.DATA32B = state;

    SERCOM3->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.ENABLE);

    /* return control of the CLK pin to the SERCOM */
    PORT->Group[0].PINCFG[16] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
}

static void spi_init(unsigned long baudrate) {
    /* configure pin PA20 (silkscreen pin "10" on the feather m4) as output for cs pin */
    PORT->Group[0].OUTSET.reg = 1U << 20;
    PORT->Group[0].PINCFG[20].reg = 0;
    PORT->Group[0].DIRSET.reg = 1U << 20;

    /* configure pin PA16 ("5" on feather m4) to use functionality D (sercom3 pad 1), drive
     strength 0, for sck, AND make sure it stays low when we momentarily disable PMUXEN */
    PORT->Group[0].OUTCLR.reg = 1U << 16;
    PORT->Group[0].DIRSET.reg = 1U << 16;
    PORT->Group[0].PINCFG[16] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
    PORT->Group[0].PMUX[16 >> 1].bit.PMUXE = 0x3;

    /* configure pin PA19 ("9" on feather m4) to use functionality D (sercom3 pad 3), drive strength 0, for mosi */
    PORT->Group[0].PINCFG[19] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 0 } };
    PORT->Group[0].PMUX[19 >> 1].bit.PMUXO = 0x3;

    /* configure pin PA18 ("6" on feather m4) to use functionality D (sercom3 pad 2), input enabled, for miso */
    PORT->Group[0].PINCFG[18] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .INEN = 1 } };
    PORT->Group[0].PMUX[18 >> 1].bit.PMUXE = 0x3;

    /* clear all interrupts */
    NVIC_ClearPendingIRQ(SERCOM3_1_IRQn);

    MCLK->APBBMASK.reg |= MCLK_APBBMASK_SERCOM3;

    /* unconditionally assume GCLK0 is running at F_CPU */
    GCLK->PCHCTRL[SERCOM3_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM3_GCLK_ID_CORE].bit.CHEN);
    GCLK->PCHCTRL[SERCOM3_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
    while (!GCLK->PCHCTRL[SERCOM3_GCLK_ID_CORE].bit.CHEN);

    /* reset spi peripheral */
    SERCOM3->SPI.CTRLA.bit.SWRST = 1;
    while (SERCOM3->SPI.CTRLA.bit.SWRST || SERCOM3->SPI.SYNCBUSY.bit.SWRST);

    SERCOM3->SPI.CTRLA = (SERCOM_SPI_CTRLA_Type) { .bit = {
        .MODE = 0x3, /* spi peripheral is in master mode */
        .DOPO = 0x2, /* clock is sercom pad 1, MOSI is pad 3 */
        .DIPO = 0x2, /* MISO is sercom pad 2 */
        .CPOL = 0, /* sck is low when idle */
        .CPHA = 0,
        .DORD = 0, /* msb first */
        .RUNSTDBY = 1
    }};

    SERCOM3->SPI.CTRLB = (SERCOM_SPI_CTRLB_Type) { .bit = {
        .RXEN = 0, /* spi receive is not enabled until needed */
        .MSSEN = 0, /* no hardware cs control */
        .CHSIZE = 0 /* eight bit characters */
    }};
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM3->SPI.BAUD.reg = F_CPU / (2U * baudrate) - 1U;

    spi_dma_init();

    /* enable spi peripheral */
    SERCOM3->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.ENABLE);
}

static char writing_a_block;
static unsigned char card_write_response;

static void spi_send_nonblocking_start(const void * buf, const size_t count) {
    while (!SERCOM3->SPI.INTFLAG.bit.TXC);
    spi_hot_switch_32_bit(1);

    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = count / 4,
        .SRCADDR.reg = ((size_t)buf) + count,
        .DSTADDR.reg = (size_t)&(SERCOM3->SPI.DATA.reg),
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
    SERCOM3->SPI.DATA.bit.DATA = 0xff;
    while (!SERCOM3->SPI.INTFLAG.bit.RXC);
    return SERCOM3->SPI.DATA.bit.DATA;
}

static_assert(2 == IDMA_SPI_WRITE, "dmac channel isr mismatch");
void DMAC_2_Handler(void) {
    if (!(DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.bit.TCMPL)) return;
    DMAC->Channel[IDMA_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* grab the CRC that the DMAC calculated on the outgoing 512 bytes... */
    while (DMAC->CRCSTATUS.bit.CRCBUSY);
    const uint16_t crc = DMAC->CRCCHKSUM.reg;

    /* TODO: this is necessary before switching from 32 to 8 bit mode, but does it still need
     to be here when NOT switching back from bit mode? */
    while (!SERCOM3->SPI.INTFLAG.bit.TXC);

    /* switch from 32-bit mode back to 8-bit mode prior to sending crc */
    spi_hot_switch_32_bit(0);

    /* blocking send of crc */
    while (!SERCOM3->SPI.INTFLAG.bit.DRE);
    SERCOM3->SPI.DATA.bit.DATA = (crc >> 8U) & 0xff;
    while (!SERCOM3->SPI.INTFLAG.bit.DRE);
    SERCOM3->SPI.DATA.bit.DATA = (crc) & 0xff;

    /* wait for crc to complete sending before enabling rx */
    while (!SERCOM3->SPI.INTFLAG.bit.TXC);

    SERCOM3->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    /* blocking receive of one byte */
    card_write_response = spi_receive_one_byte_with_rx_enabled();

    writing_a_block = 0;

    /* arm an321 page 22 fairly strongly suggests this is necessary prior to exception
     return (or will be on future processors) when the first thing the processor wants
     to do afterward depends on state changed in the handler */
    __DSB();
}

static void wait_for_card_ready(void) {
    SERCOM3->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    uint8_t res;
    do res = spi_receive_one_byte_with_rx_enabled();
    while (res != 0xff);
}

int spi_sd_flush_write(void) {
    while (*(volatile char *)&writing_a_block) yield();
    /* ensure we don't prefetch the below value before the above condition becomes true */
    __DMB();

    const uint16_t response = card_write_response;
    wait_for_card_ready();

    /* assuming we spun on WFE in one of the above, re-raise the event register explicitly,
     so that caller does not have to assume calling this function may have cleared it */
    __SEV();

    return response != 0xE5 ? -1 : 0;
}

void spi_sd_start_writing_a_block(const void * buf) {
    SERCOM3->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    while (!SERCOM3->SPI.INTFLAG.bit.DRE);
    SERCOM3->SPI.DATA.bit.DATA = 0xfc;

    writing_a_block = 1;
    spi_send_nonblocking_start(buf, 512);
}

static void spi_send(const void * buf, const size_t size) {
    SERCOM3->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    for (size_t ibyte = 0; ibyte < size; ibyte++) {
        while (!SERCOM3->SPI.INTFLAG.bit.DRE);
        SERCOM3->SPI.DATA.bit.DATA = ((const char *)buf)[ibyte];
    }

    while (!SERCOM3->SPI.INTFLAG.bit.TXC);
}

static uint32_t spi_receive_uint32(void) {
    SERCOM3->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    uint32_t response = spi_receive_one_byte_with_rx_enabled() << 24;
    response |= spi_receive_one_byte_with_rx_enabled() << 16;
    response |= spi_receive_one_byte_with_rx_enabled() << 8;
    response |= spi_receive_one_byte_with_rx_enabled();
    return response;
}

static uint8_t r1_response(void) {
    SERCOM3->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    uint8_t result, attempts = 0;
    do result = spi_receive_one_byte_with_rx_enabled();
    while (0xFF == result && attempts++ < 8);
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

void spi_sd_init(void) {
    /* TODO: ideally ensure 1 millisecond has passed since power supply has stabilized */

    /* and then clock out at least 74 cycles at 100-400 kBd with cs pin held high */
    spi_init(BAUD_RATE_SLOW);
    spi_send((unsigned char[10]) { [0 ... 9] = 0xFF }, 10);

    spi_change_baud_rate(BAUD_RATE_FAST);

    /* cmd0 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        /* send cmd0 */
        const uint8_t cmd0_r1_response = command_and_r1_response(0, 0);
        cs_high();

        if (0x01 == cmd0_r1_response) break;
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

        const uint32_t response = spi_receive_uint32();
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

    /* cmd58 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(58, 0);

        if (r1_response > 1) {
            cs_high();
            continue;
        }

        const unsigned int ocr = spi_receive_uint32();
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
}

int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address) {
    cs_low();
    wait_for_card_ready();

    /* send cmd17 or cmd18 */
    if (command_and_r1_response(blocks > 1 ? 18 : 17, block_address) != 0) return -1;

    SERCOM3->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM3->SPI.SYNCBUSY.bit.CTRLB);

    /* clock out the response in 1 + 512 + 2 byte blocks */
    for (size_t iblock = 0; iblock < blocks; iblock++) {
        uint8_t result;
        /* this can loop for a while */
        do result = spi_receive_one_byte_with_rx_enabled();
        while (0xFF == result);

        /* when we break out of the above loop, we've read the Data Token byte */
        if (0xFE != result) {
            cs_high();
            return -1;
        }

        spi_hot_switch_32_bit(1);

        uint32_t * restrict const block = ((uint32_t *)buf) + 128 * iblock;
        for (size_t iword = 0; iword < 128; iword++) {
            SERCOM3->SPI.DATA.bit.DATA = 0xffffffff;
            while (!SERCOM3->SPI.INTFLAG.bit.RXC);
            block[iword] = SERCOM3->SPI.DATA.bit.DATA;
        }

        spi_hot_switch_32_bit(0);

        /* read and discard two crc bytes */
        uint16_t crc = spi_receive_one_byte_with_rx_enabled() << 8;
        crc |= spi_receive_one_byte_with_rx_enabled();
    }

    /* if we sent cmd18, send cmd12 to stop */
    if (blocks > 1) command_and_r1_response(12, 0);

    cs_high();
    return 0;
}

void spi_sd_write_pre_erase(unsigned long blocks) {
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

int spi_sd_write_blocks_start(unsigned long long block_address) {
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

void spi_sd_write_blocks_end(void) {
    /* send stop tran token */
    spi_send((unsigned char[2]) { 0xfd, 0xff }, 2);

    wait_for_card_ready();

    cs_high();
}

int spi_sd_write_blocks(const void * buf, const unsigned long blocks, const unsigned long long block_address) {
    spi_sd_write_pre_erase(blocks);

    if (-1 == spi_sd_write_blocks_start(block_address))
        return -1;

    for (size_t iblock = 0; iblock < blocks; iblock++) {
        spi_sd_start_writing_a_block((void *)((unsigned char *)buf + 512 * iblock));

        /* this will block, but will internally call yield() and __WFI() */
        if (-1 == spi_sd_flush_write()) return -1;
    }

    spi_sd_write_blocks_end();

    return 0;
}
