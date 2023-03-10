#include <Arduino.h>
#include <stdio.h>

#include "samd51_sercom1_sdcard.h"

void printf_block_of_hex(const unsigned char * data, const size_t size) {
    for (const unsigned char * line = data; line < data + size; line += 32) {
        for (const unsigned char * cursor = line; cursor < line + 32; cursor++)
            printf("%2.2X ", *cursor);
        printf("\n");
    }
}

static unsigned char bufs[2][2048];

void setup() {
    printf("\nhello\n");

    spi_sd_init();

  static unsigned char buf[1024];

    printf("block 0:\n");
    spi_sd_read_data(buf, 512, 0);
    printf_block_of_hex(buf, 512);
    printf("\n");

    printf("block 1:\n");
    spi_sd_read_data(buf, 512, 512);
    printf_block_of_hex(buf, 512);
    printf("\n");

    printf("block 0 and 1 together:\n");
    spi_sd_read_data(buf, 1024, 0);
    printf_block_of_hex(buf, 1024);
    printf("\n");

    printf("block 0 again:\n");
    spi_sd_read_data(buf, 512, 0);
    printf_block_of_hex(buf, 512);

    printf("\n");

#if 1

    buf[1023] = 0x41;

    printf("writing:\n");
    spi_sd_write_data(buf, 1024, 0);

    memset(buf, 0, sizeof(buf));

    printf("block 0 and 1 after mod:\n");
    spi_sd_read_data(buf, 1024, 0);
    printf_block_of_hex(buf, 1024);
    printf("\n");

    buf[1023] = 0;
    printf("writing original back:\n");
    spi_sd_write_data(buf, 1024, 0);

    printf("block 0 and 1 after writing original back:\n");
    spi_sd_read_data(buf, 1024, 0);
    printf_block_of_hex(buf, 1024);
    printf("\n");
#endif

    char error = 0;
#define ERASE_CYCLE_SIZE 4194304
#define CHUNK_SIZE 2048

    for (size_t iaddress = 0; iaddress < ERASE_CYCLE_SIZE * 8; iaddress += ERASE_CYCLE_SIZE) {
        const size_t chunks = ERASE_CYCLE_SIZE / CHUNK_SIZE;
        const unsigned long millis_first = millis();

        /* this is called once to begin the entire 4 MB transaction, as seen by the card */
        spi_sd_write_data_start(ERASE_CYCLE_SIZE, iaddress);

        /* loop over the 4 MB erase cycle in 2 kB chunks */
        for (size_t ichunk = 0; ichunk < chunks; ichunk++) {
            /* fill the current 2048-byte chunk with something, while the previous chunk is written */
            unsigned char * restrict const buf_now = bufs[ichunk % 2];
            for (size_t ival = 0; ival < CHUNK_SIZE / sizeof(uint16_t); ival++)
                memcpy(buf_now + 2 * ival, &(uint16_t) { ival }, sizeof(uint16_t));
            memcpy(buf_now, &(uint32_t) { iaddress + CHUNK_SIZE * ichunk }, sizeof(uint32_t));

            /* if not the first batch, wait for the previous batch to finish */
            if (ichunk && -1 == spi_send_sd_blocks_finish()) {
                error = 1;
                fprintf(stderr, "%s: error\n", __func__);
                break;
            }

            /* this returns more or less immediately */
            spi_send_sd_blocks_start(buf_now, CHUNK_SIZE);
        }

        if (error) break;

        /* wait for the last 2 kB chunk to finish */
        spi_send_sd_blocks_finish();

        /* tell the card we have successfully reached the end of the 4 MB transaction */
        spi_sd_write_data_end();

        const unsigned long elapsed = millis() - millis_first;
        printf("%s: %lu ns mean per %u bytes, %lu kB/s\n", __func__, (unsigned long)((elapsed * 1000000ULL + chunks / 2) / chunks), CHUNK_SIZE, ((chunks * CHUNK_SIZE * 1000) / 1024 + elapsed / 2) / elapsed);
    }

    /* just reset and wait for another connection */
    NVIC_SystemReset();
}

void loop() {
    __WFI();
}
