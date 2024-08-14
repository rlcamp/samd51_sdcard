#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int spi_sd_init(void);
void spi_sd_shutdown(void);

int spi_sd_read_block(void * buf, unsigned long long block_address);

/* possibly nonblocking, finalizes prior multi-block write transaction if necessary */
int spi_sd_start_writing_next_block(const void * buf, const unsigned long long block_address);

/* if this is called before the above function, with the same argument, then the subsequent
 write of that pointer will be nonblocking. the intended use case is to allow an unmodified
 intermediate layer such as fatfs to assume writes are blocking, which is necessary for its
 writes of internal filesystem blocks, but allow application code which has an independent
 guarantee that the pointed-to memory will not be modified for the duration of the
 nonblocking write to continue to do work while the write takes place */
void spi_sd_mark_pointer_for_non_blocking_write(const void * p);

void spi_sd_mark_pointer_for_pre_erase(const void * p, const unsigned long blocks);

/* if it will be a while before the next card read/write (via fatfs or otherwise),
 call this, which will finalize any multi-block transaction in progress */
int spi_sd_flush_and_sleep(void);

#ifdef __cplusplus
}
#endif
