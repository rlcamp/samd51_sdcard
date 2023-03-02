/* this is a minimum viable, blocking implementation of an api allowing fast writes to raw microsd
 from a call site that doesn't have to be doing anything else, suitable for throughput testing */

#include "sdcard_blocking.h"
#include "spi.h"

#include <stdio.h>
#include <string.h>

#define BAUD_RATE_SLOW 250000
#define BAUD_RATE_FAST 24000000

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

static uint8_t command_and_r1_response(const uint8_t cmd, const uint32_t arg) {
    /* in spi mode we can hardcode the two nonzero CRCs we actually need */
    const uint8_t crc = (0 == cmd) ? 0x94 : (8 == cmd && 0x1aa == arg) ? 0x86 : 0;

    /* why does making this an spi_send() break things */
    spi_send((unsigned char[6]) { cmd | 0x40, arg >> 24, arg >> 16, arg >> 8, arg, crc | 0x01 }, 6);

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
    spi_receive((unsigned char[2]) { 0 }, 2);
    return 0;
}

uint16_t crc16_itu_t(uint16_t v, const unsigned char * src, size_t len) {
    for (size_t ibyte = 0; ibyte < len; ibyte++) {
        v = (v >> 8U) | (v << 8U);
        v ^= src[ibyte];
        v ^= (v & 0xffU) >> 4U;
        v ^= v << 12U;
        v ^= (v & 0xffU) << 5U;
    }

    return v;
}

void spi_sd_cmd58(void) {
    while (1) {
        fprintf(stderr, "%s: sending cmd58\n", __func__);

        cs_low();
        wait_for_card_ready();

        const uint8_t cmd58_r1_response = command_and_r1_response(58, 0);

        fprintf(stderr, "%s: cmd58_r1_response: 0x%2.2X\n", __func__, cmd58_r1_response);

        if (cmd58_r1_response > 1) {
            cs_high();

            continue;
        }

        const unsigned int ocr = spi_receive_uint32();

        cs_high();

        fprintf(stderr, "%s: cmd58 response: 0x%8.8X\n", __func__, ocr);

        if (ocr & (1U << 30))
            fprintf(stderr, "%s: bit 30 is set\n", __func__);

        break;
    }
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

        fprintf(stderr, "%s: cmd0_r1_response: 0x%2.2X\n", __func__, cmd0_r1_response);
        if (0x01 == cmd0_r1_response) break;
    }

    /* cmd8 */
    while (1) {
        fprintf(stderr, "%s: sending cmd8\n", __func__);
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(8, 0x1AA);

        if (0x1 != r1_response) {
            cs_high();
            continue;
        }

        const uint32_t response = spi_receive_uint32();

        cs_high();

        fprintf(stderr, "%s: cmd8 response: 0x%8.8X\n", __func__, (unsigned int)response);

        if (0x1AA == response) break;
    }

    /* cmd55, then acmd41 */
    while (1) {
        cs_low();
        wait_for_card_ready();

        const uint8_t cmd55_r1_response = command_and_r1_response(55, 0);

        cs_high();

        fprintf(stderr, "%s: cmd55_r1_response: 0x%2.2X\n", __func__, cmd55_r1_response);

        if (cmd55_r1_response > 1) continue;

        fprintf(stderr, "%s: sending acmd41\n", __func__);

        cs_low();
        wait_for_card_ready();

        const uint8_t acmd41_r1_response = command_and_r1_response(41, 1U << 30);

        cs_high();

        fprintf(stderr, "%s: acmd41_r1_response: 0x%2.2X\n", __func__, acmd41_r1_response);

        if (!acmd41_r1_response) break;
    }

    spi_sd_cmd58();

    /* cmd16 */
    while (1) {
        fprintf(stderr, "%s: sending cmd16\n", __func__);
        cs_low();
        wait_for_card_ready();

        const uint8_t r1_response = command_and_r1_response(16, 512);

        cs_high();

        if (r1_response <= 1) break;
        fprintf(stderr, "%s: cmd16 response: 0x%2.2X\n", __func__, r1_response);
    }
}

int spi_sd_read_data(unsigned char * buf, unsigned long size, unsigned long address) {
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

int spi_sd_write_data_start(unsigned long size, unsigned long address) {
    const size_t blocks = size / 512;

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

            fprintf(stderr, "%s: acmd23_r1_response: 0x%2.2X\n", __func__, acmd23_r1_response);

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

void spi_sd_write_data_end(const size_t size) {
    /* if we sent cmd25, send stop tran token */
    const size_t blocks = size / 512;
    if (blocks > 1) spi_send((unsigned char[2]) { 0xfd, 0xff }, 2);

    wait_for_card_ready();

    cs_high();
}

int spi_sd_write_data(unsigned char * buf, const unsigned long size, const unsigned long address) {
    /* note that address and size must be multiples of 512 */
    if (-1 == spi_sd_write_data_start(size, address)) return -1;

    for (unsigned char * stop = buf + size; buf < stop; buf += 512) {
        spi_send_sd_block_start(buf, crc16_itu_t(0, buf, 512), size);
        if (-1 == spi_send_sd_block_finish()) return -1;
    }
    spi_sd_write_data_end(size);

    return 0;
}
