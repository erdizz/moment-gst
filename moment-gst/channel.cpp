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


#include <moment-gst/channel.h>


using namespace M;
using namespace Moment;

namespace MomentGst {

GstStreamCtl::Frontend Channel::gst_stream_ctl_frontend = {
    newVideoStream,
    streamError,
    streamEos
};

void
Channel::newVideoStream (void * const _advance_ticket,
			 void * const _self)
{
    Channel * const self = static_cast <Channel*> (_self);
  // TODO
//    self->fireNewVideoStream (video_stream);
}

void
Channel::streamError (void * const _advance_ticket,
		      void * const _self)
{
    Channel * const self = static_cast <Channel*> (_self);
    Playback::AdvanceTicket * const advance_ticket = static_cast <Playback::AdvanceTicket*> (_advance_ticket);

    logD_ (_func, "channel 0x", fmt_hex, (UintPtr) self, ", "
	   "advance_ticket: 0x", (UintPtr) advance_ticket);

  // No-op
}

void
Channel::streamEos (void * const _advance_ticket,
		    void * const _self)
{
    Channel * const self = static_cast <Channel*> (_self);
    Playback::AdvanceTicket * const advance_ticket = static_cast <Playback::AdvanceTicket*> (_advance_ticket);

    logD_ (_func, "channel 0x", fmt_hex, (UintPtr) self, ", "
	   "advance_ticket: 0x", (UintPtr) advance_ticket);

    self->playback.advance (advance_ticket);
}

Playback::Frontend Channel::playback_frontend = {
    startPlaybackItem,
    stopPlaybackItem
};

void
Channel::startPlaybackItem (Playlist::Item          * const item,
			    Time                      const seek,
			    Playback::AdvanceTicket * const advance_ticket,
			    void                    * const _self)
{
    logD_ (_func_);

    Channel * const self = static_cast <Channel*> (_self);

    bool const got_chain_spec = item->chain_spec && !item->chain_spec.isNull();
    bool const got_uri = item->uri && !item->uri.isNull();

    if (got_chain_spec && got_uri) {
	logW_ (_func, "Both chain spec and uri are specified for a playlist item. "
	       "Ignoring the uri.");
    }

    if (got_chain_spec) {
	self->stream_ctl->beginVideoStream (item->chain_spec->mem(),
					    true /* is_chain */,
					    advance_ticket /* stream_ticket */,
					    advance_ticket /* stream_ticket_ref */,
					    seek);
    } else
    if (got_uri) {
	self->stream_ctl->beginVideoStream (item->uri->mem(),
					    false /* is_chain */,
					    advance_ticket /* stream_ticket */,
					    advance_ticket /* stream_ticket_ref */,
					    seek);
    } else {
	logW_ (_func, "Nor chain spec, no uri is specified for a playlist item.");
	self->playback.advance (advance_ticket);
    }
}

void
Channel::stopPlaybackItem (void * const _self)
{
    logD_ (_func_);

    Channel * const self = static_cast <Channel*> (_self);
    self->stream_ctl->endVideoStream ();
}

void
Channel::fireNewVideoStream (VideoStream * const video_stream)
{
    new_video_stream_informer.informAll (informNewVideoStream, video_stream);
}

void
Channel::informNewVideoStream (NewVideoStreamCallback   const cb,
			       void                   * const cb_data,
			       void                   * const _video_stream)
{
    VideoStream * const video_stream = static_cast <VideoStream*> (_video_stream);
    cb (video_stream, cb_data);
}

mt_const void
Channel::init (MomentServer * const moment,
	       ConstMemory    const channel_name,
	       bool           const send_metadata,
	       Size           const default_width,
	       Size           const default_height,
	       Size           const default_bitrate)
{
    playback.init (moment->getServerApp()->getTimers());

    playback.setFrontend (CbDesc<Playback::Frontend> (
	    &playback_frontend, this /* cb_data */, this /* coderef_container */));

    stream_ctl = grab (new GstStreamCtl);
    stream_ctl->init (moment,
		      moment->getServerApp()->getMainThreadContext()->getDeferredProcessor(),
		      channel_name,
		      send_metadata,
		      default_width,
		      default_height,
		      default_bitrate);

    stream_ctl->setFrontend (CbDesc<GstStreamCtl::Frontend> (
	    &gst_stream_ctl_frontend, this /* cb_data */, this /* coderef_container */));
}

Channel::Channel ()
    : playback (this /* coderef_container */),
      new_video_stream_informer (this, &mutex)
{
}

}
