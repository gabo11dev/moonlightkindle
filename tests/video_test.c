#include "video.h"
#include "geometry.h"
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int decode_packet(uint8_t *data, int size) {
    /* Exercise fragmented Moonlight buffer chains, not just contiguous packets. */
    int split = size / 2;
    LENTRY b = {.data = (char *)data + split, .length = size - split};
    LENTRY a = {.next = &b, .data = (char *)data, .length = split};
    DECODE_UNIT du = {.fullLength = size, .bufferList = &a};
    return kindle_decoder.submitDecodeUnit(&du);
}
int main(int argc, char **argv) {
    Viewport v = viewport_fit(1072, 1000, 640, 360); int x, y;
    CHECK(v.width == 1072 && v.height == 603 && v.y == 198);
    CHECK(!viewport_point(v, 10, 100, &x, &y));
    CHECK(viewport_point(v, v.x, v.y, &x, &y) && x == 0 && y == 0);
    CHECK(!viewport_point(v, v.x + v.width, v.y, &x, &y));
    CHECK(viewport_fit(0, 100, 640, 360).width == 0);
    if (argc < 2) { puts("Geometry: OK"); return 0; }
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
    avcodec_register_all();
#endif
    FILE *fp = fopen(argv[1], "rb"); CHECK(fp);
    CHECK(kindle_decoder.setup(VIDEO_FORMAT_H265, 640, 360, 10, NULL, 0) != 0);
    CHECK(kindle_decoder.setup(VIDEO_FORMAT_H264, 640, 360, 10, NULL, 0) == 0);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264); CHECK(parser);
    AVCodecContext *context = avcodec_alloc_context3(NULL); CHECK(context);
    uint8_t input[4096 + AV_INPUT_BUFFER_PADDING_SIZE]; size_t n;
    while ((n = fread(input, 1, 4096, fp)) > 0) {
        memset(input + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        uint8_t *p = input;
        while (n > 0) {
            uint8_t *out; int size;
            int used = av_parser_parse2(parser, context, &out, &size, p, n, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            CHECK(used >= 0); CHECK(used || size);
            if (size) CHECK(decode_packet(out, size) == DR_OK);
            p += used; n -= used;
        }
    }
    uint8_t *out; int size;
    av_parser_parse2(parser, context, &out, &size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (size) CHECK(decode_packet(out, size) == DR_OK);
    CHECK(video_decoded_count() == 20);
    int w, h; uint8_t *rgb = video_take_frame(&w, &h);
    CHECK(rgb && w == 640 && h == 360);
    unsigned int lo = 255, hi = 0;
    for (int i = 0; i < w*h*3; i += 3) {
        CHECK(rgb[i] == rgb[i+1] && rgb[i] == rgb[i+2]);
        if (rgb[i] < lo) lo = rgb[i]; if (rgb[i] > hi) hi = rgb[i];
    }
    CHECK(hi - lo > 100);
    free(rgb); CHECK(!video_take_frame(&w, &h));
    LENTRY bad = {.data = "x", .length = 2};
    DECODE_UNIT invalid = {.bufferList = &bad, .fullLength = 1};
    CHECK(kindle_decoder.submitDecodeUnit(&invalid) == DR_NEED_IDR);
    kindle_decoder.cleanup();
    /* Reconnection must reset both decoder and mailbox state. */
    CHECK(kindle_decoder.setup(VIDEO_FORMAT_H264, 640, 360, 10, NULL, 0) == 0);
    CHECK(video_decoded_count() == 0); kindle_decoder.cleanup();
    av_parser_close(parser); avcodec_free_context(&context); fclose(fp);
    puts("H.264: 20 frames, fragmentation, grayscale, malformed input and reconnect: OK"); return 0;
}
