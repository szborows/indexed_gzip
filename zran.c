#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#include "zran.h"


#define ZRAN_VERBOSE


#ifdef ZRAN_VERBOSE
#define zran_log(...) fprintf(stderr, __VA_ARGS__)
#else
#define zran_log(...) 
#endif


void zran_new(zran_index_t *index) {

    zran_log("zran_new\n");

    index->span = 0;
    index->have = 0;
    index->size = 0;
    index->list = NULL;
};


int zran_init(zran_index_t *index, int span) {

    zran_log("zran_init (%i)\n", span);

    if (index->list != NULL)
        return -1;

    /* Create an initial point list */
    index->list = malloc(sizeof(zran_point_t) * 8);
        
    if (index->list == NULL) {
        return -1;
    }

    index->span = span;
    index->size = 8;
    index->have = 0;
    
    return 0;
};


int zran_expand(zran_index_t *index) {

    int new_size = index->size * 2;

    zran_log("zran_expand (%i -> %i)\n", index->size, new_size);
    
    zran_point_t *new_list = realloc(index->list,
                                     sizeof(zran_point_t) * new_size);

    if (new_list == NULL) {
        return -1;
    }
    
    index->list = new_list;
    index->size = new_size;
    
    return 0;
};


int zran_free_unused(zran_index_t *index) {

    zran_log("zran_free_unused\n");

    zran_point_t *new_list;

    new_list = realloc(index->list, sizeof(zran_point_t) * index->have);

    if (new_list == NULL) {
        return -1;
    }
    
    index->list = new_list;
    index->size = index->have;

    return 0;
};


/* Deallocate an index built by build_index() */
void zran_dealloc(zran_index_t *index) {

    zran_log("zran_dealloc\n");
    
    if (index->list != NULL) {
        free(index->list);
    }
    
    index->span = 0;
    index->have = 0;
    index->size = 0;
    index->list = NULL;
};


zran_point_t * zran_get_point_at(zran_index_t *index,
                                 off_t         offset,
                                 char          compressed) {

    zran_point_t *prev;
    zran_point_t *curr;
    int           bit;
    int           i;

    prev = index->list;

    // TODO use bsearch instead of shitty linear search
    for (i = 1; i < index->size; i++) {
        
        curr = &(index->list[i]);

        if (compressed) {

            if (curr->bits > 0) bit = 1;
            else                bit = 0;
            
            if (curr->cmp_offset > offset + bit) 
                return prev;
                
        }
        else {
            if (curr->uncmp_offset > offset) 
                return prev;
        }

        prev = curr;
    }

    return NULL;
}


/* Add an entry to the access point list. */
int zran_add_point(zran_index_t  *index,
                   int            bits,
                   off_t          cmp_offset,
                   off_t          uncmp_offset,
                   unsigned       left,
                   unsigned char *window) {

    zran_log("zran_add_point(%i, %lld <-> %lld)\n", index->have, cmp_offset, uncmp_offset);

    zran_point_t *next;

    /* if list is full, make it bigger */
    if (index->have == index->size) {
        if (zran_expand(index) != 0) {
            return -1;
        }
    }

    /* fill in entry and increment how many we have */
    next               = index->list + index->have;
    next->bits         = bits;
    next->cmp_offset   = cmp_offset;
    next->uncmp_offset = uncmp_offset;
    
    if (left)
        memcpy(next->window, window + WINSIZE - left, left);
    
    if (left < WINSIZE)
        memcpy(next->window + left, window, WINSIZE - left);
    
    index->have++;

    return 0;
};


/* Make one entire pass through the compressed stream and build an index, with
   access points about every span bytes of uncompressed output -- span is
   chosen to balance the speed of random access against the memory requirements
   of the list, about 32K bytes per access point.  Note that data after the end
   of the first zlib or gzip stream in the file is ignored.  build_index()
   returns the number of access points on success (>= 1), Z_MEM_ERROR for out
   of memory, Z_DATA_ERROR for an error in the input file, or Z_ERRNO for a
   file read error.  On success, *built points to the resulting index. */
int zran_build_full_index(zran_index_t *index, FILE *in) {
    int ret;
    off_t totin, totout;        /* our own total counters to avoid 4GB limit */
    off_t last;                 /* totout value of last access point */
    z_stream strm;
    unsigned char input[CHUNK];
    unsigned char window[WINSIZE];

    zran_log("zran_build_full_index\n");

    /* initialize inflate */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, 47);      /* automatic zlib or gzip decoding */
    if (ret != Z_OK)
        return ret;

    /* inflate the input, maintain a sliding window, and build an index -- this
       also validates the integrity of the compressed data using the check
       information at the end of the gzip or zlib stream */
    totin = totout = last = 0;
    strm.avail_out = 0;
    do {
        /* get some compressed data from input file */
        strm.avail_in = fread(input, 1, CHUNK, in);
        if (ferror(in)) {
            ret = Z_ERRNO;
            goto build_index_error;
        }
        if (strm.avail_in == 0) {
            ret = Z_DATA_ERROR;
            goto build_index_error;
        }
        strm.next_in = input;

        /* process all of that, or until end of stream */
        do {
            /* reset sliding window if necessary */
            if (strm.avail_out == 0) {
                strm.avail_out = WINSIZE;
                strm.next_out = window;
            }

            /* inflate until out of input, output, or at end of block --
               update the total input and output counters */
            totin += strm.avail_in;
            totout += strm.avail_out;
            ret = inflate(&strm, Z_BLOCK);      /* return at end of block */
            totin -= strm.avail_in;
            totout -= strm.avail_out;
            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                goto build_index_error;
            if (ret == Z_STREAM_END)
                break;

            /* if at end of block, consider adding an index entry (note that if
               data_type indicates an end-of-block, then all of the
               uncompressed data from that block has been delivered, and none
               of the compressed data after that block has been consumed,
               except for up to seven bits) -- the totout == 0 provides an
               entry point after the zlib or gzip header, and assures that the
               index always has at least one access point; we avoid creating an
               access point after the last block by checking bit 6 of data_type
             */
            if ((strm.data_type & 128) && !(strm.data_type & 64) &&
                (totout == 0 || totout - last > index->span)) {

                if (zran_add_point(index, strm.data_type & 7, totin,
                                   totout, strm.avail_out, window) != 0) {
                    ret = Z_MEM_ERROR;
                    goto build_index_error;
                }
                last = totout;
            }
        } while (strm.avail_in != 0);
    } while (ret != Z_STREAM_END);

    if (zran_free_unused(index) != 0) {
        ret = Z_MEM_ERROR;
        goto build_index_error;
    }
    
    /* clean up and return index (release unused entries in list) */
    inflateEnd(&strm);
    return index->size;

    /* return error */
build_index_error:
    (void)inflateEnd(&strm);
    return ret;
};


/*
 * Seek to the approximate location of the specified offest into the 
 * uncompressed data stream. 
 *
 * If whence is not equal to SEEK_SET, returns -1.
 */ 
int zran_seek(zran_index_t  *index,
              FILE          *in,
              off_t          offset,
              int            whence,
              zran_point_t **point) {

    zran_point_t *seek_point;

    zran_log("zran_seek(%lld, %i)\n", offset, whence);

    if (whence != SEEK_SET) {
        return -1;
    }

    seek_point = zran_get_point_at(index, offset, 0);

    if (seek_point == NULL) {
        return -1;
    }

    index->uncmp_seek_offset = offset;
    offset                   = seek_point->cmp_offset;

    if (seek_point->bits > 0)
        offset -= 1;

    zran_log("Seeking to compressed stream offset %lld\n", offset);

    *point = seek_point;

    return fseeko(in, offset, SEEK_SET);
}


int zran_read(zran_index_t  *index,
              FILE          *in,
              unsigned char *buf,
              int            len) {

    int           ch;
    off_t         uncmp_offset;
    off_t         cmp_offset;
    int           skip;
    z_stream      strm;
    zran_point_t *point;
    unsigned char input[CHUNK];
    unsigned char discard[WINSIZE];

    memset(input,   0, CHUNK);
    memset(discard, 0, WINSIZE); 

    zran_log("zran_read(%i)\n", len);

    // silly input
    if (len < 0)
        return 0;

    // Get the current location
    // in the compressed stream.
    cmp_offset   = ftello(in);
    uncmp_offset = index->uncmp_seek_offset;
             
    zran_log("Offsets: compressed=%lld, uncompressed=%lld\n",
             cmp_offset,
             uncmp_offset);

    if (cmp_offset < 0) 
        return -1;

    // Get the current index point
    // that corresponds to this
    // location.
    point = zran_get_point_at(index, cmp_offset, 1);

    if (point == NULL) 
        return -1;

    zran_log("Identified access point: %lld - %lld\n",
             point->cmp_offset,
             point->uncmp_offset);
    
    /* initialize file and inflate state to start there */
    strm.zalloc   = Z_NULL;
    strm.zfree    = Z_NULL;
    strm.opaque   = Z_NULL;
    strm.avail_in = 0;
    strm.next_in  = Z_NULL;
    
    if (inflateInit2(&strm, -15) != Z_OK)
        goto fail;

    // The compressed location is
    // not byte-aligned with the
    // uncompressed location.
    if (point->bits) {
        ch = getc(in);
        if (ch == -1) 
            goto fail;

        zran_log("inflatePrime(%i, %i)\n",
                 point->bits,
                 ch >> (8 - point->bits)); 

        if (inflatePrime(&strm, point->bits, ch >> (8 - point->bits)) != Z_OK)
            goto fail;
    }

    zran_log("InflateSetDictionary( %i %i %i ...)\n",
             point->window[0],
             point->window[1],
             point->window[2]);
    if (inflateSetDictionary(&strm, point->window, WINSIZE) != Z_OK)
        goto fail;

    /* skip uncompressed bytes until offset reached, then satisfy request */
    zran_log("Initial offset: %lld - %lld\n", uncmp_offset, point->uncmp_offset);

    uncmp_offset -= point->uncmp_offset;
    
    strm.avail_in = 0;
    skip = 1; /* while skipping to offset */
    do {
        /* define where to put uncompressed data, and how much */
        if (uncmp_offset == 0 && skip) {          /* at offset now */
            strm.avail_out = len;
            strm.next_out = buf;
            skip = 0;                       /* only do this once */
        }
        if (uncmp_offset > WINSIZE) {             /* skip WINSIZE bytes */
            strm.avail_out = WINSIZE;
            strm.next_out = discard;
            uncmp_offset -= WINSIZE;
        }
        else if (uncmp_offset != 0) {             /* last skip */
            strm.avail_out = (unsigned)uncmp_offset;
            strm.next_out = discard;
            uncmp_offset = 0;
        }

        /* uncompress until avail_out filled, or end of stream */
        do {
            if (strm.avail_in == 0) {
                strm.avail_in = fread(input, 1, CHUNK, in);
                if (ferror(in)) {
                    goto fail;
                }
                if (strm.avail_in == 0) {
                    goto fail;
                }
                strm.next_in = input;
            }
            /* normal inflate */
            zran_log("Call inflate\n");
            zran_log("  Stream status:\n");
            zran_log("    avail_in:  %i\n", strm.avail_in);
            zran_log("    avail_out: %i\n", strm.avail_out);
            zran_log("    next_in:   %u\n", strm.next_in[0]);
            zran_log("    next_out:  %u\n", strm.next_out[0]);
            zran_log("    input:     %u\n", input[0]);
            zran_log("    buf:       %u\n", buf[0]);
            zran_log("    discard:   %u\n", discard[0]);
            
            ch = inflate(&strm, Z_NO_FLUSH);
            zran_log("Inflate called\n");

            if (ch == Z_STREAM_END)
                break;
            else if (ch != Z_OK) {
                zran_log("Return code not ok: %i\n", ch);
                goto fail;
            }

        } while (strm.avail_out != 0);

        /* if reach end of stream, then don't keep trying to get more */
        if (ch == Z_STREAM_END)
            break;

        /* do until offset reached and requested data read, or stream ends */
    } while (skip);

    /* compute number of uncompressed bytes read after offset */
    zran_log("Call inflateEnd\n");
    inflateEnd(&strm);
    zran_log("inflateEnd called\n");

    len = skip ? 0 : len - strm.avail_out;

    index->uncmp_seek_offset += len;

    return len;

    /* clean up and return bytes read or error */
fail:
    zran_log("(fail) Call inflateEnd\n");
    inflateEnd(&strm);
    zran_log("(fail) inflateEnd called\n");
    return -1; 
}


int zran_extract(zran_index_t  *index,
                 FILE          *in,
                 off_t          offset,
                 unsigned char *buf,
                 int            len) {
    
    zran_point_t *here;
    
    zran_log("zran_extract\n");

    /* proceed only if something reasonable to do */
    if (len < 0)
        return 0;

    zran_log("Starting seek ... \n");
    if (zran_seek(index, in, offset, SEEK_SET, &here) != 0)
        return -1;

    zran_log("Starting read ... \n");
    return zran_read(index, in, buf, len);
};
