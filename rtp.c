/*
 * RTP input/output format
 * Copyright (c) 2002 Fabrice Bellard.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "avformat.h"
#include "mpegts.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifndef __BEOS__
# include <arpa/inet.h>
#else
# include "barpainet.h"
#endif
#include <netdb.h>

//#define DEBUG

// #Chris Temp 20230320.
extern FILE *video_pfile;
extern FILE *video_pfile2;

#ifndef AV_WB16
# define AV_WB16(p, darg) do {                \
        unsigned d = (darg);                    \
        ((uint8_t*)(p))[1] = (d);               \
        ((uint8_t*)(p))[0] = (d)>>8;            \
    } while(0)
#endif

/* TODO: - add RTCP statistics reporting (should be optional).

         - add support for h263/mpeg4 packetized output : IDEA: send a
         buffer to 'rtp_write_packet' contains all the packets for ONE
         frame. Each packet should have a four byte header containing
         the length in big endian format (same trick as
         'url_open_dyn_packet_buf') 
*/

#define RTP_VERSION 2

#define RTP_MAX_SDES 256   /* maximum text length for SDES */

/* RTCP paquets use 0.5 % of the bandwidth */
#define RTCP_TX_RATIO_NUM 5
#define RTCP_TX_RATIO_DEN 1000

#define RTP_MAX_CHANNEL     2//ivan
typedef enum {
  RTCP_SR   = 200,
  RTCP_RR   = 201,
  RTCP_SDES = 202,
  RTCP_BYE  = 203,
  RTCP_APP  = 204
} rtcp_type_t;

typedef enum {
  RTCP_SDES_END    =  0,
  RTCP_SDES_CNAME  =  1,
  RTCP_SDES_NAME   =  2,
  RTCP_SDES_EMAIL  =  3,
  RTCP_SDES_PHONE  =  4,
  RTCP_SDES_LOC    =  5,
  RTCP_SDES_TOOL   =  6,
  RTCP_SDES_NOTE   =  7,
  RTCP_SDES_PRIV   =  8, 
  RTCP_SDES_IMG    =  9,
  RTCP_SDES_DOOR   = 10,
  RTCP_SDES_SOURCE = 11
} rtcp_sdes_type_t;

enum RTPPayloadType {
    RTP_PT_ULAW = 0,
    RTP_PT_GSM = 3,
    RTP_PT_G723 = 4,
    RTP_PT_ALAW = 8,
    RTP_PT_S16BE_STEREO = 10,
    RTP_PT_S16BE_MONO = 11,
    RTP_PT_MPEGAUDIO = 14,
    RTP_PT_JPEG = 26,
    RTP_PT_H261 = 31,
    RTP_PT_MPEGVIDEO = 32,
    RTP_PT_MPEG2TS = 33,
    RTP_PT_H263 = 34, /* old H263 encapsulation */
    RTP_PT_MPEG4VIDEO = 35,     //added by ivan
    RTP_PT_ADPCM = 36,          //added by ivan
    RTP_PT_MJPEG = 37,          //added by ivan
    RTP_PT_H264 = 96,          //added by ivan, 20181007 WJF, actually the H264 type is 96, but not affected by this value // #Chris Temp 20230317.
    RTP_PT_PRIVATE = 97,		//WJF 991019 modified from 96 to 97
    // RTP_PT_H265 = 98,           // #Chris Temp 20230317. Added, H.265 Payload type.
};

#define RTP_SR_SIZE 68
typedef struct fmpeg4_profile {
/* video mpeg4 info */
    int v_codec_id;
    int v_width;
    int v_height;
    int v_frame_rate;
    int v_frame_rate_base;
    int v_size;
    int v_seq;
    
/* audio adpcm wav header info*/
    int a_codec_id;
    int a_channels;
    int a_sample_rate;
    int a_bit_rate;
    int a_size;
    int a_block_align;
    int a_seq;
} fmpeg4_profile;
typedef struct RTPContext {
    int payload_type;
    int payload_record[RTP_MAX_CHANNEL];
    uint32_t ssrc;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t base_timestamp;
    uint32_t cur_timestamp;
    int max_payload_size;
    /* rtcp sender statistics receive */
    int64_t last_rtcp_ntp_time;
    int64_t first_rtcp_ntp_time;
    uint32_t last_rtcp_timestamp;
    /* rtcp sender statistics */
    unsigned int packet_count;
    unsigned int octet_count;
    unsigned int last_octet_count;
    int first_packet;
    int doing_type;
    /* buffer for output */
    uint8_t buf[RTP_MAX_PACKET_LENGTH];
    uint8_t *buf_ptr;
    fmpeg4_profile profile;
    int v_offset,a_offset;
    int first_rtcp;
	int payload_count;	//WJF 960826 added, for amr payload, concategate 3 frames to a packet
	int payload_size;	//WJF 960826 added, for amr payload, concategate 3 frames to a packet
} RTPContext;

// #Chris Temp 20230322. Added for H.265
typedef struct RTPMuxContext {
    const AVClass *av_class;
    AVFormatContext *ic;
    AVStream *st;
    int payload_type;
    uint32_t ssrc;
    const char *cname;
    int seq;
    uint32_t timestamp;
    uint32_t base_timestamp;
    uint32_t cur_timestamp;
    int max_payload_size;
    int num_frames;

    /* rtcp sender statistics */
    int64_t last_rtcp_ntp_time;
    int64_t first_rtcp_ntp_time;
    unsigned int packet_count;
    unsigned int octet_count;
    unsigned int last_octet_count;
    int first_packet;
    /* buffer for output */
    uint8_t *buf;
    uint8_t *buf_ptr;

    int max_frames_per_packet;

    /**
     * Number of bytes used for H.264 NAL length, if the MP4 syntax is used
     * (1, 2 or 4)
     */
    int nal_length_size;
    int buffered_nals;

    int flags;

    unsigned int frame_count;
} RTPMuxContext;

extern int switch_order;
static void rtcp_get_profile(RTPContext *s,AVStream *st0,AVStream *st1,unsigned char *buf);

// #Chris Temp 20230322.
// --------------------------------- H.265 Start ----------------------------------------
/* send an rtp packet. sequence number is incremented, but the caller
   must update the timestamp itself */
void ff_rtp_send_data(AVFormatContext *s1, const uint8_t *buf1, int len, int m)
{
    RTPMuxContext *s = s1->priv_data;

    /* build the RTP header */
    put_byte(&s1->pb, (RTP_VERSION << 6));
    put_byte(&s1->pb, (s->payload_type & 0x7f) | ((m & 0x01) << 7));
    put_be16(&s1->pb, s->seq);
    put_be32(&s1->pb, s->timestamp);
    put_be32(&s1->pb, s->ssrc);

    put_buffer(&s1->pb, buf1, len);
    put_flush_packet(&s1->pb);

    s->seq = (s->seq + 1) & 0xffff;
    s->octet_count += len;
    s->packet_count++;
}

static void flush_buffered(int stream_index, AVFormatContext *s1, int last)
{
    RTPMuxContext *s = s1->priv_data;
    AVStream *st = s1->streams[stream_index];
    
    if (s->buf_ptr != s->buf) 
    {
        // If we're only sending one single NAL unit, send it as such, skip
        // the STAP-A/AP framing
        if (s->buffered_nals == 1) 
        {
            if (st->codec.codec_id == CODEC_ID_H264)
                ff_rtp_send_data(s1, s->buf + 3, s->buf_ptr - s->buf - 3, last);
            else
                ff_rtp_send_data(s1, s->buf + 4, s->buf_ptr - s->buf - 4, last);
        } 
        else
            ff_rtp_send_data(s1, s->buf, s->buf_ptr - s->buf, last);
    }
    s->buf_ptr = s->buf;
    s->buffered_nals = 0;
}

static const uint8_t *ff_avc_find_startcode_internal(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4) {
        uint32_t x = *(const uint32_t*)p;
//      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
//      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
        if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
            if (p[1] == 0) {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }
            if (p[3] == 0) {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }

    for (end += 3; p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

const uint8_t *ff_avc_find_startcode(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *out= ff_avc_find_startcode_internal(p, end);
    if(p<out && out<end && !out[-1]) out--;
    return out;
}
// --------------------------------- H.265 End ----------------------------------------

int rtp_get_codec_info(AVCodecContext *codec, int payload_type)
{
    switch(payload_type) {
    case RTP_PT_ULAW:
        codec->codec_id = CODEC_ID_PCM_MULAW;
        codec->channels = 1;
        codec->sample_rate = 8000;
        break;
    case RTP_PT_ALAW:
        codec->codec_id = CODEC_ID_PCM_ALAW;
        codec->channels = 1;
        codec->sample_rate = 8000;
        break;
    case RTP_PT_S16BE_STEREO:
        codec->codec_id = CODEC_ID_PCM_S16BE;
        codec->channels = 2;
        codec->sample_rate = 44100;
        break;
    case RTP_PT_S16BE_MONO:
        codec->codec_id = CODEC_ID_PCM_S16BE;
        codec->channels = 1;
        codec->sample_rate = 44100;
        break;
    case RTP_PT_MPEGAUDIO:
        codec->codec_id = CODEC_ID_MP2;
        break;
    case RTP_PT_JPEG:
        codec->codec_id = CODEC_ID_MJPEG;
        break;
    case RTP_PT_MPEGVIDEO:
        codec->codec_id = CODEC_ID_MPEG1VIDEO;
        break;
    case RTP_PT_ADPCM:
        codec->codec_id = CODEC_ID_ADPCM_G726 ;
        codec->channels = 1;
        codec->sample_rate = 8000;
        break;
    default:
        return -1;
    }
    return 0;
}

/* return < 0 if unknown payload type */
int rtp_get_payload_type(AVCodecContext *codec)
{
    int payload_type;

    /* compute the payload type */
    payload_type = -1;
    fprintf(stderr, "\nrtp codec_id=%d", codec->codec_id ) ;	//WJF 960313 debug
    switch(codec->codec_id) {
    case CODEC_ID_PCM_MULAW:	//56
        payload_type = RTP_PT_ULAW;		//0
        break;
    case CODEC_ID_PCM_ALAW:
        payload_type = RTP_PT_ALAW;
        break;
    case CODEC_ID_PCM_S16BE:
        if (codec->channels == 1) {
            payload_type = RTP_PT_S16BE_MONO;
        } else if (codec->channels == 2) {
            payload_type = RTP_PT_S16BE_STEREO;
        }
        break;
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
        payload_type = RTP_PT_MPEGAUDIO;
        break;
    case CODEC_ID_MPEG1VIDEO:
        payload_type = RTP_PT_MPEGVIDEO;
        break;
    case CODEC_ID_MPEG4:
        payload_type = RTP_PT_MPEG4VIDEO;
        break;
    case CODEC_ID_MJPEG:
        payload_type = RTP_PT_MJPEG;
        break;
    case CODEC_ID_H264:		//34
    case CODEC_ID_H265:		// 50  #Chris Temp 20230317.
        payload_type = RTP_PT_H264;
        break;
    // case CODEC_ID_H265:		// 50  #Chris Temp 20230321.
    //     payload_type = RTP_PT_H265;
    //     break;
    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_MS:		//WJF 960313 added
    case CODEC_ID_ADPCM_G726:
        payload_type = RTP_PT_PRIVATE;		//this must match the rtpmap:xx in ffserver.c
        break;
	case CODEC_ID_MPEG2TS:
        payload_type = RTP_PT_MPEG2TS;
		break ;
    default:
        break;
    }
    return payload_type;
}

static inline uint32_t decode_be32(const uint8_t *p)
{
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static inline uint64_t decode_be64(const uint8_t *p)
{
    return ((uint64_t)decode_be32(p) << 32) | decode_be32(p + 4);
}

static int rtcp_parse_packet(AVFormatContext *s1, const unsigned char *buf, int len)
{
    RTPContext *s = s1->priv_data;

    if (buf[1] != 200)
        return -1;
    s->last_rtcp_ntp_time = decode_be64(buf + 8);
    if (s->first_rtcp_ntp_time == AV_NOPTS_VALUE)
        s->first_rtcp_ntp_time = s->last_rtcp_ntp_time;
    else if (s->first_rtcp_ntp_time > s->last_rtcp_ntp_time) //added by ivan 
        s->first_rtcp_ntp_time = s->last_rtcp_ntp_time;
    s->last_rtcp_timestamp = decode_be32(buf + 16);

    rtcp_get_profile(s,s1->streams[0],s1->streams[1],(unsigned char *)buf);
    return 0;
}

/**
 * Parse an RTP packet directly sent as raw data. Can only be used if
 * 'raw' is given as input file
 * @param s1 media file context
 * @param pkt returned packet
 * @param buf input buffer
 * @param len buffer len
 * @return zero if no error.
 */
int rtp_parse_packet(AVFormatContext *s1, AVPacket *pkt, 
                     const unsigned char *buf, int len)
{
    RTPContext *s = s1->priv_data;
    unsigned int ssrc, h;
    int payload_type, seq, delta_timestamp,idx;
    AVStream *st;
    uint32_t timestamp;
    
    if (len < 12)
        return -1;

    if ((buf[0] & 0xc0) != (RTP_VERSION << 6))
        return -1;
    if (buf[1] >= 200 && buf[1] <= 204) {
        rtcp_parse_packet(s1, buf, len);
        if(pkt&&(pkt->data!=0))
        {
            //fprintf(stderr, "free_packet\n");fflush(stdout);
            av_free_packet(pkt);
        }
 
        if(s->profile.a_size>0)
            s->seq=s->profile.a_seq-1;
        else if(s->profile.v_size>0)
            s->seq=s->profile.v_seq-1;
        else
            return -1;
             
        s->a_offset=0;
        s->v_offset=0;
        switch_order=1;
        s->first_rtcp=1; //need the first video rtcp packet for re-sync
        return -1;
    }

    payload_type = buf[1] & 0x7f;
    seq  = (buf[2] << 8) | buf[3];
    timestamp = decode_be32(buf + 4);
    ssrc = decode_be32(buf + 8);

#ifdef DEBUG //1
    fprintf(stderr, "payload_type %d, seq=%d timestamp=0x%x first_rtcp=%d\n",payload_type,seq,timestamp,s->first_rtcp);
    fflush(stdout);
#endif

    if(s->first_rtcp==0)
    {
        //fprintf(stderr, "NO first rtcp comming!\n");
        switch_order=0;
        return -1;
    }
    
#if 1 //ivan
    if(s->payload_record[0]<0)
        s->payload_record[0] = payload_type;
    else if(s->payload_record[1]<0)
        s->payload_record[1] = payload_type;
    // #Chris Temp 20230321.
    // if((payload_type==RTP_PT_MPEG4VIDEO)||(payload_type==RTP_PT_MJPEG)||(payload_type==RTP_PT_H264)||(payload_type==RTP_PT_H265))
    if((payload_type==RTP_PT_MPEG4VIDEO)||(payload_type==RTP_PT_MJPEG)||(payload_type==RTP_PT_H264))
        idx=0;
    else if(payload_type==RTP_PT_ADPCM)
        idx=1;
    else
        return -1;
#else
    if (s->payload_type < 0) {
        s->payload_type = payload_type;
        
        if (payload_type == RTP_PT_MPEG2TS) {
            /* XXX: special case : not a single codec but a whole stream */
            return -1;
        } else {
            st = av_new_stream(s1, 0);
            if (!st)
                return -1;
            rtp_get_codec_info(&st->codec, payload_type);
        }
    }

    /* NOTE: we can handle only one payload type */
    if (s->payload_type != payload_type)
        return -1;
#endif

#if defined(DEBUG) || 1
    if(seq != 0) //ivan
    if (seq != ((s->seq + 1) & 0xffff)) {
        fprintf(stderr, "RTP: PT=%02x: bad cseq %04x expected=%04x\n", 
               payload_type, seq, ((s->seq + 1) & 0xffff));
        s->seq = seq;
        if(pkt&&(pkt->data!=0))
        {
            //fprintf(stderr, "free_packet\n");fflush(stdout);
            av_free_packet(pkt);
        }
        switch_order=0;
        s->first_rtcp=0;
        return -1;
    }
    s->seq = seq;
#else
    s->seq = seq;
#endif

    len -= 12;
    buf += 12;
    st = s1->streams[idx];
    switch(st->codec.codec_id) {
    case CODEC_ID_MP2:
        /* better than nothing: skip mpeg audio RTP header */
        if (len <= 4)
            return -1;
        h = decode_be32(buf);
        len -= 4;
        buf += 4;
        av_new_packet(pkt, len);
        memcpy(pkt->data, buf, len);
        break;
    case CODEC_ID_MPEG1VIDEO:
        /* better than nothing: skip mpeg audio RTP header */
        if (len <= 4)
            return -1;
        h = decode_be32(buf);
        buf += 4;
        len -= 4;
        if (h & (1 << 26)) {
            /* mpeg2 */
            if (len <= 4)
                return -1;
            buf += 4;
            len -= 4;
        }
        av_new_packet(pkt, len);
        memcpy(pkt->data, buf, len);
        break;
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
    case CODEC_ID_H264:
    case CODEC_ID_H265: // #Chris Temp 20230318.
        // #Chris Temp 20230317.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s#1 ", __func__);
   
        if(pkt&&(pkt->data!=0)&&(s->a_offset!=0))
            av_free_packet(pkt);
        if(pkt->data==0)
        {
            if(av_new_packet(pkt,s->profile.v_size)<0)
                return -1;
            pkt->stream_index=0;
        }
        if((s->v_offset+len)>s->profile.v_size)
        {
            //fprintf(stderr, "Packet Alignment Fail! free it!\n");fflush(stdout);
            av_free_packet(pkt);
            return -1;
		}
        memcpy(pkt->data+s->v_offset, buf, len);
        s->v_offset+=len;
        break;
    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_G726:
        if(pkt&&(pkt->data!=0)&&(s->v_offset!=0))
            av_free_packet(pkt);
        if(pkt->data==0)
        {
            if(av_new_packet(pkt,s->profile.a_size)<0)
                return -1;
            pkt->stream_index=1;
        }
        if((s->a_offset+len)>s->profile.a_size)
        {
            //fprintf(stderr, "Packet Alignment Fail! free it!\n");fflush(stdout);
            av_free_packet(pkt);
            return -1;
        }
        memcpy(pkt->data+s->a_offset, buf, len);
        s->a_offset+=len;
        break;
    default:
        av_new_packet(pkt, len);
        memcpy(pkt->data, buf, len);
        break;
    }

    switch(st->codec.codec_id) {
    case CODEC_ID_MP2:
    case CODEC_ID_MPEG1VIDEO:
    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_G726:
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
    case CODEC_ID_H264:
    case CODEC_ID_H265: // #Chris Temp 20230318.
        // #Chris Temp 20230317.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s#2 ", __func__);
        if (s->last_rtcp_ntp_time != AV_NOPTS_VALUE) {
            int64_t addend;
            /* XXX: is it really necessary to unify the timestamp base ? */
            /* compute pts from timestamp with received ntp_time */
            delta_timestamp = timestamp - s->last_rtcp_timestamp;
            /* convert to 90 kHz without overflow */
            addend = (s->last_rtcp_ntp_time - s->first_rtcp_ntp_time) >> 14;
            addend = (addend * 5625) >> 14;
            pkt->pts = addend + delta_timestamp;
        }
        break;
    default:
        /* no timestamp info yet */
        break;
    }
    
    switch(st->codec.codec_id) {
    case CODEC_ID_ADPCM_IMA_WAV:
        if(s->profile.a_size==s->a_offset)
            return 0;
        return -2; //continue
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
    case CODEC_ID_H264:
    case CODEC_ID_H265: // #Chris Temp 20230318.
        // #Chris Temp 20230317.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s#3 ", __func__);
        if(s->profile.v_size==s->v_offset)
            return 0;
        return -2; //continue
    default:
        break;
    }
    return 0;
}

void rtp_dump(unsigned char *buf,unsigned int len)
{
    int i;
//    fprintf(stderr, "rtp_dump 0x%x with size 0x%x\n",buf,len);
    
    for(i=0;i<len;i++)
    {
        if((i%16)==0)
            fprintf(stderr, "\n%02d  ",i);
        fprintf(stderr, "%02x ",buf[i]);
    }
//    fprintf(stderr, "\n");
}
static void rtcp_get_profile(RTPContext *s,AVStream *st0,AVStream *st1,unsigned char *buf)
{
    //video
    st0->codec.codec_type=CODEC_TYPE_VIDEO;
    st0->codec.codec_id=s->profile.v_codec_id=decode_be32(buf+30);
    st0->codec.width=s->profile.v_width=decode_be32(buf+34);
    st0->codec.height=s->profile.v_height=decode_be32(buf+38);
    st0->codec.frame_rate=s->profile.v_frame_rate=decode_be32(buf+42);
    st0->codec.frame_rate_base=s->profile.v_frame_rate_base=decode_be32(buf+46);
    s->profile.v_size=decode_be32(buf+50);
    s->profile.v_seq=decode_be32(buf+54);
    st0->codec.extradata_size=0;
    st0->start_time=0;
    st0->duration=0;

#if defined(DEBUG) || 0
    fprintf(stderr, "\n==>id=%d,w=%d,h=%d,fr=%d,fr_base=%d,v_size=0x%x vseq=%d\n",st0->codec.codec_id,st0->codec.width,st0->codec.height,st0->codec.frame_rate,st0->codec.frame_rate_base,s->profile.v_size,s->profile.v_seq);fflush(stdout);
#endif    

    //audio
    st1->codec.codec_type=CODEC_TYPE_AUDIO;
    st1->codec.codec_id=s->profile.a_codec_id=decode_be32(buf+58);
    st1->codec.channels=s->profile.a_channels=decode_be32(buf+62);
    st1->codec.sample_rate=s->profile.a_sample_rate=decode_be32(buf+66);
    st1->codec.bit_rate=s->profile.a_bit_rate=decode_be32(buf+70);
    s->profile.a_size=decode_be32(buf+74);
    st1->codec.block_align=s->profile.a_block_align=decode_be32(buf+78);
    s->profile.a_seq=decode_be32(buf+82);
    st1->codec.extradata_size=0;

#if defined(DEBUG) || 0
    fprintf(stderr, "id=%d,c=%d,sr=%d,br=%d,a_size=0x%x,align=0x%x aseq=%d\n",st1->codec.codec_id,st1->codec.channels,st1->codec.sample_rate,st1->codec.bit_rate,s->profile.a_size,st1->codec.block_align,s->profile.a_seq);fflush(stdout);
#endif
    st1->start_time=0;
    st1->duration=0;
//getchar();
}
static int rtp_read_header(AVFormatContext *s1,
                           AVFormatParameters *ap)
{
    RTPContext *s = s1->priv_data;
    AVStream *st0,*st1;
    char buf[RTP_MAX_PACKET_LENGTH];
    int ret;
    s->payload_type = -1;
    s->payload_record[0]=-1;
    s->payload_record[1]=-1;
    s->last_rtcp_ntp_time = AV_NOPTS_VALUE;
    s->first_rtcp_ntp_time = AV_NOPTS_VALUE;
    while(1) //read until SR packet arrived
    {
        if (url_interrupt_cb())
        {
            fprintf(stderr, "rtp_read_header return\n");
            return -EIO;
            }
        ret = url_read(url_fileno(&s1->pb), buf, sizeof(buf));
        //rtp_dump(buf,ret);
        if (ret < 0)
            continue;
        if(buf[1]==RTCP_SR)
        {
            st0 = av_new_stream(s1, 0);     //fix for video
            st1 = av_new_stream(s1, 1);     //fix for audio
//            rtp_get_profile(s,st0,st1,buf);
            break;
        }
    }       
    rtcp_parse_packet(s1, buf, ret);

    if(s->profile.a_size>0)
        s->seq=s->profile.a_seq-1;
    else if(s->profile.v_size>0)
        s->seq=s->profile.v_seq-1;
        
    switch_order=1;
    s->first_rtcp=1; //need the first video rtcp packet for re-sync
        
    return 0;
}

static int rtp_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    RTPContext *s = s1->priv_data;
    char buf[RTP_MAX_PACKET_LENGTH];
    int ret;

    fprintf(stderr, "\nrtp_read_packet()" ) ;	//WJF 960313 debug
    pkt->data=0;//ivan
    /* XXX: needs a better API for packet handling ? */
    for(;;) {
        ret = url_read(url_fileno(&s1->pb), buf, sizeof(buf));
        if (ret < 0)
            goto timeout_ret;
        if (rtp_parse_packet(s1, pkt, buf, ret) == 0)
        {
            s->first_rtcp=0;
            break;
        }
    }
    
    switch_order=0;
    s->first_rtcp=0;

#if defined(DEBUG) || 0
    fprintf(stderr, "<read back size 0x%x,swtich order=0>",pkt->size);fflush(stdout);
#endif
    return 0;

timeout_ret:
    if(pkt&&(pkt->data!=0))
    {
        //fprintf(stderr, "free_packet\n");fflush(stdout);
        av_free_packet(pkt);
    }
    if(s->first_rtcp)
        ;//fprintf(stderr, "timeout\n");
    switch_order=0;
    s->first_rtcp=0;
    return AVERROR_IO;
}

static int rtp_read_close(AVFormatContext *s1)
{
    //    RTPContext *s = s1->priv_data;
    return 0;
}

static int rtp_probe(AVProbeData *p)
{
    if (strstart(p->filename, "rtp://", NULL))
        return AVPROBE_SCORE_MAX;
    return 0;
}

/* rtp output */

//static unsigned char mp4v_config[64] ;
static int mp4v_cfg_len ;
static int rtp_write_header(AVFormatContext *s1)
{
    RTPContext *s = s1->priv_data;
    int payload_type, max_packet_size;
    AVStream *st;
    int i, fr;

	fprintf(stderr, "\nrtp_wh s1->nb_streams=%d, s=0x%x",s1->nb_streams, s);    //WJF 960314 debug
#if 0 //ivan to support two RTP
    if (s1->nb_streams != 1)
        return -1;
#endif

	s->payload_count = 0 ;		//WJF 960826 added

for(i=0;i<s1->nb_streams;i++)
{
    st = s1->streams[i]; 

    payload_type = rtp_get_payload_type(&st->codec);
    if (payload_type < 0)
        payload_type = RTP_PT_PRIVATE; /* private payload type */
    s->payload_record[i] = payload_type;
    s->payload_type = payload_type;

//Start from 0 is better    s->base_timestamp = random();
    s->base_timestamp = 0;
    s->timestamp = s->base_timestamp;
    s->ssrc = random();
    s->first_packet = 1;
    s->doing_type=-1;
    s->v_offset=0;
    s->a_offset=0;
//fprintf(stderr, "init v_offset, a_offset\n");fflush(stdout);
    s->seq=0;//ivan
/* init profile data */
    if(st->codec.codec_type==CODEC_TYPE_VIDEO)
    {
        s->profile.v_codec_id=st->codec.codec_id;
        s->profile.v_width=st->codec.width;
        s->profile.v_height=st->codec.height;
        s->profile.v_frame_rate=st->codec.frame_rate;
        s->profile.v_frame_rate_base=st->codec.frame_rate_base;

		//WJF 960821 added for rtsp/rtp implementation
		if(  st->codec.frame_rate_base > 0 )
			fr = st->codec.frame_rate / st->codec.frame_rate_base ;
		else
			fr = 10 ;

		//WJF 980501 modified, use the field comment for the mp4v_config, so can handle more than two kind of resolutions
//		mp4v_cfg_len = generate_mp4v_config(mp4v_config, st->codec.width, st->codec.height, fr) ;
		mp4v_cfg_len = generate_mp4v_config( s1->comment+4, st->codec.width, st->codec.height, fr) ;
		memcpy( s1->comment, &mp4v_cfg_len, 4 ) ;
    }
    else //audio
    {
        s->profile.a_codec_id=st->codec.codec_id;
        s->profile.a_channels=st->codec.channels;
        s->profile.a_sample_rate=st->codec.sample_rate;
        s->profile.a_bit_rate=st->codec.bit_rate;
        s->profile.a_block_align=st->codec.block_align;
        //s->profile.a_bits_per_sample=st->codec.bits_per_sample;

		fprintf(stderr, "\nRTP Audio Profile(%d, %d, %d, %d)", s->profile.a_channels, s->profile.a_sample_rate, s->profile.a_bit_rate, s->profile.a_block_align ) ;    //WJF 960314 debug
	}

    max_packet_size = url_fget_max_packet_size(&s1->pb);
	fprintf(stderr, "\nurl_fget_max_packet_size=%d", max_packet_size);    //WJF 960314 debug

	//for UDP	
//temporary	max_packet_size = 1316 ;		//WJF 960816 added for debug, 1362-46
	//for TCP
//temporary	max_packet_size = 1456 ;		//WJF 960816 added for debug, 1362-46
//	fprintf(stderr, "\nrtp max_pkt_size=%d", max_packet_size);    //WJF 960314 debug

    if (max_packet_size <= 12)
        return AVERROR_IO;
    s->max_payload_size = max_packet_size - 12;

    switch(st->codec.codec_id) {
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
        s->buf_ptr = s->buf + 4;
        s->cur_timestamp = 0;
        break;
    case CODEC_ID_MPEG1VIDEO:
        s->cur_timestamp = 0;
        break;
    default:
        s->buf_ptr = s->buf;
        break;
    }
}
    return 0;
}

/* send an rtcp sender report packet */ // comes here
static void rtcp_send_sr(AVFormatContext *s1, int64_t ntp_time, uint32_t timestamp)
{
    RTPContext *s = s1->priv_data;
//    fprintf(stderr, "\nrtcp_send_sr(%d)" );

#if defined(DEBUG)
    fprintf(stderr, "RTCP: %02x %Lx %x\n", s->payload_type, ntp_time, s->timestamp);
#endif
    put_byte(&s1->pb, (RTP_VERSION << 6));
    put_byte(&s1->pb, 200);
    put_be16(&s1->pb, 6+sizeof(struct fmpeg4_profile)+1); /* length in words - 1 + profile + 1 (0xcc22) */
    put_be32(&s1->pb, s->ssrc);
    put_be64(&s1->pb, ntp_time);
    put_be32(&s1->pb, timestamp);
    put_be32(&s1->pb, s->packet_count);
    put_be32(&s1->pb, s->octet_count);
    /* profile-specific extensions */
    put_be16(&s1->pb, 0xcc22);  //mpeg4,adpcm tag notification 0xcc22
    put_be32(&s1->pb, s->profile.v_codec_id);
    put_be32(&s1->pb, s->profile.v_width);
    put_be32(&s1->pb, s->profile.v_height);
    put_be32(&s1->pb, s->profile.v_frame_rate);
    put_be32(&s1->pb, s->profile.v_frame_rate_base);
    put_be32(&s1->pb, s->profile.v_size);
    put_be32(&s1->pb, s->profile.v_seq);

    put_be32(&s1->pb, s->profile.a_codec_id);
    put_be32(&s1->pb, s->profile.a_channels);
    put_be32(&s1->pb, s->profile.a_sample_rate);
    put_be32(&s1->pb, s->profile.a_bit_rate);
    put_be32(&s1->pb, s->profile.a_size);
    put_be32(&s1->pb, s->profile.a_block_align);
    put_be32(&s1->pb, s->profile.a_seq);
    put_flush_packet(&s1->pb);
//fprintf(stderr, "rtcp send v_seq=%d a_seq=%d\n",s->profile.v_seq,s->profile.a_seq);
}

/* send an rtp packet. sequence number is incremented, but the caller
   must update the timestamp itself */
//audio comes here
static void rtp_send_data(AVFormatContext *s1, const uint8_t *buf1, int len)
{
    RTPContext *s = s1->priv_data;

#ifdef DEBUG //1
//    fprintf(stderr, "rtp_send_data size=%d payload=%d seq=%d timestamp=%d\n", len,s->payload_type,s->seq,s->timestamp);
#endif

    /* build the RTP header */
    put_byte(&s1->pb, (RTP_VERSION << 6));
    put_byte(&s1->pb, s->payload_type & 0x7f);
    
	put_be16(&s1->pb, s->seq);
    put_be32(&s1->pb, s->timestamp);
    put_be32(&s1->pb, s->ssrc);
//    fprintf(stderr, "p_b\n");	//WJF 960313 debug
    put_buffer(&s1->pb, buf1, len);
    put_flush_packet(&s1->pb);
    
    s->seq++;
    s->octet_count += len;
    s->packet_count++;
}

//D-Link
//000001B008000001B509000001000000012000C48881F4505040F1443F
static unsigned char cfg_prefix[]={0x00,0x00,0x01,0xb0,0x01,0x00,0x00,0x01,0xb5,0x09
	,0x00,0x00 ,0x01 ,0x00 ,0x00 ,0x00 ,0x01 ,0x20,0x00,0x86,0xc4, 0x00 } ;

//p_index: pointer to the length of the cfg, p_bindex: pointer to the size of bits , empty=0, max=7 
//value:the value to be added, bs:the bit length of the value to be added(1~32)
static int add_bits_to_cfg(unsigned char *cfg, int *p_index, int * p_bindex, unsigned int value, int bs)
{
	int l, i ;
	unsigned int tmp ;

//    fprintf(stderr, "\n#1 %d, %d, 0x%x, 0x%x, %d", *p_index, *p_bindex, cfg[*p_index], value, bs ) ;	//WJF 960313 debug
	while( bs > 0 )
	{
		if( *p_bindex == 0 )
			cfg[*p_index] = 0 ;

		l = bs ;
		if( l > (8-*p_bindex) )
			l = 8 - *p_bindex ;

		tmp = value ;
		tmp = (tmp << (32-bs)) >> (32-bs) ;	//clear all the left bits
		tmp = tmp >> (bs-l) ;	//clear all the right bits

		if ( (8-*p_bindex) > l )
			tmp = tmp << (8-*p_bindex-l) ;		//left shift to the left position

		cfg[*p_index] |= tmp ;

		*p_bindex += l ;

//    fprintf(stderr, "\n#2 %d, %d, %d, 0x%x, 0x%x", l, *p_index, *p_bindex, cfg[*p_index], tmp ) ;	//WJF 960313 debug
		if( *p_bindex >= 8 )
		{
			*p_bindex -= 8 ;
			*p_index += 1 ;
		}
		bs -= l ;		
	}
}

//wd:width, ht:height, fr:frame rate
int generate_mp4v_config(unsigned char *cfg, int wd, int ht, int fr)
{
	int i, len, bs, bits_len ;	//len:octet length, bs:bits size(0~7)

    fprintf(stderr, "\n%s(%d, %d)", __func__, wd, ht ) ;	//WJF 960313 debug
	//WJF 970529 added to prevent some mis-inperpretation between different players
	if( fr ==2 )
		fr = 3 ;
	else if( fr ==4 )
		fr = 5 ;

	len=0; bs=0;
	memcpy( cfg, cfg_prefix, sizeof(cfg_prefix) ) ;
	len = sizeof(cfg_prefix) ;

	add_bits_to_cfg(cfg, &len, &bs, fr, 6) ;	//add frame rate
	add_bits_to_cfg(cfg, &len, &bs, 3, 2) ;	 //add marker and vop bit

	if( fr >31 ) bits_len = 6 ;
	else if( fr >15 ) bits_len = 5 ;
	else if( fr > 7 )  bits_len = 4 ;
	else if( fr > 3 )  bits_len = 3 ;		//this is correct for quicktime, but false for VLC
	else if( fr > 1 ) bits_len = 2 ;		//this is correct for quicktime, but false for VLC
//	else if( fr > 4 )  bits_len = 3 ;		//this is correct for VLC, but false for quicktime
//	else if( fr > 2 ) bits_len = 2 ;		//this is correct for VLC, but false for quicktime
	else  bits_len = 1 ;

	add_bits_to_cfg(cfg, &len, &bs, 1, bits_len) ;	 //add min bits to value the frame rate
	add_bits_to_cfg(cfg, &len, &bs, 1, 1) ;	 //add marker bit
	add_bits_to_cfg(cfg, &len, &bs, wd, 13) ;	 //add width
	add_bits_to_cfg(cfg, &len, &bs, 1, 1) ;	 //add marker bit
	add_bits_to_cfg(cfg, &len, &bs, ht, 13) ;	 //add height
	add_bits_to_cfg(cfg, &len, &bs, 1, 1) ;	 //add marker bit
	add_bits_to_cfg(cfg, &len, &bs, 0x8c, 9) ;	 //add lst few bits
	if( bs > 0 )
		add_bits_to_cfg(cfg, &len, &bs, 0x7f>>bs, 8-bs) ;	//add padding bits
/*
	fprintf(stderr, "\nmp4v_cfg=" ) ;	//WJF 960313 debug
	for( i=0; i<len; i++)
	    fprintf(stderr, " %02X", cfg[i] ) ;	//WJF 960313 debug
*/
	return len ;
}

/* send an integer number of samples and compute time stamp and fill
   the rtp send buffer before sending. */
static void rtp_send_samples(AVFormatContext *s1,
                             const uint8_t *buf1, int size, int sample_size)
{
    RTPContext *s = s1->priv_data;
    int len, max_packet_size, n;

    max_packet_size = (s->max_payload_size / sample_size) * sample_size;
    /* not needed, but who nows */
    if ((size % sample_size) != 0)
        av_abort();
    while (size > 0) {
        len = (max_packet_size - (s->buf_ptr - s->buf));
        if (len > size)
            len = size;

        /* copy data */
        memcpy(s->buf_ptr, buf1, len);
        s->buf_ptr += len;
        buf1 += len;
        size -= len;
        n = (s->buf_ptr - s->buf);
        /* if buffer full, then send it */
        if (n >= max_packet_size) {
            rtp_send_data(s1, s->buf, n);
            s->buf_ptr = s->buf;
            /* update timestamp */
            s->timestamp += n / sample_size;
        }
    }
} 

/* NOTE: a single frame must be passed with sequence header if
   needed. XXX: use slices. */
static void rtp_send_mpegvideo(AVFormatContext *s1,
                               const uint8_t *buf1, int size)
{
    RTPContext *s = s1->priv_data;
    AVStream *st = s1->streams[0];
    int len, h, max_packet_size;
    uint8_t *q;

    max_packet_size = s->max_payload_size;

    while (size > 0) {
        /* XXX: more correct headers */
        h = 0;
        if (st->codec.sub_id == 2)
            h |= 1 << 26; /* mpeg 2 indicator */
        q = s->buf;
        *q++ = h >> 24;
        *q++ = h >> 16;
        *q++ = h >> 8;
        *q++ = h;

        if (st->codec.sub_id == 2) {
            h = 0;
            *q++ = h >> 24;
            *q++ = h >> 16;
            *q++ = h >> 8;
            *q++ = h;
        }
        
        len = max_packet_size - (q - s->buf);
        if (len > size)
            len = size;

        memcpy(q, buf1, len);
        q += len;

        /* 90 KHz time stamp */
        s->timestamp = s->base_timestamp + 
            av_rescale((int64_t)s->cur_timestamp * st->codec.frame_rate_base, 90000, st->codec.frame_rate);

		rtp_send_data(s1, s->buf, q - s->buf);

        buf1 += len;
        size -= len;
    }
    s->cur_timestamp++;
}

static void rtp_send_adpcm(int stream_index,AVFormatContext *s1,
                         const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    //AVStream *st = s1->streams[stream_index]; //ivan
    int len, max_packet_size;

	pts = pts*8/90 ;		//WJF 991019 modified, translate from 90k to 8k
//    fprintf(stderr, " A(%lld)", pts);	//WJF 960313 debug, H.264 will come here

    max_packet_size = s->max_payload_size;
    s->payload_type=s->payload_record[stream_index];//ivan

//	fprintf(stderr, " RTP(%d,%d)", stream_index, s->payload_type);	//WJF 960716 debug, comes here twice a time

    while (size > 0) {
        len = max_packet_size;
        if (len > size)
            len = size;

        s->timestamp = s->base_timestamp + pts;
//fprintf(stderr, "update(audio) timestamp to 0x%x after send pts 0x%x\n",s->timestamp,pts);
        rtp_send_data(s1, buf1, len);

        buf1 += len;
        size -= len;
    }
    s->cur_timestamp++;
}

static void rtp_send_raw(AVFormatContext *s1,
                         const uint8_t *buf1, int size)
{
    RTPContext *s = s1->priv_data;
    AVStream *st = s1->streams[0];
    int len, max_packet_size;

    fprintf(stderr, "s_raw %x\n", s1->pb.write_packet);	//WJF 960313 debug
    max_packet_size = s->max_payload_size;

    while (size > 0) {
        len = max_packet_size;
        if (len > size)
            len = size;

        /* 90 KHz time stamp */
        s->timestamp = s->base_timestamp + 
            av_rescale((int64_t)s->cur_timestamp * st->codec.frame_rate_base, 90000, st->codec.frame_rate);
        rtp_send_data(s1, buf1, len);

        buf1 += len;
        size -= len;
    }
    s->cur_timestamp++;
}

/* NOTE: size is assumed to be an integer multiple of TS_PACKET_SIZE */
static void rtp_send_mpegts_raw(AVFormatContext *s1,
                                const uint8_t *buf1, int size)
{
    RTPContext *s = s1->priv_data;
//    RTPDemuxContext *s = s1->priv_data;
    int len, out_len;

    while (size >= TS_PACKET_SIZE) {
        len = s->max_payload_size - (s->buf_ptr - s->buf);
        if (len > size)
            len = size;
        memcpy(s->buf_ptr, buf1, len);
        buf1 += len;
        size -= len;
        s->buf_ptr += len;

        out_len = s->buf_ptr - s->buf;
        if (out_len >= s->max_payload_size) {
            rtp_send_data(s1, s->buf, out_len);
            s->buf_ptr = s->buf;
        }
    }
}

//video comes here, send MP4V-ES payload data, M=1 for last segment or full AU
static void rtp_send_es_data(AVFormatContext *s1, const uint8_t *buf1, int len, unsigned char pt)
{
    RTPContext *s = s1->priv_data;
	unsigned int *ui_ptr ;
    
    // #Chris Temp 20230321.
    int i = 0;
    fprintf(stderr, "\n### #CL -> rtp_send_es_data -- pt=(%d, %x), seq=%d, ts=%d", pt, s->payload_type, s->seq, s->timestamp);
    fprintf(stderr, "\n Buf : (");
    for (i = 1; i <= 30; i++)
    {
        if (i == 30)
            fprintf(stderr, " %x)\n", buf1[i]);
        else
            fprintf(stderr, " %x", buf1[i]);
    }
    
    // #Chris Temp 20230321.
    // if (video_pfile2)
    // {
    //     fwrite(buf1, 1, len, video_pfile2);
    // }

	//s->payload_type=38, pt=96
//    fprintf(stderr, "\nrtp_send_es_data %d, pt=(%d, %x), seq=%d, ts=%d, (%x %x %x %x)", len,s->payload_type, pt, s->seq,s->timestamp, buf1[0], buf1[1], buf1[2], buf1[3]);
//    fprintf(stderr, "\nrtp_send_es_data %d, (%x %x %x %x)", len, buf1[0], buf1[1], buf1[2], buf1[3]);

    /* build the RTP header */
    put_byte(&s1->pb, (RTP_VERSION << 6));
//    put_byte(&s1->pb, s->payload_type & 0x7f);
    
	put_byte(&s1->pb, pt);		//payload type byte

	put_be16(&s1->pb, s->seq);
    put_be32(&s1->pb, s->timestamp);
    put_be32(&s1->pb, s->ssrc);
	put_buffer(&s1->pb, buf1, len);

    put_flush_packet(&s1->pb);	//in aviobuf.c
    
    s->seq++;
    s->octet_count += len;
    s->packet_count++;
}

#define USE_LONG_MPEG4V_CFG
#ifdef USE_LONG_MPEG4V_CFG
#define MP4V_TMP_BUF_SIZE	(160*1024)
static char tmp_buf[MP4V_TMP_BUF_SIZE] ;
#endif
static void rtp_send_fvideo(int stream_index,AVFormatContext *s1,
                         const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    AVStream *st = s1->streams[stream_index]; //ivan
    int len, max_packet_size;
	unsigned int *ui_ptr ;

//    fprintf(stderr, " V(%lld)", pts);	//WJF 960313 debug, H.264 will not come here

	max_packet_size = s->max_payload_size;
    s->payload_type=s->payload_record[stream_index];//ivan

	ui_ptr = buf1 ;
	if( (*ui_ptr == 0x00010000) && (ui_ptr[1] == 0x20010000) )
	{
//	    fprintf(stderr, "\nModify");	//WJF 960313 debug, H.264 will not come inside here
#ifdef USE_LONG_MPEG4V_CFG
		if( (size+10) > MP4V_TMP_BUF_SIZE )
			fprintf(stderr, "\n!!!! Not enough rtp video buffer !!!!");	//WJF 960313 debug

		memcpy( &mp4v_cfg_len, s1->comment, 4 ) ;
		memcpy( tmp_buf, s1->comment+4, mp4v_cfg_len ) ;
		memcpy( tmp_buf + mp4v_cfg_len, buf1+19, size-19 ) ;
		size += mp4v_cfg_len-19 ;
		buf1 = tmp_buf ;
#else
		memcpy( &mp4v_cfg_len, s1->comment, 4 ) ;
		memcpy( buf1+8, s1->comment+4+18, mp4v_cfg_len-18 ) ;
#endif
	}		

	while (size > 0) {
        len = max_packet_size;
        if (len > size)
            len = size;

        s->timestamp = s->base_timestamp + pts;

		//WJF 960818 modified for MP4V-ES implementation( rfc 3640 )
		if( size > len )
			rtp_send_es_data( s1, buf1, len, st->payload_type );
		else
			rtp_send_es_data( s1, buf1, len, st->payload_type | 0x80 );

        buf1 += len;
        size -= len;
    }
    s->cur_timestamp++;
}

// #Chris Temp 20230317. Added, For H.265 send nal.
static void rtp_send_h265_nal(int stream_index, AVFormatContext *s1, uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    AVStream *st = s1->streams[stream_index];
    int len, max_packet_size, first_packet = 1;
    uint8_t *p_s = buf1;
    uint8_t type = (p_s[0] >> 1) & 0x3F;
    uint8_t layer_id = ((p_s[0] & 0x01) << 5) | ((p_s[1] >> 3) & 0x1F);
    uint8_t tid = p_s[1] & 0x07;
    
    max_packet_size = s->max_payload_size;
    s->payload_type = s->payload_record[stream_index];

    if (size > max_packet_size)
    {
        p_s -= 3;
        size += 3;
        p_s[0] = 49 << 1;
        p_s[1] = (layer_id << 3) | (tid & 0x07);
        p_s[2] = type;
        p_s[2] |= 1 << 7; // set the S bit: mark as start fragment.
    }

    while (size > 0)
    {
        s->timestamp = s->base_timestamp + pts;
        
        if (!first_packet)
        {
            p_s -= 3;
            size += 3;
            p_s[0] = 49 << 1;
            p_s[1] = (layer_id << 3) | (tid & 0x07);
            p_s[2] = type;
            p_s[2] &= ~((1 << 7) | (1 << 6));  // Clear the S (start) and E (end) bits.
        }
        
        len = max_packet_size;
        if (len > size)
            len = size;
            
        if (size > len)
            rtp_send_es_data(s1, p_s, len, st->payload_type);
        else
        {
            if (!first_packet)
                p_s[2] |= 1 << 6; // Set the E (end) bit.
            
            if (type >= 32 && type <= 34) // if NAL type is VPS, SPS, PPS not set M bit.
                rtp_send_es_data(s1, p_s, len, st->payload_type);
            else
                rtp_send_es_data(s1, p_s, len, st->payload_type | 0x80);
        }

        p_s += len;
        size -= len;
        first_packet = 0;
    }
    s->cur_timestamp++;
}

// #Chris Temp 20230320. Added, For H.265.
// Although it is exactly the same as H264 at present, for future scalability, it is still written additionally.
uint8_t * find_h265_start_code(uint8_t *p_s, int size, int *s_len)
{
	uint8_t *p=p_s ;
	int i ;
	if( size < 4 ) return (p_s+size) ;
	for( i=0; i<size-3; i++)
	{
		if( (p[0] == 0) && (p[1] == 0) )
		{
			if( p[2] == 1 )
			{
			    fprintf(stderr, "\n********* Start Code 00 00 01 **********");	
				*s_len = 3 ;
				return p ;
			}
			else if( (p[2] == 0) && (p[3] == 1) )
			{
				*s_len = 4 ;
			    return p ;
			}
		}
		++p ;
	}
	*s_len = 0 ;
	return (p_s+size) ;
}

//WJF 20190127, the followings are modified for the RTSP/RTP software implementation, especially for RT3904 
static void rtp_send_h264_nal(int stream_index,AVFormatContext *s1, uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    AVStream *st = s1->streams[stream_index]; //ivan
    int i, len, max_packet_size, first_packet = 1 ;
	unsigned int *ui_ptr ;
	uint8_t *p_s=buf1, *p_e ;
    uint8_t type = p_s[0] & 0x1F;
    uint8_t nri = p_s[0] & 0x60;
    
    // #Chris Temp 20230321.
    // if (video_pfile)
    // {
    //     fwrite(p_s, 1, size, video_pfile);
    // }

    // #Chris Temp 20230320.
    // 在 NAL 分片之前打印原始 NAL 數據
    // fprintf(stderr, "\n### #CL -> rtp_send_h264_nal -> Original NAL data (size: %d):", size);

	//20190128, debug 
//	s->max_payload_size = 1444 ;


//    fprintf(stderr, "\nNAL(%d, %d, %d)", type, size, s->max_payload_size);	//WJF 960313 debug, H.264 will come here
	// rtp_dump(buf1, size<16 ? size : 16 ) ;

	max_packet_size = s->max_payload_size;
    s->payload_type=s->payload_record[stream_index];//ivan

/*	20210426 removed, this will cause broken video
	//remove emulation prevention  00 00 03 --> 00 00
	if( size > 3 )
	{
		for( i=size-3; i>=0; i--)
			if( (p_s[i]==0) && (p_s[i+1]==0) && (p_s[i+2]==3) )
			{
			    fprintf(stderr, "\n** 00 00 03 -> 00 00 **");	
//				rtp_dump( p_s+i, (size-i)<16 ? (size-i) : 16 ) ;
				memcpy( p_s+i+2, p_s+i+3, size-i-3 ) ;
				--size ;
			}
	}
*/
//	if( ((p_s[0]&0x1f) != 7) && ((p_s[0]&0x1f) != 8) )
	if( (type != 7) && (type != 8) && (size > max_packet_size) )
	{
		if( type > 5 )
		    fprintf(stderr, "\n****** NAL type %d ***********", type);	//WJF 960313 debug, special type need to take care

		--p_s ;
		++size ;
		p_s[0] = 0x1C | nri ;
		p_s[1] = 0x80 | type ;
	}

	while (size > 0) 
	{
        s->timestamp = s->base_timestamp + pts;

		if( !first_packet )
		{
			p_s -= 2 ;
			size += 2 ;
			p_s[0] = 0x1C | nri ;
			p_s[1] = type ;
		}

        len = max_packet_size;
        if (len > size)
            len = size;
        
        // #Chris Temp 20230320.
        // 打印 NAL 分片數據
        // fprintf(stderr, "\n### #CL -> rtp_send_h264_nal -> NAL fragment (size: %d):", len);

        // 檢查 NAL 分片頭信息
        // fprintf(stderr, "\n### #CL -> rtp_send_h264_nal -> Type: %d, Nri : %d", type, nri);

        // 檢查 RTP 包的 Marker 位
        // fprintf(stderr, "\n### #CL -> rtp_send_h264_nal -> Marker bit: %d\n", (st->payload_type & 0x80) ? 1 : 0);    
        
		//WJF 960818 modified for MP4V-ES implementation( rfc 3640 )
		if( size > len )
        {
            // fprintf(stderr, "\n### #CL -> rtp_send_es_data #1 (%d)", st->payload_type); // #Chris Temp 20230321.
			rtp_send_es_data( s1, p_s, len, st->payload_type );
        }
		else	//the last packet
		{
			if( !first_packet )	//segmented, and the last packet
				p_s[1] |= 0x40 ;		//the end packet

			if( type < 6 )		//for the video packet, VCL 
            {
                // fprintf(stderr, "\n### #CL -> rtp_send_es_data #2 (%d)", st->payload_type); // #Chris Temp 20230321.
			    rtp_send_es_data( s1, p_s, len, st->payload_type | 0x80 );
            }
			else			//non-VCL
            {
                // fprintf(stderr, "\n### #CL -> rtp_send_es_data #3 (%d)", st->payload_type); // #Chris Temp 20230321.
				rtp_send_es_data( s1, p_s, len, st->payload_type );
            }
		}

        p_s += len;
        size -= len;
		first_packet = 0 ;
    }
    s->cur_timestamp++;
}

//return the address of the next start code, if not find, return the end address
uint8_t * find_h264_start_code(uint8_t *p_s, int size, int *s_len)
{
	uint8_t *p=p_s ;
	int i ;
	if( size < 4 ) return (p_s+size) ;
	for( i=0; i<size-3; i++)
	{
		if( (p[0] == 0) && (p[1] == 0) )
		{
			if( p[2] == 1 )
			{
			    fprintf(stderr, "\n********* Start Code 00 00 01 **********");	
				*s_len = 3 ;
				return p ;
			}
			else if( (p[2] == 0) && (p[3] == 1) )
			{
				*s_len = 4 ;
			return p ;
			}
		}
		++p ;
	}
	*s_len = 0 ;
	return (p_s+size) ;
}

uint8_t * find_h264_insert_code(uint8_t *p_s, int size)
{
	uint8_t *p=p_s ;
	int i ;
	if( size < 3 ) return (p_s+size) ;
	for( i=0; i<size-2; i++)
	{
		if( (p[0] == 0) && (p[1] == 0) && (p[2] == 3) )
		    fprintf(stderr, "\n********* Found 00 00 03 **********");	
		++p ;
	}
	return (p_s+size) ;
}


char h264_sc[4]={0,0,0,1} ;		//h264 start code, could be 00 00 00 01  or 00 00 01
static void rtp_send_h264(int stream_index,AVFormatContext *s1, const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    AVStream *st = s1->streams[stream_index]; //ivan
    int len, max_packet_size, i, s_len=4 ;
	unsigned int *ui_ptr ;
	uint8_t *p_s, *p_e ;
    
    // #Chris Temp 20230320.
    // fprintf(stderr, "\n### #CL -> rtp_send_h264 -- total_size : %d\n", size);

	if( memcmp(buf1, h264_sc,4)!=0 )
	{
	    fprintf(stderr, "\n********* H264 not start with 00 00 00 01 **********");	
		return 0 ;
	}
	if( size <= 4 )
	{
	    fprintf(stderr, "\n********* H264 not enough size %d **********", size);	
		return 0 ;
	}

	p_s = buf1+4 ;
	size -= 4 ;

	while(size)
	{
		p_e = find_h264_start_code(p_s, size, &s_len) ;	//goto the beginning of next start code
		len = p_e - p_s ;
		rtp_send_h264_nal( stream_index, s1, p_s, len, pts) ;
		if( size > (len+s_len) )
		{
			size -= (len+s_len) ;
			p_s = p_e+s_len ;
		}
		else size =0 ;
	}

    s->cur_timestamp++;
}

// #Chris Temp 20230317. Added for rtp send H.265, Mostly the same as H264.
char h265_sc[4] = {0, 0, 0, 1};		// H.265 start code, could be 00 00 00 01 or 00 00 01
static void rtp_send_h265(int stream_index, AVFormatContext *s1, const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    int len, s_len = 4;
    int start_code_len = 0; // #Chris Temp 20230322.
    uint8_t *p_s, *p_e;

    // H.265 start code same as H264.
    if (memcmp(buf1, h265_sc, 4) == 0)
        start_code_len = 4;
    else if (memcmp(buf1, h265_sc + 1, 3) == 0)
        start_code_len = 3;
        
    if (start_code_len == 0)
    {
        fprintf(stderr, "\n********* H265 not start with 00 00 00 01 or 00 00 01 **********");
        return 0;
    }

    if (size <= 4)
    {
        fprintf(stderr, "\n********* H265 not enough size %d **********", size);
        return 0;
    }

    p_s = buf1 + start_code_len;
    size = size - start_code_len;

    while (size)
    {
        p_e = find_h265_start_code(p_s, size, &s_len);
        len = p_e - p_s;
        
        // #Chris Temp 20230322.
        rtp_send_h265_nal(stream_index, s1, p_s, len, pts);
        // rtp_send_h265_nal_new(stream_index, s1, p_s, len, pts);
        if (size > (len + s_len))
        {
            size -= (len + s_len);
            p_s = p_e + s_len;
        }
        else
            size = 0;
    }

    s->cur_timestamp++;
}

// #Chris Temp 20230322.
static void nal_send(int stream_index, AVFormatContext *s1, uint8_t *buf, int size, int last)
{
    RTPMuxContext *s = s1->priv_data;
    AVStream *st = s1->streams[stream_index];
    enum CodecID codec = st->codec.codec_id;

    if (size <= s->max_payload_size) 
    {
        int buffered_size = s->buf_ptr - s->buf;
        int header_size;
        int skip_aggregate = 0;
        
        header_size = 2;

        // Flush buffered NAL units if the current unit doesn't fit
        if (buffered_size + 2 + size > s->max_payload_size) 
        {
            flush_buffered(stream_index, s1, 0);
            buffered_size = 0;
        }
        
        // If we aren't using mode 0, and the NAL unit fits including the
        // framing (2 bytes length, plus 1/2 bytes for the STAP-A/AP marker),
        // write the unit to the buffer as a STAP-A/AP packet, otherwise flush
        // and send as single NAL.
        if (buffered_size + 2 + header_size + size <= s->max_payload_size && !skip_aggregate) 
        {
            if (buffered_size == 0) 
            {
                *s->buf_ptr++ = 48 << 1;
                *s->buf_ptr++ = 1;
            }
            AV_WB16(s->buf_ptr, size);
            s->buf_ptr += 2;
            memcpy(s->buf_ptr, buf, size);
            s->buf_ptr += size;
            s->buffered_nals++;
        } 
        else 
        {
            flush_buffered(stream_index, s1, 0);
            ff_rtp_send_data(s1, buf, size, last);
        }
    } 
    else 
    {
        int flag_byte, header_size;
        flush_buffered(stream_index, s1, 0);
       
        if (codec == CODEC_ID_H265) 
        {
            uint8_t nal_type = (buf[0] >> 1) & 0x3F;
            /*
             * create the HEVC payload header and transmit the buffer as fragmentation units (FU)
             *
             *    0                   1
             *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
             *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             *   |F|   Type    |  LayerId  | TID |
             *   +-------------+-----------------+
             *
             *      F       = 0
             *      Type    = 49 (fragmentation unit (FU))
             *      LayerId = 0
             *      TID     = 1
             */
            s->buf[0] = 49 << 1;
            s->buf[1] = 1;

            /*
             *     create the FU header
             *
             *     0 1 2 3 4 5 6 7
             *    +-+-+-+-+-+-+-+-+
             *    |S|E|  FuType   |
             *    +---------------+
             *
             *       S       = variable
             *       E       = variable
             *       FuType  = NAL unit type
             */
            s->buf[2]  = nal_type;
            /* set the S bit: mark as start fragment */
            s->buf[2] |= 1 << 7;

            /* pass the original NAL header */
            buf  += 2;
            size -= 2;

            flag_byte   = 2;
            header_size = 3;
        }

        while (size + header_size > s->max_payload_size) {
            memcpy(&s->buf[header_size], buf, s->max_payload_size - header_size);
            ff_rtp_send_data(s1, s->buf, s->max_payload_size, 0);
            buf  += s->max_payload_size - header_size;
            size -= s->max_payload_size - header_size;
            s->buf[flag_byte] &= ~(1 << 7);
        }
        s->buf[flag_byte] |= 1 << 6;
        memcpy(&s->buf[header_size], buf, size);
        ff_rtp_send_data(s1, s->buf, size + header_size, last);
    }
}

static void rtp_send_h265_new(int stream_index, AVFormatContext *s1, const uint8_t *buf1, int size)
{
    const uint8_t *r, *end = buf1 + size;
    RTPMuxContext *s = s1->priv_data;

    s->timestamp = s->cur_timestamp;
    s->buf_ptr   = s->buf;
    r = ff_avc_find_startcode(buf1, end);
    
    while (r < end) 
    {
        const uint8_t *r1;
        
        while (!*(r++));
        r1 = ff_avc_find_startcode(r, end);
        nal_send(stream_index, s1, r, r1 - r, r1 == end);
        r = r1;
    }
    flush_buffered(stream_index, s1, 1);
}

//static char rtp_buf[1514] ;
static void rtp_send_amr_audio(int stream_index,AVFormatContext *s1,
                         const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    AVStream *st = s1->streams[stream_index]; //ivan
    int len, max_packet_size;
	unsigned char * rtp_buf ;

//    fprintf(stderr, "\ns=0x%x, s->buf =0x%x ", s, s->buf);	//WJF 960313 debug
    
	max_packet_size = s->max_payload_size;
    s->payload_type=s->payload_record[stream_index];//ivan

	if( s->payload_count == 0 )
	{
		rtp_buf = s->buf ;
		rtp_buf[0] = 0xf0;
	    rtp_buf[1] = 0xbc;
		rtp_buf[2] = 0xbc;
	    rtp_buf[3] = 0xbc;
		memcpy(rtp_buf + 4, buf1, size);
//        s->timestamp = s->base_timestamp + pts;
        s->timestamp += 0x280;
		s->payload_size = size + 4 ;
		s->buf_ptr = s->buf + size + 4 ;
		++s->payload_count ;
	}
	else
	{
		memcpy(s->buf_ptr, buf1+1, size-1);
		s->payload_size += size - 1 ;
		s->buf_ptr += size - 1 ;
		++s->payload_count ;

		if( s->payload_count == 4 )
		{
			rtp_send_es_data( s1, s->buf, s->payload_size, st->payload_type | 0x80 );
			s->payload_count = 0 ;
		}
	}

//    s->cur_timestamp++;		//useless for audio
}

static void rtp_send_mp2_audio(int stream_index,AVFormatContext *s1,
                         const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    //AVStream *st = s1->streams[0];
    AVStream *st = s1->streams[stream_index]; //ivan
    int len, max_packet_size;
	unsigned char * rtp_buf ;

    // fprintf(stderr, "\ns=0x%x, s->buf =0x%x ", s, s->buf);	//WJF 960313 debug
    // #Chris Temp 20230321.
    // fprintf(stderr, "\n### #CL -> rtp_send_mp2_audio -- st->payload_type : %d", st->payload_type);

	max_packet_size = s->max_payload_size;
    s->payload_type=s->payload_record[stream_index];//ivan

    s->buf[0] = 0;
    s->buf[1] = 0;
    s->buf[2] = 0;
    s->buf[3] = 0;

	s->buf_ptr = s->buf + 4 ;
	memcpy(s->buf_ptr, buf1, size);
	size += 4 ;

//    s->timestamp += 0x280;
    s->timestamp = s->base_timestamp + pts;
	rtp_send_es_data( s1, s->buf, size, st->payload_type | 0x80 );
}

/* NOTE: we suppose that exactly one frame is given as argument here */
/* XXX: test it */
static void rtp_send_mpegaudio(AVFormatContext *s1,
                               const uint8_t *buf1, int size)
{
    RTPContext *s = s1->priv_data;
    AVStream *st = s1->streams[0];
    int len, count, max_packet_size;

    max_packet_size = s->max_payload_size;

    /* test if we must flush because not enough space */
    len = (s->buf_ptr - s->buf);
    if ((len + size) > max_packet_size) {
        if (len > 4) {
            rtp_send_data(s1, s->buf, s->buf_ptr - s->buf);
            s->buf_ptr = s->buf + 4;
            /* 90 KHz time stamp */
            s->timestamp = s->base_timestamp + 
                (s->cur_timestamp * 90000LL) / st->codec.sample_rate;
        }
    }

//	fprintf(stderr, "\nrtp_send_mpegaudio(), s=0x%x", s);    //WJF 960314 debug
//    fprintf(stderr, "\nmax=%d, size=%d", max_packet_size, size);	//WJF 960313 debug
    /* add the packet */
    if (size > max_packet_size) {
        /* big packet: fragment */
        count = 0;
        while (size > 0) {
            len = max_packet_size - 4;
            if (len > size)
                len = size;
            /* build fragmented packet */
            s->buf[0] = 0;
            s->buf[1] = 0;
            s->buf[2] = count >> 8;
            s->buf[3] = count;
            memcpy(s->buf + 4, buf1, len);
            rtp_send_data(s1, s->buf, len + 4);
            size -= len;
            buf1 += len;
            count += len;
        }
    } else {
        if (s->buf_ptr == s->buf + 4) {
            /* no fragmentation possible */
            s->buf[0] = 0;
            s->buf[1] = 0;
            s->buf[2] = 0;
            s->buf[3] = 0;
        }
        memcpy(s->buf_ptr, buf1, size);
        s->buf_ptr += size;
    }
    s->cur_timestamp += st->codec.frame_size;
}

/* write an RTP packet. 'buf1' must contain a single specific frame. */
static int rtp_write_packet(AVFormatContext *s1, int stream_index,
                            const uint8_t *buf1, int size, int64_t pts)
{
    RTPContext *s = s1->priv_data;
    AVStream *st = s1->streams[stream_index];
    int rtcp_bytes;
    int64_t ntp_time;
    uint32_t timestamp=(uint32_t)pts+s->base_timestamp;
	static int rtp_cnts=0 ;
    
	//20190127, debug, the modifcation of pts does not help on the decoding missed packets
/*	{
		static int64_t		r_pts=0 ;
		if( pts == 0 ) r_pts = 0 ;
		else r_pts += 90*100 ;
		pts = r_pts ;
		timestamp=(uint32_t)pts+s->base_timestamp ;
	}				*/

//    fprintf(stderr, "\nS(%d, %d, %lld)", size, st->codec.codec_id, pts);	//WJF 960313 debug
//	if( st->codec.codec_id !=CODEC_ID_H264 )
//		HexDump(buf1, size<64 ? size : 64 ) ;

#ifdef DEBUG
//    fprintf(stderr, "%d: write len=%d id=%d\n", stream_index, size, st->codec.codec_id);
#endif
//    fprintf(stderr, "%d: write len=%d id=%d\n", stream_index, size, st->codec.codec_id);	//WJF 960313 debug

    /* XXX: mpeg pts hardcoded. RTCP send every 0.5 seconds */
    rtcp_bytes = ((s->octet_count - s->last_octet_count) * RTCP_TX_RATIO_NUM) / 
        RTCP_TX_RATIO_DEN;

    s->doing_type=stream_index;
    
    switch(st->codec.codec_id) {
    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_G726:
        s->profile.v_size=0;
        s->profile.a_size=size;
        s->profile.v_seq=0;
        s->profile.a_seq=s->seq;
        break;
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
    case CODEC_ID_H264:
    case CODEC_ID_H265: // #Chris Temp 20230318.
        // #Chris Temp 20230317.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s#4 ", __func__);
        s->profile.v_size=size;
        s->profile.a_size=0;
        s->profile.v_seq=s->seq;
        s->profile.a_seq=0;
        break;
    default:
        s->profile.v_size=size;
        s->profile.a_size=0;
        s->profile.v_seq=s->seq;
        s->profile.a_seq=0;
        break;
//WJF 960315 debug        return -1;    
    }

    /* compute NTP time */
    /* XXX: 90 kHz timestamp hardcoded */
//    if (s->first_packet || rtcp_bytes >= 28) {
	if( ++rtp_cnts > 30 ){	//send out rtcp every 30 rtp pkts
		rtp_cnts=0 ;
	    ntp_time = (pts << 28) / 5625;
		rtcp_send_sr(s1, ntp_time, timestamp); 
	    s->last_octet_count = s->octet_count;
		s->first_packet = 0;
	}

    switch(st->codec.codec_id) {
    case CODEC_ID_PCM_MULAW:
    case CODEC_ID_PCM_ALAW:
    case CODEC_ID_PCM_U8:
    case CODEC_ID_PCM_S8:
        rtp_send_samples(s1, buf1, size, 1 * st->codec.channels);
        break;
    case CODEC_ID_PCM_U16BE:
    case CODEC_ID_PCM_U16LE:
    case CODEC_ID_PCM_S16BE:
    case CODEC_ID_PCM_S16LE:
        rtp_send_samples(s1, buf1, size, 2 * st->codec.channels);
        break;
    case CODEC_ID_MP2:
		rtp_send_mp2_audio(stream_index, s1, buf1, size, pts);
		break ;
    case CODEC_ID_MP3:
//WJF 960827 remarked off, format not correct
		rtp_send_fvideo(stream_index, s1, buf1, size, pts);		//MP4V-ES send here
        break;
	case CODEC_ID_AMR_NB:
		rtp_send_amr_audio(stream_index, s1, buf1, size, pts);
//	    fprintf(stderr, "\rtp_send_mpegaudio()");	//WJF 960313 debug
//        rtp_send_mpegaudio(s1, buf1, size);
		break ;
    case CODEC_ID_MPEG1VIDEO:
        rtp_send_mpegvideo(s1, buf1, size);
        break;
    case CODEC_ID_H264:
        // #Chris Temp 20230320.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s -- rtp_send_h264 !! ", __func__);
        // if (video_pfile)
        // {
        //     fwrite(buf1, 1, size, video_pfile);
        // }
        rtp_send_h264(stream_index, s1, buf1, size, pts);		//MP4V-ES send here
        break;
    case CODEC_ID_H265: // #Chris Temp 20230317.
        // #Chris Temp 20230320.
        // fprintf(stderr, "\n### #CL -> rtp.c -> %s -- rtp_send_h265 !! ", __func__);
        // if (video_pfile)
        // {
        //     fwrite(buf1, 1, size, video_pfile);
        // }
        rtp_send_h265(stream_index, s1, buf1, size, pts);       //MP4V-ES send here
        // rtp_send_h265_new(stream_index, s1, buf1, size); // #Chris Temp 20230322.
        break;
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
//	    fprintf(stderr, " rrr");	//WJF 960313 debug
        rtp_send_fvideo(stream_index, s1, buf1, size, pts);		//MP4V-ES send here
//	    fprintf(stderr, "ttt ");	//WJF 960313 debug
        break;
    case CODEC_ID_MPEG2TS:
        rtp_send_mpegts_raw(s1, buf1, size);
        break;
    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_G726:
        rtp_send_adpcm(stream_index, s1, buf1, size, pts);
        break;
    default:
        /* better than nothing : send the codec raw data */
        rtp_send_raw(s1, buf1, size);
        break;
    }
    return 0;
}

static int rtp_write_trailer(AVFormatContext *s1)
{
    //    RTPContext *s = s1->priv_data;
    return 0;
}

AVInputFormat rtp_demux = {
    "rtp",
    "RTP input format",
    sizeof(RTPContext),    
    rtp_probe,
    rtp_read_header,
    rtp_read_packet,
    rtp_read_close,
    .flags =0,
};

AVOutputFormat rtp_mux = {
    "rtp",
    "RTP output format",
    NULL,
    NULL,
    sizeof(RTPContext),
    CODEC_ID_PCM_MULAW,
    CODEC_ID_MPEG4,
    rtp_write_header,
    rtp_write_packet,
    rtp_write_trailer,
};

int rtp_init(void)
{
    av_register_output_format(&rtp_mux);
    av_register_input_format(&rtp_demux);
    return 0;
}
