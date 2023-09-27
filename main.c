#ifdef NON_ANCIENT_HEADER_PATHS
/* newer cmsis-atmel from upstream */
#include <samd51j19a.h>
#else
/* older cmsis-atmel from adafruit */
#include <samd.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "ff.h"

#ifndef ARDUINO
#include "samd_usb_cdc_serial.h"

int _write(int fd, void * bytes, int len) {
//    return len;
    (void)fd; /* just send stderr and stdout to same ep */
    static char initted = 0;
    if (!initted) {
        usb_cdc_serial_begin();
        usb_cdc_serial_wait_for_dtr_to_go_high();
        initted = 1;
    }

    while (usb_cdc_serial_still_sending());
    static char outbuf[256] __attribute((aligned(4)));
    memcpy(outbuf, bytes, len);
    usb_cdc_serial_write(outbuf, len);

    return len;
}
#endif

#define CHUNKSIZE 32768
#define TC4_TICKS_PER_CHUNK 1

void led_init(void) {
    /* pad PA23 on samd51 (pin 13 on the adafruit feather m4 express) */
    PORT->Group[0].OUTCLR.reg = (1U << 23);
    PORT->Group[0].DIRSET.reg = (1U << 23);
    PORT->Group[0].PINCFG[23].reg = 0;
}

void led_on(void) { PORT->Group[0].OUTSET.reg = (1ul << 23); }
void led_off(void) { PORT->Group[0].OUTCLR.reg = (1ul << 23); }

void timer4_init(void) {
    /* make sure the APB is enabled for TC4 */
    MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC4;

    /* use the 32 kHz clock peripheral as the source for TC4 */
    GCLK->PCHCTRL[TC4_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK3_Val | (1U << GCLK_PCHCTRL_CHEN_Pos);
    while (GCLK->SYNCBUSY.reg);

    /* reset the TC4 peripheral */
    TC4->COUNT16.CTRLA.bit.SWRST = 1;
    while (TC4->COUNT16.SYNCBUSY.bit.SWRST);

    /* put it in 16 bit mode */
    TC4->COUNT16.CTRLA.bit.MODE = TC_CTRLA_MODE_COUNT16_Val;

    /* timer ticks will be 32 kHz clock ticks divided by this prescaler value */
    TC4->COUNT16.CTRLA.bit.PRESCALER = TC_CTRLA_PRESCALER_DIV1_Val;

    /* counter resets after the value in cc[0], i.e. its period is that number plus one */
    TC4->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
    TC4->COUNT16.CC[0].reg = 5592;

    /* fire an interrupt whenever counter equals that value */
    TC4->COUNT16.INTENSET.reg = 0;
    TC4->COUNT16.INTENSET.bit.MC0 = 1;
    NVIC_EnableIRQ(TC4_IRQn);

    /* enable the timer */
    while (TC4->COUNT16.SYNCBUSY.reg);
    TC4->COUNT16.CTRLA.bit.ENABLE = 1;
    while (TC4->COUNT16.SYNCBUSY.bit.ENABLE);
}

static volatile _Atomic unsigned long tc4_ticks = 0;

void TC4_Handler(void) {
    if (!TC4->COUNT16.INTFLAG.bit.MC0) return;
    TC4->COUNT16.INTFLAG.reg = (TC_INTFLAG_Type){ .bit.MC0 = 1 }.reg;

    tc4_ticks++;
}

static void halt(void) {
    /* wait a couple ticks before disabling USB */
    for (const unsigned long ticks_initial = tc4_ticks; tc4_ticks - ticks_initial < 2;) __WFI();
    USB->DEVICE.CTRLA.bit.ENABLE = 0;
    led_off();
    while (1) __WFI();
}

static FATFS * fs = &(FATFS) { };
static FIL * fp = &(FIL) { };

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
#if 0
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
    timer4_init();
}

void loop(void) {
    static unsigned char chunk[CHUNKSIZE];

#ifdef TC4_TICKS_PER_CHUNK
    static unsigned long ticks_prev = 0;
    if (__DSB(), tc4_ticks - ticks_prev < TC4_TICKS_PER_CHUNK) {
        __WFI();
        return;
    }
    ticks_prev += TC4_TICKS_PER_CHUNK;
#endif
    led_on();

#if 1
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
#else
    FRESULT fres;
    UINT read_count;
    if ((fres = f_read(fp, chunk, sizeof(chunk), &read_count))) {
        fprintf(stderr, "%s: f_read(): %d\n", __func__, fres);
        halt();
    }
    if (read_count < sizeof(chunk)) {
        fprintf(stderr, "%s: short read\n", __func__);
        halt();
    }

    static size_t counter = 0;
    counter++;
#endif
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
    led_off();
}

#ifndef ARDUINO
int main(void) {
    setup();
    while (1) loop();
}
#endif
