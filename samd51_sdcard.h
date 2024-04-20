#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int spi_sd_init(void);

/* blocking...but uses dma internally and will probably expose the nonblocking api if needed */
int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address);

/* convenience function that writes multiples of 512 bytes, blocking */
int spi_sd_write_blocks(const void * buf, const unsigned long blocks, const unsigned long long block_address);

#ifdef __cplusplus
}
#endif
