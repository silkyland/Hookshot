/* (android) FFmpeg stubs: nn_video.c compiles against the real headers (HOOKSHOT_FFMPEG_INCLUDE) but there is no
   Android FFmpeg build yet, so the ~18 libavcodec/libavutil/libswscale entry points it calls answer "no decoder":
   avcodec_find_decoder* return NULL, allocations return NULL, the rest fail. The game sees a decoder that never
   opens — movies don't play (the same as a build without FFmpeg), everything else runs. Replace with real
   arm64 FFmpeg static libs when they exist (PLAN-ANDROID.md M3). */
#include <stdint.h>

void *avcodec_find_decoder_by_name(const char *name) { (void)name; return 0; }
void *avcodec_find_decoder(int id) { (void)id; return 0; }
void *avcodec_alloc_context3(const void *codec) { (void)codec; return 0; }
int avcodec_open2(void *avctx, const void *codec, void *options) { (void)avctx, (void)codec, (void)options; return -1; }
int avcodec_send_packet(void *avctx, const void *pkt) { (void)avctx, (void)pkt; return -1; }
int avcodec_receive_frame(void *avctx, void *frame) { (void)avctx, (void)frame; return -1; }
void avcodec_flush_buffers(void *avctx) { (void)avctx; }
void avcodec_free_context(void *avctx) { (void)avctx; }
void *av_frame_alloc(void) { return 0; }
void av_frame_free(void *frame) { (void)frame; }
void av_frame_unref(void *frame) { (void)frame; }
void *av_packet_alloc(void) { return 0; }
void av_packet_free(void *pkt) { (void)pkt; }
int av_new_packet(void *pkt, int size) { (void)pkt, (void)size; return -1; }
void *sws_getContext(int srcW, int srcH, int srcFormat, int dstW, int dstH, int dstFormat,
                     int flags, const void *srcFilter, const void *dstFilter, const double *param) {
    (void)srcW, (void)srcH, (void)srcFormat, (void)dstW, (void)dstH, (void)dstFormat,
    (void)flags, (void)srcFilter, (void)dstFilter, (void)param;
    return 0;
}
int sws_scale(void *c, const uint8_t *const srcSlice[], const int srcStride[], int srcSliceY, int srcSliceH,
              uint8_t *const dst[], const int dstStride[]) {
    (void)c, (void)srcSlice, (void)srcStride, (void)srcSliceY, (void)srcSliceH, (void)dst, (void)dstStride;
    return -1;
}
void sws_freeContext(void *c) { (void)c; }
