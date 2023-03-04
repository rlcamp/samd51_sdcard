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

void spi_sd_init(void);

/* blocking...but uses dma internally and will probably expose the nonblocking api if needed */
int spi_sd_read_data(unsigned char * buf, const unsigned long size, const unsigned long address);

/* initiates a transaction of one or more of the below */
int spi_sd_write_data_start(unsigned long size, unsigned long address);

/* non blocking */
void spi_send_sd_block_start(const void * buf, const size_t size_total);

/* blocks until the above has finished */
int spi_send_sd_block_finish(void);

/* terminates a transaction of one or more of the above */
void spi_sd_write_data_end(const size_t size);

/* convenience function that writes multiples of 512 bytes, blocking */
int spi_sd_write_data(unsigned char * buf, const unsigned long size, const unsigned long address);


#ifdef __cplusplus
}
#endif
