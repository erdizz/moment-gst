/*  Moment-Gst - GStreamer support module for Moment Video Server
    Copyright (C) 2011 Dmitry Shatrov

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include <moment-gst/gst_stream.h>


using namespace M;
using namespace Moment;

namespace MomentGst {

namespace {
LogGroup libMary_logGroup_bus ("moment-gst_bus", LogLevel::D);
}

mt_mutex (ctl_mutex) void
GstStream::StreamControl::reportMetaData ()
{
    logD_ (_func_);

    if (metadata_reported)
	return;
    metadata_reported = true;

    if (!send_metadata)
	return;

    VideoStream::VideoMessage msg;
    if (!RtmpServer::encodeMetaData (&metadata, page_pool, &msg)) {
	logE_ (_func, "encodeMetaData() failed");
	return;
    }

    logD_ (_func, "Firing video message");
    video_stream->fireVideoMessage (&msg);

    page_pool->msgUnref (msg.page_list.first);
}

void
GstStream::StreamControl::doAudioData (GstBuffer * const buffer)
{
//    logD_ (_func, "stream 0x", fmt_hex, (UintPtr) stream, ", "
//	   "timestamp 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer));

    // TODO Update current time efficiently.
    updateTime ();

    ctl_mutex.lock ();

    last_frame_time = getTime ();
//    logD_ (_func, "last_frame_time: 0x", fmt_hex, last_frame_time);

    if (prv_audio_timestamp >= GST_BUFFER_TIMESTAMP (buffer)) {
	logD_ (_func, "backwards timestamp: prv 0x", fmt_hex, prv_audio_timestamp,
	       ", cur 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer)); 
    }
    prv_audio_timestamp = GST_BUFFER_TIMESTAMP (buffer);

    GstBuffer *aac_codec_data_buffer = NULL;
    if (first_audio_frame) {
	GstCaps * const caps = gst_buffer_get_caps (buffer);
	{
	    gchar * const str = gst_caps_to_string (caps);
	    logD_ (_func, "caps: ", str);
	    g_free (str);
	}

	GstStructure * const structure = gst_caps_get_structure (caps, 0);
	gchar const * structure_name = gst_structure_get_name (structure);
	logD_ (_func, "structure name: ", gst_structure_get_name (structure));

	ConstMemory const structure_name_mem (structure_name, strlen (structure_name));

	gint channels;
	gint rate;
	if (!gst_structure_get_int (structure, "channels", &channels))
	    channels = 1;
	if (!gst_structure_get_int (structure, "rate", &rate))
	    rate = 44100;

	if (equal (structure_name_mem, "audio/mpeg")) {
	    gint mpegversion;
	    gint layer;

	    if (!gst_structure_get_int (structure, "mpegversion", &mpegversion))
		mpegversion = 1;
	    if (!gst_structure_get_int (structure, "layer", &layer))
		layer = 3;

	    if (mpegversion == 1 && layer == 3) {
	      // MP3
		audio_codec_id = VideoStream::AudioCodecId::MP3;
		audio_hdr = 0x22; // MP3, _ kHz, 16-bit samples, mono

		switch (rate) {
		    case 8000:
			audio_hdr &= 0x0f;
			audio_hdr |= 0xe4; // MP3 8 kHz, 11 kHz
			break;
		    case 11025:
			audio_hdr |= 0x4; // 11 kHz
			break;
		    case 22050:
			audio_hdr |= 0x8; // 22 kHz
			break;
		    case 44100:
			audio_hdr |= 0xc; // 44 kHz
			break;
		    default:
			logW_ (_func, "Unsupported bitrate: ", rate);
			audio_hdr |= 0xc; // 44 kHz
		}
	    } else {
	      // AAC
		audio_codec_id = VideoStream::AudioCodecId::AAC;
		// TODO Correct header based on caps.
	        audio_hdr = 0xae; // AAC, 44 kHz, 16-bit samples, mono

		do {
		  // Processing AacSequenceHeader.

		    GValue const * const val = gst_structure_get_value (structure, "codec_data");
		    if (!val) {
			logW_ (_func, "Codec data not found");
			break;
		    }

		    if (!GST_VALUE_HOLDS_BUFFER (val)) {
			logW_ (_func, "codec_data doesn't hold a buffer");
			break;
		    }

		    aac_codec_data_buffer = gst_value_get_buffer (val);
		    logD_ (_func, "aac_codec_data_buffer: 0x", fmt_hex, (UintPtr) aac_codec_data_buffer);
		} while (0);
	    }
	} else
	if (equal (structure_name_mem, "audio/x-speex")) {
	  // Speex
	    audio_codec_id = VideoStream::AudioCodecId::Speex;
	    audio_hdr = 0xb6; // Speex, 11 kHz, 16-bit samples, mono
	} else
	if (equal (structure_name_mem, "audio/x-nellymoser")) {
	  // Nellymoser
	    audio_codec_id = VideoStream::AudioCodecId::Nellymoser;
	    audio_hdr = 0x6e; // Nellymoser, 44 kHz, 16-bit samples, mono
	} else
	if (equal (structure_name_mem, "audio/x-adpcm")) {
	  // ADPCM
	    audio_codec_id = VideoStream::AudioCodecId::ADPCM;
	    audio_hdr = 0x1e; // ADPCM, 44 kHz, 16-bit samples, mono
	} else
	if (equal (structure_name_mem, "audio/x-raw-int")) {
	  // Linear PCM, little endian
	    audio_codec_id = VideoStream::AudioCodecId::LinearPcmLittleEndian;
	    audio_hdr = 0x3e; // Linear PCM (little endian), 44 kHz, 16-bit samples, mono
	} else
	if (equal (structure_name_mem, "audio/x-alaw")) {
	  // G.711 A-law logarithmic PCM
	    audio_codec_id = VideoStream::AudioCodecId::G711ALaw;
	    audio_hdr = 0x7e; // G.711 A-law, 44 kHz, 16-bit samples, mono
	} else
	if (equal (structure_name_mem, "audio/x-mulaw")) {
	  // G.711 mu-law logarithmic PCM
	    audio_codec_id = VideoStream::AudioCodecId::G711MuLaw;
	    audio_hdr = 0x8e; // G.711 mu-law, 44 kHz, 16-bit samples, mono
	}

	if (channels > 1) {
	    logD_ (_func, "stereo");
	    audio_hdr |= 1; // stereo
	}

	metadata.audio_sample_rate = (Uint32) rate;
	metadata.got_flags |= RtmpServer::MetaData::AudioSampleRate;

	metadata.audio_sample_size = 16;
	metadata.got_flags |= RtmpServer::MetaData::AudioSampleSize;

	metadata.num_channels = (Uint32) channels;
	metadata.got_flags |= RtmpServer::MetaData::NumChannels;

	gst_caps_unref (caps);
    }

    if (first_audio_frame) {
	first_audio_frame = false;

	if (!got_video || !first_video_frame) {
	  // There's no video or we've got the first video frame already.
	    reportMetaData ();
	    metadata_reported_cond.signal ();
	} else {
	  // Waiting for the first video frame.
	    while (got_video && first_video_frame)
		metadata_reported_cond.wait (ctl_mutex);
	}
    }

    if (audio_codec_id == VideoStream::AudioCodecId::Speex) {
	// The first two buffers for Speex are headers. They do appear to contain
	// audio data and their timestamps look random (very large).
	if (audio_skip_counter > 0) {
	    --audio_skip_counter;
	    ctl_mutex.unlock ();
	    logD_ (_func, "skipping initial audio frame, ", audio_skip_counter, " left");
	    return;
	}

	if (GST_BUFFER_TIMESTAMP (buffer) == (GstClockTime) -1) {
	    ctl_mutex.unlock ();
	    logD_ (_func, "\"-1\" timestamp, skipping frame");
	    return;
	}
    }
    Ref<VideoStream> const tmp_video_stream = video_stream;
    VideoStream::AudioCodecId const tmp_audio_codec_id = audio_codec_id;
    Byte const tmp_audio_hdr = audio_hdr;
//    logD_ (_func, "audio_hdr: ", fmt_hex, tmp_audio_hdr);

    ctl_mutex.unlock ();

    if (!tmp_video_stream)
	return;

    if (tmp_audio_codec_id == VideoStream::AudioCodecId::Unknown) {
	logD_ (_func, "unknown codec id, dropping audio frame");
	return;
    }

    if (aac_codec_data_buffer) {
      // Reporting AAC codec data if needed.

	Size msg_len = 0;

	PagePool::PageListHead page_list;
	RtmpConnection::PrechunkContext prechunk_ctx;

	Uint64 const timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (aac_codec_data_buffer) / 1000000);
	Byte aac_audio_hdr [2] = { tmp_audio_hdr, 0 };

	RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					     ConstMemory::forObject (aac_audio_hdr),
					     page_pool,
					     &page_list,
					     RtmpConnection::DefaultAudioChunkStreamId,
					     timestamp,
					     true /* first_chunk */);
	msg_len += sizeof (aac_audio_hdr);

	logD_ (_func, "AAC SEQUENCE HEADER");
	hexdump (logs, ConstMemory (GST_BUFFER_DATA (aac_codec_data_buffer), GST_BUFFER_SIZE (aac_codec_data_buffer)));

	RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					     ConstMemory (GST_BUFFER_DATA (aac_codec_data_buffer),
							  GST_BUFFER_SIZE (aac_codec_data_buffer)),
					     page_pool,
					     &page_list,
					     RtmpConnection::DefaultAudioChunkStreamId,
					     timestamp,
					     false /* first_chunk */);
	msg_len += GST_BUFFER_SIZE (aac_codec_data_buffer);

	VideoStream::AudioMessage msg;
	msg.timestamp = timestamp;
	msg.prechunk_size = RtmpConnection::PrechunkSize;
	msg.frame_type = VideoStream::AudioFrameType::AacSequenceHeader;
	msg.codec_id = tmp_audio_codec_id;

	msg.page_pool = page_pool;
	msg.page_list = page_list;
	msg.msg_len = msg_len;
	msg.msg_offset = 0;

	tmp_video_stream->fireAudioMessage (&msg);

	page_pool->msgUnref (page_list.first);
    }

    Size msg_len = 0;

    Uint64 const timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (buffer) / 1000000);
//    logD_ (_func, "timestamp: 0x", fmt_hex, timestamp, ", size: ", fmt_def, GST_BUFFER_SIZE (buffer));
//    logD_ (_func, "tmp_audio_codec_id: ", tmp_audio_codec_id);

    Byte gen_audio_hdr [2];
    Size gen_audio_hdr_len = 1;
    gen_audio_hdr [0] = tmp_audio_hdr;
    if (tmp_audio_codec_id == VideoStream::AudioCodecId::AAC) {
	gen_audio_hdr [1] = 1;
	gen_audio_hdr_len = 2;
    }

    PagePool::PageListHead page_list;
    RtmpConnection::PrechunkContext prechunk_ctx;

    // Non-prechunked variant
    // page_pool->getFillPages (&page_list, ConstMemory (gen_audio_hdr, gen_audio_hdr_len));
    RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					 ConstMemory (gen_audio_hdr, gen_audio_hdr_len),
					 page_pool,
					 &page_list,
					 RtmpConnection::DefaultAudioChunkStreamId,
					 timestamp,
					 true /* first_chunk */);
    msg_len += gen_audio_hdr_len;

    // Non-prechunked variant
    // page_pool->getFillPages (&page_list, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));
    RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					 ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)),
					 page_pool,
					 &page_list,
					 RtmpConnection::DefaultAudioChunkStreamId,
					 timestamp,
					 false /* first_chunk */);
    msg_len += GST_BUFFER_SIZE (buffer);

//    hexdump (errs, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));

    VideoStream::AudioMessage msg;
    msg.timestamp = timestamp;
    msg.prechunk_size = RtmpConnection::PrechunkSize;
    msg.frame_type = VideoStream::AudioFrameType::RawData;
    msg.codec_id = tmp_audio_codec_id;

    msg.page_pool = page_pool;
    msg.page_list = page_list;
    msg.msg_len = msg_len;
    msg.msg_offset = 0;

//    logD_ (_func, fmt_hex, msg.timestamp);

    tmp_video_stream->fireAudioMessage (&msg);

    page_pool->msgUnref (page_list.first);
}

gboolean
GstStream::StreamControl::audioDataCb (GstPad    * const /* pad */,
				       GstBuffer * const buffer,
				       gpointer    const _ctl)
{
    StreamControl * const ctl = static_cast <StreamControl*> (_ctl);
    ctl->doAudioData (buffer);
    return TRUE;
}

void
GstStream::StreamControl::handoffAudioDataCb (GstElement * const /* element */,
					      GstBuffer  * const buffer,
					      GstPad     * const /* pad */,
					      gpointer     const _ctl)
{
    StreamControl * const ctl = static_cast <StreamControl*> (_ctl);
    ctl->doAudioData (buffer);
}

void
GstStream::StreamControl::doVideoData (GstBuffer * const buffer)
{
//    logD_ (_func, "stream 0x", fmt_hex, (UintPtr) this, ", "
//	   "timestamp 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer));

    // TODO Update current time efficiently.
    updateTime ();

    ctl_mutex.lock ();

    last_frame_time = getTime ();
//    logD_ (_func, "last_frame_time: 0x", fmt_hex, last_frame_time);

    GstBuffer *avc_codec_data_buffer = NULL;
    if (first_video_frame) {
	first_video_frame = false;

	GstCaps * const caps = gst_buffer_get_caps (buffer);
	{
	    gchar * const str = gst_caps_to_string (caps);
	    logD_ (_func, "caps: ", str);
	    g_free (str);
	}

	GstStructure * const st = gst_caps_get_structure (caps, 0);
	gchar const * st_name = gst_structure_get_name (st);
	logD_ (_func, "st_name: ", gst_structure_get_name (st));
	ConstMemory const st_name_mem (st_name, strlen (st_name));

	if (equal (st_name_mem, "video/x-flash-video")) {
	   video_codec_id = VideoStream::VideoCodecId::SorensonH263;
	   video_hdr = 0x02; // Sorenson H.263
	} else
	if (equal (st_name_mem, "video/x-h264")) {
	   video_codec_id = VideoStream::VideoCodecId::AVC;
	   video_hdr = 0x07; // AVC

	   do {
	     // Processing AvcSeqienceHeader

	       GValue const * const val = gst_structure_get_value (st, "codec_data");
	       if (!val) {
		   logW_ (_func, "Codec data not found");
		   break;
	       }

	       if (!GST_VALUE_HOLDS_BUFFER (val)) {
		   logW_ (_func, "codec_data doesn't hold a buffer");
		   break;
	       }

	       avc_codec_data_buffer = gst_value_get_buffer (val);
	       logD_ (_func, "avc_codec_data_buffer: 0x", fmt_hex, (UintPtr) avc_codec_data_buffer);
	   } while (0);
	} else
	if (equal (st_name_mem, "video/x-vp6")) {
	   video_codec_id = VideoStream::VideoCodecId::VP6;
	   video_hdr = 0x04; // On2 VP6
	} else
	if (equal (st_name_mem, "video/x-flash-screen")) {
	   video_codec_id = VideoStream::VideoCodecId::ScreenVideo;
	   video_hdr = 0x03; // Screen video
	}

	if (!got_audio || !first_audio_frame) {
	  // There's no video or we've got the first video frame already.
	    reportMetaData ();
	    metadata_reported_cond.signal ();
	} else {
	  // Waiting for the first audio frame.
	    while (got_audio && first_audio_frame)
		metadata_reported_cond.wait (ctl_mutex);
	}
    }

    Ref<VideoStream> const tmp_video_stream = video_stream;
    VideoStream::VideoCodecId const tmp_video_codec_id = video_codec_id;
    Byte const tmp_video_hdr = video_hdr;

    ctl_mutex.unlock ();

    if (!tmp_video_stream)
	return;

    if (avc_codec_data_buffer) {
      // Reporting AVC codec data if needed.

	Size msg_len = 0;

	PagePool::PageListHead page_list;
	RtmpConnection::PrechunkContext prechunk_ctx;

	Uint64 const timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (avc_codec_data_buffer) / 1000000);
	Byte avc_video_hdr [5] = { 0x17, 0, 0, 0, 0 }; // AVC, seekable frame;
						       // AVC sequence header;
						       // Composition time offset = 0.

	RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					     ConstMemory::forObject (avc_video_hdr),
					     page_pool,
					     &page_list,
					     RtmpConnection::DefaultVideoChunkStreamId,
					     timestamp,
					     true /* first_chunk */);
	msg_len += sizeof (avc_video_hdr);

	logD_ (_func, "AVC SEQUENCE HEADER");
	hexdump (logs, ConstMemory (GST_BUFFER_DATA (avc_codec_data_buffer), GST_BUFFER_SIZE (avc_codec_data_buffer)));

	RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					     ConstMemory (GST_BUFFER_DATA (avc_codec_data_buffer),
							  GST_BUFFER_SIZE (avc_codec_data_buffer)),
					     page_pool,
					     &page_list,
					     RtmpConnection::DefaultVideoChunkStreamId,
					     timestamp,
					     false /* first_chunk */);
	msg_len += GST_BUFFER_SIZE (avc_codec_data_buffer);

	VideoStream::VideoMessage msg;
	msg.timestamp = timestamp;
	msg.prechunk_size = RtmpConnection::PrechunkSize;
	msg.frame_type = VideoStream::VideoFrameType::AvcSequenceHeader;
	msg.codec_id = tmp_video_codec_id;

	msg.page_pool = page_pool;
	msg.page_list = page_list;
	msg.msg_len = msg_len;
	msg.msg_offset = 0;

	tmp_video_stream->fireVideoMessage (&msg);

	page_pool->msgUnref (page_list.first);
    }

    Size msg_len = 0;

    Uint64 const timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (buffer) / 1000000);
//    logD_ (_func, "timestamp: 0x", fmt_hex, timestamp);

    VideoStream::VideoMessage msg;
    msg.frame_type = VideoStream::VideoFrameType::InterFrame;
    msg.codec_id = tmp_video_codec_id;

    Byte gen_video_hdr [5];
    Size gen_video_hdr_len = 1;
    gen_video_hdr [0] = tmp_video_hdr;
    if (tmp_video_codec_id == VideoStream::VideoCodecId::AVC) {
	gen_video_hdr [1] = 1; // AVC NALU

	// Composition time offset
	gen_video_hdr [2] = 0;
	gen_video_hdr [3] = 0;
	gen_video_hdr [4] = 0;

	gen_video_hdr_len = 5;
    }

    bool is_keyframe = false;
#if 0
    // Keyframe detection by parsing message body for Sorenson H.263
    // See ffmpeg:h263.c for reference.
    if (tmp_video_codec_id == VideoStream::VideoCodecId::SorensonH263) {
	if (GST_BUFFER_SIZE (buffer) >= 5) {
	    Byte const format = ((GST_BUFFER_DATA (buffer) [3] & 0x03) << 1) |
				((GST_BUFFER_DATA (buffer) [4] & 0x80) >> 7);
	    size_t offset = 4;
	    switch (format) {
		case 0:
		    offset += 2;
		    break;
		case 1:
		    offset += 4;
		    break;
		default:
		    break;
	    }

	    if (GST_BUFFER_SIZE (buffer) > offset) {
		if (((GST_BUFFER_DATA (buffer) [offset] & 0x60) >> 4) == 0)
		    is_keyframe = true;
	    }
	}
    } else
#endif
    {
	is_keyframe = !GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    if (is_keyframe) {
	msg.frame_type = VideoStream::VideoFrameType::KeyFrame;
	gen_video_hdr [0] |= 0x10;
    } else {
      // TODO We do not make difference between inter frames and
      // disposable inter frames for Sorenson h.263 here.
	gen_video_hdr [0] |= 0x20;
    }

    PagePool::PageListHead page_list;
    RtmpConnection::PrechunkContext prechunk_ctx;
    // Non-prechunked variant
    // page_pool->getFillPages (&page_list, ConstMemory::forObject (video_hdr));
    RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					 ConstMemory (gen_video_hdr, gen_video_hdr_len),
					 page_pool,
					 &page_list,
					 RtmpConnection::DefaultVideoChunkStreamId,
					 timestamp,
					 true /* first_chunk */);
    msg_len += gen_video_hdr_len;

    // Non-prechunked variant
    // page_pool->getFillPages (&page_list, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));
    RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
					 ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)),
					 page_pool,
					 &page_list,
					 RtmpConnection::DefaultVideoChunkStreamId,
					 timestamp,
					 false /* first_chunk */);
    msg_len += GST_BUFFER_SIZE (buffer);

//    hexdump (errs, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));

    msg.timestamp = timestamp;
    msg.prechunk_size = RtmpConnection::PrechunkSize;

    msg.page_pool = page_pool;
    msg.page_list = page_list;
    msg.msg_len = msg_len;
    msg.msg_offset = 0;

    tmp_video_stream->fireVideoMessage (&msg);

    page_pool->msgUnref (page_list.first);
}

gboolean
GstStream::StreamControl::videoDataCb (GstPad    * const /* pad */,
				       GstBuffer * const buffer,
				       gpointer    const _ctl)
{
    StreamControl * const ctl = static_cast <StreamControl*> (_ctl);
    ctl->doVideoData (buffer);
    return TRUE;
}

void
GstStream::StreamControl::handoffVideoDataCb (GstElement * const /* element */,
					      GstBuffer  * const buffer,
					      GstPad     * const /* pad */,
					      gpointer     const _ctl)
{
    StreamControl * const ctl = static_cast <StreamControl*> (_ctl);
    ctl->doVideoData (buffer);
}

#if 0
// Unused
gboolean
GstStream::busCallCb (GstBus     * const /* bus */,
		      GstMessage * const msg,
		      gpointer     const _self)
{
    logD (bus, _func, gst_message_type_get_name (GST_MESSAGE_TYPE (msg)));

    GstStream * const self = static_cast <GstStream*> (_self);

    self->mutex.lock ();

    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (self->playbin)) {
	logD (bus, _func, "PIPELINE MESSAGE");
	logD (bus, _func, gst_message_type_get_name (GST_MESSAGE_TYPE (msg)));

	if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_STATE_CHANGED) {
	    GstState new_state,
		     pending_state;
	    gst_message_parse_state_changed (msg, NULL, &new_state, &pending_state);
	    if (pending_state == GST_STATE_VOID_PENDING
		&& new_state != GST_STATE_PLAYING)
	    {
		logD (bus, _func, "PIPELINE READY");
		gst_element_set_state (self->playbin, GST_STATE_PLAYING);
	    }
	}
    }

    switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_BUFFERING: {
	    gint percent = 0;
	    gst_message_parse_buffering (msg, &percent);
	    logD (bus, _func, "GST_MESSAGE_BUFFERING ", percent, "%");
	} break;
	case GST_MESSAGE_EOS: {
	    logE (bus, _func, "GST_MESSAGE_EOS");

	  // The stream will be restarted gracefully by noVideoTimerTick().
	  // We do not restart the stream here immediately to avoid triggering
	  // a fork bomb (e.g. when network is down).

#if 0
	    restartStream ();
#endif
	} break;
	case GST_MESSAGE_ERROR: {
	    logE (bus, _func, "GST_MESSAGE_ERROR");

#if 0
	    restartStream ();
#endif
	} break;
	default:
	  // No-op
	    ;
    }

    self->mutex.unlock ();

    return TRUE;
}
#endif

GstBusSyncReply
GstStream::StreamControl::busSyncHandler (GstBus     * const /* bus */,
					  GstMessage * const msg,
					  gpointer     const _ctl)
{
    logD (bus, _func, gst_message_type_get_name (GST_MESSAGE_TYPE (msg)), ", src: 0x", fmt_hex, (UintPtr) GST_MESSAGE_SRC (msg));

    StreamControl * const ctl = static_cast <StreamControl*> (_ctl);

    ctl->ctl_mutex.lock ();
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (ctl->playbin)) {
	logD (bus, _func, "PIPELINE MESSAGE: ", gst_message_type_get_name (GST_MESSAGE_TYPE (msg)));

	switch (GST_MESSAGE_TYPE (msg)) {
	    case GST_MESSAGE_STATE_CHANGED: {
		GstState new_state,
			 pending_state;
		gst_message_parse_state_changed (msg, NULL, &new_state, &pending_state);
		if (pending_state == GST_STATE_VOID_PENDING) {
		    if (new_state == GST_STATE_PAUSED) {
			logD (bus, _func, "PAUSED");
		    } else
		    if (new_state == GST_STATE_PLAYING) {
			logD (bus, _func, "PLAYING");

			bool do_seek = false;
			Time initial_seek = ctl->initial_seek;
			if (ctl->initial_seek_pending) {
			    ctl->initial_seek_pending = false;
			    do_seek = true;
			}
			ctl->ctl_mutex.unlock ();

			if (do_seek && initial_seek > 0) {
			    if (!gst_element_seek_simple (ctl->playbin,
							  GST_FORMAT_TIME,
							  (GstSeekFlags) (GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
							  (GstClockTime) initial_seek * 1000000000LL))
			    {
				logE_ (_func, "Seek failed");
			    }
			}

			goto _return;
		    }
		}
	    } break;
	    case GST_MESSAGE_EOS: {
		logD_ (_func, "EOS");

		ctl->eos_pending = true;
		ctl->ctl_mutex.unlock ();

		ctl->gst_stream->deferred_reg.scheduleTask (&ctl->gst_stream->deferred_task, false /* permanent */);
		goto _return;
	    } break;
	    case GST_MESSAGE_ERROR: {
		logD_ (_func, "ERROR");

		ctl->error_pending = true;
		ctl->ctl_mutex.unlock ();

		ctl->gst_stream->deferred_reg.scheduleTask (&ctl->gst_stream->deferred_task, false /* permanent */);
		goto _return;
	    } break;
	    default:
	      // No-op
		;
	}

	logD (bus, _func, "MSG DONE");
    }
    ctl->ctl_mutex.unlock ();

_return:
    return GST_BUS_PASS;
}

void
GstStream::StreamControl::init (GstStream   * const gst_stream,
				PagePool    * const page_pool,
				VideoStream * const video_stream,
				bool          const send_metadata)
{
    this->gst_stream = gst_stream;
    this->page_pool = page_pool;
    this->video_stream = video_stream;
    this->send_metadata = send_metadata;

  // TODO Move the following to StreamControl() constructor.
    audio_codec_id = VideoStream::AudioCodecId::Unknown;
    first_audio_frame = true;
    // The first two buffers for Speex are headers. They do appear to contain
    // audio data and their timestamps look random (very large).
    audio_skip_counter = 2;

    video_codec_id = VideoStream::VideoCodecId::Unknown;
    first_video_frame = true;
}

GstStream::StreamControl::StreamControl ()
    : gst_stream (NULL),
      page_pool (NULL),
      video_stream (NULL),

      send_metadata (false),

      playbin (NULL),

      initial_seek (0),
      initial_seek_pending (false),

      metadata_reported (false),

      last_frame_time (0),

      audio_codec_id (VideoStream::AudioCodecId::Unknown),
      audio_hdr (0xbe /* Speex */),

      video_codec_id (VideoStream::VideoCodecId::Unknown),
      video_hdr (0x02 /* Sorenson H.263 */),

      got_video (false),
      got_audio (false),

      first_audio_frame (true),
      audio_skip_counter (0),

      first_video_frame (true),

      prv_audio_timestamp (0),

      error_pending (false),
      eos_pending (false)
{
}

bool
GstStream::deferredTask (void * const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    logD_ (_func, "this: 0x", fmt_hex, (UintPtr) self);

    self->mutex.lock ();

    if (self->stream_ctl) {
	bool fire_eos = false;
	bool fire_error = false;
	self->stream_ctl->ctl_mutex.lock ();
	if (self->stream_ctl->eos_pending) {
	    self->stream_ctl->eos_pending = false;
	    fire_eos = true;
	} else
	if (self->stream_ctl->error_pending) {
	    self->stream_ctl->error_pending = false;
	    fire_error = true;
	}

	VirtRef const stream_ticket_ref = self->stream_ctl->stream_ticket_ref;
	void * const stream_ticket = self->stream_ctl->stream_ticket;
	self->stream_ctl->ctl_mutex.unlock ();

	if (fire_eos) {
	    assert (!fire_error);
	    if (self->frontend) {
		logD_ (_func, "firing EOS");
		self->frontend.call_mutex (self->frontend->eos, self->mutex, stream_ticket);
		goto _return;
	    }
	} else
	if (fire_error) {
	    if (self->frontend) {
		logD_ (_func, "firing ERROR");
		self->frontend.call_mutex (self->frontend->error, self->mutex, stream_ticket);
		goto _return;
	    }
	}
    }

_return:
    self->mutex.unlock ();
    return false /* Do not reschedule */;
}

mt_mutex (mutex) void
GstStream::createVideoStream (Time             const initial_seek,
			      void           * const stream_ticket,
			      VirtReferenced * const stream_ticket_ref)
{
    if (!video_stream) {
	video_stream = grab (new VideoStream);
	video_stream_key = moment->addVideoStream (video_stream, stream_name->mem());
    }

    stream_ctl = grab (new StreamControl);
    stream_ctl->init (this, page_pool, video_stream, send_metadata);
    stream_ctl->stream_ticket = stream_ticket;
    stream_ctl->stream_ticket_ref = stream_ticket_ref;
    stream_ctl->initial_seek = initial_seek;
    stream_ctl->initial_seek_pending = true;

    recorder.stop ();
    recorder.setVideoStream (video_stream);
    if (recording) {
	logD_ (_func, "calling recorder.start(), record path: ", record_filename);
	recorder.start (record_filename);
    }

// Deprecated    last_frame_time = 0;

    if (no_video_timer) {
	timers->restartTimer (no_video_timer);
    } else {
	// TODO Update time efficiently.
	updateTime ();
	// TODO Release no_video_timer with timers->deleteTimer() at some point.
	no_video_timer = timers->addTimer (
		noVideoTimerTick, this /* cb_data */, this /* coderef_container */,
		15 /* TODO config param for the timeout */, true /* periodical */);
    }

    {
	this->ref ();
	GThread * const thread = g_thread_create (
		streamThreadFunc, this, FALSE /* joinable */, NULL /* error */);
	if (thread == NULL) {
	    logE_ (_func, "g_thread_create() failed");
	    this->unref ();
//	    moment->removeVideoStream (stream->video_stream_key);
	    video_stream = NULL;
	}
    }
}

gpointer
GstStream::streamThreadFunc (gpointer const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

#if 0
    // TEST
    logD_ (_func, "Sleeping...");
    sSleep (10);
    logD_ (_func, "...woke up");
#endif

    self->mutex.lock ();
  // FIXME: Race condition for concurrent invocations of streamThreadFunc.
    if (!self->createPipeline ()) {
	self->doCloseVideoStream ();
    }
    self->mutex.unlock ();

    // TODO What to do with this?
    // In GStreamer, bus messages are delivered via glib main loop. Therefore,
    // we won't get any bus messages unless special action is taken.
#if 0
    GMainLoop * const loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (loop);
    assert (0);
    g_main_loop_unref (loop);
#endif

    self->unref ();
    return (gpointer) 0;
}

mt_mutex (mutex) void
GstStream::doCloseVideoStream ()
{
    logD_ (_func_);

    if (no_video_timer) {
	timers->deleteTimer (no_video_timer);
	no_video_timer = NULL;
    }

// Deprecated    last_frame_time = 0;

    if (playbin) {
	logD_ (_func, "Destroying playbin");

#if 0
	do {
	    GstPad * const pad = gst_element_get_static_pad (encoder, "src");
	    if (!pad) {
		logE_ (_func, "gst_element_get_static_pad() failed");
		break;
	    }

	    gst_pad_remove_buffer_probe (pad, buffer_probe_handler_id);
	    gst_object_unref (pad);
	} while (0);
#endif

	gst_element_set_state (playbin, GST_STATE_NULL);
	logD_ (_func, "playbin state has been set to \"null\"");
	gst_object_unref (GST_OBJECT (playbin));
	playbin = NULL;
    }

    if (video_stream) {
	video_stream->close ();
	moment->removeVideoStream (video_stream_key);
	video_stream = NULL;

	video_stream = grab (new VideoStream);
	video_stream_key = moment->addVideoStream (video_stream, stream_name->mem());
    }

    stream_ctl = NULL;

    logD_ (_func, "done");
}

mt_mutex (mutex) mt_unlocks (stream_ctl->ctl_mutex) void
GstStream::restartStream ()
{
    assert (stream_ctl);
    VirtRef const stream_ticket_ref = stream_ctl->stream_ticket_ref;
    void * const stream_ticket = stream_ctl->stream_ticket;
    stream_ctl->ctl_mutex.unlock ();

    doCloseVideoStream ();
    // TODO FIXME Set correct initial seek
    createVideoStream (0 /* initial_seek */, stream_ticket, stream_ticket_ref.ptr());
}

void
GstStream::noVideoTimerTick (void * const _self)
{
//    logD_ (_func_);

    GstStream * const self = static_cast <GstStream*> (_self);

    // TODO Update time efficiently.
    updateTime ();
    Time const time = getTime();

    self->mutex.lock ();
//    logD_ (_func, "time: 0x", fmt_hex, time, ", last_frame_time: 0x", last_frame_time);

    if (self->stream_ctl) {
	self->stream_ctl->ctl_mutex.lock ();
	if (time > self->stream_ctl->last_frame_time &&
	    time - self->stream_ctl->last_frame_time >= 15 /* TODO Config param for the timeout */)
	{
	    logD_ (_func, "restarting stream");
	    mt_unlocks (self->stream_ctl->ctl_mutex) self->restartStream ();
	} else {
	    self->stream_ctl->ctl_mutex.unlock ();
	}
    }
    self->mutex.unlock ();
}

mt_mutex (mutex) Result
GstStream::createPipelineForChainSpec ()
{
    logD_ (_func, stream_spec);

    assert (is_chain);

    bool got_audio = false;
    bool got_video = false;

    GstElement *chain_el = NULL;
    GstElement *video_el = NULL;
    GstElement *audio_el = NULL;

  {
    GError *error = NULL;
    chain_el = gst_parse_launch (stream_spec->cstr (), &error);
    if (!chain_el) {
	if (error) {
	    logE_ (_func, "gst_parse_launch() failed: ", error->code,
		   " ", error->message);
	} else {
	    logE_ (_func, "gst_parse_launch() failed");
	}

	goto _failure;
    }

    stream_ctl->ctl_mutex.lock ();
    stream_ctl->playbin = chain_el;
    stream_ctl->ctl_mutex.unlock ();

    {
	GstBus * const bus = gst_element_get_bus (playbin);
	assert (bus);
	gst_bus_set_sync_handler (bus, StreamControl::busSyncHandler, stream_ctl);
	gst_object_unref (bus);
    }

    {
	audio_el = gst_bin_get_by_name (GST_BIN (chain_el), "audio");
	if (audio_el) {
	    GstPad * const pad = gst_element_get_static_pad (audio_el, "sink");
	    if (!pad) {
		logE_ (_func, "element called \"audio\" doesn't have a \"sink\" "
		       "pad. Chain spec: ", stream_spec);
		goto _failure;
	    }

	    got_audio = true;

#if 0
// At this moment, the caps are not negotiated yet.
	    {
	      // TEST
		GstCaps * const caps = gst_pad_get_negotiated_caps (pad);
		{
		    gchar * const str = gst_caps_to_string (caps);
		    logD_ (_func, "audio caps: ", str);
		    g_free (str);
		}
		gst_caps_unref (caps);
	    }
#endif

	    // TODO Use "handoff" signal
	    audio_probe_id = gst_pad_add_buffer_probe (
		    pad, G_CALLBACK (StreamControl::audioDataCb), stream_ctl);

	    gst_object_unref (pad);

	    gst_object_unref (audio_el);
	    audio_el = NULL;
	} else {
	    logW_ (_func, "chain \"", stream_name, "\" does not contain "
		   "an element named \"audio\". There'll be no audio "
		   "for the stream. Chain spec: ", stream_spec);
	}
    }

    {
	video_el = gst_bin_get_by_name (GST_BIN (chain_el), "video");
	if (video_el) {
	    GstPad * const pad = gst_element_get_static_pad (video_el, "sink");
	    if (!pad) {
		logE_ (_func, "element called \"video\" doesn't have a \"sink\" "
		       "pad. Chain spec: ", stream_spec);
		goto _failure;
	    }

	    got_video = true;

	    video_probe_id = gst_pad_add_buffer_probe (
		    pad, G_CALLBACK (StreamControl::videoDataCb), stream_ctl);

	    gst_object_unref (pad);

	    gst_object_unref (video_el);
	    video_el = NULL;
	} else {
	    logW_ (_func, "chain \"", stream_name, "\" does not contain "
		   "an element named \"video\". There'll be no video "
		   "for the stream. Chain spec: ", stream_spec);
	}
    }

    logD_ (_func, "chain \"", stream_name, "\" created");

    playbin = chain_el;

    stream_ctl->ctl_mutex.lock ();
    stream_ctl->got_audio = got_audio;
    stream_ctl->got_video = got_video;
    stream_ctl->ctl_mutex.unlock ();

    gst_element_set_state (chain_el, GST_STATE_PLAYING);

    return Result::Success;
  }

_failure:
    if (chain_el)
	gst_object_unref (chain_el);

    if (video_el)
	gst_object_unref (video_el);

    if (audio_el)
	gst_object_unref (audio_el);

    return Result::Failure;
}

mt_mutex (mutex) Result
GstStream::createPipelineForUri ()
{
    assert (!is_chain);

    GstElement *playbin           = NULL,
	       *audio_encoder_bin = NULL,
	       *video_encoder_bin = NULL,
	       *audio_encoder     = NULL,
	       *video_encoder     = NULL,
	       *fakeaudiosink     = NULL,
	       *fakevideosink     = NULL,
	       *videoscale        = NULL,
	       *audio_capsfilter  = NULL,
	       *video_capsfilter  = NULL;

  {
    playbin = gst_element_factory_make ("playbin2", NULL);
    if (!playbin) {
	logE_ (_func, "gst_element_factory_make() failed (playbin2)");
	goto _failure;
    }

    stream_ctl->ctl_mutex.lock ();
    stream_ctl->playbin = playbin;
    stream_ctl->ctl_mutex.unlock ();

    {
	GstBus * const bus = gst_element_get_bus (playbin);
	assert (bus);
	gst_bus_set_sync_handler (bus, StreamControl::busSyncHandler, stream_ctl);
	gst_object_unref (bus);
    }

    fakeaudiosink = gst_element_factory_make ("fakesink", NULL);
    if (!fakeaudiosink) {
	logE_ (_func, "gst_element_factory_make() failed (fakeaudiosink)");
	goto _failure;
    }
    g_object_set (G_OBJECT (fakeaudiosink),
		  "sync", TRUE,
		  "signal-handoffs", TRUE, NULL);

    fakevideosink = gst_element_factory_make ("fakesink", NULL);
    if (!fakevideosink) {
	logE_ (_func, "gst_element_factory_make() failed (fakevideosink)");
	goto _failure;
    }
    g_object_set (G_OBJECT (fakevideosink),
		  "sync", TRUE,
		  "signal-handoffs", TRUE, NULL);

#if 0
// Deprecated in favor of "handoff" signal.
    {
	GstPad * const pad = gst_element_get_static_pad (fakeaudiosink, "sink");
	audio_probe_id = gst_pad_add_buffer_probe (
		pad, G_CALLBACK (audioDataCb), this);
	gst_object_unref (pad);
    }

    {
	GstPad * const pad = gst_element_get_static_pad (fakevideosink, "sink");

	video_probe_id = gst_pad_add_buffer_probe (
		pad, G_CALLBACK (videoDataCb), this);

#if 0
	GstCaps * const caps = gst_caps_new_simple ("video/x-raw-yuv", "width", G_TYPE_INT, 64, "height", G_TYPE_INT, 48, NULL);
	gst_pad_set_caps (pad, caps);
	gst_caps_unref (caps);
#endif

	gst_object_unref (pad);
    }
#endif

    g_signal_connect (fakeaudiosink, "handoff", G_CALLBACK (StreamControl::handoffAudioDataCb), stream_ctl);
    g_signal_connect (fakevideosink, "handoff", G_CALLBACK (StreamControl::handoffVideoDataCb), stream_ctl);

    {
      // Audio transcoder.

	audio_encoder_bin = gst_bin_new (NULL);
	if (!audio_encoder_bin) {
	    logE_ (_func, "gst_bin_new() failed (audio_encoder_bin)");
	    goto _failure;
	}

	audio_capsfilter = gst_element_factory_make ("capsfilter", NULL);
	if (!audio_capsfilter) {
	    logE_ (_func, "gst_element_factory_make() failed (audio capsfilter)");
	    goto _failure;
	}
	g_object_set (GST_OBJECT (audio_capsfilter), "caps",
		      gst_caps_new_simple ("audio/x-raw-int",
					   "rate", G_TYPE_INT, 16000,
					   "channels", G_TYPE_INT, 1,
					   NULL), NULL);

//	audio_encoder = gst_element_factory_make ("ffenc_adpcm_swf", NULL);
	audio_encoder = gst_element_factory_make ("speexenc", NULL);
	if (!audio_encoder) {
	    logE_ (_func, "gst_element_factory_make() failed (speexenc)");
	    goto _failure;
	}
//	g_object_set (audio_encoder, "quality", 10, NULL);

	gst_bin_add_many (GST_BIN (audio_encoder_bin), audio_capsfilter, audio_encoder, fakeaudiosink, NULL);
	gst_element_link_many (audio_capsfilter, audio_encoder, fakeaudiosink, NULL);

	{
	    GstPad * const pad = gst_element_get_static_pad (audio_capsfilter, "sink");
	    gst_element_add_pad (audio_encoder_bin, gst_ghost_pad_new ("sink", pad));
	    gst_object_unref (pad);
	}

	audio_encoder = NULL;
	fakeaudiosink = NULL;
    }

    {
      // Transcoder to Sorenson h.263.

	video_encoder_bin = gst_bin_new (NULL);
	if (!video_encoder_bin) {
	    logE_ (_func, "gst_bin_new() failed (video_encoder_bin)");
	    goto _failure;
	}

	videoscale = gst_element_factory_make ("videoscale", NULL);
	if (!videoscale) {
	    logE_ (_func, "gst_element_factory_make() failed (videoscale)");
	    goto _failure;
	}
	g_object_set (G_OBJECT (videoscale), "add-borders", TRUE, NULL);

	video_capsfilter = gst_element_factory_make ("capsfilter", NULL);
	if (!video_capsfilter) {
	    logE_ (_func, "gst_element_factory_make() failed (video capsfilter)");
	    goto _failure;
	}

	if (default_width && default_height) {
	    g_object_set (G_OBJECT (video_capsfilter), "caps",
			  gst_caps_new_simple ("video/x-raw-yuv",
					       "width",  G_TYPE_INT, (int) default_width,
					       "height", G_TYPE_INT, (int) default_height,
					       "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
					       NULL), NULL);
	} else
	if (default_width) {
	    g_object_set (G_OBJECT (video_capsfilter), "caps",
			  gst_caps_new_simple ("video/x-raw-yuv",
					       "width",  G_TYPE_INT, (int) default_width,
					       "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
					       NULL), NULL);
	} else
	if (default_height) {
	    g_object_set (G_OBJECT (video_capsfilter), "caps",
			  gst_caps_new_simple ("video/x-raw-yuv",
					       "height", G_TYPE_INT, (int) default_height,
					       "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
					       NULL), NULL);
	}

	video_encoder = gst_element_factory_make ("ffenc_flv", NULL);
	if (!video_encoder) {
	    logE_ (_func, "gst_element_factory_make() failed (ffenc_flv)");
	    goto _failure;
	}
	encoder = video_encoder;
	// TODO Config parameter for bitrate.
//	g_object_set (G_OBJECT (video_encoder), "bitrate", 100000, NULL);
	g_object_set (G_OBJECT (video_encoder), "bitrate", (gulong) default_bitrate, NULL);

#if 0
	{
//	    GstPad * const pad = gst_element_get_static_pad (video_encoder, "sink");
	    GstPad * const pad = gst_element_get_static_pad (videoscale, "src");
	    GstCaps * const caps = gst_caps_new_simple ("video/x-raw-yuv", "width", G_TYPE_INT, 64, "height", G_TYPE_INT, 48, NULL);
	    gst_pad_set_caps (pad, caps);
	    gst_caps_unref (caps);
	    gst_object_unref (pad);
	}
#endif

#if 0
	{
	    GstPad * const pad = gst_element_get_static_pad (video_encoder, "sink");
//	    GstCaps * const caps = gst_caps_new_simple ("video/x-raw-yuv", "width", G_TYPE_INT, 32, NULL);
	    GstCaps * const caps = gst_caps_new_simple ("video/x-raw-yuv", "width", G_TYPE_INT, 64, "height", G_TYPE_INT, 48, NULL);
	    gst_pad_set_caps (pad, caps);
	    gst_caps_unref (caps);
	    gst_object_unref (pad);
	}
#endif

	gst_bin_add_many (GST_BIN (video_encoder_bin), videoscale, video_capsfilter, video_encoder, fakevideosink, NULL);
	gst_element_link_many (videoscale, video_capsfilter, video_encoder, fakevideosink, NULL);

	{
	    GstPad * const pad = gst_element_get_static_pad (videoscale, "sink");
	    gst_element_add_pad (video_encoder_bin, gst_ghost_pad_new ("sink", pad));
	    gst_object_unref (pad);
	}

	// 'videoscale', 'video_encoder' and 'fakevideosink' belong to
	// 'video_encoder_bin' now.
	videoscale    = NULL;
	video_encoder = NULL;
	fakevideosink = NULL;
    }

    g_object_set (G_OBJECT (playbin), "audio-sink", audio_encoder_bin, NULL);
    audio_encoder_bin = NULL;

    g_object_set (G_OBJECT (playbin), "video-sink", video_encoder_bin, NULL);
    video_encoder_bin = NULL;

    g_object_set (G_OBJECT (playbin), "uri", stream_spec->cstr(), NULL);

    // TODO got_video, got_auido -?

    gst_element_set_state (playbin, GST_STATE_PLAYING);
  }

    this->playbin = playbin;

    return Result::Success;

_failure:
    if (playbin)
	gst_object_unref (GST_OBJECT (playbin));
    if (audio_encoder_bin)
	gst_object_unref (GST_OBJECT (audio_encoder_bin));
    if (video_encoder_bin)
	gst_object_unref (GST_OBJECT (video_encoder_bin));
    if (audio_encoder)
	gst_object_unref (GST_OBJECT (audio_encoder));
    if (video_encoder)
	gst_object_unref (GST_OBJECT (video_encoder));
    if (fakeaudiosink)
	gst_object_unref (GST_OBJECT (fakeaudiosink));
    if (fakevideosink)
	gst_object_unref (GST_OBJECT (fakevideosink));
    if (videoscale)
	gst_object_unref (GST_OBJECT (videoscale));
    if (audio_capsfilter)
	gst_object_unref (GST_OBJECT (audio_capsfilter));
    if (video_capsfilter)
	gst_object_unref (GST_OBJECT (video_capsfilter));

    return Result::Failure;
}

mt_mutex (mutex) Result
GstStream::createPipeline ()
{
    if (is_chain)
	return createPipelineForChainSpec ();

    return createPipelineForUri ();
}

// If @is_chain is 'true', then @stream_spec is a chain spec with gst-launch
// syntax. Otherwise, @stream_spec is an uri for uridecodebin2.
void
GstStream::beginVideoStream (ConstMemory      const stream_spec,
			     bool             const is_chain,
			     void           * const stream_ticket,
			     VirtReferenced * const stream_ticket_ref,
			     Time             const seek)
{
    mutex.lock ();

    this->stream_spec = grab (new String (stream_spec));
    this->is_chain = is_chain;

    doCloseVideoStream ();
    createVideoStream (seek, stream_ticket, stream_ticket_ref);

    mutex.unlock ();
}

void
GstStream::endVideoStream ()
{
    mutex.lock ();
    doCloseVideoStream ();
    mutex.unlock ();
}

void
GstStream::init (MomentServer      * const moment,
		 DeferredProcessor * const deferred_processor,
		 ConstMemory         const stream_name,
		 bool                const recording,
		 ConstMemory         const record_filename,
		 bool                const send_metadata,
		 Uint64              const default_width,
		 Uint64              const default_height,
		 Uint64              const default_bitrate)
{
    this->moment = moment;
    this->stream_name = grab (new String (stream_name));
    this->recording = recording;
    this->record_filename = record_filename;
    this->send_metadata = send_metadata;
    this->default_width = default_width;
    this->default_height = default_height;
    this->default_bitrate = default_bitrate;

    this->timers = moment->getServerApp()->getTimers();
    this->page_pool = moment->getPagePool();

    deferred_reg.setDeferredProcessor (deferred_processor);

    recorder_thread_ctx = moment->getRecorderThreadPool()->grabThreadContext (record_filename);
    if (!recorder_thread_ctx) {
	logE_ (_func, "Couldn't get recorder thread context: ", exc->toString());
	this->recording = false;
    } else {
	recorder.init (recorder_thread_ctx, moment->getStorage());
    }

    flv_muxer.setPagePool (page_pool);

    recorder.setMuxer (&flv_muxer);
// TODO recorder frontend + error reporting
//    stream->recorder.setFrontend (CbDesc<AvRecorder::Frontend> (
//	    recorder_frontend, stream /* cb_data */, stream /* coderef_container */));
}

void
GstStream::release ()
{
    mutex.lock ();
    if (valid) {
	mutex.unlock ();
	return;
    }

    doCloseVideoStream ();
    // TODO releaseVideoStream() to release stream's resources permanently,
    // including thread context. In contrast, doCloseVideoStream() allows
    // restarting the stream.

    valid = false;
    mutex.unlock ();

    // TODO release recorder_thread_ctx
}

GstStream::GstStream ()
    : moment (NULL),
      timers (NULL),
      page_pool (NULL),

      valid (true),

      send_metadata (true),

      default_width (0),
      default_height (0),
      default_bitrate (0),

      is_chain (false),

      stream_position (0),

      recorder_thread_ctx (NULL),
      recorder (this /* coderef_container */),
      recording (false),

      playbin (NULL),
      encoder (NULL),
      audio_probe_id (0),
      video_probe_id (0),

      no_video_timer (NULL)
{
    logD_ (_func, "0x", fmt_hex, (UintPtr) this);

    deferred_task.cb = CbDesc<DeferredProcessor::TaskCallback> (
	    deferredTask, this /* cb_data */, this /* coderef_container */);
}

GstStream::~GstStream ()
{
    logD_ (_func, "0x", fmt_hex, (UintPtr) this);

    deferred_reg.release ();
}

}
