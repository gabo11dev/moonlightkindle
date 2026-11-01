/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
static AVCodecContext *codec;
static AVFrame *frame;
static AVPacket *packet;
static struct SwsContext *scaler;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *latest;
static int frame_w, frame_h, gray = 1;
static unsigned long decoded;

void video_set_grayscale(int enabled) { gray = enabled; }
unsigned long video_decoded_count(void) {
    pthread_mutex_lock(&lock); unsigned long n = decoded; pthread_mutex_unlock(&lock); return n;
}
void video_discard_frame(void) {
    pthread_mutex_lock(&lock); free(latest); latest = NULL; pthread_mutex_unlock(&lock);
}
uint8_t *video_take_frame(int *w, int *h) {
    pthread_mutex_lock(&lock);
    uint8_t *p = latest; latest = NULL; *w = frame_w; *h = frame_h;
    pthread_mutex_unlock(&lock); return p;
}
static void cleanup(void) {
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec);
    sws_freeContext(scaler); scaler = NULL;
}
static int setup(int format, int w, int h, int fps, void *ctx, int flags) {
    if (format != VIDEO_FORMAT_H264 || w < 1 || h < 1 || w > 1920 || h > 1080) return -1;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
    avcodec_register_all();
#endif
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    codec = avcodec_alloc_context3(decoder);
    frame = av_frame_alloc(); packet = av_packet_alloc();
    if (!decoder || !codec || !frame || !packet) { cleanup(); return -1; }
    codec->width = w; codec->height = h;
    codec->thread_count = 2; codec->thread_type = FF_THREAD_SLICE;
    codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec->err_recognition = AV_EF_EXPLODE;
    if (avcodec_open2(codec, decoder, NULL) < 0) { cleanup(); return -1; }
    video_discard_frame();
    pthread_mutex_lock(&lock); decoded = 0; pthread_mutex_unlock(&lock);
    return 0;
}
static int publish(void) {
    int w = frame->width, h = frame->height;
    if (w < 1 || h < 1 || w > 1920 || h > 1080) return -1;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return -1;
    /* Convert straight to luminance on e-ink, avoiding RGB conversion followed
     * by a second weighted RGB-to-gray pass. Expand backward in the same buffer
     * only because GTK2's RGB pixbuf needs three identical channels. */
    scaler = sws_getCachedContext(scaler, w, h, frame->format, w, h,
              gray ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!scaler) { free(rgb); return -1; }
    uint8_t *planes[4] = {rgb, NULL, NULL, NULL}; int strides[4] = {gray ? w : w * 3, 0, 0, 0};
    if (sws_scale(scaler, (const uint8_t *const *)frame->data, frame->linesize,
                  0, h, planes, strides) != h) { free(rgb); return -1; }
    if (gray) for (size_t i = (size_t)w * h; i-- > 0;) {
        uint8_t y = rgb[i];
        rgb[3*i] = rgb[3*i+1] = rgb[3*i+2] = y;
    }
    pthread_mutex_lock(&lock);
    free(latest); latest = rgb; frame_w = w; frame_h = h; decoded++;
    pthread_mutex_unlock(&lock); return 0;
}
static int submit(PDECODE_UNIT unit) {
    if (!codec || unit->fullLength <= 0 || unit->fullLength > 8 * 1024 * 1024) return DR_NEED_IDR;
    av_packet_unref(packet);
    if (av_new_packet(packet, unit->fullLength) < 0) return DR_NEED_IDR;
    int offset = 0;
    for (PLENTRY p = unit->bufferList; p; p = p->next) {
        if (p->length < 0 || p->length > unit->fullLength - offset) return DR_NEED_IDR;
        memcpy(packet->data + offset, p->data, p->length); offset += p->length;
    }
    if (offset != unit->fullLength) return DR_NEED_IDR;
    int ret = avcodec_send_packet(codec, packet);
    if (ret < 0) { avcodec_flush_buffers(codec); return DR_NEED_IDR; }
    while ((ret = avcodec_receive_frame(codec, frame)) >= 0) {
        if (publish() < 0) return DR_NEED_IDR;
        av_frame_unref(frame);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        avcodec_flush_buffers(codec); return DR_NEED_IDR;
    }
    return DR_OK;
}
DECODER_RENDERER_CALLBACKS kindle_decoder = {
    .setup = setup, .cleanup = cleanup, .submitDecodeUnit = submit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(2)
};
