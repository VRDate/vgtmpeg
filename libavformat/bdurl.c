/* @@--
 * 
 * Copyright (C) 2010-2015 Alberto Vigata
 *       
 * This file is part of vgtmpeg
 * 
 * a Versed Generalist Transcoder
 * 
 * vgtmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * vgtmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include "avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "dvdurl_lang.h"
#include "bdurl.h"
#include "url.h"


static int gloglevel = HB_LOG_VERBOSE;

#ifdef __GNUC__
#define BDNOT_USED __attribute__ ((unused))
#else
#define BDNOT_USED
#endif

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static int           next_packet( BLURAY *bd, uint8_t *pkt );
static int title_info_compare_mpls(const void *, const void *);

static hb_bd_t     * hb_bd_init( char * path );
static int           hb_bd_title_count( hb_bd_t * d );
static hb_title_t  * hb_bd_title_scan( hb_bd_t * d, int t, uint64_t min_duration );
static int           hb_bd_start( hb_bd_t * d, hb_title_t *title );
static void          hb_bd_stop( hb_bd_t * d );
static int           hb_bd_seek( hb_bd_t * d, float f );
static int           hb_bd_seek_pts( hb_bd_t * d, uint64_t pts );
static int           hb_bd_seek_chapter( hb_bd_t * d, int chapter );
static hb_buffer_t * hb_bd_read( hb_bd_t * d );
static int           hb_bd_chapter( hb_bd_t * d );
static void          hb_bd_close( hb_bd_t ** _d );
static void          hb_bd_set_angle( hb_bd_t * d, int angle );
static int           hb_bd_main_feature( hb_bd_t * d, hb_list_t * list_title );

/***********************************************************************
 * hb_bd_init
 ***********************************************************************
 *
 **********************************************************************/
hb_bd_t * hb_bd_init( char * path )
{
    hb_bd_t * d;
    int ii;

    d = av_mallocz( sizeof( hb_bd_t ) );

    /* Open device */
    d->bd = bd_open( path, NULL );
    if( d->bd == NULL )
    {
        /*
         * Not an error, may be a stream - which we'll try in a moment.
         */
        hb_log_level(gloglevel, "bd: not a bd - trying as a stream/file instead" );
        goto fail;
    }

    d->title_count = bd_get_titles( d->bd, TITLES_RELEVANT, 0 );  /* FIXME min duration */
    if ( d->title_count == 0 )
    {
        hb_log_level(gloglevel, "bd: not a bd - trying as a stream/file instead" );
        goto fail;
    }
    d->title_info = av_mallocz( sizeof( BLURAY_TITLE_INFO* ) * d->title_count );
    for ( ii = 0; ii < d->title_count; ii++ )
    {
        d->title_info[ii] = bd_get_title_info( d->bd, ii, 0 );  /* FIXME 0 is correct angle? */
    }
    qsort(d->title_info, d->title_count, sizeof( BLURAY_TITLE_INFO* ), title_info_compare_mpls );

    /* vgtmpeg */
    /* allocate fixed hb_buffer_t for reads */
    d->read_buffer = av_mallocz( sizeof(hb_buffer_t));
    d->read_buffer->size = HB_DVD_READ_BUFFER_SIZE;
    d->read_buffer->data = av_malloc(HB_DVD_READ_BUFFER_SIZE);

    d->path = av_strdup( path );

    return d;

fail:
    if( d->bd ) bd_close( d->bd );
    av_free( d );
    return NULL;
}

/***********************************************************************
 * hb_bd_title_count
 **********************************************************************/
int hb_bd_title_count( hb_bd_t * d )
{
    return d->title_count;
}

static void add_audio(int track, hb_list_t *list_audio, BLURAY_STREAM_INFO *bdaudio, int substream_type, uint32_t codec, uint32_t codec_param)
{
    hb_audio_t * audio;
    const iso639_lang_t * lang;
    int stream_type;

    audio = av_mallocz( sizeof( hb_audio_t ) );

    audio->id = (substream_type << 16) | bdaudio->pid;
    audio->config.in.stream_type = bdaudio->coding_type;
    audio->config.in.substream_type = substream_type;
    audio->config.in.codec = codec;
    audio->config.in.codec_param = codec_param;
    audio->config.lang.type = 0;

    lang = lang_for_code2( (char*)bdaudio->lang );

    stream_type = bdaudio->coding_type;
    snprintf( audio->config.lang.description, 
        sizeof( audio->config.lang.description ), "%s (%s)",
        strlen(lang->native_name) ? lang->native_name : lang->eng_name,
        audio->config.in.codec == HB_ACODEC_AC3 ? "AC3" : 
        ( audio->config.in.codec == HB_ACODEC_DCA ? "DTS" : 
        ( ( audio->config.in.codec & HB_ACODEC_FF_MASK ) ? 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_LPCM ? "BD LPCM" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_AC3PLUS ? "E-AC3" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_TRUHD ? "TrueHD" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_DTSHD ? "DTS-HD HRA" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_DTSHD_MASTER ? "DTS-HD MA" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_MPEG1 ? "MPEG1" : 
            ( stream_type == BLURAY_STREAM_TYPE_AUDIO_MPEG2 ? "MPEG2" : 
                                                           "Unknown FFmpeg" 
            ) ) ) ) ) ) ) : "Unknown" 
        ) ) );

    snprintf( audio->config.lang.simple, 
              sizeof( audio->config.lang.simple ), "%s",
              strlen(lang->native_name) ? lang->native_name : 
                                          lang->eng_name );

    snprintf( audio->config.lang.iso639_2, 
              sizeof( audio->config.lang.iso639_2 ), "%s", lang->iso639_2);

    hb_log_level(gloglevel, "bd: audio id=0x%x, lang=%s, 3cc=%s", audio->id,
            audio->config.lang.description, audio->config.lang.iso639_2 );

    audio->config.in.track = track;
    hb_list_add( list_audio, audio );
    return;
}

static int bd_audio_equal( BLURAY_CLIP_INFO *a, BLURAY_CLIP_INFO *b )
{
    int ii, jj, equal;

    if ( a->audio_stream_count != b->audio_stream_count )
        return 0;

    for ( ii = 0; ii < a->audio_stream_count; ii++ )
    {
        BLURAY_STREAM_INFO * s = &a->audio_streams[ii];
        equal = 0;
        for ( jj = 0; jj < b->audio_stream_count; jj++ )
        {
            if ( s->pid == b->audio_streams[jj].pid &&
                 s->coding_type == b->audio_streams[jj].coding_type)
            {
                equal = 1;
                break;
            }
        }
        if ( !equal )
            return 0;
    }
    return 1;
}
#define STR4_TO_UINT32(p) \
    ((((const uint8_t*)(p))[0] << 24) | \
     (((const uint8_t*)(p))[1] << 16) | \
     (((const uint8_t*)(p))[2] <<  8) | \
      ((const uint8_t*)(p))[3])


/***********************************************************************
 * hb_bd_title_scan
 **********************************************************************/
hb_title_t * hb_bd_title_scan( hb_bd_t * d, int tt, uint64_t min_duration )
{

    hb_title_t   * title;
    hb_chapter_t * chapter;
    int            ii, jj;
    BLURAY_TITLE_INFO * ti = NULL;
    BLURAY_STREAM_INFO * bdvideo;
    char * p_cur, * p_last;
    uint64_t pkt_count;


    hb_log_level(gloglevel, "bd: scanning title %d", tt );

    title = hb_title_init( d->path, tt );
    title->demuxer = HB_MPEG_DEMUXER;
    title->type = HB_BD_TYPE;
    title->reg_desc = STR4_TO_UINT32("HDMV");

    p_last = d->path;
    for( p_cur = d->path; *p_cur; p_cur++ )
    {
        if( p_cur[0] == '/' && p_cur[1] )
        {
            p_last = &p_cur[1];
        }
    }
    snprintf( title->name, sizeof( title->name ), "%s", p_last );
    av_strlcpy( title->path, d->path, 1024 );
    title->path[1023] = 0;

    title->vts = 0;
    title->ttn = 0;

    ti = d->title_info[tt - 1];
    if ( ti == NULL )
    {
        hb_log_level(gloglevel, "bd: invalid title" );
        goto fail;
    }
    if ( ti->clip_count == 0 )
    {
        hb_log_level(gloglevel, "bd: stream has no clips" );
        goto fail;
    }
    if ( ti->clips[0].video_stream_count == 0 )
    {
        hb_log_level(gloglevel, "bd: stream has no video" );
        goto fail;
    }

    hb_log_level(gloglevel, "bd: playlist %05d.MPLS", ti->playlist );
    title->playlist = ti->playlist;

    pkt_count = 0;
    for ( ii = 0; ii < ti->clip_count; ii++ )
    {
        pkt_count += ti->clips[ii].pkt_count;
    }
    title->block_start = 0;
    title->block_end = pkt_count;
    title->block_count = pkt_count;

    title->angle_count = ti->angle_count;

    /* Get duration */
    title->duration = ti->duration;
    title->hours    = title->duration / 90000 / 3600;
    title->minutes  = ( ( title->duration / 90000 ) % 3600 ) / 60;
    title->seconds  = ( title->duration / 90000 ) % 60;
    hb_log_level(gloglevel, "bd: duration is %02d:%02d:%02d (%"PRId64" ms)",
            title->hours, title->minutes, title->seconds,
            title->duration / 90 );

    /* ignore short titles because they're often stills */
    if( ti->duration < min_duration )
    {
        hb_log_level(gloglevel, "bd: ignoring title (too short)" );
        goto fail;
    }

    bdvideo = &ti->clips[0].video_streams[0];

    title->video_id = bdvideo->pid;
    title->video_stream_type = bdvideo->coding_type;

    hb_log_level(gloglevel, "bd: video id=0x%x, stream type=%s, format %s", title->video_id,
            bdvideo->coding_type == BLURAY_STREAM_TYPE_VIDEO_MPEG1 ? "MPEG1" :
            bdvideo->coding_type == BLURAY_STREAM_TYPE_VIDEO_MPEG2 ? "MPEG2" :
            bdvideo->coding_type == BLURAY_STREAM_TYPE_VIDEO_VC1 ? "VC-1" :
            bdvideo->coding_type == BLURAY_STREAM_TYPE_VIDEO_H264 ? "H.264" :
            "Unknown",
            bdvideo->format == BLURAY_VIDEO_FORMAT_480I ? "480i" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_576I ? "576i" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_480P ? "480p" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_1080I ? "1080i" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_720P ? "720p" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_1080P ? "1080p" :
            bdvideo->format == BLURAY_VIDEO_FORMAT_576P ? "576p" :
            "Unknown"
          );

    if ( bdvideo->coding_type == BLURAY_STREAM_TYPE_VIDEO_VC1 &&
       ( bdvideo->format == BLURAY_VIDEO_FORMAT_480I ||
         bdvideo->format == BLURAY_VIDEO_FORMAT_576I ||
         bdvideo->format == BLURAY_VIDEO_FORMAT_1080I ) )
    {
        hb_log_level(gloglevel, "bd: Interlaced VC-1 not supported" );
        goto fail;
    }

    switch( bdvideo->coding_type )
    {
        case BLURAY_STREAM_TYPE_VIDEO_MPEG1:
        case BLURAY_STREAM_TYPE_VIDEO_MPEG2:
            title->video_codec = WORK_DECMPEG2;
            title->video_codec_param = 0;
            break;

        case BLURAY_STREAM_TYPE_VIDEO_VC1:
            title->video_codec = WORK_DECAVCODECV;
            title->video_codec_param = CODEC_ID_VC1;
            break;

        case BLURAY_STREAM_TYPE_VIDEO_H264:
            title->video_codec = WORK_DECAVCODECV;
            title->video_codec_param = CODEC_ID_H264;
            title->flags |= HBTF_NO_IDR;
            break;

        default:
            hb_log_level(gloglevel, "scan: unknown video codec (0x%x)",
                    bdvideo->coding_type );
            goto fail;
    }

    switch ( bdvideo->aspect )
    {
        case BLURAY_ASPECT_RATIO_4_3:
            title->container_aspect = 4. / 3.;
            break;
        case BLURAY_ASPECT_RATIO_16_9:
            title->container_aspect = 16. / 9.;
            break;
        default:
            hb_log_level(gloglevel, "bd: unknown aspect" );
            goto fail;
    }
    hb_log_level(gloglevel, "bd: aspect = %g", title->container_aspect );

    /* Detect audio */
    // All BD clips are not all required to have the same audio.
    // But clips that have seamless transition are required
    // to have the same audio as the previous clip.
    // So find the clip that has the most other clips with the 
    // matching audio.
    // Max primary BD audios is 32
	{
		int matches;
		int most_audio = 0;
		int audio_clip_index = 0;
		for (ii = 0; ii < ti->clip_count; ii++) {
			matches = 0;
			for (jj = 0; jj < ti->clip_count; jj++) {
				if (bd_audio_equal(&ti->clips[ii], &ti->clips[jj])) {
					matches++;
				}
			}
			if (matches > most_audio) {
				most_audio = matches;
				audio_clip_index = ii;
			}
		}

		// Add all the audios found in the above clip.
		for (ii = 0; ii < ti->clips[audio_clip_index].audio_stream_count;
				ii++) {
			BLURAY_STREAM_INFO * bdaudio;

			bdaudio = &ti->clips[audio_clip_index].audio_streams[ii];

			switch (bdaudio->coding_type) {
			case BLURAY_STREAM_TYPE_AUDIO_TRUHD:
				// Add 2 audio tracks.  One for TrueHD and one for AC-3
				add_audio(ii, title->list_audio, bdaudio,
				HB_SUBSTREAM_BD_AC3, HB_ACODEC_AC3, 0);
				add_audio(ii, title->list_audio, bdaudio,
				HB_SUBSTREAM_BD_TRUEHD, HB_ACODEC_FFMPEG, CODEC_ID_TRUEHD);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_DTS:
				add_audio(ii, title->list_audio, bdaudio, 0, HB_ACODEC_DCA, 0);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_MPEG2:
			case BLURAY_STREAM_TYPE_AUDIO_MPEG1:
				add_audio(ii, title->list_audio, bdaudio, 0,
				HB_ACODEC_FFMPEG, CODEC_ID_MP2);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_AC3PLUS:
				add_audio(ii, title->list_audio, bdaudio, 0,
				HB_ACODEC_FFMPEG, CODEC_ID_EAC3);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_LPCM:
				add_audio(ii, title->list_audio, bdaudio, 0,
				HB_ACODEC_FFMPEG, CODEC_ID_PCM_BLURAY);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_AC3:
				add_audio(ii, title->list_audio, bdaudio, 0, HB_ACODEC_AC3, 0);
				break;

			case BLURAY_STREAM_TYPE_AUDIO_DTSHD_MASTER:
			case BLURAY_STREAM_TYPE_AUDIO_DTSHD:
				// Add 2 audio tracks.  One for DTS-HD and one for DTS
				add_audio(ii, title->list_audio, bdaudio, HB_SUBSTREAM_BD_DTS,
				HB_ACODEC_DCA, 0);
				// DTS-HD is special.  The substreams must be concatinated
				// DTS-core followed by DTS-hd-extensions.  Setting
				// a substream id of 0 says use all substreams.
				add_audio(ii, title->list_audio, bdaudio, 0,
				HB_ACODEC_DCA_HD, CODEC_ID_DTS);
				break;

			default:
				hb_log_level(gloglevel,
						"scan: unknown audio pid 0x%x codec 0x%x", bdaudio->pid,
						bdaudio->coding_type)
				;
				break;
			}
		}
	}

    /* Chapters */
    for ( ii = 0; ii < ti->chapter_count; ii++ )
    {
    	int seconds;
        chapter = av_mallocz( sizeof( hb_chapter_t ) );

        chapter->index = ii + 1;
        chapter->duration = ti->chapters[ii].duration;
        chapter->block_start = ti->chapters[ii].offset;

        seconds            = ( chapter->duration + 45000 ) / 90000;
        chapter->hours     = seconds / 3600;
        chapter->minutes   = ( seconds % 3600 ) / 60;
        chapter->seconds   = seconds % 60;

        hb_log_level(gloglevel, "bd: chap %d packet=%"PRIu64", %"PRId64" ms",
                chapter->index,
                chapter->block_start,
                chapter->duration / 90 );

        hb_list_add( title->list_chapter, chapter );
    }
    hb_log_level(gloglevel, "bd: title %d has %d chapters", tt, ti->chapter_count );

    /* This title is ok so far */
    goto cleanup;

fail:
    hb_title_close( &title );

cleanup:

    return title;
}

/***********************************************************************
 * hb_bd_main_feature
 **********************************************************************/
int hb_bd_main_feature( hb_bd_t * d, hb_list_t * list_title )
{
    int longest = 0;
    int ii;
    uint64_t longest_duration = 0;
    int highest_rank = 0;
    int most_chapters = 0;
    int rank[8] = {0, 1, 3, 2, 6, 5, 7, 4};
    BLURAY_TITLE_INFO * ti;

    for ( ii = 0; ii < hb_list_count( list_title ); ii++ )
    {
        hb_title_t * title = hb_list_item( list_title, ii );
        ti = d->title_info[title->index - 1];
        if ( ti ) 
        {
            BLURAY_STREAM_INFO * bdvideo = &ti->clips[0].video_streams[0];
            if ( title->duration > longest_duration * 0.7 && bdvideo->format < 8 )
            {
                if (highest_rank < rank[bdvideo->format] ||
                    ( title->duration > longest_duration &&
                          highest_rank == rank[bdvideo->format]))
                {
                    longest = title->index;
                    longest_duration = title->duration;
                    highest_rank = rank[bdvideo->format];
                    most_chapters = ti->chapter_count;
                }
                else if (highest_rank == rank[bdvideo->format] &&
                         title->duration == longest_duration &&
                         ti->chapter_count > most_chapters)
                {
                    longest = title->index;
                    most_chapters = ti->chapter_count;
                }
            }
        }
        else if ( title->duration > longest_duration )
        {
            longest_duration = title->duration;
            longest = title->index;
        }
    }
    return longest;
}

/***********************************************************************
 * hb_bd_start
 ***********************************************************************
 * Title and chapter start at 1
 **********************************************************************/
int hb_bd_start( hb_bd_t * d, hb_title_t *title )
{
    BD_EVENT event;

    d->pkt_count = title->block_count;

    // Calling bd_get_event initializes libbluray event queue.
    bd_select_title( d->bd, d->title_info[title->index - 1]->idx );
    bd_get_event( d->bd, &event );
    d->chapter = 1;
//    d->stream = hb_bd_stream_open( title );
//    if ( d->stream == NULL )
//    {
//        return 0;
//    }
    return 1;
}

/***********************************************************************
 * hb_bd_stop
 ***********************************************************************
 *
 **********************************************************************/
void BDNOT_USED hb_bd_stop( hb_bd_t * d )
{
    //if( d->stream ) hb_stream_close( &d->stream );
}

/***********************************************************************
 * hb_bd_seek
 ***********************************************************************
 *
 **********************************************************************/
int BDNOT_USED hb_bd_seek( hb_bd_t * d, float f )
{
    uint64_t packet = f * d->pkt_count;

    bd_seek(d->bd, packet * 192);
    d->next_chap = bd_get_current_chapter( d->bd ) + 1;
    //hb_ts_stream_reset(d->stream);
    return 1;
}

int BDNOT_USED hb_bd_seek_pts( hb_bd_t * d, uint64_t pts )
{
    bd_seek_time(d->bd, pts);
    d->next_chap = bd_get_current_chapter( d->bd ) + 1;
    //hb_ts_stream_reset(d->stream);
    return 1;
}

int  BDNOT_USED hb_bd_seek_chapter( hb_bd_t * d, int c )
{
    d->next_chap = c;
    bd_seek_chapter( d->bd, c - 1 );
    //hb_ts_stream_reset(d->stream);
    return 1;
}

/***********************************************************************
 * hb_bd_read
 ***********************************************************************
 *
 **********************************************************************/
hb_buffer_t * hb_bd_read( hb_bd_t * d )
{
    int result;
    int error_count = 0;
    //uint8_t buf[192];
    BD_EVENT event;
    uint64_t pos;
    uint8_t discontinuity;
    int new_chap = 0;

    discontinuity = 0;
    while ( 1 )
    {
        if ( d->next_chap != d->chapter )
        {
            new_chap = d->chapter = d->next_chap;
        }
        result = next_packet( d->bd, d->read_buffer->data );
        if ( result < 0 )
        {
            hb_error("bd: Read Error");
            pos = bd_tell( d->bd );
            bd_seek( d->bd, pos + 192 );
            error_count++;
            if (error_count > 10)
            {
                hb_error("bd: Error, too many consecutive read errors");
                return 0;
            }
            continue;
        }
        else if ( result == 0 )
        {
            return 0;
        }

        error_count = 0;
        while ( bd_get_event( d->bd, &event ) )
        {
            switch ( event.event )
            {
                case BD_EVENT_CHAPTER:
                    // The muxers expect to only get chapter 2 and above
                    // They write chapter 1 when chapter 2 is detected.
                    d->next_chap = event.param;
                    break;

                case BD_EVENT_PLAYITEM:
                    discontinuity = 1;
                    hb_log_level(gloglevel, "bd: Playitem %u", event.param);
                    break;

                case BD_EVENT_STILL:
                    bd_read_skip_still( d->bd );
                    break;

                default:
                    break;
            }
        }
        // buf+4 to skip the BD timestamp at start of packet
        d->read_buffer->discontinuity = discontinuity;
        d->read_buffer->new_chap = new_chap;
        d->read_buffer->size = 192;
        return d->read_buffer;

//        b = hb_ts_decode_pkt( d->stream, buf+4 );
//        if ( b )
//        {
//            b->discontinuity = discontinuity;
//            b->new_chap = new_chap;
//            return b;
//        }
    }
    return NULL;
}

/***********************************************************************
 * hb_bd_chapter
 ***********************************************************************
 * Returns in which chapter the next block to be read is.
 * Chapter numbers start at 1.
 **********************************************************************/
int  BDNOT_USED hb_bd_chapter( hb_bd_t * d )
{
    return d->next_chap;
}

/***********************************************************************
 * hb_bd_close
 ***********************************************************************
 * Closes and frees everything
 **********************************************************************/
void hb_bd_close( hb_bd_t ** _d )
{
    hb_bd_t * d = *_d;
    int ii;

    if ( d->title_info )
    {
        for ( ii = 0; ii < d->title_count; ii++ )
            bd_free_title_info( d->title_info[ii] );
        av_free( d->title_info );
    }
    //if( d->stream ) hb_stream_close( &d->stream );
    if( d->bd ) bd_close( d->bd );
    if( d->path ) av_free( d->path );

    if(d->read_buffer) {
        av_free(d->read_buffer->data);
        av_free(d->read_buffer);
    }

    av_free( d );
    *_d = NULL;
}

/***********************************************************************
 * hb_bd_set_angle
 ***********************************************************************
 * Sets the angle to read
 **********************************************************************/
void  BDNOT_USED hb_bd_set_angle( hb_bd_t * d, int angle )
{

    if ( !bd_select_angle( d->bd, angle) )
    {
        hb_log_level(gloglevel,"bd_select_angle failed");
    }
}

static int check_ts_sync(const uint8_t *buf)
{
    // must have initial sync byte, no scrambling & a legal adaptation ctrl
    return (buf[0] == 0x47) && ((buf[3] >> 6) == 0) && ((buf[3] >> 4) > 0);
}

static int have_ts_sync(const uint8_t *buf, int psize)
{
    return check_ts_sync(&buf[0*psize]) && check_ts_sync(&buf[1*psize]) &&
           check_ts_sync(&buf[2*psize]) && check_ts_sync(&buf[3*psize]) &&
           check_ts_sync(&buf[4*psize]) && check_ts_sync(&buf[5*psize]) &&
           check_ts_sync(&buf[6*psize]) && check_ts_sync(&buf[7*psize]);
}

#define MAX_HOLE 192*80

static uint64_t align_to_next_packet(BLURAY *bd, uint8_t *pkt)
{
    uint8_t buf[MAX_HOLE];
    uint64_t pos = 0;
    uint64_t start = bd_tell(bd);
    uint64_t orig;
    uint64_t off = 192;

    memcpy(buf, pkt, 192);
    if ( start >= 192 ) {
        start -= 192;
    }
    orig = start;

    while (1)
    {
        if (bd_read(bd, buf+off, sizeof(buf)-off) == sizeof(buf)-off)
        {
            const uint8_t *bp = buf;
            int i;

            for ( i = sizeof(buf) - 8 * 192; --i >= 0; ++bp )
            {
                if ( have_ts_sync( bp, 192 ) )
                {
                    break;
                }
            }
            if ( i >= 0 )
            {
                pos = ( bp - buf );
                break;
            }
            off = 8 * 192;
            memcpy(buf, buf + sizeof(buf) - off, off);
            start += sizeof(buf) - off;
        }
        else
        {
            return 0;
        }
    }
    off = start + pos - 4;
    // bd_seek seeks to the nearest access unit *before* the requested position
    // we don't want to seek backwards, so we need to read until we get
    // past that position.
    bd_seek(bd, off);
    while (off > bd_tell(bd))
    {
        if (bd_read(bd, buf, 192) != 192)
        {
            break;
        }
    }
    return start - orig + pos;
}

static int next_packet( BLURAY *bd, uint8_t *pkt )
{
    int result;
    uint64_t pos,pos2;

    while ( 1 )
    {
        result = bd_read( bd, pkt, 192 );
        if ( result < 0 )
        {
            return -1;
        }
        if ( result < 192 )
        {
            return 0;
        }
        // Sync byte is byte 4.  0-3 are timestamp.
        if (pkt[4] == 0x47)
        {
            return 1;
        }
        // lost sync - back up to where we started then try to re-establish.
        pos = bd_tell(bd);
        pos2 = align_to_next_packet(bd, pkt);
        if ( pos2 == 0 )
        {
            hb_log_level(gloglevel, "next_packet: eof while re-establishing sync @ %"PRId64, pos );
            return 0;
        }
        hb_log_level(gloglevel, "next_packet: sync lost @ %"PRId64", regained after %"PRId64" bytes",
                 pos, pos2 );
    }
}

static int title_info_compare_mpls(const void *va, const void *vb)
{
    BLURAY_TITLE_INFO *a, *b;

    a = *(BLURAY_TITLE_INFO**)va;
    b = *(BLURAY_TITLE_INFO**)vb;

    return a->playlist - b->playlist;
}


static int64_t hb_bd_cur_title_size( hb_bd_t *e ) {
	int64_t s = bd_get_title_size(e->bd);
    hb_log_level(gloglevel, "hb_bd_cur_title_size: %"PRId64,s);
    return s;
}

static int64_t hb_bd_seek_bytes( hb_bd_t *e, int64_t off, int mode ) {
	int64_t r = bd_seek(e->bd, off);
    hb_log_level(gloglevel, "hb_bd_seek_bytes: off %"PRId64"  ret %"PRId64, off, r);
    return (int64_t)r;
}

/* optmedia exports */
static om_handle_t    * __hb_bd_init( char * path ) { return (om_handle_t *) hb_bd_init(path); }
static void     __hb_bd_close( om_handle_t  ** _d ) { hb_bd_close((hb_bd_t **)_d); }
static int           __hb_bd_title_count( om_handle_t *d ) { return hb_bd_title_count((hb_bd_t *)d); }
static hb_title_t  * __hb_bd_title_scan( om_handle_t * d, int t, uint64_t min_duration ) { return hb_bd_title_scan((hb_bd_t *)d,t,min_duration); }
static int           __hb_bd_main_feature( om_handle_t * d, hb_list_t * list_title ) { return hb_bd_main_feature((hb_bd_t *)d,list_title);}

static hb_optmedia_func_t bd_methods = {
		__hb_bd_init,
		__hb_bd_close,
		__hb_bd_title_count,
		__hb_bd_title_scan,
		__hb_bd_main_feature
} ;

hb_optmedia_func_t *hb_optmedia_bd_methods(void) {
	return &bd_methods;
}


/* libavformat glue */
static const AVOption options[] = {
    { "wide_support", "enable wide support", offsetof(bdurl_t, wide_support), FF_OPT_TYPE_INT, {1}, -1, 1, AV_OPT_FLAG_DECODING_PARAM},
    { "min_title_duration", "minimum duration in ms to select a BD title", offsetof(bdurl_t, min_title_duration), FF_OPT_TYPE_INT, {0}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM},
    { NULL }
};

static const AVClass bdurl_class = {
    "BDURL protocol",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

#define bdurl_max(a,b) ((a)>(b)?(a):(b))
#define bdurl_min(a,b) ((a)<(b)?(a):(b))



static hb_buffer_t* bd_fragread(void *ctx) {
    hb_bd_t *bd_ctx = ctx;
    return hb_bd_read( bd_ctx );
}

static int bdurl_read(URLContext *h, unsigned char *buf, int size){
    bdurl_t *ctx = (bdurl_t *)h->priv_data;
    return fragmented_read(ctx->hb_bd, bd_fragread, &ctx->cur_read_buffer, buf, size);
}

static int  BDNOT_USED bdurl_readnew(URLContext *h, unsigned char *buf, int size)
{
    bdurl_t *ctx = (bdurl_t *)h->priv_data;

    unsigned char *bufptr = buf;
    unsigned char *bufend = buf + size;

    while( bufptr < bufend ) {
        /* if there is still a buffer we were reading */
        if( ctx->cur_read_buffer ) {
            int left_dstbytes = bufend - bufptr;
            int left_srcbytes = ctx->cur_read_buffer->size - ctx->cur_read_buffer->cur;

            int readmax = bdurl_min( left_srcbytes, left_dstbytes );

            memcpy(bufptr, ctx->cur_read_buffer->data + ctx->cur_read_buffer->cur, readmax );

            bufptr += readmax;
            ctx->cur_read_buffer->cur += readmax;

            if( ctx->cur_read_buffer->cur == ctx->cur_read_buffer->size ) {
                ctx->cur_read_buffer = 0;
            }
        } else {
            /* reading fresh data from bdread. this must return a buffer if succesful */
            ctx->cur_read_buffer = hb_bd_read( ctx->hb_bd );
            if(!ctx->cur_read_buffer) {
                hb_log_level(gloglevel,"bd_read: EOF");
                break;
            }
            ctx->cur_read_buffer->cur = 0;
        }
    }
    return bufptr - buf;
}

static int bdurl_write(URLContext *h, const unsigned char *buf, int size)
{
    return 0;
}

static int bdurl_get_handle(URLContext *h)
{
    hb_log_level(gloglevel,"bd_get_handle");
    return (intptr_t) h->priv_data;
}

static int bdurl_check(URLContext *h, int mask)
{
    int ret = mask&AVIO_FLAG_READ;
    hb_error("bd_check: mask %d",  mask);
    return ret;
}


static int bdurl_open(URLContext *h, const char *filename, int flags)
{
    const char *bdpath;
    int i,title_count;
    bdurl_t *ctx;
    int64_t min_title_duration = 0*90000;
    int urltitle = 0;
    int loglevel =  gloglevel;
    hb_title_t *t;


    //ctx = av_malloc( sizeof(bdurl_t) );
    ctx = h->priv_data;
    ctx->class = &bdurl_class;
    ctx->list_title = hb_list_init();
    ctx->selected_chapter = 1;
    ctx->min_title_duration = 15000;
    min_title_duration = (((uint64_t)ctx->min_title_duration)*90000L)/1000L;

    url_parse("bd",filename, &bdpath, &urltitle );


    ctx->hb_bd = hb_bd_init((char *)bdpath);
    if(!ctx->hb_bd) {
        hb_log_level(loglevel, "bd_open: couldn't initialize bdread");
        return -1;
    }

    title_count = hb_bd_title_count(ctx->hb_bd);
    if( urltitle>0 && urltitle<=title_count ) {
        hb_log_level(loglevel,"bd_open: opening title %d ", urltitle);
        t= hb_bd_title_scan(ctx->hb_bd, urltitle, min_title_duration );
        if(t) {
            ctx->selected_title = t;
        } else {
            return -1;
        }
    } else {
    	int selected_title_idx;
        hb_log_level(loglevel,"bd_open: bd image has %d titles", title_count);
        for (i = 0; i < title_count; i++) {
            t = hb_bd_title_scan(ctx->hb_bd, i + 1, min_title_duration);
            if (t) {
                ctx->selected_title = t;
                hb_list_add(ctx->list_title, t);
            }
        }

        selected_title_idx = hb_bd_main_feature(ctx->hb_bd, ctx->list_title);
        for (i = 0; i < hb_list_count(ctx->list_title); i++) {
        	if( ((hb_title_t *)hb_list_item(ctx->list_title, i))->index == selected_title_idx ) {
        		ctx->selected_title = hb_list_item(ctx->list_title,i);
        		break;
        	}
        }
    }

    if( title_count<=0 || !ctx->selected_title ) {
        hb_error("bd_open: no titles found");
        return -1;
    }

    hb_log_level(loglevel,"bd_open: selected title %d", ctx->selected_title->index );

    if( hb_bd_start(ctx->hb_bd, ctx->selected_title ) == 0 ) {
        hb_error("bd_open: couldn't start reading title");
        return -1;
    }



    h->priv_data = (void *)ctx;
    return 0;
}

static int64_t bdurl_seek(URLContext *h, int64_t pos, int whence)
{
    bdurl_t *ctx = h->priv_data;

    if (whence == AVSEEK_SIZE) {
        return hb_bd_cur_title_size(ctx->hb_bd);
    }
    return hb_bd_seek_bytes( ctx->hb_bd, pos, whence );
}

static int bdurl_close(URLContext *h)
{
    hb_log_level(gloglevel,"bd_close: closing");
    if( h->priv_data ) {
        bdurl_t *ctx = h->priv_data;
        if( ctx->list_title ) {
            hb_list_close( &ctx->list_title );
        }
        hb_bd_close(&ctx->hb_bd);

        //av_free(ctx);
        //h->priv_data =0;
    }
    hb_log_level(gloglevel,"bd_close: closed");
    return 0;
}



URLProtocol ff_bd_protocol = {
    .name                = "bd",
    .url_open            = bdurl_open,
    .url_read            = bdurl_read,
    .url_write           = bdurl_write,
    .url_seek            = bdurl_seek,
    .url_close           = bdurl_close,
    .url_get_file_handle = bdurl_get_handle,
    .url_check           = bdurl_check,
    .priv_data_class	= &bdurl_class,
    .priv_data_size 	= sizeof(bdurl_t)
};


