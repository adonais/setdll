#ifndef _MINI_GZIP_H_
#define _MINI_GZIP_H_

#include <stddef.h>
#include <stdint.h>

#define MAX_PATH_LEN		1024
#define	MINI_GZ_MIN(a, b)	((a) < (b) ? (a) : (b))

struct mini_gzip {
	size_t		total_len;
	size_t		data_len;
	size_t		chunk_size;

	uint32_t	magic;
#define	MINI_GZIP_MAGIC	0xbeebb00b

	uint16_t	fcrc;
	uint16_t	fextra_len;

	uint8_t		*hdr_ptr;
	uint8_t		*fextra_ptr;
	uint8_t		*fname_ptr;
	uint8_t		*fcomment_ptr;

	uint8_t		*data_ptr;
	uint8_t		pad[3];
};

#ifdef __cplusplus
extern "C" {
#endif

int   mini_gz_start(struct mini_gzip *gz_ptr, const void *mem, size_t mem_len);
int   mini_gz_unpack(struct mini_gzip* gz_ptr, void* mem_out, size_t mem_out_len);
void* gzip_compress(uint8_t* data, size_t len, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif
