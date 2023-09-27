# SAMD51 SD card logger

The library code in this repository implements low-power SD card access from a SAMD51 using an SPI SERCOM and DMA. Glue code is also provided such that [fatfs](http://elm-chan.org/fsw/ff/) can be layered on top of it, and an example Arduino sketch is included.

### Prerequisites

The SAMD51-specific code does not depend on the Arduino ecosystem, or anything else, except the CMSIS and CMSIS-Atmel headers (which are provided by Adafruit and other board support packages within the Arduino ecosystem, but can also be acquired and used standalone).

The board-agnostic glue code between the SAMD51-specific code and the fatfs layer depends on two unmodified header files from fatfs, which are not included in this repository.

The example Arduino sketch depends on fatfs and the Arduino ecosystem.

To use this example: clone the repo, drop in `ff.c`, `ff.h`, `diskio.h`, and `ffunicode.c` from the latest distribution of [fatfs](http://elm-chan.org/fsw/ff/), open the dummy empty .ino file in the Arduino IDE, and compile and flash to an Adafruit Feather M4 with a SD card slot breakout wired up to the default SPI pins on the silkscreen (with "D4" used as the chip select pin).

### Caveats

This code is optimized for the case where almost all sectors that fatfs wants to write are consecutive, and no pre-erasing is done. Not all cards may behave optimally with this use pattern, but the two I tested were fine with it, at least after having been formatted with the official sdcard.org tool.

A fair amount of work went into making the underlying SPI SD writes non-blocking for multiple contiguous sectors staged in SRAM, before it was recognized that when adding a FAT filesystem, the only practical way to retain any kind of guarantee of progress by non-interrupt code while waiting for the SD card would be with task-based concurrency of one form or another. Therefore a dummy yield() function with weak linkage is included, which will be called prior to `__WFI()` in most places where the code must wait for a previously dispatched transaction to finish.

The Arduino sketch is not optimized for power consumption. However, the same file containing the setup() and loop() functions can be used outside the Arduino IDE and linked with [a minimal samd51 bringup implementation](https://github.com/rlcamp/samd51_blink/blob/main/samd51_init.c) and run at 48 MHz instead of the default 120 MHz, and the USB CDC serial disabled, in which case the same example code draws very little power (on the order of 10 mW total for Feather M4 + microSD card with the card on my bench, when logging 192 kB/s, using a 32 kB buffer).
