#include "spi.h"
#include <samd.h>
#include <assert.h>
#include <stdio.h>

#define ICHANNEL_SPI_WRITE 1
#define ICHANNEL_SPI_READ 2

/* smaller values use more cpu while waiting but have lower latency */
#define CARD_BUSY_BYTES_PER_CHECK 16

__attribute__((weak, aligned(16))) SECTION_DMAC_DESCRIPTOR DmacDescriptor dmac_descriptors[8] = { 0 }, dmac_writeback[8] = { 0 };

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
        DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xF);
    }

    /* must agree with ICHANNEL_SPI_WRITE */
    static_assert(1 == ICHANNEL_SPI_WRITE, "dmac channel isr mismatch");
    NVIC_EnableIRQ(DMAC_1_IRQn);
    NVIC_SetPriority(DMAC_1_IRQn, (1 << __NVIC_PRIO_BITS) - 1);

    /* reset channel */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 0;
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.SWRST = 1;

    /* clear sw trigger */
    DMAC->SWTRIGCTRL.reg &= ~(1 << ICHANNEL_SPI_WRITE);

    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.RUNSTDBY = 1;
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.TRIGSRC = 0x07; /* trigger when sercom1 is ready to send a new byte */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.TRIGACT = DMAC_CHCTRLA_TRIGACT_BURST_Val; /* transfer one byte when triggered */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.BURSTLEN = DMAC_CHCTRLA_BURSTLEN_SINGLE_Val; /* one burst = one beat */

    /* must agree with ICHANNEL_SPI_READ */
    static_assert(2 == ICHANNEL_SPI_READ, "dmac channel isr mismatch");
    NVIC_EnableIRQ(DMAC_2_IRQn);
    NVIC_SetPriority(DMAC_2_IRQn, (1 << __NVIC_PRIO_BITS) - 1);

    /* reset channel */
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.ENABLE = 0;
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.SWRST = 1;

    /* clear sw trigger */
    DMAC->SWTRIGCTRL.reg &= ~(1 << ICHANNEL_SPI_READ);

    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.RUNSTDBY = 1;
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.TRIGSRC = 0x06; /* trigger when sercom1 has received one new byte */
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.TRIGACT = DMAC_CHCTRLA_TRIGACT_BURST_Val; /* transfer one byte when triggered */
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.BURSTLEN = DMAC_CHCTRLA_BURSTLEN_SINGLE_Val; /* one burst = one beat */
}

void cs_init(void) {
    /* configure pin PA14 (arduino pin D4 on the feather m4) as output for cs pin */
    PORT->Group[0].OUTSET.reg = 1U << 14;
    PORT->Group[0].PINCFG[14].reg = 0;
    PORT->Group[0].DIRSET.reg = 1U << 14;
}

void cs_high(void) {
    PORT->Group[0].OUTSET.reg = 1U << 14;
}

void cs_low(void) {
    PORT->Group[0].OUTCLR.reg = 1U << 14;
}

void spi_change_baud_rate(unsigned long baudrate) {
    SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

    SERCOM1->SPI.BAUD.reg = 48000000U / (2U * baudrate) - 1U;

    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

void spi_init(unsigned long baudrate) {
    /* sercom1 pad 2 is miso, pad 1 is sck, pad 3 is mosi. hw cs is not used */

    /* configure pin PA17 ("SCK" on feather m4) to use functionality C (sercom1 pad 1), drive strength 1, for sck */
    PORT->Group[0].PINCFG[17] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
    PORT->Group[0].PMUX[17 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB23 ("MO" on feather m4) to use functionality C (sercom1 pad 3), drive strength 1, for mosi */
    PORT->Group[1].PINCFG[23] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .DRVSTR = 1 } };
    PORT->Group[1].PMUX[23 >> 1].bit.PMUXO = 0x2;

    /* configure pin PB22 ("MI" on feather m4) to use functionality C (sercom1 pad 2), input enabled, for miso */
    PORT->Group[1].PINCFG[22] = (PORT_PINCFG_Type) { .bit = { .PMUXEN = 1, .INEN = 1 } };
    PORT->Group[1].PMUX[22 >> 1].bit.PMUXE = 0x2;

    /* clear all interrupts */
    NVIC_ClearPendingIRQ(SERCOM1_1_IRQn);

    MCLK->APBAMASK.reg |= MCLK_APBAMASK_SERCOM1;

    /* core clock */
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].reg = (F_CPU == 48000000 ? GCLK_PCHCTRL_GEN_GCLK0 : GCLK_PCHCTRL_GEN_GCLK1) | GCLK_PCHCTRL_CHEN;
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
    }};

    SERCOM1->SPI.CTRLB = (SERCOM_SPI_CTRLB_Type) { .bit = {
        .RXEN = 0, /* spi receive is not enabled until needed */
        .MSSEN = 0, /* no hardware cs control */
        .CHSIZE = 0 /* eight bit characters */
    }};
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.BAUD.reg = 48000000U / (2U * baudrate) - 1U;

    NVIC_SetPriority(SERCOM1_1_IRQn, (1 << __NVIC_PRIO_BITS) - 1);

    spi_dma_init();

    /* enable spi peripheral */
    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

static volatile char busy = 0;
static volatile char waiting_while_card_busy;
static unsigned char card_busy_result;

static volatile char waiting_while_card_write;
static unsigned char card_write_response;

void spi_wait_while_card_busy_nonblocking_wait(void) {
    while (waiting_while_card_busy) __WFI();
}

static void spi_wait_while_card_busy_nonblocking_start(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* TODO: almost all of this descriptor reconfig can usually be skipped */
    static const unsigned char all_ones = 0xff;
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = CARD_BUSY_BYTES_PER_CHECK,
        .SRCADDR.reg = (size_t)&all_ones,
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 0, /* read from the same value every time */
            .bit.DSTINC = 0, /* write to the same register every time */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_NOACT_Val, /* do not fire an interrupt */
        }
    };

    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_READ) = (DmacDescriptor) {
        .BTCNT.reg = CARD_BUSY_BYTES_PER_CHECK, /* number of beats in transaction, where one beat is one byte */
        .SRCADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .DSTADDR.reg = (size_t)&card_busy_result,
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 0, /* read from the same register every time */
            .bit.DSTINC = 0, /* read to the same byte every time */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val, /* fire an interrupt */
        }
    };

    /* clear prior interrupt flags */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* disable interrupt for write channel, enable for read channel */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTENCLR.bit.TCMPL = 1;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTENSET.bit.TCMPL = 1;

    waiting_while_card_busy = 1;

    /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
    __DSB();

    /* setting this does nothing yet */
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.ENABLE = 1;

    /* setting this starts the transaction */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;
}

void DMAC_1_Handler(void) {
    if (!(DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTFLAG.bit.TCMPL)) return;
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    busy = 0;

    if (waiting_while_card_write) {
        /* grab the CRC that the DMAC calculated on the outgoing 512 bytes... */
        while (DMAC->CRCSTATUS.bit.CRCBUSY);
        const uint16_t crc = DMAC->CRCCHKSUM.reg;

        /* blocking send of crc */
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = (crc >> 8U) & 0xff;
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = (crc) & 0xff;

        /* wait for crc to complete sending before enabling rx */
        while (!SERCOM1->SPI.INTFLAG.bit.TXC);

        SERCOM1->SPI.CTRLB.bit.RXEN = 1;
        while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

        /* blocking receive of one byte */
        SERCOM1->SPI.DATA.bit.DATA = 0xff;
        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
        card_write_response = SERCOM1->SPI.DATA.bit.DATA;

        spi_wait_while_card_busy_nonblocking_start();
        waiting_while_card_write = 0;
    }
}

void DMAC_2_Handler(void) {
    if (!(DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.bit.TCMPL)) return;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    busy = 0;

    if (waiting_while_card_busy) {
        if (0xff == card_busy_result)
            waiting_while_card_busy = 0;
        else {
            /* need to try again */
            DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.ENABLE = 1;
            DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;

            return;
        }
    }
}

static void spi_receive_nonblocking_start(void * buf, const size_t count) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    static const unsigned char all_ones = 0xff;
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = count,
        .SRCADDR.reg = (size_t)&all_ones,
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 0, /* read from the same value every time */
            .bit.DSTINC = 0, /* write to the same register every time */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_NOACT_Val, /* do not fire an interrupt */
        }
    };

    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_READ) = (DmacDescriptor) {
        .BTCNT.reg = count, /* number of beats in transaction, where one beat is one byte */
        .SRCADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .DSTADDR.reg = ((size_t)buf) + count,
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 0, /* read from the same register every time */
            .bit.DSTINC = 1, /* increment destination register after every byte */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val, /* fire an interrupt */
        }
    };

    /* clear prior interrupt flags */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* disable interrupt for write channel, enable for read channel */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTENCLR.bit.TCMPL = 1;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTENSET.bit.TCMPL = 1;

    busy = 1;

    /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
    __DSB();

    /* setting this does nothing yet */
    DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.ENABLE = 1;

    /* setting this starts the transaction */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;
}

static void spi_send_nonblocking_start(const void * buf, const size_t count) {
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = count,
        .SRCADDR.reg = ((size_t)buf) + count,
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 1, /* inc from (srcaddr.reg - count) to (srcadddr.reg - 1) inclusive */
            .bit.DSTINC = 0, /* write to the same register every time */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val, /* fire an interrupt after every block */
        }
    };

    /* clear pending interrupt from before */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    /* enable interrupt on write completion */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHINTENSET.bit.TCMPL = 1;

    /* reset the crc */
    DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0 }.reg;
    DMAC->CRCCHKSUM.reg = 0;
    DMAC->CRCCTRL.reg = (DMAC_CRCCTRL_Type) { .bit.CRCSRC = 0x21 }.reg;

    busy = 1;

    /* ensure changes to descriptors have propagated to sram prior to enabling peripheral */
    __DSB();

    /* setting this starts the transaction */
    DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;
}

void spi_send_nonblocking_wait(void) {
    while (busy) __WFI();
}

int spi_send_sd_block_finish(void) {
    spi_send_nonblocking_wait();
    spi_wait_while_card_busy_nonblocking_wait();

    uint16_t response = card_write_response;
    if (response != 0xE5)
        fprintf(stderr, "%s(%d): response 0x%2.2X\n", __func__, __LINE__, response);

    return response != 0xE5 ? -1 : 0;
}

void spi_send_sd_block_start(const void * buf, const size_t size_total) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = (size_total / 512) > 1 ? 0xfc : 0xfe;

    waiting_while_card_write = 1;

    spi_send_nonblocking_start(buf, 512);
}

void spi_receive_nonblocking_wait(void) {
    while (busy) __WFI();
}

void spi_receive(void * buf, const size_t size) {
    spi_receive_nonblocking_start(buf, size);
    spi_receive_nonblocking_wait();
}

void spi_send(const void * buf, const size_t size) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    spi_send_nonblocking_start(buf, size);
    spi_send_nonblocking_wait();
    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
}
