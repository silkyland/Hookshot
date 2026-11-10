/* (android) AV1 on the platform MediaCodec, for nn::codec's AV1 decoder (host/nn_video.c): the same
   mf_av1_open/decode/flush/close entry points host/mf_av1.c gives Windows' Media Foundation, here over
   NDK MediaCodec. The game's submissions are AV1 temporal units (low-overhead OBUs, as in MP4), fed
   buffer-to-buffer; decoded frames come back as CPU-readable YUV and are handed on as tight NV12 through
   the picture callback, as mf_av1.c's are.

   Codec choice: AMediaCodec_createDecoderByType("video/av01") fails where the device has no AV1 decoder,
   and open() then returns NULL — nn_video.c's no-decoder path marks the stream's end and the game goes on
   without the movie. Output formats handled: planar 4:2:0 (COLOR_FormatYUV420Planar), semi-planar NV12 /
   NV21, and flexible (treated as planar; logged). Anything else gives up the same way. */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dav1d/dav1d.h>

#include "../hos.h"
#include <windows.h>   /* (the posix shim: InterlockedIncrement) */

/* a picture: luma rows and the interleaved chroma rows, both `stride` bytes apart */
typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);

/* MediaCodec color formats (android.media.MediaCodecInfo) */
#define CF_YUV420_PLANAR        0x13
#define CF_YUV420_SEMIPLANAR    0x15   /* NV12 */
#define CF_YUV420_SEMIPLANAR21  0x16   /* NV21 */
#define CF_YUV420_FLEXIBLE      0x7f420888

typedef struct {
    Dav1dContext *dav1d;   /* bundled libdav1d (default); codec is NULL then */
    AMediaCodec *codec;
    const char *name;
    int hw;          /* the hardware decoder: given up for the software one if it never outputs */
    AMediaFormat *outfmt;
    int w, h;          /* the stream's size (what the game asked for) */
    int cw, ch;        /* the decoder's coded size (planes are laid out on this) */
    int stride, slice_height;
    int color;         /* the output buffers' COLOR_Format* */
    int eos_in;        /* END_OF_STREAM is queued: a new stream needs a flush first */
    int dead;
    long n_in, n_out, n_drop, n_wait;   /* temporal units queued / pictures out / dropped / back-pressure waits */
    uint8_t *nv12;     /* scratch NV12, pitch w */
    size_t nv12_size;
} Av1;

static const char *cf_name(int cf) {
    switch (cf) {
    case CF_YUV420_PLANAR: return "I420";
    case CF_YUV420_SEMIPLANAR: return "NV12";
    case CF_YUV420_SEMIPLANAR21: return "NV21";
    case CF_YUV420_FLEXIBLE: return "flexible";
    default: return "?";
    }
}

/* one output buffer -> a tight NV12 picture at the stream's size (top-left crop of the coded frame) */
static void emit(Av1 *m, const uint8_t *base, size_t size, int64_t pts_ns, mf_av1_picture_fn fn, void *user) {
    const int dw = m->w > 0 ? m->w : m->cw, dh = m->h > 0 ? m->h : m->ch;
    const int cw = m->cw > 0 ? m->cw : dw, ch = m->ch > 0 ? m->ch : dh;
    if (dw <= 0 || dh <= 0 || (dw & 1) || (dh & 1)) return;
    const size_t need = (size_t)dw * dh * 3 / 2;
    if (m->nv12_size < need) {
        free(m->nv12);
        m->nv12 = (uint8_t *)malloc(need);
        m->nv12_size = m->nv12 ? need : 0;
        if (!m->nv12) return;
    }
    const int pw = m->stride > 0 ? m->stride : cw;
    const int ph = m->slice_height > 0 ? m->slice_height : ch;
    if (pw < cw || ph < ch || (pw & 1) || (ph & 1)) return;
    const size_t yplane = (size_t)pw * ph, cwh = (size_t)(pw / 2) * (ph / 2);
    if (yplane > size) return;
    uint8_t *y = m->nv12, *uv = m->nv12 + (size_t)dw * dh;
    const int cw2 = cw > dw ? dw : cw, ch2 = ch > dh ? dh : ch;
    for (int r = 0; r < ch2; r++)
        memcpy(y + (size_t)r * dw, base + (size_t)r * pw, (size_t)cw2);
    if (m->color == CF_YUV420_PLANAR || m->color == CF_YUV420_FLEXIBLE) {
        /* I420: Y, then U, then V planes, cw/2 x ch/2 each */
        const uint8_t *up = base + yplane, *vp = base + yplane + cwh;
        if ((size_t)(vp - base) + cwh <= size)
            for (int r = 0; r < ch2 / 2; r++)
                for (int x = 0; x < cw2 / 2; x++) {
                    uv[(size_t)r * dw + 2 * x] = up[(size_t)r * (pw / 2) + x];
                    uv[(size_t)r * dw + 2 * x + 1] = vp[(size_t)r * (pw / 2) + x];
                }
    } else {
        /* semi-planar: interleaved chroma right after the luma rows, cw bytes a row */
        const uint8_t *c = base + yplane;
        const int swap = m->color == CF_YUV420_SEMIPLANAR21;
        if (yplane + (size_t)pw * (ph / 2) <= size)
            for (int r = 0; r < ch2 / 2; r++) {
                const uint8_t *s = c + (size_t)r * pw;
                uint8_t *d = uv + (size_t)r * dw;
                for (int x = 0; x < cw2 / 2; x++) {
                    d[2 * x] = s[2 * x + (swap ? 1 : 0)];
                    d[2 * x + 1] = s[2 * x + (swap ? 0 : 1)];
                }
            }
    }
    fn(user, y, uv, dw, dh, dw, pts_ns);
}

static void read_out_format(Av1 *m) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(m->codec);
    if (!f) return;
    int32_t v;
    m->color = AMediaFormat_getInt32(f, "color-format", &v) ? v : 0;
    int32_t w = 0, h = 0;
    AMediaFormat_getInt32(f, "width", &w);
    AMediaFormat_getInt32(f, "height", &h);
    m->cw = w > 0 ? w : m->w;
    m->ch = h > 0 ? h : m->h;
    m->stride = AMediaFormat_getInt32(f, "stride", &v) && v > 0 ? v : m->cw;
    m->slice_height = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : m->ch;
    if (m->outfmt) AMediaFormat_delete(m->outfmt);
    m->outfmt = f;
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 4)
        hos_log("video: MediaCodec AV1 out: %s %dx%d stride %d slice %d (stream %dx%d)", cf_name(m->color), m->cw,
                m->ch, m->stride, m->slice_height, m->w, m->h);
}

/* resolutions whose hardware decoder gave nothing readable: software from then on for them */
static struct { int w, h; } hw_off[8];
static int hw_is_off(int w, int h);
static void hw_set_off(int w, int h);
static void hw_give_up(Av1 *m);

/* every picture the decoder has ready (timeoutUs <= 0: only what is already out) */
static void pump(Av1 *m, mf_av1_picture_fn fn, void *user, int64_t timeoutUs, int *eos_seen) {
    for (int guard = 0; guard < 256; guard++) {
        AMediaCodecBufferInfo info;
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(m->codec, &info, timeoutUs);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            read_out_format(m);
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED || idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return;
        if (idx < 0) { hos_log("video: MediaCodec AV1 dequeueOutputBuffer failed (%zd) after %ld in, %ld out: decoder given up", idx, m->n_in, m->n_out); m->dead = 1; return; }
        size_t capacity = 0;
        uint8_t *buf = AMediaCodec_getOutputBuffer(m->codec, (size_t)idx, &capacity);
        const int range_ok = buf && info.offset >= 0 && info.size >= 0 &&
                             (size_t)info.offset <= capacity && (size_t)info.size <= capacity - (size_t)info.offset;
        if (range_ok && !info.size && !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) && m->hw) {
            m->hw = 0;
            hw_give_up(m);
        }
        if (range_ok && info.size > 0) {
            m->n_out++;
            if (m->n_out <= 3 || m->n_out % 300 == 0)
                hos_log("video: MediaCodec AV1 picture %ld out (in %ld, dropped %ld, %d bytes)", m->n_out, m->n_in,
                        m->n_drop, info.size);
            emit(m, buf + info.offset, (size_t)info.size, (int64_t)info.presentationTimeUs * 1000, fn, user);
        } else if (info.size > 0) {
            hos_log("video: MediaCodec AV1 invalid output range: offset %d size %d capacity %zu", info.offset, info.size,
                    capacity);
        }
        AMediaCodec_releaseOutputBuffer(m->codec, (size_t)idx, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) { *eos_seen = 1; return; }
    }
}

/* bundled libdav1d (the default): the platform's dav1d C2 component keeps each submitted work
   pending until its picture is out — a sequence-header-only unit and the frames inside its 4-deep
   delay pin every input buffer, and with the pool full the pipeline stalls for good. Decoding here
   emits a picture per temporal unit and can always be drained; HOS_AV1_MC=1 takes the platform
   decoder instead. */
static void d1_picture(Av1 *m, Dav1dPicture *p, mf_av1_picture_fn fn, void *user) {
    const int w = p->p.w, h = p->p.h;
    if (p->p.bpc != 8 || !p->data[0] || !p->data[1] || !p->data[2] || (w & 1) || (h & 1)) {
        hos_log("video: dav1d picture %dx%d bpc %d skipped", w, h, p->p.bpc);
        return;
    }
    const size_t need = (size_t)w * h * 3 / 2;
    if (m->nv12_size < need) {
        free(m->nv12);
        m->nv12 = (uint8_t *)malloc(need);
        m->nv12_size = m->nv12 ? need : 0;
        if (!m->nv12) return;
    }
    uint8_t *y = m->nv12, *uv = m->nv12 + (size_t)w * h;
    for (int r = 0; r < h; r++)
        memcpy(y + (size_t)r * w, (const uint8_t *)p->data[0] + (size_t)r * p->stride[0], (size_t)w);
    const uint8_t *u = (const uint8_t *)p->data[1], *v = (const uint8_t *)p->data[2];
    for (int r = 0; r < h / 2; r++)
        for (int x = 0; x < w / 2; x++) {
            uv[(size_t)r * w + 2 * x] = u[(size_t)r * p->stride[1] + x];
            uv[(size_t)r * w + 2 * x + 1] = v[(size_t)r * p->stride[1] + x];
        }
    m->n_out++;
    if (m->n_out <= 3 || m->n_out % 300 == 0)
        hos_log("video: dav1d picture %ld out (in %ld, %dx%d)", m->n_out, m->n_in, w, h);
    fn(user, y, uv, w, h, w, p->m.timestamp);
}

/* every picture the decoder has ready (EOS included: get_picture in drain mode returns the delayed ones) */
static void d1_drain(Av1 *m, mf_av1_picture_fn fn, void *user) {
    for (int guard = 0; guard < 256; guard++) {
        Dav1dPicture p;
        memset(&p, 0, sizeof p);
        if (dav1d_get_picture(m->dav1d, &p) < 0) return;
        d1_picture(m, &p, fn, user);
        dav1d_picture_unref(&p);
    }
}

static int d1_decode(Av1 *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    if (data) {
        Dav1dData d;
        memset(&d, 0, sizeof d);
        uint8_t *b = dav1d_data_create(&d, n);
        if (!b) {
            hos_log("video: dav1d input alloc failed: decoder given up");
            m->dead = 1;
            return 1;
        }
        memcpy(b, data, n);
        d.m.timestamp = pts;
        for (int guard = 0; guard < 256; guard++) {
            const int r = dav1d_send_data(m->dav1d, &d);
            if (r == 0) break;
            if (r != DAV1D_ERR(EAGAIN)) {
                hos_log("video: dav1d send_data failed (%d) after %ld units: decoder given up", r, m->n_in);
                m->dead = 1;
                break;
            }
            d1_drain(m, fn, user);   /* a held packet frees once its pictures come out */
        }
        dav1d_data_unref(&d);
        m->n_in++;
        d1_drain(m, fn, user);
    } else {
        m->eos_in = 1;
        d1_drain(m, fn, user);
    }
    return 1;
}

static AMediaCodec *av1_codec_open(int w, int h, int hw, const char **name_out) {
    AMediaCodec *codec = NULL;
    const char *name = "by type";
    if (hw) {
        codec = AMediaCodec_createCodecByName("c2.android.av1.decoder");
        if (codec) name = "c2.android.av1.decoder";
        else {
            codec = AMediaCodec_createCodecByName("OMX.qcom.video.decoder.av1");
            if (codec) name = "OMX.qcom.video.decoder.av1";
        }
    }
    if (!codec) codec = AMediaCodec_createDecoderByType("video/av01");
    if (!codec) return NULL;
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, "mime", "video/av01");
    AMediaFormat_setInt32(fmt, "width", w);
    AMediaFormat_setInt32(fmt, "height", h);
    if (AMediaCodec_configure(codec, fmt, NULL, NULL, 0) != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        hos_log("video: the AV1 decoder %s refused %dx%d", name, w, h);
        AMediaCodec_delete(codec);
        AMediaFormat_delete(fmt);
        return NULL;
    }
    AMediaFormat_delete(fmt);
    *name_out = name;
    return codec;
}

static int hw_is_off(int w, int h) {
    for (int i = 0; i < 8; i++) if (hw_off[i].w == w && hw_off[i].h == h) return 1;
    return 0;
}
static void hw_set_off(int w, int h) {
    for (int i = 0; i < 8; i++) if (!hw_off[i].w) { hw_off[i].w = w, hw_off[i].h = h; return; }
}

void *mf_av1_open(int w, int h) {
    Av1 *m = (Av1 *)calloc(1, sizeof *m);
    m->w = w, m->h = h, m->cw = w, m->ch = h;
    m->stride = w, m->slice_height = h;
    /* (HOS_AV1_MC=1: the platform MediaCodec decoder instead of bundled dav1d) */
    static int mc = -1;
    if (mc < 0) { const char *e = getenv("HOS_AV1_MC"); mc = e ? atoi(e) : 0; }
    if (!mc) {
        Dav1dSettings s;
        dav1d_default_settings(&s);
        if (dav1d_open(&m->dav1d, &s) == 0) {
            m->name = "dav1d";
            hos_log("video: AV1 on bundled dav1d (%dx%d)", w, h);
            return m;
        }
        hos_log("video: bundled dav1d refused to open: MediaCodec next");
    }
    /* (HOS_AV1_HW=1: a hardware decoder first, where the device has one; the default is the software one:
       dav1d, faster than libgav1, and the only one this tablet has) */
    static int hw = -1;
    if (hw < 0) { const char *e = getenv("HOS_AV1_HW"); hw = e ? atoi(e) : 0; }
    if (hw_is_off(w, h)) hw = 0;
    const char *name = NULL;
    AMediaCodec *codec = av1_codec_open(w, h, hw, &name);
    if (!codec && hw) codec = av1_codec_open(w, h, 0, &name);
    if (!codec) {
        hos_log("video: no AV1 decoder on this device (MediaCodec has no video/av01)");
        free(m);
        return NULL;
    }
    m->codec = codec;
    m->name = name;
    m->hw = hw && name[0] != 'b';
    hos_log("video: AV1 on MediaCodec %s (%dx%d)", name, w, h);
    return m;
}

/* the hardware decoder never gave a picture though many units went in (it wants codec-specific data, or
   cannot read these OBUs): closed, and the next open takes the software decoder */
static void hw_give_up(Av1 *m) {
    /* only marked: this decoder may still be in a call; it is let go by mf_av1_close */
    m->hw = 0;
    m->dead = 1;
    hos_log("video: MediaCodec AV1 %s gave no picture for %ld units (or no readable buffer): software from now on", m->name, m->n_in);
    hw_set_off(m->w, m->h);
}

/* one temporal unit (pts in ns), or data NULL: the stream's end (the rest out) */
int mf_av1_decode(void *h, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    Av1 *m = (Av1 *)h;
    if (!m) return 1;
    if (m->dav1d) return m->dead ? 1 : d1_decode(m, data, n, pts, fn, user);
    if (m->dead) return 1;
    if (data && m->n_in < 3) hos_log("video: MediaCodec AV1 unit %ld: %zu bytes", m->n_in, n);
    if (m->eos_in && data) {   /* a stream after the end: flush and start again */
        AMediaCodec_flush(m->codec);
        m->eos_in = 0;
    }
    int eos_seen = 0;
    if (data) {
        /* Keep a unit pending when MediaCodec has no input slot; nn_video.c retries it after polling output. */
        ssize_t idx = AMediaCodec_dequeueInputBuffer(m->codec, 0);
        if (idx < 0) {
            pump(m, fn, user, 10000, &eos_seen);
            idx = AMediaCodec_dequeueInputBuffer(m->codec, 0);
        }
        if (idx < 0) {
            if (++m->n_wait <= 3 || m->n_wait % 300 == 0) {
                const uint8_t *p = data;
                hos_log("[DBG-AV1] input blocked idx %zd size %zu pts %lld bytes %02x %02x %02x %02x (in %ld, out %ld, waits %ld)",
                        idx, n, (long long)pts, n > 0 ? p[0] : 0, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0,
                        n > 3 ? p[3] : 0, m->n_in, m->n_out, m->n_wait);
            }
            return 0;
        }
        size_t cap = 0;
        uint8_t *in = AMediaCodec_getInputBuffer(m->codec, (size_t)idx, &cap);
        if (!in || n > cap) {
            hos_log("video: MediaCodec AV1 input buffer too small (%zu > %zu): unit dropped", n, cap);
            m->n_drop++;
            AMediaCodec_queueInputBuffer(m->codec, (size_t)idx, 0, 0, 0, 0);
        } else {
            const uint64_t pts_us = pts > 0 ? (uint64_t)(pts / 1000) : 0;
            if (AMediaCodec_queueInputBuffer(m->codec, (size_t)idx, 0, n, pts_us, 0) != AMEDIA_OK) {
                hos_log("video: MediaCodec AV1 queueInputBuffer failed: decoder given up");
                m->dead = 1;
                return 1;
            }
            if (m->n_in < 16) {
                hos_log("[DBG-AV1] queued unit %ld size %zu pts %llu obu %u bytes %02x %02x %02x %02x",
                        m->n_in, n, (unsigned long long)pts_us, n ? (data[0] >> 3) & 0x0f : 0,
                        n > 0 ? data[0] : 0, n > 1 ? data[1] : 0, n > 2 ? data[2] : 0, n > 3 ? data[3] : 0);
            }
            m->n_in++;
        }
        pump(m, fn, user, 0, &eos_seen);
        if (m->n_in && m->n_in % 120 == 0) hos_log("video: MediaCodec AV1: in %ld, out %ld, dropped %ld", m->n_in, m->n_out, m->n_drop);
        if (m->hw && m->n_in >= 64 && m->n_out == 0) hw_give_up(m);
    } else {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(m->codec, 20000);
        if (idx < 0) {
            pump(m, fn, user, 10000, &eos_seen);
            idx = AMediaCodec_dequeueInputBuffer(m->codec, 0);
        }
        if (idx < 0) return 0;
        if (AMediaCodec_queueInputBuffer(m->codec, (size_t)idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK) {
            hos_log("video: MediaCodec AV1 could not queue end-of-stream");
            m->dead = 1;
            return 1;
        }
        m->eos_in = 1;
        /* the end: the rest out, waited for briefly (the movie's last pictures) */
        for (int i = 0; i < 50 && !eos_seen && !m->dead; i++) pump(m, fn, user, 20000, &eos_seen);
    }
    return 1;
}

/* pictures the decoder finished since the last call (it works on its own threads: an output delay of a few units) */
void mf_av1_poll(void *h, mf_av1_picture_fn fn, void *user) {
    Av1 *m = (Av1 *)h;
    if (!m || m->dead) return;
    if (m->dav1d) { d1_drain(m, fn, user); return; }
    int eos_seen = 0;
    pump(m, fn, user, 0, &eos_seen);
}

void mf_av1_flush(void *h) {
    Av1 *m = (Av1 *)h;
    if (!m || m->dead) return;
    if (m->dav1d) { dav1d_flush(m->dav1d); m->eos_in = 0; return; }
    AMediaCodec_flush(m->codec);
    m->eos_in = 0;
}

void mf_av1_close(void *h) {
    Av1 *m = (Av1 *)h;
    if (!m) return;
    if (m->dav1d) dav1d_close(&m->dav1d);
    if (m->codec) {
        AMediaCodec_stop(m->codec);
        AMediaCodec_delete(m->codec);
    }
    if (m->outfmt) AMediaFormat_delete(m->outfmt);
    free(m->nv12);
    free(m);
}
