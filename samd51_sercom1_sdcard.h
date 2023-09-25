#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_sd_init(void);

/* blocking...but uses dma internally and will probably expose the nonblocking api if needed */
int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address);

/* optionally, call this prior to the below, to send ACMD23 */
void spi_sd_write_pre_erase(unsigned long blocks);

/* initiates a transaction of one or more of the below */
int spi_sd_write_blocks_start(unsigned long long block_address);

/* non blocking */
void spi_sd_write_more_blocks(const void * buf, const unsigned long blocks);

/* blocks until the above has finished */
int spi_sd_flush_write(void);

/* terminates a transaction of one or more of the above */
void spi_sd_write_blocks_end(void);

/* convenience function that writes multiples of 512 bytes, blocking */
int spi_sd_write_blocks(const void * buf, const unsigned long blocks, const unsigned long long block_address);

#ifdef __cplusplus
}
#endif
