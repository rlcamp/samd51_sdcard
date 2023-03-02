#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_sd_init(void);
void spi_sd_cmd58(void);
int spi_sd_write_data(unsigned char * buf, const unsigned long size, const unsigned long address);
int spi_sd_read_data(unsigned char * buf, const unsigned long size, const unsigned long address);

int spi_sd_write_data_start(unsigned long size, unsigned long address);
int spi_sd_write_one_data_block(unsigned char * block, const size_t size_total);
void spi_sd_write_data_end(const size_t size);

#ifdef __cplusplus
}
#endif
