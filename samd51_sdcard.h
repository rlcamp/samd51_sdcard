#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int spi_sd_init(unsigned baud_rate_reduction);
void spi_sd_shutdown(void);
void spi_sd_restore_baud_rate(void);

/* these are blocking, but internally call yield() */
int spi_sd_read_blocks(void * buf, unsigned long blocks, unsigned long long block_address);

int spi_sd_write_pre_erase(unsigned long blocks);
int spi_sd_write_blocks_start(unsigned long long block_address);
int spi_sd_write_some_blocks(const void * buf, const unsigned long blocks);
void spi_sd_write_blocks_end(void);

int spi_sd_write_blocks(const void * buf, const unsigned long blocks, const unsigned long long block_address);

/* debug stuff */
extern unsigned long last_successful_write_block_address;

#ifdef __cplusplus
}
#endif
