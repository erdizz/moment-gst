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

static LogGroup libMary_logGroup_chains ("moment-gst_chains", LogLevel::I);
static LogGroup libMary_logGroup_pipeline ("moment-gst_pipeline", LogLevel::I);
static LogGroup libMary_logGroup_stream ("moment-gst_stream", LogLevel::I);
static LogGroup libMary_logGroup_bus ("moment-gst_bus", LogLevel::I);
static LogGroup libMary_logGroup_frames ("moment-gst_frames", LogLevel::E); // E is the default
static LogGroup libMary_logGroup_novideo ("moment-gst_novideo", LogLevel::I);

void
GstStream::workqueueThreadFunc (void * const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    updateTime ();

    self->mutex.lock ();

    self->tlocal = libMary_getThreadLocal();

    for (;;) {
        if (self->stream_closed) {
            self->mutex.unlock ();
            return;
        }

        while (self->workqueue_list.isEmpty()) {
            self->workqueue_cond.wait (self->mutex);
            updateTime ();

            if (self->stream_closed) {
                self->mutex.unlock ();
                return;
            }
        }

        Ref<WorkqueueItem> const workqueue_item = self->workqueue_list.getFirst();
        self->workqueue_list.remove (self->workqueue_list.getFirstElement());

        self->mutex.unlock ();

        switch (workqueue_item->item_type) {
            case WorkqueueItem::ItemType_CreatePipeline:
                self->doCreatePipeline ();
                break;
            case WorkqueueItem::ItemType_ReleasePipeline:
                self->doReleasePipeline ();
                break;
            default:
                unreachable ();
        }

        self->mutex.lock ();
    }
}

void
GstStream::createPipelineForChainSpec ()
{
    logD (chains, _func, stream_spec);

    assert (is_chain);

    if (!stream_spec->mem().len())
	return;

    GstElement *chain_el = NULL;
    GstElement *in_stats_el = NULL;
    GstElement *video_el = NULL;
    GstElement *audio_el = NULL;
    GstElement *mix_video_el = NULL;
    GstElement *mix_audio_el = NULL;

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

	mutex.lock ();
	goto _failure;
    }

    mutex.lock ();

    if (stream_closed)
	goto _failure;

    playbin = chain_el;
    gst_object_ref (playbin);

    {
	in_stats_el = gst_bin_get_by_name (GST_BIN (chain_el), "in_stats");
	if (in_stats_el) {
	    GstPad * const pad = gst_element_get_static_pad (in_stats_el, "sink");
	    if (!pad) {
		logE_ (_func, "element called \"in_stats\" doesn't have a \"sink\" "
		       "pad. Chain spec: ", stream_spec);
		goto _failure;
	    }

	    got_in_stats = true;

	    gst_pad_add_buffer_probe (pad, G_CALLBACK (GstStream::inStatsDataCb), this);

	    gst_object_unref (pad);

	    gst_object_unref (in_stats_el);
	    in_stats_el = NULL;
	}
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
		    pad, G_CALLBACK (GstStream::audioDataCb), this);

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

	    // TODO Use "handoff" signal
	    video_probe_id = gst_pad_add_buffer_probe (
		    pad, G_CALLBACK (GstStream::videoDataCb), this);

	    gst_object_unref (pad);

	    gst_object_unref (video_el);
	    video_el = NULL;
	} else {
	    logW_ (_func, "chain \"", stream_name, "\" does not contain "
		   "an element named \"video\". There'll be no video "
		   "for the stream. Chain spec: ", stream_spec);
	}
    }

    mix_audio_el = gst_bin_get_by_name (GST_BIN (chain_el), "mix_audio");
    if (mix_audio_el) {
	mix_audio_src = GST_APP_SRC (mix_audio_el);
	g_object_ref (mix_audio_src);
    }

    mix_video_el = gst_bin_get_by_name (GST_BIN (chain_el), "mix_video");
    if (mix_video_el) {
	mix_video_src = GST_APP_SRC (mix_video_el);
	g_object_ref (mix_video_src);
    }

    logD (chains, _func, "chain \"", stream_name, "\" created");

    if (!mt_unlocks (mutex) setPipelinePlaying ()) {
	mutex.lock ();
	goto _failure;
    }

    goto _return;
  }

mt_mutex (mutex) _failure:
    mt_unlocks (mutex) pipelineCreationFailed ();

_return:
    if (chain_el)
	gst_object_unref (chain_el);
    if (in_stats_el)
	gst_object_unref (in_stats_el);
    if (video_el)
	gst_object_unref (video_el);
    if (audio_el)
	gst_object_unref (audio_el);
    if (mix_video_el)
	gst_object_unref (mix_video_el);
    if (mix_audio_el)
	gst_object_unref (mix_audio_el);
}

void
GstStream::createPipelineForUri ()
{
    assert (!is_chain);

    if (!stream_spec->mem().len())
	return;

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
	mutex.lock ();
	goto _failure;
    }

    mutex.lock ();

    if (stream_closed)
	goto _failure;

    this->playbin = playbin;
    gst_object_ref (this->playbin);

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

    g_signal_connect (fakeaudiosink, "handoff", G_CALLBACK (GstStream::handoffAudioDataCb), this);
    g_signal_connect (fakevideosink, "handoff", G_CALLBACK (GstStream::handoffVideoDataCb), this);

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

	audio_capsfilter = NULL;
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
	video_capsfilter = NULL;
	video_encoder = NULL;
	fakevideosink = NULL;
    }

    g_object_set (G_OBJECT (playbin), "audio-sink", audio_encoder_bin, NULL);
    audio_encoder_bin = NULL;

    g_object_set (G_OBJECT (playbin), "video-sink", video_encoder_bin, NULL);
    video_encoder_bin = NULL;

    g_object_set (G_OBJECT (playbin), "uri", stream_spec->cstr(), NULL);

    // TODO got_video, got_auido -?

    if (!mt_unlocks (mutex) setPipelinePlaying ()) {
	mutex.lock ();
	goto _failure;
    }
  }

    goto _return;

mt_mutex (mutex) _failure:
    mt_unlocks (mutex) pipelineCreationFailed ();

_return:
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
}

mt_unlocks (mutex) Result
GstStream::setPipelinePlaying ()
{
    GstElement * const chain_el = playbin;
    assert (chain_el);
    gst_object_ref (chain_el);

    if (no_video_timeout > 0) {
	no_video_timer = timers->addTimer (noVideoTimerTick,
					   this /* cb_data */,
					   this /* coderef_container */,
					   no_video_timeout /* TODO config param for the timeout */,
					   true /* periodical */);
    }

    changing_state_to_playing = true;
    mutex.unlock ();

    {
	GstBus * const bus = gst_element_get_bus (chain_el);
	assert (bus);
	gst_bus_set_sync_handler (bus, GstStream::busSyncHandler, this);
	gst_object_unref (bus);
    }

    logD (pipeline, _func, "Setting pipeline state to PAUSED");
    if (gst_element_set_state (chain_el, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
	logE_ (_func, "gst_element_set_state() failed (PAUSED)");
	goto _failure;
    }

    mutex.lock ();
    changing_state_to_playing = false;
    if (stream_closed) {
        mutex.unlock ();

	logD (pipeline, _func, "Setting pipeline state to NULL");
	if (gst_element_set_state (chain_el, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
	    logE_ (_func, "gst_element_set_state() failed (NULL)");

//      doReleasePipeline ();
    } else {
      mutex.unlock ();
    }

    gst_object_unref (chain_el);
    return Result::Success;

_failure:
    logD (pipeline, _func, "Setting pipeline state to NULL");
    if (gst_element_set_state (chain_el, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
	logE_ (_func, "gst_element_set_state() failed (NULL #2)");

    gst_object_unref (chain_el);
    return Result::Failure;
}

mt_unlocks (mutex) void
GstStream::pipelineCreationFailed ()
{
    if (no_video_timer) {
	timers->deleteTimer (no_video_timer);
	no_video_timer = NULL;
    }

    stream_closed = true;

    GstElement * const tmp_playbin = playbin;
    playbin = NULL;

    GstElement * const tmp_mix_audio_src = GST_ELEMENT (mix_audio_src);
    mix_audio_src = NULL;

    GstElement * const tmp_mix_video_src = GST_ELEMENT (mix_video_src);
    mix_video_src = NULL;

    eos_pending = true;

    mutex.unlock ();

    reportStatusEvents ();

    if (tmp_playbin) {
	// Extra transition to NULL state for extra safety.
	logD (pipeline, _func, "Setting pipeline state to NULL");
	if (gst_element_set_state (tmp_playbin, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
	    logE_ (_func, "gst_element_set_state() failed (NULL)");

	gst_object_unref (tmp_playbin);
    }

    if (tmp_mix_audio_src)
	gst_object_unref (tmp_mix_audio_src);

    if (tmp_mix_video_src)
	gst_object_unref (tmp_mix_video_src);
}

void
GstStream::doReleasePipeline ()
{
    logD (pipeline, _func_);

    mutex.lock ();

    if (no_video_timer) {
	timers->deleteTimer (no_video_timer);
	no_video_timer = NULL;
    }

    GstElement * const tmp_playbin = playbin;
    playbin = NULL;

    GstElement * const tmp_mix_audio_src = GST_ELEMENT (mix_audio_src);
    mix_audio_src = NULL;

    GstElement * const tmp_mix_video_src = GST_ELEMENT (mix_video_src);
    mix_video_src = NULL;

    bool to_null_state = false;
    if (!changing_state_to_playing)
	to_null_state = true;

    stream_closed = true;
    mutex.unlock ();

    if (tmp_playbin) {
	if (to_null_state) {
	    logD (pipeline, _func, "Setting pipeline state to NULL");
	    if (gst_element_set_state (tmp_playbin, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
		logE_ (_func, "gst_element_set_state() failed (NULL)");
	}

	gst_object_unref (tmp_playbin);
    }

    if (tmp_mix_audio_src)
	gst_object_unref (tmp_mix_audio_src);

    if (tmp_mix_video_src)
	gst_object_unref (tmp_mix_video_src);
}

void
GstStream::releasePipeline ()
{
    mutex.lock ();

    while (!workqueue_list.isEmpty()) {
        Ref<WorkqueueItem> &last_item = workqueue_list.getLast();
        switch (last_item->item_type) {
            case WorkqueueItem::ItemType_CreatePipeline:
                workqueue_list.remove (workqueue_list.getLastElement());
                break;
            case WorkqueueItem::ItemType_ReleasePipeline:
                mutex.unlock ();
                return;
            default:
                unreachable ();
        }
    }

    Ref<WorkqueueItem> const new_item = grab (new WorkqueueItem);
    new_item->item_type = WorkqueueItem::ItemType_ReleasePipeline;

    workqueue_list.prepend (new_item);
    workqueue_cond.signal ();

    bool const join = (tlocal != libMary_getThreadLocal());

    mutex.unlock ();

    if (join) {
        //#error joining from the same thread - WRONG
        workqueue_thread->join ();
    }
}

mt_mutex (mutex) void
GstStream::reportMetaData ()
{
    logD (stream, _func_);

    if (metadata_reported) {
	return;
    }
    metadata_reported = true;

    if (!send_metadata)
	return;

    VideoStream::VideoMessage msg;
    if (!RtmpServer::encodeMetaData (&metadata, page_pool, &msg)) {
	logE_ (_func, "encodeMetaData() failed");
	return;
    }

    logD (stream, _func, "Firing video message");
    // TODO unlock/lock?
    video_stream->fireVideoMessage (&msg);

    page_pool->msgUnref (msg.page_list.first);
}

static void
dumpBufferFlags (GstBuffer * const buffer)
{
    Uint32 flags = (Uint32) GST_BUFFER_FLAGS (buffer);
    if (flags != GST_BUFFER_FLAGS (buffer))
	log__ (_func, "flags do not fit into Uint32");

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_READONLY)) {
	log__ (_func, "GST_BUFFER_FLAG_READONLY");
	flags ^= GST_BUFFER_FLAG_READONLY;
    }

#if 0
    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_MEDIA4)) {
	log__ (_func, "GST_BUFFER_FLAG_MEDIA4");
	flags ^= GST_BUFFER_FLAG_MEDIA4;
    }
#endif

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_PREROLL)) {
	log__ (_func, "GST_BUFFER_FLAG_PREROLL");
	flags ^= GST_BUFFER_FLAG_PREROLL;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_DISCONT)) {
	log__ (_func, "GST_BUFFER_FLAG_DISCONT");
	flags ^= GST_BUFFER_FLAG_DISCONT;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_IN_CAPS)) {
	log__ (_func, "GST_BUFFER_FLAG_IN_CAPS");
	flags ^= GST_BUFFER_FLAG_IN_CAPS;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_GAP)) {
	log__ (_func, "GST_BUFFER_FLAG_GAP");
	flags ^= GST_BUFFER_FLAG_GAP;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
	log__ (_func, "GST_BUFFER_FLAG_DELTA_UNIT");
	flags ^= GST_BUFFER_FLAG_DELTA_UNIT;
    }

#if 0
    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_MEDIA1)) {
	log__ (_func, "GST_BUFFER_FLAG_MEDIA1");
	flags ^= GST_BUFFER_FLAG_MEDIA1;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_MEDIA2)) {
	log__ (_func, "GST_BUFFER_FLAG_MEDIA2");
	flags ^= GST_BUFFER_FLAG_MEDIA2;
    }

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_MEDIA3)) {
	log__ (_func, "GST_BUFFER_FLAG_MEDIA3");
	flags ^= GST_BUFFER_FLAG_MEDIA3;
    }
#endif

    if (flags)
	log__ (_func, "Extra flags: 0x", fmt_hex, flags);
}

static Ref<String>
gstStateToString (GstState const state)
{
    ConstMemory mem;

    switch (state) {
	case GST_STATE_VOID_PENDING:
	    mem = "VOID_PENDING";
	    break;
	case GST_STATE_NULL:
	    mem = "NULL";
	    break;
	case GST_STATE_READY:
	    mem = "READY";
	    break;
	case GST_STATE_PAUSED:
	    mem = "PAUSED";
	    break;
	case GST_STATE_PLAYING:
	    mem = "PLAYING";
	    break;
	default:
	    mem = "Unknown";
    }

    return grab (new String (mem));
}

gboolean
GstStream::inStatsDataCb (GstPad    * const /* pad */,
			  GstBuffer * const buffer,
			  gpointer    const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    self->mutex.lock ();
    self->rx_bytes += GST_BUFFER_SIZE (buffer);
    self->mutex.unlock ();

    return TRUE;
}

void
GstStream::doAudioData (GstBuffer * const buffer)
{
    // TODO Update current time efficiently.
    updateTime ();

    logD (frames, _func, "stream 0x", fmt_hex, (UintPtr) this, ", "
	  "timestamp 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer), ", "
	  "flags 0x", fmt_hex, GST_BUFFER_FLAGS (buffer), ", "
	  "size: ", fmt_def, GST_BUFFER_SIZE (buffer));
    if (logLevelOn (frames, LogLevel::Debug)) {
	dumpBufferFlags (buffer);

	if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_IN_CAPS) ||
	    GST_BUFFER_TIMESTAMP (buffer) == (GstClockTime) -1)
	{
            logLock ();
	    hexdump (logs, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));
            logUnlock ();
	}
    }

    mutex.lock ();

    rx_audio_bytes += GST_BUFFER_SIZE (buffer);

    last_frame_time = getTime ();
    logD (frames, _func, "last_frame_time: 0x", fmt_hex, last_frame_time);

    if (prv_audio_timestamp > GST_BUFFER_TIMESTAMP (buffer)) {
	logW (frames, _func, "backwards timestamp: prv 0x", fmt_hex, prv_audio_timestamp,
	      ", cur 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer)); 
    }
    prv_audio_timestamp = GST_BUFFER_TIMESTAMP (buffer);

    VideoStream::AudioFrameType codec_data_type = VideoStream::AudioFrameType::Unknown;
    GstBuffer *codec_data_buffers [2];
    Count num_codec_data_buffers = 0;
//    GstBuffer *aac_codec_data_buffer = NULL;
    if (first_audio_frame) {
	GstCaps * const caps = gst_buffer_get_caps (buffer);
	{
	    gchar * const str = gst_caps_to_string (caps);
	    logD (stream, _func, "caps: ", str);
	    g_free (str);
	}

	GstStructure * const structure = gst_caps_get_structure (caps, 0);
	gchar const * structure_name = gst_structure_get_name (structure);
	logD (stream, _func, "structure name: ", gst_structure_get_name (structure));

	ConstMemory const structure_name_mem (structure_name, strlen (structure_name));

	gint channels;
	gint rate;
	if (!gst_structure_get_int (structure, "channels", &channels))
	    channels = 1;
	if (!gst_structure_get_int (structure, "rate", &rate))
	    rate = 44100;

        audio_rate = rate;
        audio_channels = channels;

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

#if 0
		    aac_codec_data_buffer = gst_value_get_buffer (val);
		    logD (stream, _func, "aac_codec_data_buffer: 0x", fmt_hex, (UintPtr) aac_codec_data_buffer);
#endif
		    codec_data_type = VideoStream::AudioFrameType::AacSequenceHeader;
		    codec_data_buffers [0] = gst_value_get_buffer (val);
		    num_codec_data_buffers = 1;
		} while (0);
	    }
	} else
	if (equal (structure_name_mem, "audio/x-speex")) {
	  // Speex
	    audio_codec_id = VideoStream::AudioCodecId::Speex;
	    audio_hdr = 0xb6; // Speex, 11 kHz, 16-bit samples, mono

	    do {
	      // Processing Speex stream headers.

		GValue const * const val = gst_structure_get_value (structure, "streamheader");
		if (!val) {
		    logW_ (_func, "Speex streamheader not found");
		    break;
		}

		if (!GST_VALUE_HOLDS_ARRAY (val)) {
		    logW_ (_func, "streamheader doesn't hold an array");
		    break;
		}

		codec_data_type = VideoStream::AudioFrameType::SpeexHeader;

		guint const arr_size = gst_value_array_get_size (val);
		for (guint i = 0; i < arr_size; ++i) {
		    GValue const * const elem_val = gst_value_array_get_value (val, i);
		    if (!elem_val) {
			logW_ (_func, "Speex streamheader: NULL array element #", i);
		    } else
		    if (!GST_VALUE_HOLDS_BUFFER (elem_val)) {
			logW_ (_func, "Speex streamheader element #", i, " doesn't hold a buffer");
		    } else {
			codec_data_buffers [i] = gst_value_get_buffer (elem_val);
			++num_codec_data_buffers;
		    }
		}

		logD (stream, _func, "Speex streamheader: num_codec_data_buffers: ", num_codec_data_buffers);
	    } while (0);
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
	    logD (stream, _func, "stereo");
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

	if (send_metadata) {
	    if (!got_video || !first_video_frame) {
	      // There's no video or we've got the first video frame already.
		reportMetaData ();
		metadata_reported_cond.signal ();
	    } else {
	      // Waiting for the first video frame.
		while (got_video && first_video_frame)
		    // TODO FIXME This is a perfect way to hang up a thread.
		    metadata_reported_cond.wait (mutex);
	    }
	}
    }

    bool skip_frame = false;
    {
	if (audio_skip_counter > 0) {
	    --audio_skip_counter;
	    logD (frames, _func, "Skipping audio frame, audio_skip_counter: ", audio_skip_counter, " left");
	    skip_frame = true;
	}

//	if (initial_play_pending && !play_pending) {
	if (initial_seek_pending && initial_seek > 0) {
	    // We have not started playing yet. This is most likely a preroll frame.
	    logD (frames, _func, "Skipping an early preroll frame");
	    skip_frame = true;
	}
    }

    VideoStream::AudioCodecId const tmp_audio_codec_id = audio_codec_id;
    Byte const tmp_audio_hdr = audio_hdr;
    unsigned const tmp_audio_rate = audio_rate;
    unsigned const tmp_audio_channels = audio_channels;
    mutex.unlock ();

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_IN_CAPS) ||
	GST_BUFFER_TIMESTAMP (buffer) == (GstClockTime) -1)
    {
	skip_frame = true;
    }

    if (tmp_audio_codec_id == VideoStream::AudioCodecId::Unknown) {
	logD (frames, _func, "unknown codec id, dropping audio frame");
	return;
    }

    if (num_codec_data_buffers > 0) {
      // Reporting codec data if needed.

	for (Size i = 0; i < num_codec_data_buffers; ++i) {
	    Size msg_len = 0;

//	    Uint64 const cd_timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (codec_data_buffers [i]) / 1000000);
	    Uint64 const cd_timestamp = 0;

	    PagePool::PageListHead page_list;
	    RtmpConnection::PrechunkContext prechunk_ctx;

	    Byte msg_audio_hdr [2] = { tmp_audio_hdr, 0 };
	    Size msg_audio_hdr_len = 1;
	    if (codec_data_type == VideoStream::AudioFrameType::AacSequenceHeader)
		msg_audio_hdr_len = 2;

            if (enable_prechunking) {
                RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                                     ConstMemory (msg_audio_hdr, msg_audio_hdr_len),
                                                     page_pool,
                                                     &page_list,
                                                     RtmpConnection::DefaultAudioChunkStreamId,
                                                     cd_timestamp,
                                                     true /* first_chunk */);
            } else {
                page_pool->getFillPages (&page_list, ConstMemory (msg_audio_hdr, msg_audio_hdr_len));
            }
	    msg_len += msg_audio_hdr_len;

	    logD (frames, _func, "CODEC DATA");
	    if (logLevelOn (frames, LogLevel::D)) {
                logLock ();
		hexdump (logs, ConstMemory (GST_BUFFER_DATA (codec_data_buffers [i]), GST_BUFFER_SIZE (codec_data_buffers [i])));
                logUnlock ();
            }

            if (enable_prechunking) {
                RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                                     ConstMemory (GST_BUFFER_DATA (codec_data_buffers [i]),
                                                                  GST_BUFFER_SIZE (codec_data_buffers [i])),
                                                     page_pool,
                                                     &page_list,
                                                     RtmpConnection::DefaultAudioChunkStreamId,
                                                     cd_timestamp,
                                                     false /* first_chunk */);
            } else {
                page_pool->getFillPages (&page_list,
                                         ConstMemory (GST_BUFFER_DATA (codec_data_buffers [i]),
                                                      GST_BUFFER_SIZE (codec_data_buffers [i])));
            }
	    msg_len += GST_BUFFER_SIZE (codec_data_buffers [i]);

	    VideoStream::AudioMessage msg;
	    msg.timestamp = cd_timestamp;
	    msg.prechunk_size = (enable_prechunking ? RtmpConnection::PrechunkSize : 0);
	    msg.frame_type = codec_data_type;
	    msg.codec_id = tmp_audio_codec_id;
            msg.rate = tmp_audio_rate;
            msg.channels = tmp_audio_channels;

	    msg.page_pool = page_pool;
	    msg.page_list = page_list;
	    msg.msg_len = msg_len;
	    msg.msg_offset = 0;

            // TODO unlock/lock?
	    video_stream->fireAudioMessage (&msg);

	    page_pool->msgUnref (page_list.first);
	}
    }

    if (skip_frame)
	return;

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

    if (enable_prechunking) {
        RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                             ConstMemory (gen_audio_hdr, gen_audio_hdr_len),
                                             page_pool,
                                             &page_list,
                                             RtmpConnection::DefaultAudioChunkStreamId,
                                             timestamp,
                                             true /* first_chunk */);
    } else {
        page_pool->getFillPages (&page_list, ConstMemory (gen_audio_hdr, gen_audio_hdr_len));
    }
    msg_len += gen_audio_hdr_len;

    if (enable_prechunking) {
        RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                             ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)),
                                             page_pool,
                                             &page_list,
                                             RtmpConnection::DefaultAudioChunkStreamId,
                                             timestamp,
                                             false /* first_chunk */);
    } else {
        page_pool->getFillPages (&page_list, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));
    }
    msg_len += GST_BUFFER_SIZE (buffer);

//    hexdump (errs, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));

    VideoStream::AudioMessage msg;
    msg.timestamp = timestamp;
    msg.prechunk_size = (enable_prechunking ? RtmpConnection::PrechunkSize : 0);
    msg.frame_type = VideoStream::AudioFrameType::RawData;
    msg.codec_id = tmp_audio_codec_id;

    msg.page_pool = page_pool;
    msg.page_list = page_list;
    msg.msg_len = msg_len;
    msg.msg_offset = 0;
    msg.rate = tmp_audio_rate;
    msg.channels = tmp_audio_channels;

//    logD_ (_func, fmt_hex, msg.timestamp);

    // TODO unlock/lock?
    video_stream->fireAudioMessage (&msg);

    page_pool->msgUnref (page_list.first);
}

gboolean
GstStream::audioDataCb (GstPad    * const /* pad */,
			GstBuffer * const buffer,
			gpointer    const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);
    self->doAudioData (buffer);
    return TRUE;
}

void
GstStream::handoffAudioDataCb (GstElement * const /* element */,
			       GstBuffer  * const buffer,
			       GstPad     * const /* pad */,
			       gpointer     const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);
    self->doAudioData (buffer);
}

void
GstStream::doVideoData (GstBuffer * const buffer)
{
    // TODO Update current time efficiently.
    updateTime ();

    logD (frames, _func, "stream 0x", fmt_hex, (UintPtr) this, ", "
	  "timestamp 0x", fmt_hex, GST_BUFFER_TIMESTAMP (buffer), ", "
	  "flags 0x", fmt_hex, GST_BUFFER_FLAGS (buffer));
    if (logLevelOn (frames, LogLevel::Debug))
	dumpBufferFlags (buffer);

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_PREROLL))
	logD (frames, _func, "preroll buffer");

#if 0
    // TEST Allowing both audio and video to flow through the same pin.
    {
	GstCaps * const caps = gst_buffer_get_caps (buffer);
	logD_ (_func, "caps: 0x", fmt_hex, (UintPtr) caps);
	if (!caps)
	    return;

	{
	    gchar * const str = gst_caps_to_string (caps);
	    logD_ (_func, "caps: ", str);
	    g_free (str);
	}

	GstStructure * const st = gst_caps_get_structure (caps, 0);
	gchar const * st_name = gst_structure_get_name (st);
	logD_ (_func, "st_name: ", gst_structure_get_name (st));
	ConstMemory const st_name_mem (st_name, strlen (st_name));

	ConstMemory const audio_str ("audio");
	if (st_name_mem.len() >= audio_str.len()
	    && memcmp (st_name_mem.mem(), audio_str.mem(), audio_str.len()) == 0)
	{
	    doAudioData (buffer);
	    return;
	}
    }
#endif

#if 0
    // TEST
    if (GST_BUFFER_SIZE (buffer) <= 64) {
	logD_ (_func, "Ignoring small buffer");
	return;
    }
#endif

    mutex.lock ();

    rx_video_bytes += GST_BUFFER_SIZE (buffer);

    last_frame_time = getTime ();
    logD (frames, _func, "last_frame_time: 0x", fmt_hex, last_frame_time);

    GstBuffer *avc_codec_data_buffer = NULL;
    if (first_video_frame) {
	first_video_frame = false;

	GstCaps * const caps = gst_buffer_get_caps (buffer);
	{
	    gchar * const str = gst_caps_to_string (caps);
	    logD (frames, _func, "caps: ", str);
	    g_free (str);
	}

	GstStructure * const st = gst_caps_get_structure (caps, 0);
	gchar const * st_name = gst_structure_get_name (st);
	logD (frames, _func, "st_name: ", gst_structure_get_name (st));
	ConstMemory const st_name_mem (st_name, strlen (st_name));

	if (equal (st_name_mem, "video/x-flash-video")) {
	   video_codec_id = VideoStream::VideoCodecId::SorensonH263;
	   video_hdr = 0x02; // Sorenson H.263
	} else
	if (equal (st_name_mem, "video/x-h264")) {
	   video_codec_id = VideoStream::VideoCodecId::AVC;
	   video_hdr = 0x07; // AVC

	   do {
	     // Processing AvcSequenceHeader

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
	       logD (frames, _func, "avc_codec_data_buffer: 0x", fmt_hex, (UintPtr) avc_codec_data_buffer);
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

	if (send_metadata) {
	    if (!got_audio || !first_audio_frame) {
	      // There's no video or we've got the first video frame already.
		reportMetaData ();
		metadata_reported_cond.signal ();
	    } else {
	      // Waiting for the first audio frame.
		while (got_audio && first_audio_frame)
		    // TODO FIXME This is a perfect way to hang up a thread.
		    metadata_reported_cond.wait (mutex);
	    }
	}
    }

    bool skip_frame = false;
    {
	if (video_skip_counter > 0) {
	    --video_skip_counter;
	    logD (frames, _func, "Skipping video frame, video_skip_counter: ", video_skip_counter);
	    skip_frame = true;
	}

//	if (initial_play_pending && !play_pending) {
	if (initial_seek_pending && initial_seek > 0) {
	    // We have not started playing yet. This is most likely a preroll frame.
	    logD (frames, _func, "Skipping an early preroll frame");
	    skip_frame = true;
	}
    }

    VideoStream::VideoCodecId const tmp_video_codec_id = video_codec_id;
    Byte const tmp_video_hdr = video_hdr;
    mutex.unlock ();

    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_IN_CAPS) ||
	GST_BUFFER_TIMESTAMP (buffer) == (GstClockTime) -1)
    {
	skip_frame = true;
    }

    if (avc_codec_data_buffer) {
      // Reporting AVC codec data if needed.

        // Timestamps for codec data buffers are seemingly random.
//	Uint64 const timestamp = (Uint64) (GST_BUFFER_TIMESTAMP (avc_codec_data_buffer) / 1000000);
        Uint64 const timestamp = 0;

	Size msg_len = 0;

	PagePool::PageListHead page_list;
	RtmpConnection::PrechunkContext prechunk_ctx;

	Byte avc_video_hdr [5] = { 0x17, 0, 0, 0, 0 }; // AVC, seekable frame;
						       // AVC sequence header;
						       // Composition time offset = 0.

        if (enable_prechunking) {
            RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                                 ConstMemory::forObject (avc_video_hdr),
                                                 page_pool,
                                                 &page_list,
                                                 RtmpConnection::DefaultVideoChunkStreamId,
                                                 timestamp,
                                                 true /* first_chunk */);
            msg_len += sizeof (avc_video_hdr);
        } else {
// TODO FIXME This should be done in mod_rtmp
// TEST (uncomment)            page_pool->getFillPages (&page_list, ConstMemory::forObject (avc_video_hdr));
//            msg_len += sizeof (avc_video_hdr);
        }

	logD (frames,_func, "AVC SEQUENCE HEADER");
	if (logLevelOn (frames, LogLevel::D)) {
            logLock ();
	    hexdump (logs, ConstMemory (GST_BUFFER_DATA (avc_codec_data_buffer), GST_BUFFER_SIZE (avc_codec_data_buffer)));
            logUnlock ();
        }

#if 0
	// TEST
	if (GST_BUFFER_SIZE (avc_codec_data_buffer) >= 4) {
	    GST_BUFFER_DATA (avc_codec_data_buffer) [0] = 0x01;
	    GST_BUFFER_DATA (avc_codec_data_buffer) [1] = 0x4d;
	    GST_BUFFER_DATA (avc_codec_data_buffer) [2] = 0x40;
	    GST_BUFFER_DATA (avc_codec_data_buffer) [3] = 0x28;
	}
#endif

        if (enable_prechunking) {
            RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                                 ConstMemory (GST_BUFFER_DATA (avc_codec_data_buffer),
                                                              GST_BUFFER_SIZE (avc_codec_data_buffer)),
                                                 page_pool,
                                                 &page_list,
                                                 RtmpConnection::DefaultVideoChunkStreamId,
                                                 timestamp,
                                                 false /* first_chunk */);
        } else {
            page_pool->getFillPages (&page_list,
                                     ConstMemory (GST_BUFFER_DATA (avc_codec_data_buffer),
                                                  GST_BUFFER_SIZE (avc_codec_data_buffer)));
        }
	msg_len += GST_BUFFER_SIZE (avc_codec_data_buffer);

	VideoStream::VideoMessage msg;
	msg.timestamp = timestamp;
	msg.prechunk_size = (enable_prechunking ? RtmpConnection::PrechunkSize : 0);
	msg.frame_type = VideoStream::VideoFrameType::AvcSequenceHeader;
	msg.codec_id = tmp_video_codec_id;

	msg.page_pool = page_pool;
	msg.page_list = page_list;
	msg.msg_len = msg_len;
	msg.msg_offset = 0;

        if (logLevelOn (frames, LogLevel::D)) {
            logD_ (_func, "Firing video message (AVC sequence header):");
            logLock ();
            PagePool::dumpPages (logs, &page_list);
            logUnlock ();
        }

        // TODO unlock/lock?
	video_stream->fireVideoMessage (&msg);

	page_pool->msgUnref (page_list.first);
    } // if (avc_codec_data_buffer)

    if (skip_frame) {
	logD (frames, "skipping frame");
	return;
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

    if (enable_prechunking) {
        RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                             ConstMemory (gen_video_hdr, gen_video_hdr_len),
                                             page_pool,
                                             &page_list,
                                             RtmpConnection::DefaultVideoChunkStreamId,
                                             timestamp,
                                             true /* first_chunk */);
        msg_len += gen_video_hdr_len;
    } else {
// TEST (uncomment)
//        page_pool->getFillPages (&page_list, ConstMemory (gen_video_hdr, gen_video_hdr_len));
//        msg_len += gen_video_hdr_len;
    }

    if (enable_prechunking) {
        RtmpConnection::fillPrechunkedPages (&prechunk_ctx,
                                             ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)),
                                             page_pool,
                                             &page_list,
                                             RtmpConnection::DefaultVideoChunkStreamId,
                                             timestamp,
                                             false /* first_chunk */);
    } else {
        page_pool->getFillPages (&page_list, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));
    }
    msg_len += GST_BUFFER_SIZE (buffer);

//    hexdump (errs, ConstMemory (GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)));

    msg.timestamp = timestamp;
    msg.prechunk_size = (enable_prechunking ? RtmpConnection::PrechunkSize : 0);

    msg.page_pool = page_pool;
    msg.page_list = page_list;
    msg.msg_len = msg_len;
    msg.msg_offset = 0;

    if (logLevelOn (frames, LogLevel::D)) {
        logD_ (_func, "Firing video message:");
        logLock ();
        PagePool::dumpPages (logs, &page_list);
        logUnlock ();
    }

//    logD_ (_func, "firing video message, ", msg_len, " bytes");
    // TODO unlock/lock?
    video_stream->fireVideoMessage (&msg);

    page_pool->msgUnref (page_list.first);
}

gboolean
GstStream::videoDataCb (GstPad    * const /* pad */,
			GstBuffer * const buffer,
			gpointer    const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);
    self->doVideoData (buffer);
    return TRUE;
}

void
GstStream::handoffVideoDataCb (GstElement * const /* element */,
			       GstBuffer  * const buffer,
			       GstPad     * const /* pad */,
			       gpointer     const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);
    self->doVideoData (buffer);
}

GstBusSyncReply
GstStream::busSyncHandler (GstBus     * const /* bus */,
			   GstMessage * const msg,
			   gpointer     const _self)
{
    logD (bus, _func, gst_message_type_get_name (GST_MESSAGE_TYPE (msg)), ", src: 0x", fmt_hex, (UintPtr) GST_MESSAGE_SRC (msg));

    GstStream * const self = static_cast <GstStream*> (_self);

    self->mutex.lock ();
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (self->playbin)) {
	logD (bus, _func, "PIPELINE MESSAGE: ", gst_message_type_get_name (GST_MESSAGE_TYPE (msg)));

	switch (GST_MESSAGE_TYPE (msg)) {
	    case GST_MESSAGE_STATE_CHANGED: {
		GstState old_state,
			 new_state,
			 pending_state;
		gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
		logD (stream, _func, "STATE_CHANGED from ", gstStateToString (old_state), " "
		      "to ", gstStateToString (new_state), ", "
		      "pending state: ", gstStateToString (pending_state));
		if (pending_state == GST_STATE_VOID_PENDING) {
		    if (new_state == GST_STATE_PAUSED) {
			logD (bus, _func, "PAUSED");

#if 0
			bool do_seek = false;
			Time initial_seek = self->initial_seek;
			if (self->initial_seek_pending) {
			    self->initial_seek_pending = false;
			    do_seek = true;
			}
#endif

			if (self->initial_seek_pending) {
			    self->initial_seek_pending = false;
			    if (self->initial_seek > 0) {
				self->seek_pending = true;
			    } else {
				if (self->initial_play_pending) {
				    self->initial_play_pending = false;
				    self->play_pending = true;
				}
			    }
			} else {
			    if (self->initial_play_pending) {
				self->initial_play_pending = false;
				self->play_pending = true;
			    }
			}

			self->mutex.unlock ();

			if (self->frontend)
			    self->frontend.call (self->frontend->statusEvent);

			goto _return;

#if 0
#if 0
			GstElement * const tmp_playbin = self->playbin;
			gst_object_ref (tmp_playbin);
#endif
			self->mutex.unlock ();

			// TODO This looks like a minor race. Another seek may be requested
			//      at the same time.
			if (do_seek && initial_seek > 0) {
			    seek_pending = true;
//			    goto _return;
#if 0
			    logD_ (_func, "Calling gst_element_seek_simple()");
			    if (!gst_element_seek_simple (tmp_playbin,
							  GST_FORMAT_TIME,
							  (GstSeekFlags) (GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
							  (GstClockTime) initial_seek * 1000000000LL))
			    {
				logE_ (_func, "Seek failed");
			    }
			    logD_ (_func, "gst_element_seek_simple() returned");
#endif
			} else {
			    play_pending = true;
			}

#if 0
			if (gst_element_set_state (tmp_playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
			    logE_ (_func, "gst_element_set_state() failed (PLAYING)");

			gst_object_unref (tmp_playbin);
#endif
			goto _return;
#endif
		    } else
		    if (new_state == GST_STATE_PLAYING) {
			logD (bus, _func, "PLAYING");
		    }
		}
	    } break;
	    case GST_MESSAGE_EOS: {
		logD (stream, _func, "EOS");

		self->eos_pending = true;
		self->mutex.unlock ();

		if (self->frontend)
		    self->frontend.call (self->frontend->statusEvent);

		goto _return;
	    } break;
	    case GST_MESSAGE_ERROR: {
		logD (stream, _func, "ERROR");

		self->error_pending = true;
		self->mutex.unlock ();

		if (self->frontend)
		    self->frontend.call (self->frontend->statusEvent);

		goto _return;
	    } break;
	    default:
	      // No-op
		;
	}

	logD (bus, _func, "MSG DONE");
    }
    self->mutex.unlock ();

_return:
    return GST_BUS_PASS;
}

void
GstStream::noVideoTimerTick (void * const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    Time const time = getTime();

    self->mutex.lock ();
    logD (novideo, _func, "time: 0x", fmt_hex, time, ", last_frame_time: 0x", self->last_frame_time);

    if (self->stream_closed) {
	self->mutex.unlock ();
	return;
    }

    if (time > self->last_frame_time &&
	time - self->last_frame_time >= 15 /* TODO Config param for the timeout */)
    {
	logD (novideo, _func, "firing \"no video\" event");

	if (self->no_video_timer) {
	    self->timers->deleteTimer (self->no_video_timer);
	    self->no_video_timer = NULL;
	}

	self->no_video_pending = true;
	self->mutex.unlock ();

	self->reportStatusEvents ();
    } else {
	self->got_video_pending = true;
	self->mutex.unlock ();

	self->reportStatusEvents ();
    }
}

VideoStream::EventHandler GstStream::mix_stream_handler = {
    mixStreamAudioMessage,
    mixStreamVideoMessage,
    NULL /* rtmpCommandMessage */,
    NULL /* closed */,
    NULL /* numWatchersChanged */
};

void
GstStream::mixStreamAudioMessage (VideoStream::AudioMessage * const mt_nonnull /* audio_msg */,
				  void * const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    logD_ (_func_);

    self->mutex.lock ();
    if (!self->mix_audio_src) {
	self->mutex.unlock ();
	return;
    }

  // TODO

    self->mutex.unlock ();
}

void
GstStream::mixStreamVideoMessage (VideoStream::VideoMessage * const mt_nonnull video_msg,
				  void * const _self)
{
    GstStream * const self = static_cast <GstStream*> (_self);

    logD_ (_func_);

    self->mutex.lock ();
    if (!self->mix_video_src) {
	self->mutex.unlock ();
	return;
    }

    GstBuffer * const buffer = gst_buffer_new_and_alloc (video_msg->msg_len);
    assert (buffer);

    {
	PagePool *normalized_page_pool;
	PagePool::PageListHead normalized_pages;
	Size normalized_offset = 0;
	bool unref_normalized_pages;
	if (video_msg->prechunk_size == 0) {
	    logD_ (_func, "non-prechunked data");

	    normalized_pages = video_msg->page_list;
	    unref_normalized_pages = false;
	} else {
	    logD_ (_func, "prechunked data");

	    unref_normalized_pages = true;

	    RtmpConnection::normalizePrechunkedData (video_msg->page_pool,
						     &video_msg->page_list,
						     video_msg->msg_offset,
						     video_msg->prechunk_size,
						     video_msg->page_pool,
						     &normalized_page_pool,
						     &normalized_pages,
						     &normalized_offset);
	    // TODO This assertion should become unnecessary (support msg_offs
	    //      in PagePool::PageListArray).
	    assert (normalized_offset == 0);
	}

	PagePool::PageListArray pl_array (normalized_pages.first, video_msg->msg_len);
	if (video_msg->msg_len > 0)
	    pl_array.get (1 /* TEST FLV VIDEO TAG STRIPPING */ /* offset */, Memory (GST_BUFFER_DATA (buffer), video_msg->msg_len - 1));

	if (unref_normalized_pages)
	    normalized_page_pool->msgUnref (normalized_pages.first);
    }

    gst_buffer_set_caps (buffer, self->mix_video_caps);
    GST_BUFFER_TIMESTAMP (buffer) = (Uint64) video_msg->timestamp * 1000000;
    GST_BUFFER_SIZE (buffer) = video_msg->msg_len;
    GST_BUFFER_DURATION (buffer) = 0;

    if (video_msg->frame_type.isInterFrame())
	GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    logD_ (_func, "pushing buffer");
    GstFlowReturn const res = gst_app_src_push_buffer (self->mix_video_src, buffer);
    if (res != GST_FLOW_OK)
	logD_ (_func, "res: ", (unsigned long) res);

    self->mutex.unlock ();
}

void
GstStream::doCreatePipeline ()
{
    if (is_chain)
	createPipelineForChainSpec ();
    else
	createPipelineForUri ();
}

void
GstStream::createPipeline ()
{
    mutex.lock ();

    while (!workqueue_list.isEmpty()) {
        Ref<WorkqueueItem> &last_item = workqueue_list.getLast();
        switch (last_item->item_type) {
            case WorkqueueItem::ItemType_CreatePipeline:
                mutex.unlock ();
                return;
            case WorkqueueItem::ItemType_ReleasePipeline:
                mutex.unlock ();
                return;
            default:
                unreachable ();
        }
    }

    Ref<WorkqueueItem> const new_item = grab (new WorkqueueItem);
    new_item->item_type = WorkqueueItem::ItemType_CreatePipeline;

    workqueue_list.prepend (new_item);
    workqueue_cond.signal ();

    mutex.unlock ();
}

void
GstStream::reportStatusEvents ()
{
    logD (stream, _func_);

    mutex.lock ();
    if (reporting_status_events) {
	mutex.unlock ();
	return;
    }
    reporting_status_events = true;

    while (eos_pending       ||
	   error_pending     ||
	   no_video_pending  ||
	   got_video_pending ||
	   seek_pending      ||
	   play_pending)
    {
	if (close_notified) {
	    logD (stream, _func, "close_notified");

	    mutex.unlock ();
	    return;
	}

	if (eos_pending) {
	    logD (stream, _func, "eos_pending");
	    eos_pending = false;
	    close_notified = true;

	    if (frontend) {
		logD (stream, _func, "firing EOS");
		mt_unlocks_locks (mutex) frontend.call_mutex (frontend->eos, mutex);
	    }

	    break;
	}

	if (error_pending) {
	    logD (stream, _func, "error_pending");
	    error_pending = false;
	    close_notified = true;

	    if (frontend) {
		logD (stream, _func, "firing ERROR");
		mt_unlocks_locks (mutex) frontend.call_mutex (frontend->error, mutex);
	    }

	    break;
	}

	if (no_video_pending) {
	    if (got_video_pending)
		got_video_pending = false;

	    logD (stream, _func, "no_video_pending");
	    no_video_pending = false;
	    if (stream_closed) {
		mutex.unlock ();
		return;
	    }

	    if (frontend) {
		logD (stream, _func, "firing NO_VIDEO");
		mt_unlocks_locks (mutex) frontend.call_mutex (frontend->noVideo, mutex);
	    }
	}

	if (got_video_pending) {
	    logD (stream, _func, "got_video_pending");
	    got_video_pending = false;
	    if (stream_closed) {
		mutex.unlock ();
		return;
	    }

	    if (frontend)
		mt_unlocks_locks (mutex) frontend.call_mutex (frontend->gotVideo, mutex);
	}

	if (seek_pending) {
	    logD (stream, _func, "seek_pending");
	    assert (!play_pending);
	    seek_pending = false;
	    if (stream_closed) {
		mutex.unlock ();
		return;
	    }

	    if (initial_seek > 0) {
		logD (stream, _func, "initial_seek: ", initial_seek);

		Time const tmp_initial_seek = initial_seek;

		GstElement * const tmp_playbin = playbin;
		gst_object_ref (tmp_playbin);
		mutex.unlock ();

		bool seek_failed = false;
		if (!gst_element_seek_simple (tmp_playbin,
					      GST_FORMAT_TIME,
					      (GstSeekFlags) (GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
					      (GstClockTime) tmp_initial_seek * 1000000000LL))
		{
		    seek_failed = true;
		    logE_ (_func, "Seek failed");
		}

		gst_object_unref (tmp_playbin);
		mutex.lock ();
		if (seek_failed)
		    play_pending = true;
	    } else {
		play_pending = true;
	    }
	}

	if (play_pending) {
	    logD (stream, _func, "play_pending");
	    assert (!seek_pending);
	    play_pending = false;
	    if (stream_closed) {
		mutex.unlock ();
		return;
	    }

	    GstElement * const tmp_playbin = playbin;
	    gst_object_ref (tmp_playbin);
	    mutex.unlock ();

	    logD (stream, _func, "Setting pipeline state to PLAYING");
	    if (gst_element_set_state (tmp_playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		logE_ (_func, "gst_element_set_state() failed (PLAYING)");

	    gst_object_unref (tmp_playbin);
	    mutex.lock ();
	}
    }

    logD (stream, _func, "done");
    reporting_status_events = false;
    mutex.unlock ();
}

void
GstStream::getTrafficStats (TrafficStats * const ret_traffic_stats)
{
  StateMutexLock l (mutex);

    ret_traffic_stats->rx_bytes = rx_bytes;
    ret_traffic_stats->rx_audio_bytes = rx_audio_bytes;
    ret_traffic_stats->rx_video_bytes = rx_video_bytes;
}

void
GstStream::resetTrafficStats ()
{
    rx_bytes = 0;
    rx_audio_bytes = 0;
    rx_video_bytes = 0;
}

mt_const void
GstStream::init (ConstMemory   const stream_name,
		 ConstMemory   const stream_spec,
		 bool          const is_chain,
		 Timers      * const timers,
		 PagePool    * const page_pool,
		 VideoStream * const video_stream,
		 VideoStream * const mix_video_stream,
		 Time          const initial_seek,
		 bool          const send_metadata,
                 bool          const enable_prechunking,
		 Uint64        const default_width,
		 Uint64        const default_height,
		 Uint64        const default_bitrate,
		 Time          const no_video_timeout)
{
    this->stream_name = grab (new String (stream_name));

    this->stream_spec = grab (new String (stream_spec));
    this->is_chain = is_chain;

    this->timers = timers;
    this->page_pool = page_pool;
    this->video_stream = video_stream;
    this->mix_video_stream = mix_video_stream;

    this->send_metadata = send_metadata;
    this->enable_prechunking = enable_prechunking;

    this->default_width = default_width;
    this->default_height = default_height;
    this->default_bitrate = default_bitrate;

    this->no_video_timeout = no_video_timeout;

    this->initial_seek = initial_seek;

    {
        workqueue_thread =
                grab (new Thread (CbDesc<Thread::ThreadFunc> (workqueueThreadFunc,
                                                              this,
                                                              this)));
        if (!workqueue_thread->spawn (true /* joinable */))
            logE_ (_func, "Failed to spawn workqueue thread: ", exc->toString());
    }

    if (mix_video_stream) {
	mix_audio_caps = gst_caps_new_simple ("audio/x-speex",
#if 0
					      "rate", G_TYPE_INT, 16000,
					      "channels", G_TYPE_INT, 1,
#endif
					      NULL);

//	mix_video_caps = gst_caps_new_simple ("video/x-flv",
	mix_video_caps = gst_caps_new_simple ("video/x-flash-video",
#if 0
					      "width",  G_TYPE_INT, (int) default_width,
					      "height", G_TYPE_INT, (int) default_height,
#endif
					      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
					      NULL);

	mix_video_stream->getEventInformer()->subscribe (
		CbDesc<VideoStream::EventHandler> (
			&mix_stream_handler,
			this,
			this));
    }
}

GstStream::GstStream ()
    : timers    (this /* coderef_container */),
      page_pool (this /* coderef_container */),

      video_stream (NULL),
      mix_video_stream (NULL),

      mix_audio_caps (NULL),
      mix_video_caps (NULL),

      is_chain (false),

      send_metadata (false),
      enable_prechunking (false),

      default_width   (0),
      default_height  (0),
      default_bitrate (0),

      no_video_timeout (30),

      no_video_timer (NULL),

      playbin (NULL),
      audio_probe_id (0),
      video_probe_id (0),

      mix_audio_src (NULL),
      mix_video_src (NULL),

      initial_seek (0),
      initial_seek_pending (true),
      initial_play_pending (true),

      metadata_reported (false),

      last_frame_time (0),

      audio_codec_id (VideoStream::AudioCodecId::Unknown),
      audio_rate (44100),
      audio_channels (1),
      audio_hdr (0xbe /* Speex */),

      video_codec_id (VideoStream::VideoCodecId::Unknown),
      video_hdr (0x02 /* Sorenson H.263 */),

      got_in_stats (false),
      got_video (false),
      got_audio (false),

      first_audio_frame (true),
      audio_skip_counter (0),
      video_skip_counter (0),

      first_video_frame (true),

      prv_audio_timestamp (0),

      changing_state_to_playing (false),
      reporting_status_events (false),

      seek_pending (false),
      play_pending (false),

      no_video_pending (false),
      got_video_pending (false),
      error_pending  (false),
      eos_pending    (false),
      close_notified (false),

      stream_closed (false),

      rx_bytes (0),
      rx_audio_bytes (0),
      rx_video_bytes (0),

      tlocal (NULL)
{
}

GstStream::~GstStream ()
{
    logD (pipeline, _func, "~GstStream()");

    mutex.lock ();
    if (!stream_closed) {
        unreachable ();
    }
    mutex.unlock ();

    if (mix_audio_caps)
	gst_caps_unref (mix_audio_caps);
    if (mix_video_caps)
	gst_caps_unref (mix_video_caps);
}

}

