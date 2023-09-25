#include <Arduino.h>
#include <stdio.h>
#include <assert.h>
#include "ff.h"

#define CHUNKSIZE 2048
#define MICROS_PER_CHUNK 5333U

void led_init(void) {
    /* pad PA23 on samd51 (pin 13 on the adafruit feather m4 express) */
    PORT->Group[0].OUTCLR.reg = (1U << 23);
    PORT->Group[0].DIRSET.reg = (1U << 23);
    PORT->Group[0].PINCFG[23].reg = 0;
}

void led_on(void) { PORT->Group[0].OUTSET.reg = (1ul << 23); }
void led_off(void) { PORT->Group[0].OUTCLR.reg = (1ul << 23); }

static void halt(void) {
    delay(100);
    USB->DEVICE.CTRLA.bit.ENABLE = 0;
    led_off();
    while (1) __WFI();
}

static FATFS * fs = &(FATFS) { };
static FIL * fp = &(FIL) { };

static unsigned long micros_prev __attribute((unused));

void setup(void) {
    led_init();

    /* this will block until usb host opens the tty */
    printf("hello\n");

    led_on();

    FRESULT fres;

    fprintf(stderr, "attempting to mount\n");
    if ((fres = f_mount(fs, "", 1))) {
        fprintf(stderr, "%s: f_mount(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: mounted\n", __func__);

#if 0
#if 1
    if ((fres = f_open(fp, "HELLO.TXT", FA_READ))) {
        fprintf(stderr, "%s: f_open(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: opened file for reading\n", __func__);

    UINT read_count;
    char buf[32];
    if ((fres = f_read(fp, buf, sizeof(buf) - 1, &read_count))) {
        fprintf(stderr, "%s: f_read(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: file contains %u bytes\n", __func__, read_count);
    buf[read_count] = '\0';
    fprintf(stderr, "%s: file contents: \"%s\"", __func__, buf);
#else
    char path[] = "HELLO.TXT";
    if ((fres = f_open(fp, path, FA_WRITE | FA_CREATE_ALWAYS))) {
        fprintf(stderr, "%s: f_open(): %d\n", __func__, fres);
        halt();
    }
    char buf[] = "hello from feather m4\n";
    UINT write_count;
    if ((fres = f_write(fp, buf, strlen(buf), &write_count))) {
        fprintf(stderr, "%s: f_read(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: wrote %u bytes\n", __func__, write_count);

#endif
    if ((fres = f_close(fp))) {
        fprintf(stderr, "%s: f_close(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: closed file\n", __func__);

    if ((fres = f_unmount(""))) {
        fprintf(stderr, "%s: f_unmount(): %d\n", __func__, fres);
        halt();
    }
    fprintf(stderr, "%s: unmounted\n", __func__);
    halt();
#else
    char path[] = "HELLO.BIN";
    if ((fres = f_open(fp, path, FA_WRITE | FA_CREATE_ALWAYS))) {
        fprintf(stderr, "%s: f_open(): %d\n", __func__, fres);
        halt();
    }

#endif

#ifdef MICROS_PER_CHUNK
    micros_prev = micros();
#endif
}

void loop(void) {
    static unsigned char chunk[CHUNKSIZE];

    led_off();

#ifdef MICROS_PER_CHUNK
    if (micros() - micros_prev < MICROS_PER_CHUNK) {
        __WFI();
        return;
    }
    micros_prev += MICROS_PER_CHUNK;
#endif
    led_on();

    static size_t counter = 0;
    /* touch the beginning and end of the chunk */
    memcpy(chunk + 0, &counter, sizeof(counter));
    memcpy(chunk + sizeof(chunk) - sizeof(counter), &counter, sizeof(counter));
    counter++;

    FRESULT fres;
    UINT write_count;
    if ((fres = f_write(fp, chunk, sizeof(chunk), &write_count))) {
        fprintf(stderr, "%s: f_write(): %d\n", __func__, fres);
        halt();
    }

    if (write_count < sizeof(chunk)) {
        fprintf(stderr, "%s: short write\n", __func__);
        halt();
    }

    if (counter >= 32768) {
        fprintf(stderr, "%s: finished\n", __func__);
          if ((fres = f_close(fp))) {
              fprintf(stderr, "%s: f_close(): %d\n", __func__, fres);
              halt();
          }
          fprintf(stderr, "%s: closed file\n", __func__);

          if ((fres = f_unmount(""))) {
              fprintf(stderr, "%s: f_unmount(): %d\n", __func__, fres);
              halt();
          }
          fprintf(stderr, "%s: unmounted\n", __func__);
          halt();
    }
}