#include "samd51_sercom1_sdcard.h"

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
#define BAUD_RATE_FAST (F_CPU / 4)
/* note you will have problems if you try to run spi at 24 MBd on a 48 MHz clock */

static_assert(((F_CPU / (2U * BAUD_RATE_FAST) - 1U) + 1U) * (2U * BAUD_RATE_FAST) == F_CPU,
              "baud rate not possible");

#define IDMA_SPI_WRITE 2

/* smaller values use more cpu while waiting but have lower latency */
#define CARD_BUSY_BYTES_PER_CHECK 16

/* do not use SECTION_DMAC_DESCRIPTOR because the linker script does not define hsram */
__attribute__((weak, aligned(16))) DmacDescriptor dmac_descriptors[8] = { 0 }, dmac_writeback[8] = { 0 };

extern void yield(void);
__attribute((weak)) void yield(void) { __DSB(); __WFE(); }

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

    SERCOM1->SPI.BAUD.reg = F_CPU / (2U * baudrate) - 1U;

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

    /* unconditionally assume GCLK0 is running at F_CPU */
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN = 0;
    while (GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);
    GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
    while (!GCLK->PCHCTRL[SERCOM1_GCLK_ID_CORE].bit.CHEN);

    NVIC_EnableIRQ(SERCOM1_2_IRQn);
    NVIC_SetPriority(SERCOM1_2_IRQn, (1 << __NVIC_PRIO_BITS) - 1);

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

    SERCOM1->SPI.BAUD.reg = F_CPU / (2U * baudrate) - 1U;

    spi_dma_init();

    /* enable spi peripheral */
    SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);
}

static char writing_a_block;
static unsigned char card_write_response;

static void spi_send_nonblocking_start(const void * buf, const size_t count) {
    *(((DmacDescriptor *)DMAC->BASEADDR.bit.BASEADDR) + IDMA_SPI_WRITE) = (DmacDescriptor) {
        .BTCNT.reg = count,
        .SRCADDR.reg = ((size_t)buf) + count,
        .DSTADDR.reg = (size_t)&(SERCOM1->SPI.DATA.reg),
        .BTCTRL = { .bit = {
            .VALID = 1,
            .BLOCKACT = DMAC_BTCTRL_BLOCKACT_INT_Val,
            .SRCINC = 1,
            .DSTINC = 0, /* write to the same register every time */
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

static uint32_t spi_receive_one_byte_with_rx_enabled(void) {
    SERCOM1->SPI.DATA.bit.DATA = 0xff;
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
    card_write_response = spi_receive_one_byte_with_rx_enabled();

    writing_a_block = 0;
}

static char waiting_for_card_ready = 0;
static uint32_t * reading_a_block_cursor = NULL, * reading_a_block_stop = NULL;

void SERCOM1_2_Handler(void) {
    if (reading_a_block_stop) {
        *(reading_a_block_cursor++) = SERCOM1->SPI.DATA.bit.DATA;
        if (reading_a_block_cursor != reading_a_block_stop)
            SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;
        else {
            reading_a_block_stop = NULL;
            SERCOM1->SPI.INTENCLR.reg = (SERCOM_SPI_INTENCLR_Type) { .bit.RXC = 1 }.reg;
        }
    } else {
        if (SERCOM1->SPI.DATA.bit.DATA != 0xff)
            SERCOM1->SPI.DATA.bit.DATA = 0xff;
        else {
            waiting_for_card_ready = 0;
            SERCOM1->SPI.INTENCLR.reg = (SERCOM_SPI_INTENSET_Type) { .bit.RXC = 1 }.reg;
        }
    }
}

static void wait_for_card_ready(void) {
    waiting_for_card_ready = 1;
    __DSB();

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    SERCOM1->SPI.INTENSET.reg = (SERCOM_SPI_INTENSET_Type) { .bit.RXC = 1 }.reg;
    SERCOM1->SPI.DATA.bit.DATA = 0xff;
    while (*(volatile char *)&waiting_for_card_ready) yield();
}

int spi_sd_flush_write(void) {
    while (*(volatile char *)&writing_a_block) yield();
    wait_for_card_ready();
    uint16_t response = card_write_response;
//    if (response != 0xE5) fprintf(stderr, "%s(%d): response 0x%2.2X\n", __func__, __LINE__, response);

    return response != 0xE5 ? -1 : 0;
}

void spi_sd_start_writing_a_block(const void * buf) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    while (!SERCOM1->SPI.INTFLAG.bit.DRE);
    SERCOM1->SPI.DATA.bit.DATA = 0xfc;

    writing_a_block = 1;
    spi_send_nonblocking_start(buf, 512);
}

static void spi_send(const void * buf, const size_t size) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 0;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    for (size_t ibyte = 0; ibyte < size; ibyte++) {
        while (!SERCOM1->SPI.INTFLAG.bit.DRE);
        SERCOM1->SPI.DATA.bit.DATA = ((const char *)buf)[ibyte];
    }

    while (!SERCOM1->SPI.INTFLAG.bit.TXC);
}

static uint32_t spi_receive_uint32(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

    uint32_t response = spi_receive_one_byte_with_rx_enabled() << 24;
    response |= spi_receive_one_byte_with_rx_enabled() << 16;
    response |= spi_receive_one_byte_with_rx_enabled() << 8;
    response |= spi_receive_one_byte_with_rx_enabled();
    return response;
}

static uint8_t r1_response(void) {
    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

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

int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address) {
    cs_low();
    wait_for_card_ready();

    /* send cmd17 or cmd18 */
    if (command_and_r1_response(blocks > 1 ? 18 : 17, block_address) != 0) return -1;

    SERCOM1->SPI.CTRLB.bit.RXEN = 1;
    while (SERCOM1->SPI.SYNCBUSY.bit.CTRLB);

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

        SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
        while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

        SERCOM1->SPI.CTRLC.bit.DATA32B = 1;

        SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
        while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

        reading_a_block_cursor = (void *)((unsigned char *)buf) + 512 * iblock;
        reading_a_block_stop = reading_a_block_cursor + (512 / 4);
        __DSB();

        SERCOM1->SPI.INTENSET.reg = (SERCOM_SPI_INTENSET_Type) { .bit.RXC = 1 }.reg;
        SERCOM1->SPI.DATA.bit.DATA = 0xffffffff;

        while (*(volatile uint32_t **)&reading_a_block_stop) yield();

        SERCOM1->SPI.CTRLA.bit.ENABLE = 0;
        while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

        SERCOM1->SPI.CTRLC.bit.DATA32B = 0;

        SERCOM1->SPI.CTRLA.bit.ENABLE = 1;
        while (SERCOM1->SPI.SYNCBUSY.bit.ENABLE);

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

    for (const unsigned char * c = buf, * s = c + 512 * blocks; c < s; c += 512) {
        spi_sd_start_writing_a_block(c);

        /* this will block, but will internally call yield() and __WFI() */
        if (-1 == spi_sd_flush_write()) return -1;
    }

    spi_sd_write_blocks_end();

    return 0;
}
