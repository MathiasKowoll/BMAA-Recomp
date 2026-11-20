// The game's movies. They are Bink files, and the game plays them through libBinkMacx86.dylib, a 32-bit library of
// its own that is not part of the executable and is not translated. The 22 calls the game makes to it are answered
// here on FFmpeg's Bink decoder instead: same structures (as the game's code reads them), same order of calls
// (BinkWait -> BinkDoFrame -> BinkGetRects -> BinkNextFrame), the picture written into the plane buffers the game
// registered and then copies to its textures (Y, cR = V, cB = U; one row every "buffer width" bytes).
// The sound of a movie is the tracks the game chose with BinkSetSoundTrack, mixed to stereo and played by an
// AudioQueue of ours; it is decoded as the file is read, a frame's worth with each picture, so it keeps step.
// BMAA_TRACE_BINK=1 names the calls. BMAA_NO_BINK=1 leaves the calls unanswered, as before.
#include "rt.h"
#include <AudioToolbox/AudioToolbox.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t aot_malloc(uint32_t n); void aot_free(uint32_t p);
void unimpl_import(CPU *c, const char *name);
#define RET(v) (c->eax = (uint32_t)(v))
static int off(void) { static int v = -1; if (v < 0) v = getenv("BMAA_NO_BINK") != NULL; return v; }
static int tracing(void) { static int v = -1; if (v < 0) v = getenv("BMAA_TRACE_BINK") != NULL; return v; }
#define T(...) do { if (tracing()) { fprintf(stderr, "  [bink] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define OFF(name) if (off()) { unimpl_import(c, name); return; }

// the game's BINK structure, as far as it reads it
enum { B_WIDTH = 0, B_HEIGHT = 4, B_FRAMES = 8, B_FRAMENUM = 0xc, B_LASTFRAME = 0x10, B_RATE = 0x14, B_RATEDIV = 0x18,
       B_READERROR = 0x1c, B_OPENFLAGS = 0x20, B_SIZE = 0x400 };
enum { BINKSNDTRACK = 0x4000, BINKFROMMEMORY = 0x04000000 };
// BINKFRAMEBUFFERS: { TotalFrames, YABufferWidth, YABufferHeight, cRcBBufferWidth, cRcBBufferHeight, FrameNum,
//                     Frames[2] of 4 planes { Allocate, Buffer, BufferPitch } }
enum { FB_TOTAL = 0, FB_YW = 4, FB_YH = 8, FB_CW = 12, FB_CH = 16, FB_NUM = 20, FB_FRAMES = 24, FB_SET = 48, FB_PLANE = 12 };

enum { NMOVIE = 8, NTRACK = 16, RING = 48000 * 2 * 2 };   // two seconds of stereo
struct movie {
    uint32_t g;                         // the game's BINK structure; 0 = free
    const uint8_t *mem; int64_t size, pos;
    AVFormatContext *fmt; AVIOContext *io;
    AVCodecContext *vdec; int vstream;
    AVCodecContext *adec[NTRACK]; int astream[NTRACK]; uint32_t atrack[NTRACK]; int natrack;
    AVFrame *pic, *snd; AVPacket *pkt;
    uint32_t have;                      // the frame number pic holds (1-based), 0 = none
    uint32_t fb;                        // the frame buffers the game registered
    uint64_t t0; int started;           // when frame 1 was due
    int paused; uint64_t paused_at;
    AudioQueueRef q; pthread_mutex_t lock;
    float *ring; unsigned rd, wr, fill;
    unsigned gbase, glen, gw[NTRACK];   // the stretch being filled: where it starts, how long, how far each track got
};
static struct movie movies[NMOVIE];
static uint32_t chosen[NTRACK]; static int nchosen = -1;      // BinkSetSoundTrack; -1 = never called (track 0)
static float track_vol[256];

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static struct movie *find(uint32_t g) {
    if (!g) return NULL;
    for (int i = 0; i < NMOVIE; i++) if (movies[i].g == g) return &movies[i];
    return NULL;
}

// ---- the file: the game hands the whole movie in memory ----
static int mem_read(void *o, uint8_t *buf, int n) {
    struct movie *m = o;
    if (m->pos >= m->size) return AVERROR_EOF;
    if (n > m->size - m->pos) n = (int)(m->size - m->pos);
    memcpy(buf, m->mem + m->pos, (size_t)n); m->pos += n;
    return n;
}
static int64_t mem_seek(void *o, int64_t p, int whence) {
    struct movie *m = o;
    if (whence == AVSEEK_SIZE) return m->size;
    if (whence == SEEK_CUR) p += m->pos; else if (whence == SEEK_END) p += m->size;
    if (p < 0 || p > m->size) return AVERROR(EINVAL);
    return m->pos = p;
}

// ---- sound ----
static void aq_fill(void *o, AudioQueueRef q, AudioQueueBufferRef b) {
    struct movie *m = o;
    float *out = b->mAudioData; unsigned want = b->mAudioDataBytesCapacity / 4, got = 0;
    pthread_mutex_lock(&m->lock);
    while (got < want && m->fill && !m->paused) { out[got++] = m->ring[m->rd]; m->rd = (m->rd + 1) % RING; m->fill--; }
    pthread_mutex_unlock(&m->lock);
    while (got < want) out[got++] = 0;
    b->mAudioDataByteSize = want * 4;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static void sound_open(struct movie *m) {
    if (!m->natrack) return;
    AudioStreamBasicDescription f = { 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
    f.mSampleRate = m->adec[0]->sample_rate;
    m->ring = calloc(RING, sizeof(float));
    pthread_mutex_init(&m->lock, NULL);
    if (AudioQueueNewOutput(&f, aq_fill, m, NULL, NULL, 0, &m->q)) { m->q = NULL; return; }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        if (!AudioQueueAllocateBuffer(m->q, 1024 * 8, &b)) aq_fill(m, m->q, b);
    }
    AudioQueueStart(m->q, NULL);
}
// One decoded block of one track, added into the stretch of sound that belongs to the picture being read. The tracks
// of a stretch arrive one after another, each from its start, so every track keeps its own place in it and the
// stretch grows (silent) as far as the longest one reaches.
static void sound_mix(struct movie *m, int k, const AVFrame *s) {
    if (!m->q) return;
    int ch = s->ch_layout.nb_channels; unsigned n = (unsigned)s->nb_samples * 2;
    float vol = track_vol[m->atrack[k] & 255];
    const float *l = (const float *)s->extended_data[0], *r = ch > 1 ? (const float *)s->extended_data[1] : l;
    float g = (ch > 1 ? 1.0f : 0.7071f) * vol;
    pthread_mutex_lock(&m->lock);
    unsigned end = m->gw[k] + n;
    if (end > m->glen) {
        unsigned more = end - m->glen, room = RING - m->fill;
        if (more > room) more = room & ~1u;
        for (unsigned i = 0; i < more; i++) m->ring[(m->wr + i) % RING] = 0;
        m->wr = (m->wr + more) % RING; m->fill += more; m->glen += more;
    }
    for (unsigned i = 0; i + 1 < n && m->gw[k] + i + 1 < m->glen; i += 2) {
        m->ring[(m->gbase + m->gw[k] + i) % RING] += l[i / 2] * g;
        m->ring[(m->gbase + m->gw[k] + i + 1) % RING] += r[i / 2] * g;
    }
    m->gw[k] += n;
    pthread_mutex_unlock(&m->lock);
}
static void sound_next_stretch(struct movie *m) {
    if (!m->q) return;
    pthread_mutex_lock(&m->lock);
    m->gbase = m->wr; m->glen = 0; memset(m->gw, 0, sizeof m->gw);
    pthread_mutex_unlock(&m->lock);
}

// ---- pictures ----
// read on until the picture `want` (1-based) is in m->pic; sound met on the way is queued
static int decode_to(struct movie *m, uint32_t want) {
    if (m->have == want) return 1;
    if (want < m->have || want > m->have + 30) {                  // not the next ones: start from a key frame
        av_seek_frame(m->fmt, m->vstream, (int64_t)want - 1, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m->vdec);
        for (int k = 0; k < m->natrack; k++) avcodec_flush_buffers(m->adec[k]);
        m->have = 0;
        if (m->q) { pthread_mutex_lock(&m->lock); m->fill = 0; m->rd = m->wr = 0; pthread_mutex_unlock(&m->lock); sound_next_stretch(m); }
    }
    int guard = 0;
    while (m->have != want && guard++ < 4000) {
        int e = av_read_frame(m->fmt, m->pkt);
        if (e < 0) return 0;
        if (m->pkt->stream_index == m->vstream) {
            if (!avcodec_send_packet(m->vdec, m->pkt) && !avcodec_receive_frame(m->vdec, m->pic))
                m->have = m->pkt->pts != AV_NOPTS_VALUE ? (uint32_t)m->pkt->pts + 1 : m->have + 1;
            sound_next_stretch(m);                                // the sound that follows belongs to the next picture
        } else {
            for (int k = 0; k < m->natrack; k++) {
                if (m->astream[k] != m->pkt->stream_index) continue;
                if (!avcodec_send_packet(m->adec[k], m->pkt))
                    while (!avcodec_receive_frame(m->adec[k], m->snd)) sound_mix(m, k, m->snd);
            }
        }
        av_packet_unref(m->pkt);
    }
    return m->have == want;
}
static void copy_plane(uint32_t plane, const uint8_t *src, int stride, uint32_t w, uint32_t h, int srcw, int srch) {
    uint32_t buf = rd32(plane + 4), pitch = rd32(plane + 8);
    if (!buf || !src) return;
    if (!pitch) pitch = w;
    uint32_t cw = w < (uint32_t)srcw ? w : (uint32_t)srcw, chh = h < (uint32_t)srch ? h : (uint32_t)srch;
    for (uint32_t y = 0; y < chh; y++) memcpy(G2H(buf + y * pitch), src + (size_t)y * (size_t)stride, cw);
}

static void movie_close(struct movie *m) {
    if (m->q) { AudioQueueStop(m->q, true); AudioQueueDispose(m->q, true); pthread_mutex_destroy(&m->lock); }
    free(m->ring);
    if (m->vdec) avcodec_free_context(&m->vdec);
    for (int k = 0; k < m->natrack; k++) avcodec_free_context(&m->adec[k]);
    av_frame_free(&m->pic); av_frame_free(&m->snd); av_packet_free(&m->pkt);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    if (m->g) aot_free(m->g);
    memset(m, 0, sizeof *m);
}

// HBINK BinkOpen(name or memory, flags)
void I__BinkOpen(CPU *c) {
    OFF("_BinkOpen");
    uint32_t src = ARG(0), flags = ARG(1);
    RET(0);
    struct movie *m = NULL;
    for (int i = 0; i < NMOVIE && !m; i++) if (!movies[i].g) m = &movies[i];
    if (!m || !src) return;
    memset(m, 0, sizeof *m);
    int e;
    if (flags & BINKFROMMEMORY) {                                  // the whole movie in the game's memory
        m->mem = G2H(src);
        m->size = (int64_t)rd32(src + 4) + 8;                      // the header says how long the file is
        uint8_t *iob = av_malloc(65536);
        m->io = avio_alloc_context(iob, 65536, 0, m, mem_read, NULL, mem_seek);
        m->fmt = avformat_alloc_context(); m->fmt->pb = m->io;
        e = avformat_open_input(&m->fmt, NULL, av_find_input_format("bink"), NULL);
    } else {                                                       // or a file, by its path (the long ones)
        e = avformat_open_input(&m->fmt, (const char *)G2H(src), av_find_input_format("bink"), NULL);
    }
    if (e < 0) {
        m->fmt = NULL;
        fprintf(stderr, "  [bink] BinkOpen: FFmpeg does not open %s%.200s\n", flags & BINKFROMMEMORY ? "the movie in memory" : "", flags & BINKFROMMEMORY ? "" : (const char *)G2H(src));
        movie_close(m); return;
    }
    avformat_find_stream_info(m->fmt, NULL);
    m->vstream = -1;
    for (unsigned i = 0; i < m->fmt->nb_streams; i++) {
        AVStream *st = m->fmt->streams[i];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m->vstream < 0) {
            m->vdec = avcodec_alloc_context3(codec); avcodec_parameters_to_context(m->vdec, st->codecpar);
            if (avcodec_open2(m->vdec, codec, NULL) < 0) { avcodec_free_context(&m->vdec); continue; }
            m->vstream = (int)i;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m->natrack < NTRACK) {
            int want = nchosen < 0 ? st->id == 0 : 0;
            for (int k = 0; k < nchosen; k++) if (chosen[k] == (uint32_t)st->id) want = 1;
            if (!want || !(flags & BINKSNDTRACK ? 1 : st->id == 0)) { st->discard = AVDISCARD_ALL; continue; }
            AVCodecContext *a = avcodec_alloc_context3(codec); avcodec_parameters_to_context(a, st->codecpar);
            if (avcodec_open2(a, codec, NULL) < 0) { avcodec_free_context(&a); continue; }
            m->adec[m->natrack] = a; m->astream[m->natrack] = (int)i; m->atrack[m->natrack] = (uint32_t)st->id; m->natrack++;
        }
    }
    if (m->vstream < 0) { fprintf(stderr, "  [bink] BinkOpen: no picture in the movie\n"); movie_close(m); return; }
    m->pic = av_frame_alloc(); m->snd = av_frame_alloc(); m->pkt = av_packet_alloc();
    AVStream *v = m->fmt->streams[m->vstream];
    uint32_t frames = (uint32_t)(v->nb_frames > 0 ? v->nb_frames : v->duration);
    uint32_t rate = (uint32_t)v->avg_frame_rate.num, div = (uint32_t)v->avg_frame_rate.den;
    if (!rate || !div) { rate = (uint32_t)v->time_base.den; div = (uint32_t)v->time_base.num; }
    m->g = aot_malloc(B_SIZE); memset(G2H(m->g), 0, B_SIZE);
    wr32(m->g + B_WIDTH, (uint32_t)m->vdec->width); wr32(m->g + B_HEIGHT, (uint32_t)m->vdec->height);
    wr32(m->g + B_FRAMES, frames); wr32(m->g + B_FRAMENUM, 1);
    wr32(m->g + B_RATE, rate); wr32(m->g + B_RATEDIV, div); wr32(m->g + B_OPENFLAGS, flags);
    for (int i = 0; i < 256; i++) if (track_vol[i] == 0 && nchosen < 0) track_vol[i] = 1;
    sound_open(m);
    fprintf(stderr, "  [bink] a movie opens: %dx%d, %u frames at %u/%u, %d sound track(s) of %u\n", m->vdec->width, m->vdec->height,
            frames, rate, div, m->natrack, m->fmt->nb_streams - 1);
    RET(m->g);
}
void I__BinkClose(CPU *c) { OFF("_BinkClose"); struct movie *m = find(ARG(0)); T("BinkClose"); if (m) movie_close(m); }

// void BinkSetSoundTrack(count, ids): which tracks the next movie plays
void I__BinkSetSoundTrack(CPU *c) {
    OFF("_BinkSetSoundTrack");
    static int first = 1;
    if (first) { first = 0; for (int i = 0; i < 256; i++) track_vol[i] = 1; }
    nchosen = (int)ARG(0) < NTRACK ? (int)ARG(0) : NTRACK;
    for (int k = 0; k < nchosen; k++) chosen[k] = ARG(1) ? rd32(ARG(1) + 4 * (uint32_t)k) : (uint32_t)k;
    if (tracing()) { fprintf(stderr, "  [bink] BinkSetSoundTrack:"); for (int k = 0; k < nchosen; k++) fprintf(stderr, " %u", chosen[k]); fputc('\n', stderr); }
}
void I__BinkSetSoundSystem(CPU *c) { OFF("_BinkSetSoundSystem"); RET(1); }
// void BinkSetVolume(bink, track, volume): 32768 is the file's own level
void I__BinkSetVolume(CPU *c) { OFF("_BinkSetVolume"); float v = (float)(int32_t)ARG(2) / 32768.0f; track_vol[ARG(1) & 255] = v < 0 ? 0 : v > 2 ? 2 : v; }
void I__BinkSetPan(CPU *c) { OFF("_BinkSetPan"); }
void I__BinkSetIOSize(CPU *c) { OFF("_BinkSetIOSize"); }
void I__BinkControlBackgroundIO(CPU *c) { OFF("_BinkControlBackgroundIO"); RET(0); }
void I__BinkService(CPU *c) { OFF("_BinkService"); }

// void BinkGetFrameBuffersInfo(bink, buffers): one frame, three planes, for the game to allocate
void I__BinkGetFrameBuffersInfo(CPU *c) {
    OFF("_BinkGetFrameBuffersInfo");
    struct movie *m = find(ARG(0)); uint32_t fb = ARG(1);
    if (!m || !fb) return;
    uint32_t w = (uint32_t)m->vdec->width, h = (uint32_t)m->vdec->height;
    memset(G2H(fb), 0, FB_FRAMES + 2 * FB_SET);
    wr32(fb + FB_TOTAL, 1); wr32(fb + FB_YW, w); wr32(fb + FB_YH, h); wr32(fb + FB_CW, (w + 1) / 2); wr32(fb + FB_CH, (h + 1) / 2);
    for (int p = 0; p < 3; p++) wr32(fb + FB_FRAMES + (uint32_t)p * FB_PLANE, 1);     // Y, cR, cB wanted; no alpha
    T("BinkGetFrameBuffersInfo: %ux%u", w, h);
}
void I__BinkRegisterFrameBuffers(CPU *c) { OFF("_BinkRegisterFrameBuffers"); struct movie *m = find(ARG(0)); if (m) m->fb = ARG(1); }

// S32 BinkWait(bink): 0 when it is time for the frame, 1 while it is early
void I__BinkWait(CPU *c) {
    OFF("_BinkWait");
    struct movie *m = find(ARG(0));
    RET(0);
    if (!m || !m->started || m->paused) return;
    uint64_t rate = rd32(m->g + B_RATE), div = rd32(m->g + B_RATEDIV), n = rd32(m->g + B_FRAMENUM);
    if (!rate) return;
    uint64_t due = m->t0 + (n - 1) * div * 1000000000ull / rate;
    RET(now_ns() < due);
}
// S32 BinkDoFrame(bink): the current frame into the registered buffers
void I__BinkDoFrame(CPU *c) {
    OFF("_BinkDoFrame");
    struct movie *m = find(ARG(0));
    RET(0);
    if (!m) return;
    uint32_t n = rd32(m->g + B_FRAMENUM);
    if (!m->started) {                                            // the clock starts with the first picture shown
        uint64_t rate = rd32(m->g + B_RATE), div = rd32(m->g + B_RATEDIV);
        m->started = 1; m->t0 = now_ns() - (rate ? (uint64_t)(n - 1) * div * 1000000000ull / rate : 0);
    }
    if (!decode_to(m, n)) { T("BinkDoFrame: frame %u could not be read", n); return; }
    uint32_t fb = m->fb;
    if (!fb) return;
    wr32(fb + FB_NUM, 0);
    uint32_t set = fb + FB_FRAMES;
    int w = m->pic->width, h = m->pic->height;
    copy_plane(set, m->pic->data[0], m->pic->linesize[0], rd32(fb + FB_YW), rd32(fb + FB_YH), w, h);
    copy_plane(set + FB_PLANE, m->pic->data[2], m->pic->linesize[2], rd32(fb + FB_CW), rd32(fb + FB_CH), (w + 1) / 2, (h + 1) / 2);
    copy_plane(set + 2 * FB_PLANE, m->pic->data[1], m->pic->linesize[1], rd32(fb + FB_CW), rd32(fb + FB_CH), (w + 1) / 2, (h + 1) / 2);
}
// void BinkNextFrame(bink)
void I__BinkNextFrame(CPU *c) {
    OFF("_BinkNextFrame");
    struct movie *m = find(ARG(0));
    if (!m) return;
    uint32_t n = rd32(m->g + B_FRAMENUM), frames = rd32(m->g + B_FRAMES);
    wr32(m->g + B_LASTFRAME, n);
    if (n >= frames) { n = 1; m->started = 0; } else n++;         // past the end it starts again (a looping movie)
    wr32(m->g + B_FRAMENUM, n);
}
// S32 BinkGetRects(bink, flags): how many rectangles changed; the whole picture, always
void I__BinkGetRects(CPU *c) { OFF("_BinkGetRects"); RET(find(ARG(0)) ? 1 : 0); }
// S32 BinkShouldSkip(bink): are we so late that this frame should not be shown (more than four frames behind)
void I__BinkShouldSkip(CPU *c) {
    OFF("_BinkShouldSkip");
    struct movie *m = find(ARG(0));
    RET(0);
    if (!m || !m->started || m->paused) return;
    uint64_t rate = rd32(m->g + B_RATE), div = rd32(m->g + B_RATEDIV), n = rd32(m->g + B_FRAMENUM);
    if (!rate || n >= rd32(m->g + B_FRAMES)) return;
    uint64_t due = m->t0 + (n + 3) * div * 1000000000ull / rate;
    RET(now_ns() > due);
}
// void BinkGoto(bink, frame, flags)
void I__BinkGoto(CPU *c) {
    OFF("_BinkGoto");
    struct movie *m = find(ARG(0));
    if (!m) return;
    uint32_t n = ARG(1), frames = rd32(m->g + B_FRAMES);
    if (n < 1) n = 1;
    if (n > frames) n = frames;
    wr32(m->g + B_FRAMENUM, n); m->started = 0;
    T("BinkGoto %u", n);
}
// U32 BinkGetKeyFrame(bink, frame, flags): any frame can be asked for here; reading starts from the key frame before it
void I__BinkGetKeyFrame(CPU *c) { OFF("_BinkGetKeyFrame"); RET(ARG(1)); }
// S32 BinkPause(bink, pause)
void I__BinkPause(CPU *c) {
    OFF("_BinkPause");
    struct movie *m = find(ARG(0));
    RET(0);
    if (!m) return;
    int p = ARG(1) != 0;
    if (p && !m->paused) m->paused_at = now_ns();
    if (!p && m->paused && m->started) m->t0 += now_ns() - m->paused_at;
    m->paused = p;
    RET(p);
}
// void BinkGetSummary(bink, summary): { Width, Height, TotalTime, FileFrameRate, FileFrameRateDiv, FrameRate,
//                                       FrameRateDiv, TotalOpenTime, TotalFrames, ... }
void I__BinkGetSummary(CPU *c) {
    OFF("_BinkGetSummary");
    struct movie *m = find(ARG(0)); uint32_t s = ARG(1);
    if (!m || !s) return;
    memset(G2H(s), 0, 124);
    uint32_t rate = rd32(m->g + B_RATE), div = rd32(m->g + B_RATEDIV), frames = rd32(m->g + B_FRAMES);
    wr32(s, rd32(m->g + B_WIDTH)); wr32(s + 4, rd32(m->g + B_HEIGHT));
    wr32(s + 8, rate ? (uint32_t)((uint64_t)frames * div * 1000 / rate) : 0);
    wr32(s + 0xc, rate); wr32(s + 0x10, div); wr32(s + 0x14, rate); wr32(s + 0x18, div);
    wr32(s + 0x20, frames);
}
// void BinkGetRealtime(bink, realtime, frames): { FrameNum, FrameRate, FrameRateDiv, Frames, ... }
void I__BinkGetRealtime(CPU *c) {
    OFF("_BinkGetRealtime");
    struct movie *m = find(ARG(0)); uint32_t r = ARG(1);
    if (!m || !r) return;
    memset(G2H(r), 0, 52);
    wr32(r, rd32(m->g + B_FRAMENUM)); wr32(r + 4, rd32(m->g + B_RATE)); wr32(r + 8, rd32(m->g + B_RATEDIV));
    wr32(r + 0xc, 1);
}
