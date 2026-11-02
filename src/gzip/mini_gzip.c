/*
 * BSD 2-clause license
 * Copyright (c) 2013 Wojciech A. Koszek <wkoszek@FreeBSD.org>
 *
 * Based on:
 *
 * https://github.com/strake/gzip.git
 *
 * I had to rewrite it, since strake's version was powered by UNIX FILE* API,
 * while the key objective was to perform memory-to-memory operations
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef MINI_GZ_DEBUG
#include <stdio.h>
#endif

#include "miniz.h"
#include "mini_gzip.h"

void *gzip_compress(uint8_t *data, size_t len, size_t *out_len)
{
    int flush;
    int status;
    int buf_len;
    void *buf;
    z_stream strm;

    /* Allocate buffer */
    buf_len = (int)len + 32;
    buf = malloc(buf_len);
    if (!buf) {
        return NULL;
    }

    /*
     * Miniz don't support GZip format directly, instead we will:
     *
     * - append manual GZip magic bytes
     * - inflate raw content
     * - append manual CRC32 data
     */

    /* GZIP Magic bytes */
    uint8_t *pb = (uint8_t*)buf;

    pb[0] = 0x1F;
    pb[1] = 0x8B;
    pb[2] = 8;
    pb[3] = 0;
    pb[4] = 0;
    pb[5] = 0;
    pb[6] = 0;
    pb[7] = 0;
    pb[8] = 0;
    pb[9] = 0xFF;
    pb += 10;

    /* Prepare streaming buffer context */
    memset(&strm, '\0', sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree  = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in   = data;
    strm.avail_in  = (unsigned int)len;
    strm.total_out = 0;

    flush = Z_NO_FLUSH;
    deflateInit2(&strm, Z_BEST_COMPRESSION,
                 Z_DEFLATED, -Z_DEFAULT_WINDOW_BITS, 9, Z_DEFAULT_STRATEGY);

    while (1) {
        strm.next_out  = pb + strm.total_out;
        strm.avail_out = buf_len - strm.total_out;

        if (strm.avail_in == 0) {
            flush = Z_FINISH;
        }

        status = deflate(&strm, flush);
        if (status == Z_STREAM_END) {
            break;
        }
        else if (status != Z_OK) {
            deflateEnd(&strm);
            free(buf);
            return NULL;
        }
    }

    if (deflateEnd(&strm) != Z_OK) {
        free(buf);
        return NULL;
    }
    *out_len = strm.total_out;

    /* Construct the GZip CRC32 (footer) */
    mz_ulong crc;
    int footer_start = strm.total_out + 10;

    crc = mz_crc32(MZ_CRC32_INIT, data, len);
    pb = (uint8_t*)buf;
    pb[footer_start] = crc & 0xFF;
    pb[footer_start + 1] = (crc >> 8) & 0xFF;
    pb[footer_start + 2] = (crc >> 16) & 0xFF;
    pb[footer_start + 3] = (crc >> 24) & 0xFF;
    pb[footer_start + 4] = len & 0xFF;
    pb[footer_start + 5] = (len >> 8) & 0xFF;
    pb[footer_start + 6] = (len >> 16) & 0xFF;
    pb[footer_start + 7] = (len >> 24) & 0xFF;

    /* Set the real buffer size for the caller */
    *out_len += 10 + 8;

    return buf;
}

int
mini_gz_start(struct mini_gzip *gz_ptr, const void *mem, size_t mem_len)
{
	uint8_t		*hptr, *hauxptr, *mem8_ptr;
	uint16_t	fextra_len;

	assert(gz_ptr != NULL);

	mem8_ptr = (uint8_t *)mem;
	hptr = mem8_ptr + 0;		// .gz header
	hauxptr = mem8_ptr + 10;	// auxillary header

	gz_ptr->hdr_ptr = hptr;
	gz_ptr->data_ptr = 0;
	gz_ptr->data_len = 0;
	gz_ptr->total_len = mem_len;
	gz_ptr->chunk_size = 1024;

	if (hptr[0] != 0x1F || hptr[1] != 0x8B) {
		//GZDBG("hptr[0] = %02x hptr[1] = %02x\n", hptr[0], hptr[1]);
		return (-1);
	}
	if (hptr[2] != 8) {
		return (-2);
	}
	if (hptr[3] & 0x4) {
		fextra_len = hauxptr[1] << 8 | hauxptr[0];
		gz_ptr->fextra_len = fextra_len;
		hauxptr += 2;
		gz_ptr->fextra_ptr = hauxptr;
	}
	if (hptr[3] & 0x8) {
		gz_ptr->fname_ptr = hauxptr;
		while (*hauxptr != '\0') {
			hauxptr++;
		}
		hauxptr++;
	}
	if (hptr[3] & 0x10) {
		gz_ptr->fcomment_ptr = hauxptr;
		while (*hauxptr != '\0') {
			hauxptr++;
		}
		hauxptr++;
	}
	if (hptr[3] & 0x2) /* FCRC */ {
		gz_ptr->fcrc = (*(uint16_t *)hauxptr);
		hauxptr += 2;
	}
	gz_ptr->data_ptr = hauxptr;
	gz_ptr->data_len = mem_len - (hauxptr - hptr);
	gz_ptr->magic = MINI_GZIP_MAGIC;
	return (0);
}

void
mini_gz_chunksize_set(struct mini_gzip *gz_ptr, int chunk_size)
{

	assert(gz_ptr != 0);
	assert(gz_ptr->magic == MINI_GZIP_MAGIC);
	gz_ptr->chunk_size = chunk_size;
}

void
mini_gz_init(struct mini_gzip *gz_ptr)
{

	memset(gz_ptr, 0xffffffff, sizeof(*gz_ptr));
	gz_ptr->magic = MINI_GZIP_MAGIC;
	mini_gz_chunksize_set(gz_ptr, 1024);
}


int
mini_gz_unpack(struct mini_gzip *gz_ptr, void *mem_out, size_t mem_out_len)
{
	z_stream s;
	int	ret, in_bytes_avail, bytes_to_read;

	assert(gz_ptr != 0);
	assert(gz_ptr->data_len > 0);
	assert(gz_ptr->magic == MINI_GZIP_MAGIC);

	memset (&s, 0, sizeof (z_stream));
	inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS);
	in_bytes_avail = (int)gz_ptr->data_len;
	s.avail_out = (unsigned int)mem_out_len;
	s.next_in = gz_ptr->data_ptr;
	s.next_out = (unsigned char*)mem_out;
	for (;;) {
		bytes_to_read = (int)MINI_GZ_MIN((int)gz_ptr->chunk_size, in_bytes_avail);
		s.avail_in += bytes_to_read;
		ret = mz_inflate(&s, MZ_SYNC_FLUSH);
		in_bytes_avail -= bytes_to_read;
		if (s.avail_out == 0 ) {
			break;
		}
		assert(ret != MZ_BUF_ERROR);
		if (ret == MZ_PARAM_ERROR) {
			free(mem_out);
			return (-1);
		}
		if (ret == MZ_DATA_ERROR) {
			free(mem_out);
			return (-2);
		}
		if (ret == MZ_STREAM_END) {
			break;
		}
	}
	ret = inflateEnd(&s);
	if (ret != Z_OK) {
		free(mem_out);
		return (-4);
	}
	return (s.total_out);
}
