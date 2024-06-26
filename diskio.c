/* minimum viable glue code between fatfs and samd51 sercom spi sd code */

/* definitions of datatypes and of functions expected by ff.c */
#include "ff.h"
#include "diskio.h"

/* block device implementation code being wrapped by this */
#include "samd51_sdcard.h"

/* needed for INT_MAX, this will go away */
#include <limits.h>
#include <stdio.h>

unsigned char diskio_verbose = 0;

unsigned char diskio_initted = 0;

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return diskio_initted ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    if (!diskio_initted) {
        if (-1 == spi_sd_init()) return STA_NOINIT;
    }
    diskio_initted = 1;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;

    if (diskio_verbose)
        fprintf(stderr, "%s(%d): reading %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);
    /* this will block, but will internally call yield() and __WFI() */
    return -1 == spi_sd_read_blocks(buff, count, sector) ? RES_ERROR : 0;
}

DRESULT disk_write(BYTE pdrv, const BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (diskio_verbose)
        fprintf(stderr, "%s(%d): writing %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);
    return -1 == spi_sd_write_blocks(buff, count, sector) ? RES_ERROR : 0;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void * buff) {
    (void)pdrv;
    if (CTRL_SYNC == cmd) return 0;
    else if (GET_BLOCK_SIZE == cmd)
        *(LBA_t *)buff = 1; /* TODO: populate this from actual */
    else if (GET_SECTOR_COUNT == cmd)
        *(LBA_t *)buff = INT_MAX; /* TODO: populate this from actual */
    else return RES_PARERR;
    return 0;
}

#if __has_include(<samd51.h>)
/* newer cmsis-atmel from upstream */
#include <samd51.h>
#else
/* older cmsis-atmel from adafruit */
#include <samd.h>
#endif

DWORD get_fattime(void) {
    if (!RTC->MODE2.CTRLA.bit.ENABLE) return 0;

    RTC->MODE2.CTRLA.bit.CLOCKSYNC = 1;
    while (RTC->MODE2.SYNCBUSY.reg);
    const RTC_MODE2_CLOCK_Type now = RTC->MODE2.CLOCK;

    return ((now.bit.YEAR + 2000 - 1980) << 25U |
            now.bit.MONTH << 21U |
            now.bit.DAY << 16U |
            now.bit.HOUR << 11U |
            now.bit.MINUTE << 5U |
            now.bit.SECOND >> 1U);
}
