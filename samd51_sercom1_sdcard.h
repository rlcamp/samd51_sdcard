#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_sd_init(void);

/* blocking...but uses dma internally and will probably expose the nonblocking api if needed */
int spi_sd_read_data(unsigned char * buf, const unsigned long size, const unsigned long long address);

/* initiates a transaction of one or more of the below */
int spi_sd_write_data_start(unsigned long size, unsigned long long address);

/* non blocking */
void spi_send_sd_block_start(const void * buf);

/* blocks until the above has finished */
int spi_send_sd_block_finish(void);

/* terminates a transaction of one or more of the above */
void spi_sd_write_data_end(void);

/* convenience function that writes multiples of 512 bytes, blocking */
int spi_sd_write_data(unsigned char * buf, const unsigned long size, const unsigned long long address);

#ifdef __cplusplus
}
#endif
