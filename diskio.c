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
        for (size_t ipass = 0;; ipass++) {
            if (ipass > 0 && diskio_verbose)
                dprintf(2, "%s: retrying at lower baud rate %u\r\n", __func__, (unsigned)ipass + 1);
            if (spi_sd_init(ipass) != -1) break;
            if (ipass > 3) return STA_NOINIT;
        }
        spi_sd_restore_baud_rate();
    }
    diskio_initted = 1;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;

    if (diskio_verbose)
        dprintf(2, "%s(%d): reading %u blocks starting at %u\r\n", __func__, __LINE__, count, (unsigned)sector);

    for (size_t ipass = 0;; ipass++) {
        if (ipass > 0) {
            if (diskio_verbose)
                dprintf(2, "%s: retrying at lower baud rate %u\r\n", __func__, (unsigned)ipass + 1);
            if (-1 == spi_sd_init(ipass)) continue;
        }

        /* this will block, but will internally call yield() and __WFI() */
        if (spi_sd_read_blocks(buff, count, sector) != -1) break;
        if (ipass > 3) return RES_ERROR;
    }

    spi_sd_restore_baud_rate();
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

    for (size_t ipass = 0;; ipass++) {
        if (ipass > 0) {
            if (diskio_verbose)
                dprintf(2, "%s: retrying at lower baud rate %u\r\n", __func__, (unsigned)ipass + 1);
            if (-1 == spi_sd_init(ipass)) continue;
        }

        if (spi_sd_write_blocks(buff, count, sector) != -1) break;
        if (ipass > 3) return RES_ERROR;

    }

    spi_sd_restore_baud_rate();
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
