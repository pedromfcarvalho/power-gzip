/*
 * NX-GZIP compression accelerator user library
 * implementing zlib compression library interfaces
 *
 * Copyright (C) IBM Corporation, 2011-2017
 *
 * Licenses for GPLv2 and Apache v2.0:
 *
 * GPLv2:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * Apache v2.0:
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Bulent Abali <abali@us.ibm.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <endian.h>
#include "zlib.h"
#include "copy-paste.h"
#include "nx-ftw.h"
#include "nxu.h"
#include "nx.h"
#include "nx-gzip.h"
#include "nx_zlib.h"
#include "nx_dbg.h"

#define INF_HIS_LEN (1<<15) /* Fixed 32K history length */
#define fifo_out_len_check(s) \
do { if ((s)->cur_out > (s)->len_out/2) { \
	memmove((s)->fifo_out, (s)->fifo_out + (s)->cur_out - INF_HIS_LEN, INF_HIS_LEN + (s)->used_out); \
	(s)->cur_out = INF_HIS_LEN; } \
} while(0)
#define fifo_in_len_check(s) \
do { if ((s)->cur_in > (s)->len_in/2) { \
	memmove((s)->fifo_in, (s)->fifo_in + (s)->cur_in, (s)->used_in); \
	(s)->cur_in = 0; } \
} while(0)

static int nx_inflate_(nx_streamp s, int flush);

int nx_inflateResetKeep(z_streamp strm)
{
	nx_streamp s;
	if (strm == Z_NULL)
		return Z_STREAM_ERROR;
	s = (nx_streamp) strm->state;
	strm->total_in = strm->total_out = s->total_in = 0;
	strm->msg = Z_NULL;
	return Z_OK;
}

int nx_inflateReset(z_streamp strm)
{
	nx_streamp s;
	if (strm == Z_NULL)
		return Z_STREAM_ERROR;

	s = (nx_streamp) strm->state;
	strm->msg = Z_NULL;

	if (s->wrap)
		s->adler = s->wrap & 1;

	s->total_in = s->total_out = 0;
	
	s->used_in = s->used_out = 0;
	s->cur_in = 0;
	s->cur_out = INF_HIS_LEN; /* keep a 32k gap here */
	s->inf_state = 0;
	s->resuming = 0;
	s->history_len = 0;

	s->nxcmdp  = &s->nxcmd0;

	s->crc32 = INIT_CRC;
	s->adler32 = INIT_ADLER;
	s->ckidx = 0;
	s->cksum = INIT_CRC;	
	s->havedict = 0;
		
	return nx_inflateResetKeep(strm);
}

static int nx_inflateReset2(z_streamp strm, int windowBits)
{
	int wrap;
	nx_streamp s;

	if (strm == Z_NULL) return Z_STREAM_ERROR;
	s = (nx_streamp) strm->state;
	if (s == NULL) return Z_STREAM_ERROR;
	
	/* extract wrap request from windowBits parameter */
	if (windowBits < 0) {
		wrap = HEADER_RAW;
		windowBits = -windowBits;
	}
	else if (windowBits >= 8 && windowBits <= 15)
		wrap = HEADER_ZLIB;
	else if (windowBits >= 8+16 && windowBits <= 15+16)
		wrap = HEADER_GZIP;
	else if (windowBits >= 8+32 && windowBits <= 15+32)
		wrap = HEADER_ZLIB | HEADER_GZIP; /* auto detect header */
	else
		return Z_STREAM_ERROR;

	s->wrap = wrap;
	s->windowBits = windowBits;

	return nx_inflateReset(strm);
}

int nx_inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size)
{
	int ret;
	nx_streamp s;
	nx_devp_t h;

	nx_hw_init();

	if (version == Z_NULL || version[0] != ZLIB_VERSION[0] ||
	    stream_size != (int)(sizeof(z_stream)))
		return Z_VERSION_ERROR;

	if (strm == Z_NULL) return Z_STREAM_ERROR;
	
	/* statistic */
	zlib_stats_inc(&zlib_stats.inflateInit);

	strm->msg = Z_NULL; /* in case we return an error */

	h = nx_open(-1); /* if want to pick specific NX device, set env NX_GZIP_DEV_NUM */
	if (!h) {
		prt_err("cannot open NX device\n");
		return Z_STREAM_ERROR;
	}

	s = nx_alloc_buffer(sizeof(*s), nx_config.page_sz, 0);
	memset(s, 0, sizeof(*s));

	if (s == NULL) {
		prt_err("nx_alloc_buffer\n");		
		return Z_MEM_ERROR;
	}

	s->zstrm   = strm;
	s->nxcmdp  = &s->nxcmd0;
	s->page_sz = nx_config.page_sz;
	s->nxdevp  = h;
	// s->gzhead  = NULL;
	s->gzhead  = nx_alloc_buffer(sizeof(gz_header), nx_config.page_sz, 0);
	s->ddl_in  = s->dde_in;
	s->ddl_out = s->dde_out;

	/* small input data will be buffered here */
	s->fifo_in = NULL;

	/* overflow buffer */
	s->fifo_out = NULL;
	
	strm->state = (void *) s;

	ret = nx_inflateReset2(strm, windowBits);
	if (ret != Z_OK) {
		prt_err("nx_inflateReset2\n");
		goto reset_err;
	}

	return ret;

reset_err:	
alloc_err:
	if (s->gzhead)
		nx_free_buffer(s->gzhead, 0, 0);
	if (s)
		nx_free_buffer(s, 0, 0);
	strm->state = Z_NULL;
	return ret;
}

int nx_inflateInit_(z_streamp strm, const char *version, int stream_size)
{
	return nx_inflateInit2_(strm, DEF_WBITS, version, stream_size);
}

int nx_inflateEnd(z_streamp strm)
{
	nx_streamp s;	

	if (strm == Z_NULL) return Z_STREAM_ERROR;
	s = (nx_streamp) strm->state;
	if (s == NULL) return Z_STREAM_ERROR;

	/* statistic */
	zlib_stats_inc(&zlib_stats.inflateEnd);

	/* TODO add here Z_DATA_ERROR if the stream was freed
	   prematurely (when some input or output was discarded). */

	nx_inflateReset(strm);
	
	nx_free_buffer(s->fifo_in, s->len_in, 0);
	nx_free_buffer(s->fifo_out, s->len_out, 0);
	nx_close(s->nxdevp);
	
	if (s->gzhead != NULL) nx_free_buffer(s->gzhead, sizeof(gz_header), 0);

	nx_free_buffer(s, sizeof(*s), 0);

	return Z_OK;
}

int nx_inflate(z_streamp strm, int flush)
{
	int rc = Z_OK;
	nx_streamp s;
	unsigned int avail_in_slot, avail_out_slot;
	uint64_t t1, t2;	

	if (strm == Z_NULL) return Z_STREAM_ERROR;
	s = (nx_streamp) strm->state;
	if (s == NULL) return Z_STREAM_ERROR;

	if (flush == Z_BLOCK || flush == Z_TREES) {
		strm->msg = (char *)"Z_BLOCK or Z_TREES not implemented";
		prt_err("Z_BLOCK or Z_TREES not implemented!\n");
		return Z_STREAM_ERROR;
	}

	if (s->fifo_out == NULL) {
		/* overflow buffer is about 40% of s->avail_in */
		s->len_out = (INF_HIS_LEN*2 + (s->zstrm->avail_in * 40)/100);
		if (NULL == (s->fifo_out = nx_alloc_buffer(s->len_out, nx_config.page_sz, 0))) {
			prt_err("nx_alloc_buffer for inflate fifo_out\n");
			return Z_MEM_ERROR;
		}
	}

	/* statistic */ 
	if (nx_gzip_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		avail_in_slot = strm->avail_in / 4096;
		if (avail_in_slot >= ZLIB_SIZE_SLOTS)
			avail_in_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_avail_in[avail_in_slot]++;

		avail_out_slot = strm->avail_out / 4096;
		if (avail_out_slot >= ZLIB_SIZE_SLOTS)
			avail_out_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_avail_out[avail_out_slot]++;
		zlib_stats.inflate++;

		zlib_stats.inflate_len += strm->avail_in;
		t1 = get_nxtime_now();
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	s->next_in = s->zstrm->next_in;
	s->avail_in = s->zstrm->avail_in;
	s->next_out = s->zstrm->next_out;
	s->avail_out = s->zstrm->avail_out;

inf_forever:
	/* inflate state machine */
	
	switch (s->inf_state) {
		unsigned int c, copy;

	case inf_state_header:
		if (s->wrap == (HEADER_ZLIB | HEADER_GZIP)) {
			/* auto detect zlib/gzip */
			nx_inflate_get_byte(s, c);
			if (c == 0x1f) /* looks like gzip */
				s->inf_state = inf_state_gzip_id2;
			/* looks like zlib */
			else if (((c & 0xf0) == 0x80) && ((c & 0x0f) < 8)) 
				s->inf_state = inf_state_zlib_flg; 
			else {
				strm->msg = (char *)"incorrect header";
				s->inf_state = inf_state_data_error;
			}
		}
		else if (s->wrap == HEADER_ZLIB) {
			/* look for a zlib header */
			s->inf_state = inf_state_zlib_id1;
			if (s->gzhead != NULL)
				s->gzhead->done = -1;			
		}
		else if (s->wrap == HEADER_GZIP) {
			/* look for a gzip header */			
			if (s->gzhead != NULL)
				s->gzhead->done = 0;
			s->inf_state = inf_state_gzip_id1;
		}
		else {
			/* raw inflate doesn't use checksums but we do
			 * it anyway since comes for free */
			s->crc32 = INIT_CRC;
			s->adler32 = INIT_ADLER;			
			s->inf_state = inf_state_inflate; /* go to inflate proper */
		}
		break;
		
	case inf_state_gzip_id1:		
		nx_inflate_get_byte(s, c);
		if (c != 0x1f) {
			strm->msg = (char *)"incorrect gzip header";			
			s->inf_state = inf_state_data_error;
			break;
		}

		s->inf_state = inf_state_gzip_id2;
		/* fall thru */
		
	case inf_state_gzip_id2:

		nx_inflate_get_byte(s, c);
		if (c != 0x8b) {
			strm->msg = (char *)"incorrect gzip header"; 			
			s->inf_state = inf_state_data_error;
			break;
		}

		s->inf_state = inf_state_gzip_cm;
		/* fall thru */

	case inf_state_gzip_cm:

		nx_inflate_get_byte(s, c);
		if (c != 0x08) {
			strm->msg = (char *)"unknown compression method";
			s->inf_state = inf_state_data_error;
			break;
		}

		s->inf_state = inf_state_gzip_flg;
		/* fall thru */		
		
	case inf_state_gzip_flg:
		
		nx_inflate_get_byte(s, c);
		s->gzflags = c;

		if (s->gzflags & 0xe0 != 0) { /* reserved bits are set */
			strm->msg = (char *)"unknown header flags set";
			s->inf_state = inf_state_data_error;
			break;
		}

		if (s->gzhead != NULL) {
			/* FLG field of the file says this is compressed text */
			s->gzhead->text = (int) (s->gzflags & 1);
			s->gzhead->time = 0;			
		}

		s->inf_held = 0;
		s->inf_state = inf_state_gzip_mtime;
		/* fall thru */

	case inf_state_gzip_mtime:

		if (s->gzhead != NULL){
			while( s->inf_held < 4) { /* need 4 bytes for MTIME */
				nx_inflate_get_byte(s, c);
				s->gzhead->time = s->gzhead->time << 8 | c;
				++ s->inf_held;
			}
			s->gzhead->time = le32toh(s->gzhead->time);
			s->inf_held = 0;
			assert( ((s->gzhead->time & (1<<31)) == 0) );
			/* assertion is a reminder for endian check; either
			   fires right away or in the year 2038 if we're still
			   alive */
		}
		s->inf_state = inf_state_gzip_xfl;		
		/* fall thru */

	case inf_state_gzip_xfl:		

		nx_inflate_get_byte(s, c);
		if (s->gzhead != NULL)
			s->gzhead->xflags = c;

		s->inf_state = inf_state_gzip_os;					
		/* fall thru */
		
	case inf_state_gzip_os:

		nx_inflate_get_byte(s, c);		
		if (s->gzhead != NULL)
			s->gzhead->os = c;

		s->inf_held = 0;
		s->length = 0;		
		s->inf_state = inf_state_gzip_xlen;
		/* fall thru */		

	case inf_state_gzip_xlen:

		if (s->gzflags & 0x04) { /* fextra was set */
			while( s->inf_held < 2 ) {
				nx_inflate_get_byte(s, c);
				s->length = s->length | (c << (s->inf_held * 8));
				++ s->inf_held;
			}

			s->length = le32toh(s->length);
			if (s->gzhead != NULL) 
				s->gzhead->extra_len = s->length;
		}
		else if (s->gzhead != NULL)
			s->gzhead->extra = NULL;
		s->inf_held = 0;
		s->inf_state = inf_state_gzip_extra;
		/* fall thru */
	case inf_state_gzip_extra:

		if (s->gzflags & 0x04) { /* fextra was set */
			copy = s->length;
			if (copy > s->avail_in) copy = s->avail_in;
			if (copy) {
				if (s->gzhead != NULL &&
				    s->gzhead->extra != NULL) {
					unsigned int len = s->gzhead->extra_len - s->length;
					memcpy(s->gzhead->extra + len, s->next_in,
					       len + copy > s->gzhead->extra_max ?
					       s->gzhead->extra_max - len : copy);
				}
				update_stream_in(s,copy);
				s->length -= copy;
			}
			if (s->length) goto inf_return; /* more extra data to copy */
		}

		s->length = 0;
		s->inf_state = inf_state_gzip_name;
		/* fall thru */
		
	case inf_state_gzip_name:

		if (s->gzflags & 0x08) { /* fname was set */
			if (s->avail_in == 0) goto inf_return;
			if (s->gzhead != NULL && s->gzhead->name != NULL)
				s->gzhead->name[s->gzhead->name_max-1] = 0;
			copy = 0;
			do {
				c = (unsigned int)(s->next_in[copy++]);
				if (s->gzhead != NULL &&
				    s->gzhead->name != NULL &&
				    s->length < s->gzhead->name_max )
					s->gzhead->name[s->length++] = (char) c;
				/* copy until the \0 character is
				   found.  inflate original looks
				   buggy to me looping without limits. 
				   malformed input file should not sigsegv zlib */
			} while (!!c && copy < s->avail_in && s->length < s->gzhead->name_max);
			s->avail_in -= copy;
			s->next_in  += copy;
			if (!!c) goto inf_return; /* need more name */
		}
		else if (s->gzhead != NULL)
			s->gzhead->name = NULL;

		s->length = 0;
		s->inf_state = inf_state_gzip_comment;
		/* fall thru */

	case inf_state_gzip_comment:

		if (s->gzflags & 0x10) { /* fcomment was set */
			if (s->avail_in == 0) goto inf_return;
			if (s->gzhead != NULL && s->gzhead->comment != NULL) {
				/* terminate with \0 for safety */
				s->gzhead->comment[s->gzhead->comm_max-1] = 0;
			}
			copy = 0;
			do {
				c = (unsigned int)(s->next_in[copy++]);
				if (s->gzhead != NULL &&
				    s->gzhead->comment != NULL &&
				    s->length < s->gzhead->comm_max )
					s->gzhead->comment[s->length++] = (char) c;
			} while (!!c && copy < s->avail_in && s->length < s->gzhead->comm_max);
			s->avail_in -= copy;
			s->next_in  += copy;
			if (!!c) goto inf_return; /* need more comment */
		}
		else if (s->gzhead != NULL)
			s->gzhead->comment = NULL;

		s->length = 0;
		s->inf_held = 0;
		s->inf_state = inf_state_gzip_hcrc;
		/* fall thru */
			
	case inf_state_gzip_hcrc:

		if (s->gzflags & 0x02) { /* fhcrc was set */

			while( s->inf_held < 2 ) {
				nx_inflate_get_byte(s, c);
				s->hcrc16 = s->hcrc16 << 8 | c;
				++ s->inf_held;
			}
			s->hcrc16 = le16toh(s->hcrc16);
			s->gzhead->hcrc = 1;
			s->gzhead->done = 1;

			/* Compare stored and compute hcrc checksums here */

			if (s->hcrc16 != s->cksum & 0xffff) {
				strm->msg = (char *)"header crc mismatch";
				s->inf_state = inf_state_data_error;
				break;
			}
		}
		else if (s->gzhead != NULL)
			s->gzhead->hcrc = 0;
		
		s->inf_held = 0;
		s->adler = s->crc32 = INIT_CRC;
		s->inf_state = inf_state_inflate; /* go to inflate proper */
		
		break;
		
	case inf_state_zlib_id1:
		nx_inflate_get_byte(s, c);
		if ((c & 0x0f) != 0x08) {
			strm->msg = (char *)"unknown compression method";
			s->inf_state = inf_state_data_error;
			break;
		} else if (((c & 0xf0) >> 4) >= 8) {
			strm->msg = (char *)"invalid window size";
			s->inf_state = inf_state_data_error;
			break;
		}
		else {
			s->inf_state = inf_state_zlib_flg; /* zlib flg field */
			s->zlib_cmf = c;
		}
		/* fall thru */

	case inf_state_zlib_flg:
		nx_inflate_get_byte(s, c);
		if ( ((s->zlib_cmf * 256 + c) % 31) != 0 ) {
			strm->msg = (char *)"incorrect header check";
			s->inf_state = inf_state_data_error;
			break;
		}
		/* FIXME: Need double check and test here */
		if (c & 1<<5) {
			s->inf_state = inf_state_zlib_dictid;
			s->dictid = 0;			
		}
		else {
			s->inf_state = inf_state_inflate; /* go to inflate proper */
			s->adler = s->adler32 = INIT_ADLER;
		} 
		s->inf_held = 0;
		break;

	case inf_state_zlib_dictid:		
		while( s->inf_held < 4) { 
			nx_inflate_get_byte(s, c);
			s->dictid = (s->dictid << 8) | (c & 0xff);
			++ s->inf_held;
		}

		strm->adler = s->dictid; /* ask user to supply this dictionary */		
		s->inf_state = inf_state_zlib_dict;
		s->inf_held = 0;

	case inf_state_zlib_dict:				

		if (s->havedict == 0) {
			/* RESTORE(); ?? */
			return Z_NEED_DICT;
		}
		s->adler = s->adler32 = INIT_ADLER;
		s->inf_state = inf_state_inflate; /* go to inflate proper */

	case inf_state_inflate:
		rc = nx_inflate_(s, flush);
		goto inf_return;
	case inf_state_data_error:
		rc = Z_DATA_ERROR;
		goto inf_return;
	case inf_state_mem_error:
		rc = Z_MEM_ERROR;
		break;
	case inf_state_buf_error:
		rc = Z_BUF_ERROR;
		break;		
		
	default:
		rc = Z_STREAM_ERROR;
		break;
	}
	goto inf_forever;
	
inf_return:

	/* statistic */
	if (nx_gzip_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		t2 = get_nxtime_now();
		zlib_stats.inflate_time += get_nxtime_diff(t1,t2);
		pthread_mutex_unlock(&zlib_stats_mutex);	
	}
	return rc;
}	

static int nx_inflate_(nx_streamp s, int flush)
{
	/* queuing, file ops, byte counting */
	int read_sz, n;
	int write_sz, free_space, copy_sz, source_sz, target_sz;
	uint64_t last_comp_ratio; /* 1000 max */
	uint64_t total_out;
	int is_final = 0, is_eof;
	int cnt = 0;
	void* fsa;
	long loop_cnt = 0, loop_max = 0xffff;
	
	/* nx hardware */
	int sfbt, subc, spbc, tpbc, nx_ce, fc, resuming = 0;
	int history_len = 0;
	nx_gzip_crb_cpb_t *cmdp = s->nxcmdp;
        nx_dde_t *ddl_in = s->ddl_in;
        nx_dde_t *ddl_out = s->ddl_out;
	int pgfault_retries, nx_space_retries;
	int cc, rc;

	print_dbg_info(s, __LINE__);

	if ((flush == Z_FINISH) && (s->avail_in == 0) && (s->used_out == 0))
		return Z_STREAM_END;

	if (s->avail_in == 0 && s->used_in == 0 &&
	    s->avail_out == 0 && s->used_out == 0)
		return Z_STREAM_END;

copy_fifo_out_to_next_out:
	if (++loop_cnt == loop_max) {
		prt_err("cannot make progress; too many loops loop_cnt = %d\n", loop_cnt);
		return Z_STREAM_END;
	}

	/* if fifo_out is not empty, first copy contents to next_out.
	 * Remember to keep up to last 32KB as the history in fifo_out. */
	if (s->used_out > 0) {
		write_sz = NX_MIN(s->used_out, s->avail_out);
		if (write_sz > 0) {
			memcpy(s->next_out, s->fifo_out + s->cur_out, write_sz);
			update_stream_out(s, write_sz);
			update_stream_out(s->zstrm, write_sz);
			s->used_out -= write_sz;
			s->cur_out += write_sz;
			fifo_out_len_check(s);
		}
		print_dbg_info(s, __LINE__);

		if (s->used_out > 0 && s->avail_out == 0) return Z_OK; /* Need more space */

		/* if final is find, will not go ahead */
		if (s->is_final == 1 && s->used_in == 0) return Z_STREAM_END;
	}

	assert(s->used_out == 0);

	/* if s->avail_out and  s->avail_in is 0, return */
	if (s->avail_out == 0 || (s->avail_in == 0 && s->used_in == 0)) return Z_OK;
	if (s->used_out == 0 && s->avail_in == 0 && s->used_in == 0) return Z_OK;
	/* we should flush all data to next_out here, s->used_out should be 0 */

small_next_in:
	/* used_in is the data amount waiting in fifo_in; avail_in is
	   the data amount waiting in the user buffer next_in */
	if (s->avail_in < nx_config.soft_copy_threshold && s->avail_out > 0) {
		if (s->fifo_in == NULL) {
			// s->len_in = nx_config.inflate_fifo_in_len;
			s->len_in = nx_config.soft_copy_threshold * 2;
			if (NULL == (s->fifo_in = nx_alloc_buffer(s->len_in, nx_config.page_sz, 0))) {
				prt_err("nx_alloc_buffer for inflate fifo_in\n");		
				return Z_MEM_ERROR;
			}
		}
		/* reset fifo head to reduce unnecessary wrap arounds */
		s->cur_in = (s->used_in == 0) ? 0 : s->cur_in;	
		fifo_in_len_check(s);
		free_space = s->len_in - s->cur_in - s->used_in;

		read_sz = NX_MIN(free_space, s->avail_in);
		if (read_sz > 0) {
			/* copy from next_in to the offset cur_in + used_in */
			memcpy(s->fifo_in + s->cur_in + s->used_in, s->next_in, read_sz);
			update_stream_in(s, read_sz);
			update_stream_in(s->zstrm, read_sz);
			s->used_in = s->used_in + read_sz;
		}
		else {
			/* should never come here */
			prt_err("unexpected error\n");
			return Z_STREAM_END;
		}
	}
	else {
		/* if avai_in > nx_config.soft_copy_threshold, do nothing */
	}
	print_dbg_info(s, __LINE__);
	
decomp_state:

	/* NX decompresses input data */
	
	/* address/len lists */
	clearp_dde(ddl_in);
	clearp_dde(ddl_out);	
	
	/* FC, CRC, HistLen, Table 6-6 */
	if (s->resuming) {
		/* Resuming a partially decompressed input.  The key
		   to resume is supplying the max 32KB dictionary
		   (history) to NX, which is basically the last 32KB
		   or less of the output earlier produced. And also
		   make sure partial checksums are carried forward
		*/
		fc = GZIP_FC_DECOMPRESS_RESUME; 
		/* Crc of prev job passed to the job to be resumed */
		cmdp->cpb.in_crc   = cmdp->cpb.out_crc;
		cmdp->cpb.in_adler = cmdp->cpb.out_adler;

		/* Round up the history size to quadword. Section 2.10 */
		s->history_len = (s->history_len + 15) / 16;
		putnn(cmdp->cpb, in_histlen, s->history_len);
		s->history_len = s->history_len * 16; /* convert to bytes */

		if (s->history_len > 0) {
			assert(s->cur_out >= s->history_len);
			nx_append_dde(ddl_in, s->fifo_out + (s->cur_out - s->history_len),
					      s->history_len);
		}
		print_dbg_info(s, __LINE__);
	}
	else {
		/* First decompress job */
		fc = GZIP_FC_DECOMPRESS; 

		s->history_len = 0;
		/* writing a 0 clears out subc as well */
		cmdp->cpb.in_histlen = 0;
		s->total_out = 0;

		/* initialize the crc values */
		put32(cmdp->cpb, in_crc, INIT_CRC );
		put32(cmdp->cpb, in_adler, INIT_ADLER);
		put32(cmdp->cpb, out_crc, INIT_CRC );
		put32(cmdp->cpb, out_adler, INIT_ADLER);

		/* Assuming 10% compression ratio initially; I use the
		   most recently measured compression ratio as a
		   heuristic to estimate the input and output
		   sizes. If we give too much input, the target buffer
		   overflows and NX cycles are wasted, and then we
		   must retry with smaller input size. 1000 is 100%  */
		s->last_comp_ratio = 100UL;
	}
	/* clear then copy fc to the crb */
	cmdp->crb.gzip_fc = 0; 
	putnn(cmdp->crb, gzip_fc, fc);

	/*
	 * NX source buffers
	 */
	nx_append_dde(ddl_in, s->fifo_in + s->cur_in, s->used_in);
	nx_append_dde(ddl_in, s->next_in, s->avail_in); /* limitation here? */
	source_sz = getp32(ddl_in, ddebc);
	ASSERT( source_sz > s->history_len );

	/*
	 * NX target buffers
	 */
	assert(s->used_out == 0);
	int len_next_out = s->avail_out; /* should we have a limitation here? */
	nx_append_dde(ddl_out, s->next_out, len_next_out);
	nx_append_dde(ddl_out, s->fifo_out + s->cur_out + s->used_out, s->len_out - s->cur_out - s->used_out);
	target_sz = len_next_out + s->len_out - s->cur_out - s->used_out;

	prt_info("len_next_out %d len_out %d cur_out %d used_out %d source_sz %d history_len %d\n",
		len_next_out, s->len_out, s->cur_out, s->used_out, source_sz, s->history_len);

	/* Some NX condition codes require submitting the NX job
	  again.  Kernel doesn't fault-in NX page faults. Expects user
	  code to touch pages */
	pgfault_retries = nx_config.retry_max;
	nx_space_retries = 0;

restart_nx:

 	putp32(ddl_in, ddebc, source_sz);  

	/* fault in pages */
	nx_touch_pages( (void *)cmdp, sizeof(nx_gzip_crb_cpb_t), nx_config.page_sz, 0);
	nx_touch_pages_dde(ddl_in, source_sz, nx_config.page_sz, 0);
	nx_touch_pages_dde(ddl_out, target_sz, nx_config.page_sz, 1);

	/* 
	 * send job to NX 
	 */
	cc = nx_submit_job(ddl_in, ddl_out, cmdp, s->nxdevp);

	switch (cc) {

	case ERR_NX_TRANSLATION:

		/* We touched the pages ahead of time. In the most
		   common case we shouldn't be here. But may be some
		   pages were paged out. Kernel should have placed the
		   faulting address to fsaddr */
		print_dbg_info(s, __LINE__);

		/* Touch 1 byte, read-only  */
		/* nx_touch_pages( (void *)cmdp->crb.csb.fsaddr, 1, nx_config.page_sz, 0);*/
		/* get64 does the endian conversion */

		prt_warn(" pgfault_retries %d crb.csb.fsaddr %p source_sz %d target_sz %d\n",
			pgfault_retries, (void *)cmdp->crb.csb.fsaddr, source_sz, target_sz);

		if (pgfault_retries == nx_config.retry_max) {
			/* try once with exact number of pages */
			--pgfault_retries;
			goto restart_nx;
		}
		else if (pgfault_retries > 0) {
			/* if still faulting try fewer input pages
			 * assuming memory outage */
			if (source_sz > nx_config.page_sz)
				source_sz = NX_MAX(source_sz / 2, nx_config.page_sz);
			--pgfault_retries;
			goto restart_nx;
		}
		else {
			/* TODO what to do when page faults are too many?
			   Kernel MM would have killed the process. */
			prt_err("cannot make progress; too many page fault retries cc= %d\n", cc);
			rc = Z_ERRNO;
			goto err5;
		}

	case ERR_NX_DATA_LENGTH:
		/* Not an error in the most common case; it just says
		   there is trailing data that we must examine */

		/* CC=3 CE(1)=0 CE(0)=1 indicates partial completion
		   Fig.6-7 and Table 6-8 */
		nx_ce = get_csb_ce_ms3b(cmdp->crb.csb);

		if (!csb_ce_termination(nx_ce) &&
		    csb_ce_partial_completion(nx_ce)) {
			/* check CPB for more information
			   spbc and tpbc are valid */
			sfbt = getnn(cmdp->cpb, out_sfbt); /* Table 6-4 */
			subc = getnn(cmdp->cpb, out_subc); /* Table 6-4 */
			spbc = get32(cmdp->cpb, out_spbc_decomp);
			tpbc = get32(cmdp->crb.csb, tpbc);
			ASSERT(target_sz >= tpbc);
			goto ok_cc3; /* not an error */
		}
		else {
			/* History length error when CE(1)=1 CE(0)=0. 
			   We have a bug */
			rc = Z_ERRNO;
			prt_err("history length error cc= %d\n", cc);
			goto err5;
		}
		
	case ERR_NX_TARGET_SPACE:
		/* Target buffer not large enough; retry smaller input
		   data; give at least 1 byte. SPBC/TPBC are not valid */
		ASSERT( source_sz > s->history_len );
		source_sz = ((source_sz - s->history_len + 2) / 2) + s->history_len;
		prt_warn("ERR_NX_TARGET_SPACE; retry with smaller input data src %d hist %d\n", source_sz, s->history_len);
		nx_space_retries++;
		goto restart_nx;

	case ERR_NX_OK:

		/* This should not happen for gzip formatted data;
		 * we need trailing crc and isize */
		prt_info("ERR_NX_OK\n");
		spbc = get32(cmdp->cpb, out_spbc_decomp);
		tpbc = get32(cmdp->crb.csb, tpbc);
		ASSERT(target_sz >= tpbc);			
		ASSERT(spbc >= s->history_len);
		source_sz = spbc - s->history_len;		
		goto offsets_state;

	default:
		prt_err("error: cc = %u, cc = 0x%x\n", cc, cc);
		char* csb = (char*) (&cmdp->crb.csb);
		for(int i = 0; i < 4; i++) /* dump first 32 bits of csb */
			prt_err("CSB: 0x %02x %02x %02x %02x\n", csb[0], csb[1], csb[2], csb[3]);
		rc = Z_ERRNO;
		goto err5; 
	}

ok_cc3:

	prt_info("cc3: sfbt: %x\n", sfbt);

	ASSERT(spbc > s->history_len);
	source_sz = spbc - s->history_len;

	/* Table 6-4: Source Final Block Type (SFBT) describes the
	   last processed deflate block and clues the software how to
	   resume the next job.  SUBC indicates how many input bits NX
	   consumed but did not process.  SPBC indicates how many
	   bytes of source were given to the accelerator including
	   history bytes.
	*/
	switch (sfbt) { 
		int dhtlen;
		
	case 0b0000: /* Deflate final EOB received */

		/* Calculating the checksum start position. */
		source_sz = source_sz - subc / 8;
		s->is_final = 1;
		break;

		/* Resume decompression cases are below. Basically
		   indicates where NX has suspended and how to resume
		   the input stream */
		
	case 0b1000: /* Within a literal block; use rembytecount */
	case 0b1001: /* Within a literal block; use rembytecount; bfinal=1 */

		/* Supply the partially processed source byte again */
		source_sz = source_sz - ((subc + 7) / 8);

		/* SUBC LS 3bits: number of bits in the first source
		 * byte need to be processed. */
		/* 000 means all 8 bits;  Table 6-3 */
		/* Clear subc, histlen, sfbt, rembytecnt, dhtlen  */
		cmdp->cpb.in_subc = 0;
		cmdp->cpb.in_sfbt = 0;
		putnn(cmdp->cpb, in_subc, subc % 8);
		putnn(cmdp->cpb, in_sfbt, sfbt);
		putnn(cmdp->cpb, in_rembytecnt, getnn( cmdp->cpb, out_rembytecnt));
		break;
		
	case 0b1010: /* Within a FH block; */
	case 0b1011: /* Within a FH block; bfinal=1 */

		source_sz = source_sz - ((subc + 7) / 8);

		/* Clear subc, histlen, sfbt, rembytecnt, dhtlen */
		cmdp->cpb.in_subc = 0;
		cmdp->cpb.in_sfbt = 0;		
		putnn(cmdp->cpb, in_subc, subc % 8);
		putnn(cmdp->cpb, in_sfbt, sfbt);		
		break;
		
	case 0b1100: /* Within a DH block; */
	case 0b1101: /* Within a DH block; bfinal=1 */

		source_sz = source_sz - ((subc + 7) / 8);		

		/* Clear subc, histlen, sfbt, rembytecnt, dhtlen */
		cmdp->cpb.in_subc = 0;
		cmdp->cpb.in_sfbt = 0;				
		putnn(cmdp->cpb, in_subc, subc % 8);
		putnn(cmdp->cpb, in_sfbt, sfbt);
		
		dhtlen = getnn(cmdp->cpb, out_dhtlen);
		putnn(cmdp->cpb, in_dhtlen, dhtlen);
		ASSERT(dhtlen >= 42);
		
		/* Round up to a qword */
		dhtlen = (dhtlen + 127) / 128;

		/* Clear any unused bits in the last qword */
		/* cmdp->cpb.in_dht[dhtlen-1].dword[0] = 0; */
		/* cmdp->cpb.in_dht[dhtlen-1].dword[1] = 0; */

		while (dhtlen > 0) { /* Copy dht from cpb.out to cpb.in */
			--dhtlen;
			cmdp->cpb.in_dht[dhtlen] = cmdp->cpb.out_dht[dhtlen];
		}
		break;		
		
	case 0b1110: /* Within a block header; bfinal=0; */
		     /* Also given if source data exactly ends (SUBC=0) with EOB code */
		     /* with BFINAL=0. Means the next byte will contain a block header. */
	case 0b1111: /* within a block header with BFINAL=1. */

		source_sz = source_sz - ((subc + 7) / 8);
		
		/* Clear subc, histlen, sfbt, rembytecnt, dhtlen */
		cmdp->cpb.in_subc = 0;
		cmdp->cpb.in_sfbt = 0;
		putnn(cmdp->cpb, in_subc, subc % 8);
		putnn(cmdp->cpb, in_sfbt, sfbt);		
	}

offsets_state:	

	/* Adjust the source and target buffer offsets and lengths  */
	/* source_sz is the real used in size */
	if (source_sz > s->used_in) {
		update_stream_in(s, source_sz - s->used_in);
		update_stream_in(s->zstrm, source_sz - s->used_in);
		s->used_in = 0;
	}
	else {
		s->used_in -= source_sz;
		s->cur_in += source_sz;
		fifo_in_len_check(s);
	}

	int overflow_len = tpbc - len_next_out;
	if (overflow_len <= 0) { /* there is no overflow */
		assert(s->used_out == 0);
		int need_len = NX_MIN(INF_HIS_LEN, tpbc);
		memcpy(s->fifo_out + s->cur_out, s->next_out + tpbc - need_len, need_len);
		s->cur_out += need_len;
		fifo_out_len_check(s);
		update_stream_out(s, tpbc);
		update_stream_out(s->zstrm, tpbc);
	}
	else if (overflow_len > 0 && overflow_len < INF_HIS_LEN){
		int need_len = INF_HIS_LEN - overflow_len;
		need_len = NX_MIN(need_len, len_next_out);
		int len;
		if (len_next_out + overflow_len > INF_HIS_LEN) {
			len = INF_HIS_LEN - overflow_len;
			memcpy(s->fifo_out + s->cur_out - len, s->next_out + len_next_out - len, len);
		}
		else {
			len = INF_HIS_LEN - (len_next_out + overflow_len);
			memcpy(s->fifo_out + s->cur_out - len_next_out - len, s->fifo_out + s->cur_out - len, len);
			memcpy(s->fifo_out + s->cur_out - len_next_out, s->next_out, len_next_out);
		}
			
		s->used_out += overflow_len;
		update_stream_out(s, len_next_out);
		update_stream_out(s->zstrm, len_next_out);
	}
	else {/* overflow_len > 1<<15 */
		s->used_out += overflow_len;
		update_stream_out(s, len_next_out);
		update_stream_out(s->zstrm, len_next_out);
	}

	s->history_len = (s->total_out + s->used_out > nx_config.window_max) ? nx_config.window_max : (s->total_out + s->used_out);

	s->last_comp_ratio = (1000UL * ((uint64_t)source_sz + 1)) / ((uint64_t)tpbc + 1);
	last_comp_ratio = NX_MAX( NX_MIN(1000UL, s->last_comp_ratio), 1 );

	s->resuming = 1;

	if (s->is_final == 1 || cc == ERR_NX_OK) {
		/* update total_in */
		s->total_in = s->total_in - s->used_in;
		s->zstrm->total_in = s->total_in;
		s->is_final = 1;
		s->used_in = 0;
		if (s->used_out == 0) {
			print_dbg_info(s, __LINE__);
			return Z_STREAM_END;
		}
		else
			goto copy_fifo_out_to_next_out;
	}

	if (s->avail_in > 0 && s->avail_out > 0) {
		goto copy_fifo_out_to_next_out;
	}
	
	if (s->used_in > 1 && s->avail_out > 0 && nx_space_retries > 0) {
		goto copy_fifo_out_to_next_out;
	}

	if (flush == Z_FINISH) return Z_STREAM_END;

	print_dbg_info(s, __LINE__);
	return Z_OK;
err5:
	prt_err("rc %d\n", rc);
	return rc;
}


/* 
   Use NX gzip wrap function to copy data.  crc and adler are output
   checksum values only because GZIP_FC_WRAP doesn't take any initial
   values.
*/
static inline int __nx_copy(char *dst, char *src, uint32_t len, uint32_t *crc, uint32_t *adler, nx_devp_t nxdevp)
{
	nx_gzip_crb_cpb_t cmd;
	int cc;
	int pgfault_retries;

	pgfault_retries = nx_config.retry_max;
	
	ASSERT(!!dst && !!src && len > 0);

 restart_copy:
	/* setup command crb */
	clear_struct(cmd.crb);
	put32(cmd.crb, gzip_fc, GZIP_FC_WRAP);
	put64(cmd.crb, csb_address, (uint64_t) &cmd.crb.csb & csb_address_mask);

	putnn(cmd.crb.source_dde, dde_count, 0);          /* direct dde */
	put32(cmd.crb.source_dde, ddebc, len);            /* bytes */
	put64(cmd.crb.source_dde, ddead, (uint64_t) src); /* src address */

	putnn(cmd.crb.target_dde, dde_count, 0); 
	put32(cmd.crb.target_dde, ddebc, len);   
	put64(cmd.crb.target_dde, ddead, (uint64_t) dst);

	/* fault in src and target pages */
	nx_touch_pages(dst, len, nx_config.page_sz, 1);
	nx_touch_pages(src, len, nx_config.page_sz, 0);
	
	cc = nx_submit_job(&cmd.crb.source_dde, &cmd.crb.target_dde, &cmd, nxdevp);

	if (cc == ERR_NX_OK) {
		/* TODO check endianness compatible with the combine functions */
		if (!!crc) *crc     = get32( cmd.cpb, out_crc );
		if (!!adler) *adler = get32( cmd.cpb, out_adler );
	}
	else if ((cc == ERR_NX_TRANSLATION) && (pgfault_retries > 0)) {
		--pgfault_retries;
		goto restart_copy;
	}
	
	return cc;
}

/*
  Use NX-gzip hardware to copy src to dst. May use several NX jobs
  crc and adler are inputs and outputs.
*/
int nx_copy(char *dst, char *src, uint64_t len, uint32_t *crc, uint32_t *adler, nx_devp_t nxdevp)
{
	int cc = ERR_NX_OK;
	uint32_t in_crc, in_adler, out_crc, out_adler;

	if (len < nx_config.soft_copy_threshold && !crc && !adler) {
		memcpy(dst, src, len);
		return cc;
	}
	
	/* caller supplies initial cksums */
	if (!!crc) in_crc = *crc;
	if (!!adler) in_adler = *adler;
	    
	while (len > 0) {
		uint64_t job_len = NX_MIN((uint64_t)nx_config.per_job_len, len);
		cc = __nx_copy(dst, src, (uint32_t)job_len, &out_crc, &out_adler, nxdevp);
		if (cc != ERR_NX_OK)
			return cc;
		/* combine initial cksums with the computed cksums */
		if (!!crc) in_crc = nx_crc32_combine(in_crc, out_crc, job_len);
		if (!!adler) in_adler = nx_adler32_combine(in_adler, out_adler, job_len);		
		len = len - job_len;
		dst = dst + job_len;
		src = src + job_len;
	}
	/* return final cksums */
	if (!!crc) *crc = in_crc;
	if (!!adler) *adler = in_adler;
	return cc;
}

#ifdef ZLIB_API
int inflateInit_(z_streamp strm, const char *version, int stream_size)
{
	return nx_inflateInit_(strm, version, stream_size);
}
int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size)
{
	return nx_inflateInit2_(strm, windowBits, version, stream_size);
}
int inflateEnd(z_streamp strm)
{
	return nx_inflateEnd(strm);
}
int inflate(z_streamp strm, int flush)
{
	return nx_inflate(strm, flush);
}
#endif

