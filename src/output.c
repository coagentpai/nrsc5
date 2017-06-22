/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <string.h>

#include "bitreader.h"
#include "bitwriter.h"
#include "defines.h"
#include "output.h"

static ao_sample_format sample_format = {
    16,
    44100,
    2,
    AO_FMT_LITTLE,
    "L,R"
};

void hdc_to_aac(bitreader_t *br, bitwriter_t *bw);

static void write_adts_header(FILE *fp, unsigned int len)
{
    uint8_t hdr[7];
    bitwriter_t bw;

    bw_init(&bw, hdr);
    bw_addbits(&bw, 0xFFF, 12); // sync word
    bw_addbits(&bw, 0, 1); // MPEG-4
    bw_addbits(&bw, 0, 2); // Layer
    bw_addbits(&bw, 1, 1); // no CRC
    bw_addbits(&bw, 1, 2); // AAC-LC
    bw_addbits(&bw, 7, 4); // 22050 HZ
    bw_addbits(&bw, 0, 1); // private bit
    bw_addbits(&bw, 2, 3); // 2-channel configuration
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, 0, 1);
    bw_addbits(&bw, len + 7, 13); // frame length
    bw_addbits(&bw, 0x7FF, 11); // buffer fullness (VBR)
    bw_addbits(&bw, 0, 2); // 1 AAC frame per ADTS frame

    fwrite(hdr, 7, 1, fp);
}

static void dump_adts(FILE *fp, uint8_t *pkt, unsigned int len)
{
    uint8_t tmp[1024];
    bitreader_t br;
    bitwriter_t bw;

    br_init(&br, pkt, len);
    bw_init(&bw, tmp);
    hdc_to_aac(&br, &bw);
    len = bw_flush(&bw);

    write_adts_header(fp, len);
    fwrite(tmp, len, 1, fp);
    fflush(fp);
}

static void dump_hdc(FILE *fp, uint8_t *pkt, unsigned int len)
{
    write_adts_header(fp, len);
    fwrite(pkt, len, 1, fp);
    fflush(fp);
}

void output_push(output_t *st, uint8_t *pkt, unsigned int len)
{
    void *buffer;
    NeAACDecFrameInfo info;

    if (st->method == OUTPUT_ADTS)
    {
        dump_adts(st->outfp, pkt, len);
        return;
    }
    else if (st->method == OUTPUT_HDC)
    {
        dump_hdc(st->outfp, pkt, len);
        return;
    }

    buffer = NeAACDecDecode(st->handle, &info, pkt, len);
    if (info.error > 0)
    {
        log_error("Decode error: %s", NeAACDecGetErrorMessage(info.error));
    }

    if (info.error == 0 && info.samples > 0)
    {
        unsigned int bytes = info.samples * sample_format.bits / 8;
        output_buffer_t *ob;

        assert(bytes == AUDIO_FRAME_BYTES);

#ifdef USE_THREADS
        pthread_mutex_lock(&st->mutex);
        while (st->free == NULL)
            pthread_cond_wait(&st->cond, &st->mutex);
        ob = st->free;
        st->free = ob->next;
        pthread_mutex_unlock(&st->mutex);

        memcpy(ob->data, buffer, bytes);

        pthread_mutex_lock(&st->mutex);
        ob->next = NULL;
        if (st->tail)
            st->tail->next = ob;
        else
            st->head = ob;
        st->tail = ob;
        pthread_mutex_unlock(&st->mutex);
        pthread_cond_signal(&st->cond);
#else
        ao_play(st->dev, (void *)buffer, AUDIO_FRAME_BYTES);
#endif
    }
}

#ifdef USE_THREADS
static void *output_worker(void *arg)
{
    output_t *st = arg;

    while (1)
    {
        output_buffer_t *ob;

        pthread_mutex_lock(&st->mutex);
        while (st->head == NULL)
            pthread_cond_wait(&st->cond, &st->mutex);
        ob = st->head;
        pthread_mutex_unlock(&st->mutex);

        ao_play(st->dev, (void *)ob->data, AUDIO_FRAME_BYTES);

        pthread_mutex_lock(&st->mutex);
        st->head = ob->next;
        if (st->head == NULL)
            st->tail = NULL;
        ob->next = st->free;
        st->free = ob;
        pthread_mutex_unlock(&st->mutex);
        pthread_cond_signal(&st->cond);
    }
}
#endif

void output_reset(output_t *st)
{
    unsigned long samprate = 22050;
    output_buffer_t *ob;

    if (st->method == OUTPUT_ADTS || st->method == OUTPUT_HDC)
        return;

    if (st->handle)
        NeAACDecClose(st->handle);

    NeAACDecInitHDC(&st->handle, &samprate);

#ifdef USE_THREADS
    // find the end of the head list
    for (ob = st->head; ob && ob->next != NULL; ob = ob->next) { }

    // if the head list is non-empty, prepend to free list
    if (ob != NULL)
    {
        ob->next = st->free;
        st->free = st->head;
    }

    st->head = NULL;
    st->tail = NULL;
#endif
}

void output_init_adts(output_t *st, const char *name)
{
    st->method = OUTPUT_ADTS;

    if (strcmp(name, "-") == 0)
        st->outfp = stdout;
    else
        st->outfp = fopen(name, "wb");
    if (st->outfp == NULL)
        FATAL_EXIT("Unable to open output adts file.");
}

void output_init_hdc(output_t *st, const char *name)
{
    st->method = OUTPUT_HDC;

    if (strcmp(name, "-") == 0)
        st->outfp = stdout;
    else
        st->outfp = fopen(name, "wb");
    if (st->outfp == NULL)
        FATAL_EXIT("Unable to open output adts-hdc file.");
}

static void output_init_ao(output_t *st, int driver, const char *name)
{
    unsigned int i;

    if (name)
        st->dev = ao_open_file(driver, name, 1, &sample_format, NULL);
    else
        st->dev = ao_open_live(driver, &sample_format, NULL);
    if (st->dev == NULL)
        FATAL_EXIT("Unable to open output wav file.");

#ifdef USE_THREADS
    st->head = NULL;
    st->tail = NULL;
    st->free = NULL;

    for (i = 0; i < 32; ++i)
    {
        output_buffer_t *ob = malloc(sizeof(output_buffer_t));
        ob->next = st->free;
        st->free = ob;
    }

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, output_worker, st);
#endif

    st->handle = NULL;
    output_reset(st);
}

void output_init_wav(output_t *st, const char *name)
{
    st->method = OUTPUT_WAV;

    ao_initialize();
    output_init_ao(st, ao_driver_id("wav"), name);
}

void output_init_live(output_t *st)
{
    st->method = OUTPUT_LIVE;

    ao_initialize();
    output_init_ao(st, ao_default_driver_id(), NULL);
}
