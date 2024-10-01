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
        dprintf(2, "%s(%d): reading %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);

    for (size_t iblock = 0; iblock < count; iblock++)
        if (-1 == spi_sd_read_block((void *)((unsigned char *)buff + 512 * iblock), sector + iblock)) return RES_ERROR;

    return 0;
}

DRESULT disk_write(BYTE pdrv, const BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (diskio_verbose) {
        static LBA_t sector_next = (LBA_t)-1;
        if (sector != sector_next)
            dprintf(2, "%s(%d): writing block(s) starting at %u\r\n", __func__, __LINE__, (unsigned)sector);
        sector_next = sector + 1;
    }

    for (size_t iblock = 0; iblock < count; iblock++)
        if (-1 == spi_sd_start_writing_next_block((void *)((unsigned char *)buff + 512 * iblock), sector + iblock)) return RES_ERROR;

    return 0;
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
