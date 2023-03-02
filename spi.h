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

unsigned char spi_send_sd_block(const void * buf, const uint16_t crc, const size_t size_total);

#ifdef __cplusplus
}
#endif
