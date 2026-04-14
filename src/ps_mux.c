#include "ps_mux.h"

typedef struct { uint8_t *buf; int bitpos; } BitWriter;

static inline void bw_put(BitWriter *w, uint64_t v, int nbits) {
    for (int i=nbits-1;i>=0;--i) {
        int bit = (v>>i) & 1;
        int byte = w->bitpos >> 3;
        int off  = 7 - (w->bitpos & 7);
        w->buf[byte] = (uint8_t)((w->buf[byte] & ~(1<<off)) | (bit<<off));
        w->bitpos++;
    }
}

static uint32_t calculate_psm_crc32(const uint8_t *data, int length) {
    uint32_t crc = 0xFFFFFFFF;
    const uint32_t poly = 0x04C11DB7;
    for (int i=0;i<length;i++) {
        crc ^= ((uint32_t)data[i] << 24);
        for (int j=0;j<8;j++) crc = (crc & 0x80000000) ? ((crc<<1) ^ poly) : (crc<<1);
    }
    return crc;
}

static int generate_psm_header(uint8_t *buffer) {
    uint8_t psm_data[] = {
        0x00,0x00,0x01,0xBC,
        0x00,0x0E,
        0xE1,0xFF,
        0x00,0x00,
        0x00,0x04,
        0x1B,0xE0,0x00,0x00
    };
    memcpy(buffer, psm_data, sizeof(psm_data));
    uint32_t crc = calculate_psm_crc32(psm_data, (int)sizeof(psm_data));
    buffer[sizeof(psm_data)+0] = (crc>>24)&0xFF;
    buffer[sizeof(psm_data)+1] = (crc>>16)&0xFF;
    buffer[sizeof(psm_data)+2] = (crc>>8)&0xFF;
    buffer[sizeof(psm_data)+3] = (crc)&0xFF;
    return (int)sizeof(psm_data) + 4;
}

static int generate_ps_header(uint8_t *buffer, int64_t pts_90k) {
    memset(buffer, 0, 14);
    uint64_t scr_base = (uint64_t)pts_90k & ((1ULL<<33)-1);
    uint32_t mux_rate = (uint32_t)(((VIDEO_BITRATE/8) + 49) / 50);
    if (mux_rate==0) mux_rate=1;
    if (mux_rate>0x3FFFFF) mux_rate=0x3FFFFF;

    BitWriter w = { buffer, 0 };
    bw_put(&w, 0x000001BA, 32);
    bw_put(&w, 0x01, 2);
    bw_put(&w, (scr_base>>30)&0x7, 3); bw_put(&w, 1,1);
    bw_put(&w, (scr_base>>15)&0x7FFF, 15); bw_put(&w, 1,1);
    bw_put(&w, (scr_base)&0x7FFF, 15); bw_put(&w, 1,1);
    bw_put(&w, 0, 9); bw_put(&w, 1,1);
    bw_put(&w, mux_rate, 22);
    bw_put(&w, 1,1); bw_put(&w,1,1);
    bw_put(&w, 0x1F, 5);
    bw_put(&w, 0, 3);
    return 14;
}

static int generate_pes_header(uint8_t *buffer, int h264_size, int64_t pts_90k, int *hdr_len) {
    uint8_t *p = buffer;
    *p++=0x00; *p++=0x00; *p++=0x01; *p++=0xE0;
    int pes_packet_len = h264_size + 8;
    if (pes_packet_len > 0xFFFF) pes_packet_len = 0;
    *p++ = (pes_packet_len>>8)&0xFF;
    *p++ = (pes_packet_len)&0xFF;
    *p++ = 0x84;
    *p++ = 0x80;
    *p++ = 0x05;

    uint64_t pts = ((uint64_t)pts_90k) & ((1ULL<<33)-1);
    *p++ = 0x20 | ((pts>>29)&0x0E) | 0x01;
    *p++ = (pts>>22)&0xFF;
    *p++ = ((pts>>14)&0xFE) | 0x01;
    *p++ = (pts>>7)&0xFF;
    *p++ = ((pts<<1)&0xFE) | 0x01;

    *hdr_len = (int)(p-buffer);
    return 0;
}

int generate_ps_packet(uint8_t *ps_buffer, int buf_size,
                       const uint8_t *h264_data, int h264_size,
                       int64_t pts_90k, int is_keyframe,
                       int *ps_size) {
    uint8_t *ptr = ps_buffer;
    int total = 0;

    int hlen = generate_ps_header(ptr, pts_90k);
    ptr += hlen; total += hlen;

    int64_t cur_s = pts_90k / 90000;
    int need_psm = 0;
    if (is_keyframe) { need_psm = 1; g_last_psm_sent = (int)cur_s; LOG_DEBUG("关键帧，强制发送PSM"); }
    else if ((int)cur_s - g_last_psm_sent >= 1) { need_psm = 1; g_last_psm_sent = (int)cur_s; LOG_DEBUG("每1秒发送PSM"); }

    if (need_psm) {
        uint8_t psm[32];
        int psm_len = generate_psm_header(psm);
        if (total + psm_len > buf_size) return -1;
        memcpy(ptr, psm, (size_t)psm_len);
        ptr += psm_len; total += psm_len; LOG_DEBUG("添加PSM，长度=%d", psm_len);
    }

    int pes_hdr = 0;
    if (generate_pes_header(ptr, h264_size, pts_90k, &pes_hdr) < 0) return -1;
    if (total + pes_hdr + h264_size > buf_size) return -1;
    ptr += pes_hdr; total += pes_hdr;

    memcpy(ptr, h264_data, (size_t)h264_size);
    ptr += h264_size; total += h264_size;

    *ps_size = total;
    return 0;
}
