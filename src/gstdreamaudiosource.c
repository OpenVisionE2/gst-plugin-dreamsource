/*
 * GStreamer dreamaudiosource
 * Copyright 2014-2015 Andreas Frisch <fraxinas@opendreambox.org>
 *
 * This program is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 Unported
 * License. To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-nc-sa/3.0/ or send a letter to
 * Creative Commons,559 Nathan Abbott Way,Stanford,California 94305,USA.
 *
 * Alternatively, this program may be distributed and executed on
 * hardware which is licensed by Dream Property GmbH.
 *
 * This program is NOT free software. It is open source, you are allowed
 * to modify it (if you keep the license), but it may not be commercially
 * distributed other than under the conditions noted above.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include "gstdreamaudiosource.h"

GST_DEBUG_CATEGORY_STATIC (dreamaudiosource_debug);
#define GST_CAT_DEFAULT dreamaudiosource_debug

GType gst_dreamaudiosource_input_mode_get_type (void)
{
	static volatile gsize input_mode_type = 0;
	static const GEnumValue input_mode[] = {
		{GST_DREAMAUDIOSOURCE_INPUT_MODE_LIVE, "GST_DREAMAUDIOSOURCE_INPUT_MODE_LIVE", "live"},
		{GST_DREAMAUDIOSOURCE_INPUT_MODE_HDMI_IN, "GST_DREAMAUDIOSOURCE_INPUT_MODE_HDMI_IN", "hdmi_in"},
		{GST_DREAMAUDIOSOURCE_INPUT_MODE_BACKGROUND, "GST_DREAMAUDIOSOURCE_INPUT_MODE_BACKGROUND", "background"},
		{0, NULL, NULL},
	};

	if (g_once_init_enter (&input_mode_type)) {
		GType tmp = g_enum_register_static ("GstDreamAudioSourceInputMode", input_mode);
		g_once_init_leave (&input_mode_type, tmp);
	}
	return (GType) input_mode_type;
}

enum
{
	SIGNAL_GET_DTS_OFFSET,
	LAST_SIGNAL
};
enum
{
	ARG_0,
	ARG_BITRATE,
	ARG_INPUT_MODE
};

static guint gst_dreamaudiosource_signals[LAST_SIGNAL] = { 0 };

#define DEFAULT_BITRATE     128
#define DEFAULT_INPUT_MODE  GST_DREAMAUDIOSOURCE_INPUT_MODE_LIVE
#define DEFAULT_BUFFER_SIZE 16

static GstStaticPadTemplate srctemplate =
    GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS	("audio/mpeg, "
	"mpegversion = 4,"
	"stream-format = (string) adts")
    );

#define gst_dreamaudiosource_parent_class parent_class
G_DEFINE_TYPE (GstDreamAudioSource, gst_dreamaudiosource, GST_TYPE_PUSH_SRC);

static GstCaps *gst_dreamaudiosource_getcaps (GstBaseSrc * bsrc, GstCaps * filter);
static gboolean gst_dreamaudiosource_unlock (GstBaseSrc * bsrc);
static gboolean gst_dreamaudiosource_unlock_stop (GstBaseSrc * bsrc);
static void gst_dreamaudiosource_dispose (GObject * gobject);
static GstFlowReturn gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf);

static void gst_dreamaudiosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_dreamaudiosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_dreamaudiosource_change_state (GstElement * element, GstStateChange transition);
static gint64 gst_dreamaudiosource_get_dts_offset (GstDreamAudioSource *self);

static gboolean gst_dreamaudiosource_encoder_init (GstDreamAudioSource * self);
static void gst_dreamaudiosource_encoder_release (GstDreamAudioSource * self);

static void gst_dreamaudiosource_read_thread_func (GstDreamAudioSource * self);

#ifdef PROVIDE_CLOCK
static GstClock *gst_dreamaudiosource_provide_clock (GstElement * elem);
// static GstClockTime gst_dreamaudiosource_get_encoder_time_ (GstClock * clock, GstBaseSrc * bsrc);
#endif

static void
gst_dreamaudiosource_class_init (GstDreamAudioSourceClass * klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;
	GstBaseSrcClass *gstbasesrc_class;
	GstPushSrcClass *gstpush_src_class;

	gobject_class = (GObjectClass *) klass;
	gstelement_class = (GstElementClass *) klass;
	gstbasesrc_class = (GstBaseSrcClass *) klass;
	gstpush_src_class = (GstPushSrcClass *) klass;

	gobject_class->set_property = gst_dreamaudiosource_set_property;
	gobject_class->get_property = gst_dreamaudiosource_get_property;
	gobject_class->dispose = gst_dreamaudiosource_dispose;

	gst_element_class_add_pad_template (gstelement_class,
					    gst_static_pad_template_get (&srctemplate));

	gst_element_class_set_static_metadata (gstelement_class,
	    "Dream Audio source", "Source/Audio",
	    "Provide an audio elementary stream from Dreambox encoder device",
	    "Andreas Frisch <fraxinas@opendreambox.org>");

	gstelement_class->change_state = gst_dreamaudiosource_change_state;

	gstbasesrc_class->get_caps = gst_dreamaudiosource_getcaps;
	gstbasesrc_class->unlock = gst_dreamaudiosource_unlock;
	gstbasesrc_class->unlock_stop = gst_dreamaudiosource_unlock_stop;

	gstpush_src_class->create = gst_dreamaudiosource_create;

#ifdef PROVIDE_CLOCK
	gstelement_class->provide_clock = GST_DEBUG_FUNCPTR (gst_dreamaudiosource_provide_clock);
// 	g_type_class_ref (GST_TYPE_SYSTEM_CLOCK);
#endif

	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
	  g_param_spec_int ("bitrate", "Bitrate (kb/s)",
	    "Bitrate in kbit/sec", 16, 320, DEFAULT_BITRATE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, ARG_INPUT_MODE,
	  g_param_spec_enum ("input-mode", "Input Mode",
	    "Select the input source of the audio stream",
	    GST_TYPE_DREAMAUDIOSOURCE_INPUT_MODE, DEFAULT_INPUT_MODE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_dreamaudiosource_signals[SIGNAL_GET_DTS_OFFSET] =
		g_signal_new ("get-dts-offset",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDreamAudioSourceClass, get_dts_offset),
		NULL, NULL, gst_dreamsource_marshal_INT64__VOID, G_TYPE_INT64, 0);

	klass->get_dts_offset = gst_dreamaudiosource_get_dts_offset;
}

static gint64
gst_dreamaudiosource_get_dts_offset (GstDreamAudioSource *self)
{
	GST_DEBUG_OBJECT (self, "gst_dreamaudiosource_get_dts_offset %" GST_TIME_FORMAT"", GST_TIME_ARGS (self->dts_offset) );
	return self->dts_offset;
}

static void gst_dreamaudiosource_set_bitrate (GstDreamAudioSource * self, uint32_t bitrate)
{
	g_mutex_lock (&self->mutex);
	uint32_t abr = bitrate*1000;
	if (!self->encoder || !self->encoder->fd)
	{
		self->audio_info.bitrate = bitrate;
		g_mutex_unlock (&self->mutex);
		return;
	}

	int ret = ioctl(self->encoder->fd, AENC_SET_BITRATE, &abr);
	if (ret != 0)
	{
		GST_WARNING_OBJECT (self, "can't set audio bitrate to %i bytes/s!", abr);
		g_mutex_unlock (&self->mutex);
		return;
	}
	GST_INFO_OBJECT (self, "set audio bitrate to %i kBytes/s", bitrate);
	self->audio_info.bitrate = bitrate;
	g_mutex_unlock (&self->mutex);
}

void gst_dreamaudiosource_set_input_mode (GstDreamAudioSource *self, GstDreamAudioSourceInputMode mode)
{
	g_return_if_fail (GST_IS_DREAMAUDIOSOURCE (self));
	GEnumValue *val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (GST_TYPE_DREAMAUDIOSOURCE_INPUT_MODE)), mode);
	if (!val)
	{
		GST_ERROR_OBJECT (self, "no such input_mode %i!", mode);
		goto out;
	}
	const gchar *value_nick = val->value_nick;

	g_mutex_lock (&self->mutex);
	if (!self->encoder || !self->encoder->fd)
	{
		self->input_mode = mode;
		goto out;
	}
	int int_mode = mode;
	int ret = ioctl(self->encoder->fd, AENC_SET_SOURCE, &int_mode);
	if (ret != 0)
	{
		GST_WARNING_OBJECT (self, "can't set input mode to %s (%i) error: %s", value_nick, mode, strerror(errno));
		goto out;
	}
	GST_INFO_OBJECT (self, "successfully set input mode to %s (%i)", value_nick, mode);
	self->input_mode = mode;
out:
	g_mutex_unlock (&self->mutex);
	return;
}

GstDreamAudioSourceInputMode gst_dreamaudiosource_get_input_mode (GstDreamAudioSource *self)
{
	GstDreamAudioSourceInputMode result;
	g_return_val_if_fail (GST_IS_DREAMAUDIOSOURCE (self), -1);
	GST_OBJECT_LOCK (self);
	result =self->input_mode;
	GST_OBJECT_UNLOCK (self);
	return result;
}

gboolean
gst_dreamaudiosource_plugin_init (GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT (dreamaudiosource_debug, "dreamaudiosource", 0, "dreamaudiosource");
	return gst_element_register (plugin, "dreamaudiosource", GST_RANK_PRIMARY, GST_TYPE_DREAMAUDIOSOURCE);
}

static void
gst_dreamaudiosource_init (GstDreamAudioSource * self)
{
	self->encoder = NULL;
	self->descriptors_available = 0;
	self->input_mode = DEFAULT_INPUT_MODE;

	self->buffer_size = DEFAULT_BUFFER_SIZE;
	g_queue_init (&self->current_frames);
	self->readthread = NULL;

	g_mutex_init (&self->mutex);
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;

	gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
	gst_base_src_set_live (GST_BASE_SRC (self), TRUE);

	self->encoder = NULL;
	self->encoder_clock = NULL;

#ifdef dump
	self->dumpfd = open("/media/hdd/movie/dreamaudiosource.dump", O_WRONLY | O_CREAT | O_TRUNC);
	GST_DEBUG_OBJECT (self, "dumpfd = %i (%s)", self->dumpfd, (self->dumpfd > 0) ? "OK" : strerror(errno));
#endif
}

static gboolean gst_dreamaudiosource_encoder_init (GstDreamAudioSource * self)
{
	GST_LOG_OBJECT (self, "initializating encoder...");
	self->encoder = malloc(sizeof(EncoderInfo));

	if (!self->encoder) {
		GST_ERROR_OBJECT (self,"out of space");
		return FALSE;
	}

	char fn_buf[32];
	sprintf(fn_buf, "/dev/aenc%d", 0);
	self->encoder->fd = open(fn_buf, O_RDWR | O_SYNC);
	if (self->encoder->fd <= 0) {
		GST_ERROR_OBJECT (self,"cannot open device %s (%s)", fn_buf, strerror(errno));
		free(self->encoder);
		self->encoder = NULL;
		return FALSE;
	}

	self->encoder->buffer = malloc(ABUFSIZE);
	if (!self->encoder->buffer) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer");
		return FALSE;
	}

	self->encoder->cdb = (unsigned char *)mmap (0, AMMAPSIZE, PROT_READ, MAP_PRIVATE, self->encoder->fd, 0);

	if (!self->encoder->cdb || self->encoder->cdb == MAP_FAILED) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer: %s (%d)", strerror(errno));
		self->encoder->cdb = NULL;
		return FALSE;
	}

	int control_sock[2];
	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
	{
		GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE, (NULL), GST_ERROR_SYSTEM);
		return GST_STATE_CHANGE_FAILURE;
	}
	READ_SOCKET (self) = control_sock[0];
	WRITE_SOCKET (self) = control_sock[1];
	fcntl (READ_SOCKET (self), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (self), F_SETFL, O_NONBLOCK);

	self->memtrack_list = NULL;
	self->encoder->used_range_min = UINT32_MAX;
	self->encoder->used_range_max = 0;

	gst_dreamaudiosource_set_bitrate (self, self->audio_info.bitrate);
	gst_dreamaudiosource_set_input_mode (self, self->input_mode);

#ifdef PROVIDE_CLOCK
	self->encoder_clock = gst_dreamsource_clock_new ("GstDreamAudioSourceClock", self->encoder->fd);
	GST_DEBUG_OBJECT (self, "self->encoder_clock = %" GST_PTR_FORMAT, self->encoder_clock);
	GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
#endif

	GST_LOG_OBJECT (self, "encoder %s successfully initialized", fn_buf);
	return TRUE;
}

static void gst_dreamaudiosource_encoder_release (GstDreamAudioSource * self)
{
	GST_LOG_OBJECT (self, "releasing encoder...");
	if (self->encoder) {
		if (self->encoder->buffer)
			free(self->encoder->buffer);
		if (self->encoder->cdb)
			munmap(self->encoder->cdb, AMMAPSIZE);
		if (self->encoder->fd)
			close(self->encoder->fd);
		free(self->encoder);
	}
	g_list_free(self->memtrack_list);
	self->encoder = NULL;
	close (READ_SOCKET (self));
	close (WRITE_SOCKET (self));
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;
	if (self->encoder_clock) {
		gst_object_unref (self->encoder_clock);
		self->encoder_clock = NULL;
	}
}

static void
gst_dreamaudiosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (object);

	switch (prop_id) {
		case ARG_BITRATE:
			gst_dreamaudiosource_set_bitrate(self, g_value_get_int (value));
			break;
		case ARG_INPUT_MODE:
			     gst_dreamaudiosource_set_input_mode (self, g_value_get_enum (value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
gst_dreamaudiosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (object);

	switch (prop_id) {
		case ARG_BITRATE:
			g_value_set_int (value, self->audio_info.bitrate/1000);
			break;
		case ARG_INPUT_MODE:
			g_value_set_enum (value, gst_dreamaudiosource_get_input_mode (self));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static GstCaps *
gst_dreamaudiosource_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	GstPadTemplate *pad_template;
	GstCaps *caps;

	pad_template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS(self), "src");
	g_return_val_if_fail (pad_template != NULL, NULL);

	if (self->encoder == NULL) {
		GST_DEBUG_OBJECT (self, "encoder not opened -> use template caps");
		caps = gst_pad_template_get_caps (pad_template);
	}
	else
	{
		caps = gst_caps_make_writable(gst_pad_template_get_caps (pad_template));
	}

	GST_DEBUG_OBJECT (self, "return caps %" GST_PTR_FORMAT, caps);
	return caps;
}

static gboolean gst_dreamaudiosource_unlock (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	GST_DEBUG_OBJECT (self, "stop creating buffers");
	g_mutex_lock (&self->mutex);
	self->flushing = TRUE;
	GST_DEBUG_OBJECT (self, "set flushing TRUE");
	g_cond_signal (&self->cond);
	g_mutex_unlock (&self->mutex);
	return TRUE;
}

static gboolean gst_dreamaudiosource_unlock_stop (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	GST_DEBUG_OBJECT (self, "start creating buffers...");
	g_mutex_lock (&self->mutex);
	self->flushing = FALSE;
	g_queue_foreach (&self->current_frames, (GFunc) gst_buffer_unref, NULL);
	g_queue_clear (&self->current_frames);
	g_mutex_unlock (&self->mutex);
	return TRUE;
}

static void gst_dreamaudiosource_free_buffer (struct _buffer_memorytracker * memtrack)
{
	GstDreamAudioSource * self = memtrack->self;
	GST_OBJECT_LOCK(self);
	GST_TRACE_OBJECT (self, "freeing %" GST_PTR_FORMAT " uiOffset=%i uiLength=%i used_range_min=%i", memtrack->buffer, memtrack->uiOffset, memtrack->uiLength, self->encoder->used_range_min);
	GList *list = g_list_first (self->memtrack_list);
	guint abs_minimum = UINT32_MAX;
	guint abs_maximum = 0;
	struct _buffer_memorytracker * mt;
	int count = 0;
	self->memtrack_list = g_list_remove(list, memtrack);
	free(memtrack);
	list = g_list_first (self->memtrack_list);
	while (list) {
		mt = list->data;
		if (abs_minimum > 0 && mt->uiOffset < abs_minimum)
			abs_minimum = mt->uiOffset;
		if (mt->uiOffset+mt->uiLength > abs_maximum)
			abs_maximum = mt->uiOffset+mt->uiLength;
		count++;
		list = g_list_next (list);
	}
	GST_TRACE_OBJECT (self, "new abs_minimum=%i, abs_maximum=%i", abs_minimum, abs_maximum);
	self->encoder->used_range_min = abs_minimum;
	self->encoder->used_range_max = abs_maximum;
	GST_OBJECT_UNLOCK(self);
}

static void gst_dreamaudiosource_read_thread_func (GstDreamAudioSource * self)
{
	EncoderInfo *enc = self->encoder;
	GstBuffer *readbuf;

	if (!enc) {
		GST_WARNING_OBJECT (self, "encoder device not opened!");
		return;
	}

	GST_DEBUG_OBJECT (self, "enter read thread");

	GstMessage *message;
	GValue val = { 0 };

	message = gst_message_new_stream_status (GST_OBJECT_CAST (self), GST_STREAM_STATUS_TYPE_ENTER, GST_ELEMENT_CAST (GST_OBJECT_PARENT(self)));
	g_value_init (&val, GST_TYPE_G_THREAD);
	g_value_set_boxed (&val, self->readthread);
	gst_message_set_stream_status_object (message, &val);
	g_value_unset (&val);
	GST_DEBUG_OBJECT (self, "posting ENTER stream status");
	gst_element_post_message (GST_ELEMENT_CAST (self), message);
	GstClockTime clock_time;

	while (TRUE) {
		readbuf = NULL;
		{
			struct pollfd rfd[2];
			int timeout, nfds;

			rfd[0].fd = READ_SOCKET (self);
			rfd[0].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

			GST_DEBUG_OBJECT (self, "polling... self->descriptors_available=%i! fd=%i", self->descriptors_available, enc->fd);
			if (self->descriptors_available == 0)
			{
				rfd[1].fd = enc->fd;
				rfd[1].events = POLLIN;
				self->descriptors_count = 0;
				timeout = 200;
				nfds = 2;
			}
			else
			{
				rfd[1].revents = 0;
				nfds = 1;
				timeout = 0;
			}

			int ret = poll(rfd, nfds, timeout);

			if (G_UNLIKELY (ret == -1))
			{
				GST_ERROR_OBJECT (self, "SELECT ERROR!");
				goto stop_running;
			}
			else if ( ret == 0 && self->descriptors_available == 0 )
			{
				g_mutex_lock (&self->mutex);
				gst_clock_get_internal_time(self->encoder_clock);
				if (self->flushing)
				{
					GST_DEBUG_OBJECT (self, "FLUSHING!");
					g_cond_signal (&self->cond);
					g_mutex_unlock (&self->mutex);
					continue;
				}
				g_mutex_unlock (&self->mutex);
				GST_DEBUG_OBJECT (self, "SELECT TIMEOUT");
				//!!! TODO generate valid dummy payload
				readbuf = gst_buffer_new();
			}
			else if ( rfd[0].revents )
			{
				char command;
				READ_COMMAND (self, command, ret);
				if (command == CONTROL_STOP)
				{
					GST_DEBUG_OBJECT (self, "CONTROL_STOP!");
					goto stop_running;
				}
			}
			else if ( G_LIKELY(rfd[1].revents & POLLIN) )
			{
				clock_time = gst_clock_get_internal_time (self->encoder_clock);
				int rlen = read(enc->fd, enc->buffer, ABUFSIZE);
				if (rlen <= 0 || rlen % ABDSIZE ) {
					if ( errno == 512 )
						goto stop_running;
					GST_WARNING_OBJECT (self, "read error %s (%i)", strerror(errno), errno);
					goto stop_running;
				}
				self->descriptors_available = rlen / ABDSIZE;
				GST_LOG_OBJECT (self, "encoder buffer was empty, %d descriptors available", self->descriptors_available);
			}
		}

		while (self->descriptors_count < self->descriptors_available)
		{
			GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT(self));
			GstClockTime encoder_pts = GST_CLOCK_TIME_NONE;
			gint64 dts_pts_offset;

			off_t offset = self->descriptors_count * ABDSIZE;
			AudioBufferDescriptor *desc = (AudioBufferDescriptor*)(&enc->buffer[offset]);

			uint32_t f = desc->stCommon.uiFlags;

			if (G_UNLIKELY (f & CDB_FLAG_METADATA))
			{
				GST_LOG_OBJECT (self, "CDB_FLAG_METADATA... skip outdated packet");
				self->descriptors_count = self->descriptors_available;
				continue;
			}

			struct _buffer_memorytracker * memtrack = malloc(sizeof(struct _buffer_memorytracker));
			if (G_UNLIKELY (!memtrack))
			{
				GST_ERROR_OBJECT (self, "can't allocate buffer_memorytracker");
				goto stop_running;
			}

			GST_LOG_OBJECT (self, "descriptors_count=%d, descriptors_available=%d\tuiOffset=%d, uiLength=%d", self->descriptors_count, self->descriptors_available, desc->stCommon.uiOffset, desc->stCommon.uiLength);

			if (self->encoder->used_range_min == UINT32_MAX)
				self->encoder->used_range_min = desc->stCommon.uiOffset;

			if (self->encoder->used_range_max == 0)
				self->encoder->used_range_max = desc->stCommon.uiOffset+desc->stCommon.uiLength;

// 			if (desc->stCommon.uiOffset < self->encoder->used_range_max && desc->stCommon.uiOffset+desc->stCommon.uiLength > self->encoder->used_range_min)
// 			{
// 				GST_WARNING_OBJECT (self, "encoder overwrites buffer memory that is still in use! uiOffset=%i uiLength=%i used_range_min=%i used_range_max=%i", desc->stCommon.uiOffset, desc->stCommon.uiLength, self->encoder->used_range_min, self->encoder->used_range_max);
// 				self->descriptors_count++;
// 				readbuf = gst_buffer_new();
// 				continue;
// 			}

			// uiDTS since kernel driver booted
			if (f & CDB_FLAG_PTS_VALID)
			{
				encoder_pts = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS);
				GST_LOG_OBJECT (self, "f & CDB_FLAG_PTS_VALID && encoder's uiPTS=%" GST_TIME_FORMAT"", GST_TIME_ARGS(encoder_pts));

				g_mutex_lock (&self->mutex);
				if (G_UNLIKELY (self->dts_offset == GST_CLOCK_TIME_NONE))
				{
					if (self->dreamvideosrc)
					{
						guint64 videosource_dts_offset;
						g_signal_emit_by_name(self->dreamvideosrc, "get-dts-offset", &videosource_dts_offset);
						if (videosource_dts_offset != GST_CLOCK_TIME_NONE)
						{
							GST_DEBUG_OBJECT (self, "use DREAMVIDEOSOURCE's dts_offset=%" GST_TIME_FORMAT "", GST_TIME_ARGS (videosource_dts_offset) );
							self->dts_offset = videosource_dts_offset;
						}
					}
					if (self->dts_offset == GST_CLOCK_TIME_NONE)
					{
						self->dts_offset = encoder_pts;
						GST_DEBUG_OBJECT (self, "use mpeg stream pts as dts_offset=%" GST_TIME_FORMAT" (%lld)", GST_TIME_ARGS (self->dts_offset), desc->stCommon.uiPTS);
					}
				}
				g_mutex_unlock (&self->mutex);
			}

			if (G_UNLIKELY (self->dts_offset == GST_CLOCK_TIME_NONE))
			{
				GST_DEBUG_OBJECT (self, "dts_offset is still unknown, skipping frame...");
				self->descriptors_count++;
				break;
			}

			readbuf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, enc->cdb, AMMAPSIZE, desc->stCommon.uiOffset, desc->stCommon.uiLength, memtrack, (GDestroyNotify) gst_dreamaudiosource_free_buffer);

			memtrack->self = self;
			memtrack->buffer = readbuf;
			memtrack->uiOffset = desc->stCommon.uiOffset;
			memtrack->uiLength = desc->stCommon.uiLength;
			self->memtrack_list = g_list_append(self->memtrack_list, memtrack);

			if (encoder_pts != GST_CLOCK_TIME_NONE)
			{
				GstClockTime pts_clock_time = encoder_pts - self->dts_offset;
				GstClockTime internal, external;
				GstClockTime rate_n, rate_d;
				GstClockTimeDiff diff;

				gst_clock_get_calibration (self->encoder_clock, &internal, &external, &rate_n, &rate_d);
				if (internal > pts_clock_time) {
					diff = internal - pts_clock_time;
					diff = gst_util_uint64_scale (diff, rate_n, rate_d);
					pts_clock_time = external - diff;
				} else {
					diff = pts_clock_time - internal;
					diff = gst_util_uint64_scale (diff, rate_n, rate_d);
					pts_clock_time = external + diff;
				}

				GST_BUFFER_DTS(readbuf) = pts_clock_time - base_time;
				GST_BUFFER_PTS(readbuf) = pts_clock_time - base_time;

GST_LOG_OBJECT (self, "post-calibration\n"
"%" GST_TIME_FORMAT "=base_time       %" GST_TIME_FORMAT "=clock_time\n"
"%" GST_TIME_FORMAT "=encoder_pts     %" GST_TIME_FORMAT "=pts_clock_time     %" GST_TIME_FORMAT "=GST_BUFFER_PTS\n"
,
	GST_TIME_ARGS (base_time), GST_TIME_ARGS (clock_time),
	GST_TIME_ARGS (encoder_pts), GST_TIME_ARGS (pts_clock_time), GST_TIME_ARGS (GST_BUFFER_PTS(readbuf))
);
			}

#ifdef dump
			int wret = write(self->dumpfd, (unsigned char*)(enc->cdb + desc->stCommon.uiOffset), desc->stCommon.uiLength);
			GST_LOG_OBJECT (self, "read %i dumped %i total %" G_GSIZE_FORMAT " ", desc->stCommon.uiLength, wret, gst_buffer_get_size (*outbuf) );
#endif
			self->descriptors_count++;
			break;
		}

		if (self->descriptors_count == self->descriptors_available)
		{
			GST_LOG_OBJECT (self, "self->descriptors_count == self->descriptors_available -> release %i consumed descriptors", self->descriptors_count);
			/* release consumed descs */
			if (write(enc->fd, &self->descriptors_count, sizeof(self->descriptors_count)) != sizeof(self->descriptors_count)) {
				GST_WARNING_OBJECT (self, "release consumed descs write error!");
				goto stop_running;
			}
			self->descriptors_available = 0;
		}

		if (readbuf)
		{
			g_mutex_lock (&self->mutex);
			if (!self->flushing)
			{
				while (g_queue_get_length (&self->current_frames) >= self->buffer_size)
				{
					GstBuffer * oldbuf = g_queue_pop_head (&self->current_frames);
					GST_WARNING_OBJECT (self, "dropping %" GST_PTR_FORMAT " because of queue overflow! buffers count=%i", oldbuf, g_queue_get_length (&self->current_frames));
					gst_buffer_unref(oldbuf);
				}
				g_queue_push_tail (&self->current_frames, readbuf);
			}
			else
				gst_buffer_unref(readbuf);
			g_cond_signal (&self->cond);
			GST_INFO_OBJECT (self, "read %" GST_PTR_FORMAT " to queue", readbuf );
			g_mutex_unlock (&self->mutex);
		}
	}

	g_assert_not_reached ();
	return;

	stop_running:
	{
		g_mutex_unlock (&self->mutex);
		g_cond_signal (&self->cond);
		GST_DEBUG ("stop running, exit thread");
		message = gst_message_new_stream_status (GST_OBJECT_CAST (self), GST_STREAM_STATUS_TYPE_ENTER, GST_ELEMENT_CAST (GST_OBJECT_PARENT(self)));
		g_value_init (&val, GST_TYPE_G_THREAD);
		g_value_set_boxed (&val, self->readthread);
		gst_message_set_stream_status_object (message, &val);
		g_value_unset (&val);
		GST_DEBUG_OBJECT (self, "posting LEAVE stream status");
		gst_element_post_message (GST_ELEMENT_CAST (self), message);
		return;
	}
}

static GstFlowReturn
gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (psrc);

	GST_LOG_OBJECT (self, "new buffer requested");

	g_mutex_lock (&self->mutex);
	while (g_queue_is_empty (&self->current_frames) && !self->flushing)
	{
		g_cond_wait (&self->cond, &self->mutex);
		GST_INFO_OBJECT (self, "waiting for buffer from encoder");
	}

	*outbuf = g_queue_pop_head (&self->current_frames);
	g_mutex_unlock (&self->mutex);

	if (*outbuf)
	{
		GST_INFO_OBJECT (self, "pushing %" GST_PTR_FORMAT "", *outbuf );
		return GST_FLOW_OK;
	}
	GST_INFO_OBJECT (self, "FLUSHING");
	return GST_FLOW_FLUSHING;
}

static GstStateChangeReturn gst_dreamaudiosource_change_state (GstElement * element, GstStateChange transition)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (element);
	GstStateChangeReturn sret = GST_STATE_CHANGE_SUCCESS;
	int ret;

	switch (transition) {
		case GST_STATE_CHANGE_NULL_TO_READY:
		{
			if (!gst_dreamaudiosource_encoder_init (self))
				return GST_STATE_CHANGE_FAILURE;
			GST_DEBUG_OBJECT (self, "GST_STATE_CHANGE_NULL_TO_READY");
			break;
		}
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_READY_TO_PAUSED");
			self->dreamvideosrc = gst_bin_get_by_name_recurse_up(GST_BIN(GST_ELEMENT_PARENT(self)), "dreamvideosource0");

			self->dts_offset = GST_CLOCK_TIME_NONE;
#ifdef PROVIDE_CLOCK
			gst_element_post_message (element, gst_message_new_clock_provide (GST_OBJECT_CAST (element), self->encoder_clock, TRUE));
#endif
			self->flushing = TRUE;
			self->readthread = g_thread_try_new ("dreamaudiosrc-read", (GThreadFunc) gst_dreamaudiosource_read_thread_func, self, NULL);
			GST_DEBUG_OBJECT (self, "started readthread @%p", self->readthread );
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			g_mutex_lock (&self->mutex);
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_PAUSED_TO_PLAYING");
			GstClock *pipeline_clock = gst_element_get_clock (GST_ELEMENT (self));
			if (pipeline_clock)
			{
				if (pipeline_clock != self->encoder_clock)
				{
					gst_clock_set_master (self->encoder_clock, pipeline_clock);
					GST_DEBUG_OBJECT (self, "slaved to pipeline_clock %" GST_PTR_FORMAT "", pipeline_clock);
				}
				else
					GST_DEBUG_OBJECT (self, "encoder_clock is master clock");
			}
				else
					GST_WARNING_OBJECT (self, "no pipeline clock!");
			ret = ioctl(self->encoder->fd, AENC_START);
			if ( ret != 0 )
				goto fail;
			self->descriptors_available = 0;
			CLEAR_COMMAND (self);
			GST_INFO_OBJECT (self, "started encoder!");
			g_mutex_unlock (&self->mutex);
			break;
		default:
			break;
	}

	if (GST_ELEMENT_CLASS (parent_class)->change_state)
		sret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

	switch (transition) {
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			g_mutex_lock (&self->mutex);
			GST_DEBUG_OBJECT (self, "GST_STATE_CHANGE_PLAYING_TO_PAUSED self->descriptors_count=%i self->descriptors_available=%i", self->descriptors_count, self->descriptors_available);
			while (self->descriptors_count < self->descriptors_available)
			{
				GST_LOG_OBJECT (self, "flushing self->descriptors_count=%i");
				self->descriptors_count++;
			}
			if (self->descriptors_count)
				write(self->encoder->fd, &self->descriptors_count, sizeof(self->descriptors_count));
			ret = ioctl(self->encoder->fd, AENC_STOP);
			if ( ret != 0 )
				goto fail;
#ifdef PROVIDE_CLOCK
			gst_clock_set_master (self->encoder_clock, NULL);
#endif
			GST_INFO_OBJECT (self, "stopped encoder!");
			g_mutex_unlock (&self->mutex);
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_PAUSED_TO_READY");
#ifdef PROVIDE_CLOCK
			gst_element_post_message (element, gst_message_new_clock_lost (GST_OBJECT_CAST (element), self->encoder_clock));
#endif
			GST_DEBUG_OBJECT (self, "stopping readthread @%p...", self->readthread);
			SEND_COMMAND (self, CONTROL_STOP);
			g_thread_join (self->readthread);
			if (self->dreamvideosrc)
				gst_object_unref(self->dreamvideosrc);
			self->dreamvideosrc = NULL;
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			gst_dreamaudiosource_encoder_release (self);
			GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_READY_TO_NULL, close control sockets");
			break;
		default:
			break;
	}

	return sret;
fail:
	GST_ERROR_OBJECT(self,"can't perform encoder ioctl!");
	g_mutex_unlock (&self->mutex);
	return GST_STATE_CHANGE_FAILURE;
}

static void
gst_dreamaudiosource_dispose (GObject * gobject)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (gobject);
#ifdef dump
	close(self->dumpfd);
#endif
	g_mutex_clear (&self->mutex);
	g_cond_clear (&self->cond);
	GST_DEBUG_OBJECT (self, "disposed");
	G_OBJECT_CLASS (parent_class)->dispose (gobject);
}

#ifdef PROVIDE_CLOCK
static GstClock *gst_dreamaudiosource_provide_clock (GstElement * element)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (element);

	if (!self->encoder || self->encoder->fd < 0)
	{
		GST_DEBUG_OBJECT (self, "encoder device not started, can't provide clock!");
		return NULL;
	}

	return GST_CLOCK_CAST (gst_object_ref (self->encoder_clock));
}

#endif

