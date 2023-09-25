/* minimum viable glue code between fatfs and samd51 sercom spi sd code */

/* definitions of datatypes and of functions expected by ff.c */
#include "ff.h"
#include "diskio.h"

/* block device implementation code being wrapped by this */
#include "samd51_sercom1_sdcard.h"

/* needed for INT_MAX, this will go away */
#include <limits.h>

static char initted = 0;

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return initted ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    if (!initted) spi_sd_init();
    initted = 1;
    return 0;
}

static char write_in_progress = 0;

DRESULT disk_read(BYTE pdrv, BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;

    if (write_in_progress) {
        if (-1 == spi_sd_flush_write()) return RES_PARERR;
        /* TODO: error handling */
        spi_sd_write_blocks_end();
        write_in_progress = 0;
    }

    return -1 == spi_sd_read_blocks(buff, count, sector) ? RES_PARERR : 0;
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE * buff, LBA_t sector, UINT count) {
    (void)pdrv;
    static LBA_t next_sector_of_transaction_in_progress;
    if (!write_in_progress || (write_in_progress && next_sector_of_transaction_in_progress != sector)) {
        if (write_in_progress) {
            if (-1 == spi_sd_flush_write()) return RES_ERROR;
          /* TODO: error handling */
            spi_sd_write_blocks_end();
        }
        if (-1 == spi_sd_write_blocks_start(sector)) return RES_ERROR;
    }
    spi_sd_write_more_blocks(buff, count);
    if (-1 == spi_sd_flush_write()) return RES_ERROR;
    next_sector_of_transaction_in_progress = sector + count;
    write_in_progress = 1;
    return 0;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void * buff) {
    (void)pdrv;
    if (CTRL_SYNC == cmd) {
        if (-1 == spi_sd_flush_write()) return RES_ERROR;
        if (write_in_progress)
            spi_sd_write_blocks_end();
        write_in_progress = 0;
        return 0;
    }
    else if (GET_BLOCK_SIZE == cmd) {
        LBA_t * out = buff;
        *out = 1; /* TODO: populate this from actual */
        return 0;
    }
    else if (GET_SECTOR_COUNT == cmd) {
        LBA_t * out = buff;
        *out = INT_MAX; /* TODO: populate this from actual */
    }

    return RES_PARERR;
}
