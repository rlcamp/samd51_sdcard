# Minimalist SAMD51 SD card writer

The library code in this repository implements low-power SD card access from a SAMD51 using an SPI SERCOM and DMA. Glue code is also provided such that [fatfs](http://elm-chan.org/fsw/ff/) can be layered on top of it.

### Prerequisites

The SAMD51-specific code depends only on the CMSIS and CMSIS-Atmel headers (which are provided by Adafruit and other board support packages within the Arduino ecosystem, but can also be acquired and used standalone).

The board-agnostic glue code between the SAMD51-specific code and the fatfs layer depends on two unmodified header files from fatfs, which are not included in this repository.

To use this: clone the repo, drop in `ff.c`, `ff.h`, `diskio.h`, and `ffunicode.c` from the latest distribution of [fatfs](http://elm-chan.org/fsw/ff/), and call the usual functions documented therein.

### Caveats

A fair amount of work went into making the underlying SPI SD writes non-blocking for multiple contiguous sectors staged in SRAM, before it was recognized that when adding a FAT filesystem, the only practical way to retain any kind of guarantee of progress by non-interrupt code while waiting for the SD card would be with task-based concurrency of one form or another. Therefore a dummy yield() function with weak linkage is included, which will be called in most places where the code must wait for a previously dispatched transaction to finish.

Writes of individual blocks of 512 bytes from the application layer, via an intermediate layer such as fatfs, can be made partially nonblocking by first calling a function which promises the underlying card layer that the pointed-to memory will not go out of scope during the write. This allows fatfs to continue to assume that its own writes are blocking, while still allowing the application layer to make progress during writes when possible.
