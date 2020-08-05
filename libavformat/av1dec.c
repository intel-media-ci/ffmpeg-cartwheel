/*
 * AV1 Annex B demuxer
 * Copyright (c) 2019 James Almer <jamrial@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavcodec/av1_parse.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"

//2 + max leb 128 size
#define MAX_HEAD_SIZE 10

typedef struct AnnexBContext {
    const AVClass *class;
    AVBSFContext *bsf;
    uint32_t temporal_unit_size;
    uint32_t frame_unit_size;
    AVRational framerate;
} AnnexBContext;

static int leb(AVIOContext *pb, uint32_t *len) {
    int more, i = 0;
    uint8_t byte;
    *len = 0;
    do {
        unsigned bits;
        byte = avio_r8(pb);
        more = byte & 0x80;
        bits = byte & 0x7f;
        if (i <= 3 || (i == 4 && bits < (1 << 4)))
            *len |= bits << (i * 7);
        else if (bits)
            return AVERROR_INVALIDDATA;
        if (++i == 8 && more)
            return AVERROR_INVALIDDATA;
        if (pb->eof_reached || pb->error)
            return pb->error ? pb->error : AVERROR(EIO);
    } while (more);
    return i;
}

static int read_obu(const uint8_t *buf, int size, int64_t *obu_size, int *type, int *has_size_flag)
{
    int start_pos, temporal_id, spatial_id;

    return parse_obu_header(buf, size, obu_size, &start_pos,
                           type, &temporal_id, &spatial_id, has_size_flag);
}

static int annexb_probe(const AVProbeData *p)
{
    AVIOContext pb;
    int64_t obu_size;
    uint32_t temporal_unit_size, frame_unit_size, obu_unit_size;
    int seq = 0;
    int ret, type, cnt = 0, has_size_flag;

    ffio_init_context(&pb, p->buf, p->buf_size, 0,
                      NULL, NULL, NULL, NULL);

    ret = leb(&pb, &temporal_unit_size);
    if (ret < 0)
        return 0;
    cnt += ret;
    ret = leb(&pb, &frame_unit_size);
    if (ret < 0 || ((int64_t)frame_unit_size + ret) > temporal_unit_size)
        return 0;
    cnt += ret;
    temporal_unit_size -= ret;
    ret = leb(&pb, &obu_unit_size);
    if (ret < 0 || ((int64_t)obu_unit_size + ret) >= frame_unit_size)
        return 0;
    cnt += ret;

    temporal_unit_size -= obu_unit_size + ret;
    frame_unit_size -= obu_unit_size + ret;

    avio_skip(&pb, obu_unit_size);
    if (pb.eof_reached || pb.error)
        return 0;

    // Check that the first OBU is a Temporal Delimiter.
    ret = read_obu(p->buf + cnt, FFMIN(p->buf_size - cnt, obu_unit_size), &obu_size, &type, &has_size_flag);
    if (ret < 0 || type != AV1_OBU_TEMPORAL_DELIMITER || obu_size > 0)
        return 0;
    cnt += obu_unit_size;

    do {
        ret = leb(&pb, &obu_unit_size);
        if (ret < 0 || ((int64_t)obu_unit_size + ret) > frame_unit_size)
            return 0;
        cnt += ret;

        avio_skip(&pb, obu_unit_size);
        if (pb.eof_reached || pb.error)
            return 0;

        ret = read_obu(p->buf + cnt, FFMIN(p->buf_size - cnt, obu_unit_size), &obu_size, &type, &has_size_flag);
        if (ret < 0)
            return 0;
        cnt += obu_unit_size;

        switch (type) {
        case AV1_OBU_SEQUENCE_HEADER:
            seq = 1;
            break;
        case AV1_OBU_FRAME:
        case AV1_OBU_FRAME_HEADER:
            return seq ? AVPROBE_SCORE_EXTENSION + 1 : 0;
        case AV1_OBU_TILE_GROUP:
        case AV1_OBU_TEMPORAL_DELIMITER:
            return 0;
        default:
            break;
        }

        temporal_unit_size -= obu_unit_size;
        frame_unit_size -= obu_unit_size;
    } while (frame_unit_size);

    return 0;
}

static int read_header(AVFormatContext *s, AVRational framerate, AVBSFContext **bsf, void *c)
{
    const AVBitStreamFilter *filter = av_bsf_get_by_name("av1_frame_merge");
    AVStream *st;
    int ret;

    if (!filter) {
        av_log(c, AV_LOG_ERROR, "av1_frame_merge bitstream filter "
               "not found. This is a bug, please report it.\n");
        return AVERROR_BUG;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_AV1;
    st->need_parsing = AVSTREAM_PARSE_HEADERS;

    st->internal->avctx->framerate = framerate;
    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

    ret = av_bsf_alloc(filter, bsf);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy((*bsf)->par_in, st->codecpar);
    if (ret < 0) {
        av_bsf_free(bsf);
        return ret;
    }

    ret = av_bsf_init(*bsf);
    if (ret < 0)
        av_bsf_free(bsf);

    return ret;

}

static int annexb_read_header(AVFormatContext *s)
{
    AnnexBContext *c = s->priv_data;
    return read_header(s, c->framerate, &c->bsf, c);
}

static int annexb_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AnnexBContext *c = s->priv_data;
    uint32_t obu_unit_size;
    int ret, len;

retry:
    if (avio_feof(s->pb)) {
        if (c->temporal_unit_size || c->frame_unit_size)
            return AVERROR(EIO);
        goto end;
    }

    if (!c->temporal_unit_size) {
        len = leb(s->pb, &c->temporal_unit_size);
        if (len < 0) return AVERROR_INVALIDDATA;
    }

    if (!c->frame_unit_size) {
        len = leb(s->pb, &c->frame_unit_size);
        if (len < 0 || ((int64_t)c->frame_unit_size + len) > c->temporal_unit_size)
            return AVERROR_INVALIDDATA;
        c->temporal_unit_size -= len;
    }

    len = leb(s->pb, &obu_unit_size);
    if (len < 0 || ((int64_t)obu_unit_size + len) > c->frame_unit_size)
        return AVERROR_INVALIDDATA;

    ret = av_get_packet(s->pb, pkt, obu_unit_size);
    if (ret < 0)
        return ret;
    if (ret != obu_unit_size)
        return AVERROR(EIO);

    c->temporal_unit_size -= obu_unit_size + len;
    c->frame_unit_size -= obu_unit_size + len;

end:
    ret = av_bsf_send_packet(c->bsf, pkt);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to send packet to "
                                "av1_frame_merge filter\n");
        return ret;
    }

    ret = av_bsf_receive_packet(c->bsf, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        av_log(s, AV_LOG_ERROR, "av1_frame_merge filter failed to "
                                "send output packet\n");

    if (ret == AVERROR(EAGAIN))
        goto retry;

    return ret;
}

static int annexb_read_close(AVFormatContext *s)
{
    AnnexBContext *c = s->priv_data;

    av_bsf_free(&c->bsf);
    return 0;
}

typedef struct LowOverheadContext {
    const AVClass *class;
    AVBSFContext *bsf;
    AVRational framerate;
    uint8_t prefetched[MAX_HEAD_SIZE];
    //prefetched len
    int len;
} LowOverheadContext;

static int low_overhead_probe(const AVProbeData *p)
{
    AVIOContext pb;
    int64_t obu_size;
    int ret, type, cnt = 0, has_size_flag;

    ffio_init_context(&pb, p->buf, p->buf_size, 0,
                      NULL, NULL, NULL, NULL);

    // Check that the first OBU is a Temporal Delimiter.
    ret = read_obu(p->buf + cnt, FFMIN(p->buf_size - cnt, MAX_HEAD_SIZE), &obu_size, &type, &has_size_flag);
    if (ret < 0 || type != AV1_OBU_TEMPORAL_DELIMITER || obu_size != 0 || !has_size_flag)
        return 0;
    cnt += ret;

    ret = read_obu(p->buf + cnt, FFMIN(p->buf_size - cnt, MAX_HEAD_SIZE), &obu_size, &type, &has_size_flag);
    if (ret < 0 || type != AV1_OBU_SEQUENCE_HEADER || !has_size_flag)
        return 0;
    cnt += ret;

    ret = read_obu(p->buf + cnt, FFMIN(p->buf_size - cnt, MAX_HEAD_SIZE), &obu_size, &type, &has_size_flag);
    if (ret < 0 || (type != AV1_OBU_FRAME && type != AV1_OBU_FRAME_HEADER) || obu_size <= 0 || !has_size_flag)
        return 0;
    return AVPROBE_SCORE_EXTENSION + 1;
}

static int low_overhead_read_header(AVFormatContext *s)
{
    LowOverheadContext *c = s->priv_data;
    return read_header(s, c->framerate, &c->bsf, c);
}

static int low_overhead_prefetch(AVFormatContext *s)
{
    LowOverheadContext *c = s->priv_data;
    int ret;
    int size = MAX_HEAD_SIZE - c->len;
    if (size > 0 && !avio_feof(s->pb)) {
        ret = avio_read(s->pb, &c->prefetched[c->len], size);
        if (ret < 0)
            return ret;
        c->len += ret;
    }
    return c->len;
}

static int low_overhead_read_data(AVFormatContext *s, AVPacket *pkt, int size)
{
    int left;
    LowOverheadContext *c = s->priv_data;
    int ret = av_new_packet(pkt, size);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Failed to allocate packet for obu\n");
        return ret;
    }
    if (size <= c->len) {
        memcpy(pkt->data, c->prefetched, size);

        left = c->len - size;
        memcpy(c->prefetched, c->prefetched + size, left);
        c->len = left;
    } else {
        memcpy(pkt->data, c->prefetched, c->len);
        left = size - c->len;
        ret = avio_read(s->pb, pkt->data + c->len, left);
        if (ret != left) {
            av_log(c, AV_LOG_ERROR, "Failed to read %d frome file\n", left);
            return ret;
        }
        c->len = 0;
    }
    return 0;
}

static int low_overhead_get_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t obu_size;
    int ret, type, has_size_flag;
    LowOverheadContext *c = s->priv_data;
    ret = low_overhead_prefetch(s);
    if (ret < 0)
        return ret;
    if (ret) {
        ret = read_obu(c->prefetched, c->len, &obu_size, &type, &has_size_flag);
        if (ret < 0 && !has_size_flag) {
            av_log(c, AV_LOG_ERROR, "Failed to read obu\n");
            return ret;
        }
        ret = low_overhead_read_data(s, pkt, ret);
    }
    return ret;
}

static int low_overhead_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    LowOverheadContext *c = s->priv_data;
    int ret;

    while (1) {
        ret = low_overhead_get_packet(s, pkt);
        if (ret < 0)
            return ret;
        ret = av_bsf_send_packet(c->bsf, pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to send packet to "
                                    "av1_frame_merge filter\n");
            return ret;
        }
        ret = av_bsf_receive_packet(c->bsf, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            av_log(s, AV_LOG_ERROR, "av1_frame_merge filter failed to "
                                "send output packet\n");

        if (ret != AVERROR(EAGAIN))
            break;
    }

    return ret;
}

static int low_overhead_read_close(AVFormatContext *s)
{
    LowOverheadContext *c = s->priv_data;

    av_bsf_free(&c->bsf);
    return 0;
}

#define DEC AV_OPT_FLAG_DECODING_PARAM

#define OFFSET(x) offsetof(AnnexBContext, x)
static const AVOption annexb_options[] = {
    { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

#define OFFSET(x) offsetof(LowOverheadContext, x)
static const AVOption low_overhead_options[] = {
    { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

static const AVClass annexb_demuxer_class = {
    .class_name = "AV1 Annex B demuxer",
    .item_name  = av_default_item_name,
    .option     = annexb_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_av1_demuxer = {
    .name           = "av1",
    .long_name      = NULL_IF_CONFIG_SMALL("AV1 Annex B"),
    .priv_data_size = sizeof(AnnexBContext),
    .read_probe     = annexb_probe,
    .read_header    = annexb_read_header,
    .read_packet    = annexb_read_packet,
    .read_close     = annexb_read_close,
    .extensions     = "obu",
    .flags          = AVFMT_GENERIC_INDEX,
    .priv_class     = &annexb_demuxer_class,
};

static const AVClass low_overhead_demuxer_class = {
    .class_name = "AV1 low overhead OBU demuxer",
    .item_name  = av_default_item_name,
    .option     = low_overhead_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_av1_low_overhead_demuxer = {
    .name           = "av1",
    .long_name      = NULL_IF_CONFIG_SMALL("AV1 low overhead OBU"),
    .priv_data_size = sizeof(LowOverheadContext),
    .read_probe     = low_overhead_probe,
    .read_header    = low_overhead_read_header,
    .read_packet    = low_overhead_read_packet,
    .read_close     = low_overhead_read_close,
    .extensions     = "obu",
    .flags          = AVFMT_GENERIC_INDEX,
    .priv_class     = &low_overhead_demuxer_class,
};
