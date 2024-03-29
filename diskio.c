/* minimum viable glue code between fatfs and samd51 sercom spi sd code */

/* definitions of datatypes and of functions expected by ff.c */
#include "ff.h"
#include "diskio.h"

/* block device implementation code being wrapped by this */
#include "samd51_sdcard.h"

/* needed for INT_MAX, this will go away */
#include <limits.h>
#include <stdio.h>

static char initted = 0;

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return initted ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    if (!initted) {
        if (-1 == spi_sd_init()) return STA_NOINIT;
    }
    initted = 1;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;

//    fprintf(stderr, "%s(%d): reading %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);
    /* this will block, but will internally call yield() and __WFI() */
    return -1 == spi_sd_read_blocks(buff, count, sector) ? RES_ERROR : 0;
}

DRESULT disk_write(BYTE pdrv, const BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;
//    fprintf(stderr, "%s(%d): writing %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);
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
