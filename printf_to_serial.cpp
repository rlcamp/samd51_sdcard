/* allows c code to printf() to arduino Serial, assuming newlib is the libc */
#ifdef __arm__
/* TODO: proper check for newlib */

#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif
#include <Arduino.h>

extern "C"
int _write(int file, char * buf, int bytes) {
    if (file != 1 && file != 2) return -1;
    (void)file;

    static char initted = 0;
    if (!initted) {
        Serial.begin(9600);
        while (!Serial) __WFE();
        initted = 1;
    }

    if (!Serial.dtr()) NVIC_SystemReset();
    Serial.write(buf, bytes);
    return bytes;
}

#elif defined (__AVR__)
/* TODO: implement using avr-libc stuff */
#else
#error not implemented
#endif
