#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_init(unsigned long baud);
void spi_change_baud_rate(unsigned long baud);

void spi_send(const void * buf, const size_t size);
void spi_receive(void * buf, const size_t size);

void cs_high(void);
void cs_low(void);
void cs_init(void);

void spi_send_sd_block_start(const void * buf, const size_t size_total);
int spi_send_sd_block_finish(void);

#ifdef __cplusplus
}
#endif
