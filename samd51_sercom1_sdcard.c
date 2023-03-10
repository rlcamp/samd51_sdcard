#include "samd51_sercom1_sdcard.h"
#include <samd.h>
#include <assert.h>

#define BAUD_RATE_SLOW 250000
#define BAUD_RATE_FAST 24000000

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

    /* initialize unchanging properties of write descriptor */
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE) = (DmacDescriptor) {
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.DSTINC = 0, /* write to the same register every time */
        }
    };

    /* initialize unchanging properties of write descriptor */
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_READ) = (DmacDescriptor) {
        .SRCADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = {
            .bit.VALID = 1,
            .bit.SRCINC = 0, /* read from the same register every time */
            .bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val, /* fire an interrupt */
        }
    };

    __DSB();
}

static void cs_init(void) {
    /* configure pin PA14 (arduino pin D4 on the feather m4) as output for cs pin */
    PORT->Group[0].OUTSET.reg = 1U << 14;
    PORT->Group[0].PINCFG[14].reg = 0;
    PORT->Group[0].DIRSET.reg = 1U << 14;
}

static void cs_high(void) {
    PORT->Group[0].OUTSET.reg = 1U << 14;
}

static void cs_low(void) {
    PORT->Group[0].OUTCLR.reg = 1U << 14;
}

static void spi_change_baud_rate(unsigned long baudrate) {
    SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

    SERCOM1->SPI.BAUD.reg = 48000000U / (2U * baudrate) - 1U;

    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

static void spi_init(unsigned long baudrate) {
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

/* used only for determining which ancillary traffic to send before/after blocks */
static volatile size_t blocks_total_in_transaction;

static const unsigned char * card_write_cursor, * card_write_cursor_stop;
static unsigned char card_write_response;

static void spi_send_nonblocking_start(const void * buf, const size_t count) {
    DmacDescriptor * descriptor_write = ((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE;
    descriptor_write->BTCNT.reg = count;
    descriptor_write->SRCADDR.reg = ((size_t)buf) + count;
    descriptor_write->BTCTRL.bit.SRCINC = 1;
    descriptor_write->BTCTRL.bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val;

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

void spi_send_sd_next_block_start(void) {
    if (!card_write_cursor) return;
    if (card_write_cursor == card_write_cursor_stop) {
        card_write_cursor = NULL;
        return;
    }

    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = blocks_total_in_transaction > 1 ? 0xfc : 0xfe;

    card_write_cursor += 512;
    spi_send_nonblocking_start(card_write_cursor - 512, 512);
}

static void spi_wait_while_card_busy_nonblocking_start(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    /* make a few blocking attempts to see if the card is ready immediately */
    for (size_t ipass = 0; ipass < 4; ipass++) {
        SERCOM1->SPI.DATA.bit.DATA = 0xff;
        while (!SERCOM1->SPI.INTFLAG.bit.RXC);
        if (0xff == SERCOM1->SPI.DATA.bit.DATA) {
            spi_send_sd_next_block_start();
            return;
        }
    }

    static const unsigned char all_ones = 0xff;
    DmacDescriptor * descriptor_write = ((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE;
    descriptor_write->BTCNT.reg = CARD_BUSY_BYTES_PER_CHECK;
    descriptor_write->SRCADDR.reg = (size_t)&all_ones;
    descriptor_write->BTCTRL.bit.SRCINC = 0;
    descriptor_write->BTCTRL.bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_NOACT_Val;

    DmacDescriptor * descriptor_read = ((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_READ;
    descriptor_read->BTCNT.reg = CARD_BUSY_BYTES_PER_CHECK;
    descriptor_read->DSTADDR.reg = (size_t)&card_busy_result;
    descriptor_read->BTCTRL.bit.DSTINC = 0; /* read to the same byte every time */

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

    if (card_write_cursor) {
        /* grab the CRC that the DMAC calculated on the outgoing 512 bytes... */
        while (DMAC->CRCSTATUS.bit.CRCBUSY);
        const uint16_t crc = DMAC->CRCCHKSUM.reg;

        /* TODO: somewhat hack fix, might not be the actual fix */
        while (!SERCOM1->SPI.INTFLAG.bit.TXC);

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
    }
}

void DMAC_2_Handler(void) {
    if (!(DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.bit.TCMPL)) return;
    DMAC->Channel[ICHANNEL_SPI_READ].CHINTFLAG.reg = DMAC_CHINTENCLR_TCMPL;

    busy = 0;

    if (waiting_while_card_busy) {
        if (0xff == card_busy_result) {
            waiting_while_card_busy = 0;

            spi_send_sd_next_block_start();
        } else {
            /* need to try again */
            DMAC->Channel[ICHANNEL_SPI_READ].CHCTRLA.bit.ENABLE = 1;
            DMAC->Channel[ICHANNEL_SPI_WRITE].CHCTRLA.bit.ENABLE = 1;
        }
    }
}

static void spi_receive_nonblocking_start(void * buf, const size_t count) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    static const unsigned char all_ones = 0xff;
    DmacDescriptor * descriptor_write = ((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_WRITE;
    descriptor_write->BTCNT.reg = count;
    descriptor_write->SRCADDR.reg = (size_t)&all_ones;
    descriptor_write->BTCTRL.bit.SRCINC = 0;
    descriptor_write->BTCTRL.bit.BLOCKACT = DMAC_BTCTRL_BLOCKACT_NOACT_Val;

    DmacDescriptor * descriptor_read = ((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + ICHANNEL_SPI_READ;
    descriptor_read->BTCNT.reg = count;
    descriptor_read->DSTADDR.reg = ((size_t)buf) + count,
    descriptor_read->BTCTRL.bit.DSTINC = 1; /* read to the same byte every time */

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

void spi_send_nonblocking_wait(void) {
    while (busy) __WFI();
}

int spi_send_sd_blocks_finish(void) {
    while (card_write_cursor || waiting_while_card_busy) __WFI();

    uint16_t response = card_write_response;
//    if (response != 0xE5) fprintf(stderr, "%s(%d): response 0x%2.2X\n", __func__, __LINE__, response);

    return response != 0xE5 ? -1 : 0;
}

void spi_send_sd_blocks_start(const void * buf, const size_t size) {
    card_write_cursor = buf;
    card_write_cursor_stop = buf + size;

    spi_send_sd_next_block_start();
}

static void spi_receive_nonblocking_wait(void) {
    while (busy) __WFI();
}

static void spi_receive(void * buf, const size_t size) {
    spi_receive_nonblocking_start(buf, size);
    spi_receive_nonblocking_wait();
}

static void spi_send(const void * buf, const size_t size) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    spi_send_nonblocking_start(buf, size);
    spi_send_nonblocking_wait();
    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
}

static uint32_t spi_receive_uint32(void) {
    unsigned char response[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    spi_receive(response, sizeof(response));
    return (response[0] << 24) | (response[1] << 16) | (response[2] << 8) | response[3];
}

static uint8_t r1_response(void) {
    uint8_t result = 0xFF, attempts = 0;
    do spi_receive(&result, 1);
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

static void wait_for_card_ready(void) {
    uint8_t result;
    do spi_receive(&result, 1);
    while (0xFF != result);
}

static int rx_data_block(unsigned char * buf) {
    /* this can loop for a while */
    uint8_t result = 0xFF;
    do spi_receive(&result, 1);
    while (0xFF == result);

    /* when we break out of the above loop, we've read the Data Token byte */
    if (0xFE != result) return -1;

    /* read the 512 bytes with MOSI held high */
    spi_receive(buf, 512);

    /* read and discard two crc bytes */
    unsigned char crcbuf[2];
    spi_receive(crcbuf, 2);
//    const uint16_t crc = crcbuf[0] << 8 | crcbuf[1];

//    fprintf(stderr, "%s: 0x%4.4X\n", __func__, crc);
    return 0;
}

void spi_sd_init(void) {
    cs_init();

    /* must wait 1 millisecond after power supply has stabilized */
    //   delay(1);

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

//        fprintf(stderr, "%s: cmd0_r1_response: 0x%2.2X\n", __func__, cmd0_r1_response);
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

//        fprintf(stderr, "%s: cmd8 response: 0x%8.8X\n", __func__, (unsigned int)response);
        if (0x1AA == response) break;
    }

    /* cmd55, then acmd41 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t cmd55_r1_response = command_and_r1_response(55, 0);
        cs_high();

        if (cmd55_r1_response > 1) continue;

//        fprintf(stderr, "%s: sending acmd41\n", __func__);
        cs_low();
        wait_for_card_ready();

        const uint8_t acmd41_r1_response = command_and_r1_response(41, 1U << 30);
        cs_high();

//        fprintf(stderr, "%s: acmd41_r1_response: 0x%2.2X\n", __func__, acmd41_r1_response);
        if (!acmd41_r1_response) break;
    }

    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t cmd58_r1_response = command_and_r1_response(58, 0);

        if (cmd58_r1_response > 1) {
            cs_high();

            continue;
        }

        const unsigned int ocr = spi_receive_uint32();
        (void)ocr;
        cs_high();

//        fprintf(stderr, "%s: cmd58 response: 0x%8.8X\n", __func__, ocr);
//        if (ocr & (1U << 30)) fprintf(stderr, "%s: bit 30 is set\n", __func__);

        break;
    }

    /* cmd16 */
    while (1) {
//        fprintf(stderr, "%s: sending cmd16\n", __func__);
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(16, 512);
        cs_high();

//        fprintf(stderr, "%s: cmd16 response: 0x%2.2X\n", __func__, r1_response);
        if (r1_response <= 1) break;
    }

#if 1
    /* cmd59 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(59, 1);

        cs_high();

//        fprintf(stderr, "%s: cmd59 response: 0x%2.2X\n", __func__, r1_response);
        if (r1_response <= 1) break;
    }
#endif
}

int spi_sd_read_data(unsigned char * buf, unsigned long size, unsigned long long address) {
    /* note that address must be a multiple of 512 */
    const size_t blocks = size / 512;

    cs_low();
    wait_for_card_ready();

    /* send cmd17 or cmd18 */
    if (command_and_r1_response(blocks > 1 ? 18 : 17, address / 512) != 0) return -1;

    /* clock out the response in 1 + 512 + 2 byte blocks */
    for (size_t iblock = 0; iblock < blocks; iblock++)
        if (-1 == rx_data_block(buf + 512 * iblock)) {
            cs_high();
            return -1;
        }

    /* if we sent cmd18, send cmd12 to stop */
    if (blocks > 1) command_and_r1_response(12, 0);

    cs_high();
    return 0;
}

int spi_sd_write_data_start(unsigned long size, unsigned long long address) {
    const size_t blocks = size / 512;
    blocks_total_in_transaction = blocks;

    if (blocks > 1)
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

    cs_low();
    wait_for_card_ready();

    const uint8_t response = command_and_r1_response(blocks > 1 ? 25 : 24, address / 512);
    if (response != 0) {
        cs_high();
        return -1;
    }

    /* extra byte prior to data packet */
    spi_send((unsigned char[1]) { 0xff }, 1);

    return 0;
}

void spi_sd_write_data_end(void) {
    /* if we sent cmd25, send stop tran token */
    if (blocks_total_in_transaction > 1) spi_send((unsigned char[2]) { 0xfd, 0xff }, 2);

    wait_for_card_ready();

    cs_high();
}

int spi_sd_write_data(unsigned char * buf, const unsigned long size, const unsigned long long address) {
    /* note that address and size must be multiples of 512 */
    if (-1 == spi_sd_write_data_start(size, address))
        return -1;

    spi_send_sd_blocks_start(buf, size);

    if (-1 == spi_send_sd_blocks_finish()) return -1;
    spi_sd_write_data_end();

    return 0;
}
