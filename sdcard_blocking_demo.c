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

    spi_sd_cmd58();

#define ERASE_CYCLE_SIZE 4194304
    for (size_t ival = 0; ival < 128; ival++) {
        buf[ival * 4 + 0] = ival;
        buf[ival * 4 + 1] = 0;
        buf[ival * 4 + 2] = 1;
        buf[ival * 4 + 3] = 2;
    }

    for (size_t iaddress = 0; iaddress < ERASE_CYCLE_SIZE * 8; iaddress += ERASE_CYCLE_SIZE) {
        const unsigned long micros_first = micros();
        unsigned long micros_before = micros_first;
        unsigned long elapsed_numerator = 0, elapsed_denominator = 0, elapsed_max = 0;

        spi_sd_write_data_start(ERASE_CYCLE_SIZE, iaddress);

        for (size_t iblock = 0; iblock < ERASE_CYCLE_SIZE / 512; iblock++) {

            spi_sd_write_one_data_block(buf, ERASE_CYCLE_SIZE);
            const unsigned long micros_now = micros();
            const unsigned long elapsed = micros_now - micros_before;
            elapsed_numerator += elapsed;
            elapsed_denominator++;
            if (elapsed > elapsed_max) {
                elapsed_max = elapsed;
                printf("%s: %lu us elapsed for block %u\n", __func__, elapsed, iblock);
            }
            micros_before = micros_now;
        }

        spi_sd_write_data_end(ERASE_CYCLE_SIZE);

        printf("%s: %lu us elapsed total\n", __func__, micros() - micros_first);
        printf("%s: %lu us max per block, %lu ns mean\n", __func__, elapsed_max, (unsigned long)((elapsed_numerator * 1000LLU + elapsed_denominator / 2) / elapsed_denominator));
    }
}

void loop() {
    __WFI();
}
