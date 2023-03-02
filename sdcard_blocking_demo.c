#include <Arduino.h>
#include <stdio.h>

#include "sdcard_blocking.h"
#include "spi.h"

void printf_block_of_hex(const unsigned char * data, const size_t size) {
    for (const unsigned char * line = data; line < data + size; line += 32) {
        for (const unsigned char * cursor = line; cursor < line + 32; cursor++)
            printf("%2.2X ", *cursor);
        printf("\n");
    }
}

void setup() {
    printf("\nhello\n");

    spi_sd_init();

    unsigned char buf[1024];
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
    for (size_t iaddress = 0; iaddress < ERASE_CYCLE_SIZE * 8; iaddress += ERASE_CYCLE_SIZE) {
        const size_t blocks = ERASE_CYCLE_SIZE / 512;
        const unsigned long millis_first = millis();

        spi_sd_write_data_start(ERASE_CYCLE_SIZE, iaddress);

        for (size_t iblock = 0; iblock < blocks; iblock++) {
            unsigned char * restrict const buf_now = buf + 512 * (iblock % 2);

            for (size_t ival = 0; ival < 128; ival++) {
                buf_now[ival * 4 + 0] = ival;
                buf_now[ival * 4 + 1] = 0;
                buf_now[ival * 4 + 2] = 1;
                buf_now[ival * 4 + 3] = 2;
            }

            if (iblock)
                if (-1 == spi_send_sd_block_finish()) {
                    error = 1;
                    break;
                }
            spi_send_sd_block_start(buf_now, ERASE_CYCLE_SIZE);
        }

        if (error) break;

        spi_send_sd_block_finish();

        spi_sd_write_data_end(ERASE_CYCLE_SIZE);

        const unsigned long elapsed = millis() - millis_first;

        printf("%s: %lu ns mean per 512 bytes, %lu kB/s\n", __func__, (unsigned long)((elapsed * 1000000ULL + blocks / 2) / blocks), ((blocks * 512 * 1000) / 1024 + elapsed / 2) / elapsed);
    }
}

void loop() {
    __WFI();
}
