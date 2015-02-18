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
	SIGNAL_GET_BASE_PTS,
	LAST_SIGNAL
};
enum
{
	ARG_0,
	ARG_BITRATE,
	ARG_INPUT_MODE
};

static guint gst_dreamaudiosource_signals[LAST_SIGNAL] = { 0 };

#define DEFAULT_BITRATE    128
#define DEFAULT_INPUT_MODE GST_DREAMAUDIOSOURCE_INPUT_MODE_LIVE

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

static GstCaps *gst_dreamaudiosource_getcaps (GstBaseSrc * psrc, GstCaps * filter);
static gboolean gst_dreamaudiosource_start (GstBaseSrc * bsrc);
static gboolean gst_dreamaudiosource_stop (GstBaseSrc * bsrc);
static gboolean gst_dreamaudiosource_unlock (GstBaseSrc * bsrc);
static void gst_dreamaudiosource_dispose (GObject * gobject);
static GstFlowReturn gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf);

static void gst_dreamaudiosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_dreamaudiosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_dreamaudiosource_change_state (GstElement * element, GstStateChange transition);
static gint64 gst_dreamaudiosource_get_base_pts (GstDreamAudioSource *self);

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
	gstbasesrc_class->start = gst_dreamaudiosource_start;
	gstbasesrc_class->stop = gst_dreamaudiosource_stop;
	gstbasesrc_class->unlock = gst_dreamaudiosource_unlock;

	gstpush_src_class->create = gst_dreamaudiosource_create;

	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
	  g_param_spec_int ("bitrate", "Bitrate (kb/s)",
	    "Bitrate in kbit/sec", 16, 320, DEFAULT_BITRATE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, ARG_INPUT_MODE,
	  g_param_spec_enum ("input-mode", "Input Mode",
		"Select the input source of the audio stream",
		GST_TYPE_DREAMAUDIOSOURCE_INPUT_MODE, DEFAULT_INPUT_MODE,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_dreamaudiosource_signals[SIGNAL_GET_BASE_PTS] =
		g_signal_new ("get-base-pts",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDreamAudioSourceClass, get_base_pts),
		NULL, NULL, gst_dreamsource_marshal_INT64__VOID, G_TYPE_INT64, 0);

	klass->get_base_pts = gst_dreamaudiosource_get_base_pts;

}

static gint64
gst_dreamaudiosource_get_base_pts (GstDreamAudioSource *self)
{
	GST_DEBUG_OBJECT (self, "gst_dreamaudiosource_get_base_pts %" GST_TIME_FORMAT"", GST_TIME_ARGS (self->base_pts) );
	return self->base_pts;
}

static void gst_dreamaudiosource_set_bitrate (GstDreamAudioSource * self, uint32_t bitrate)
{
	if (!self->encoder || !self->encoder->fd)
		return;
	uint32_t abr = bitrate*1000;
	int ret = ioctl(self->encoder->fd, AENC_SET_BITRATE, &abr);
	if (ret != 0)
	{
		GST_WARNING_OBJECT (self, "can't set audio bitrate to %i bytes/s!", abr);
		return;
	}
	GST_INFO_OBJECT (self, "set audio bitrate to %i kBytes/s", bitrate);
	self->audio_info.bitrate = abr;
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
	GST_DEBUG_OBJECT (self, "setting input_mode to %s (%i)...", value_nick, mode);
	g_mutex_lock (&self->mutex);
	if (!self->encoder || !self->encoder->fd)
	{
		GST_ERROR_OBJECT (self, "can't set input mode because encoder device not opened!");
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
	self->buffers_list = NULL;
	self->descriptors_available = 0;
	self->input_mode = DEFAULT_INPUT_MODE;

	g_mutex_init (&self->mutex);
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;

	gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
	gst_base_src_set_live (GST_BASE_SRC (self), TRUE);

	self->encoder = malloc(sizeof(EncoderInfo));

	if(!self->encoder) {
		GST_ERROR_OBJECT(self,"out of space");
		return;
	}

	char fn_buf[32];
	sprintf(fn_buf, "/dev/aenc%d", 0);
	self->encoder->fd = open(fn_buf, O_RDWR | O_SYNC);
	if(self->encoder->fd <= 0) {
		GST_ERROR_OBJECT(self,"cannot open device %s (%s)", fn_buf, strerror(errno));
		free(self->encoder);
		self->encoder = NULL;
		return;
	}

	int flags = fcntl(self->encoder->fd, F_GETFL, 0);
	fcntl(self->encoder->fd, F_SETFL, flags | O_NONBLOCK);

	self->encoder->buffer = malloc(ABUFSIZE);
	if(!self->encoder->buffer) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer");
		return;
	}

	self->encoder->cdb = (unsigned char *)mmap(0, AMMAPSIZE, PROT_READ, MAP_PRIVATE, self->encoder->fd, 0);
	if(!self->encoder->cdb || self->encoder->cdb== MAP_FAILED) {
		GST_ERROR_OBJECT(self,"cannot mmap cdb: %s (%d)", strerror(errno));
		return;
	}

#ifdef dump
	self->dumpfd = open("/media/hdd/movie/dreamaudiosource.dump", O_WRONLY | O_CREAT | O_TRUNC);
	GST_DEBUG_OBJECT (self, "dumpfd = %i (%s)", self->dumpfd, (self->dumpfd > 0) ? "OK" : strerror(errno));
#endif
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
	SEND_COMMAND (self, CONTROL_STOP);
	return TRUE;
}

static void
gst_dreamaudiosource_free_buffer (struct _bufferdebug * bufferdebug)
{
	GST_OBJECT_LOCK (bufferdebug->self);
	GList *list = g_list_first (bufferdebug->self->buffers_list);
	int count = 0;
	while (list) {
		GST_TRACE_OBJECT (bufferdebug->self, "buffers_list[%i] = %" GST_PTR_FORMAT "", count, list->data);
		count++;
		list = g_list_next (list);
	}
	bufferdebug->self->buffers_list = g_list_remove(g_list_first (bufferdebug->self->buffers_list), bufferdebug->buffer);
	GST_TRACE_OBJECT (bufferdebug->self, "removing %" GST_PTR_FORMAT " @ %" GST_TIME_FORMAT " from list -> new length=%i", bufferdebug->buffer, GST_TIME_ARGS (bufferdebug->buffer_pts), g_list_length(bufferdebug->self->buffers_list));
	GST_OBJECT_UNLOCK (bufferdebug->self);
}

static GstFlowReturn
gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (psrc);
	EncoderInfo *enc = self->encoder;

	static int dumpoffset;

	GST_LOG_OBJECT (self, "new buffer requested");

	if (!enc) {
		GST_WARNING_OBJECT (self, "encoder device not opened!");
		return GST_FLOW_ERROR;
	}

	while (1)
	{
		*outbuf = NULL;

		if (self->descriptors_available == 0)
		{
			self->descriptors_count = 0;
			struct pollfd rfd[2];

			rfd[0].fd = enc->fd;
			rfd[0].events = POLLIN;
			rfd[1].fd = READ_SOCKET (self);
			rfd[1].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

			int ret = poll(rfd, 2, 200);

			if (G_UNLIKELY (ret == -1))
			{
				GST_ERROR_OBJECT (self, "SELECT ERROR!");
				return GST_FLOW_ERROR;
			}
			else if ( ret == 0 )
			{
				GST_LOG_OBJECT (self, "SELECT TIMEOUT");
				//!!! TODO generate dummy payload
				*outbuf = gst_buffer_new();
			}
			else if ( G_UNLIKELY (rfd[1].revents) )
			{
				char command;
				READ_COMMAND (self, command, ret);
				if (command == CONTROL_STOP)
				{
					GST_LOG_OBJECT (self, "CONTROL_STOP!");
					return GST_FLOW_FLUSHING;
				}
			}
			else if ( G_LIKELY(rfd[0].revents & POLLIN) )
			{
				int rlen = read(enc->fd, enc->buffer, ABUFSIZE);
				if (rlen <= 0 || rlen % ABDSIZE ) {
					if ( errno == 512 )
						return GST_FLOW_FLUSHING;
					GST_WARNING_OBJECT (self, "read error %s (%i)", strerror(errno), errno);
					return GST_FLOW_ERROR;
				}
				self->descriptors_available = rlen / ABDSIZE;
				GST_LOG_OBJECT (self, "encoder buffer was empty, %d descriptors available", self->descriptors_available);
			}
		}

		while (self->descriptors_count < self->descriptors_available) {
			off_t offset = self->descriptors_count * ABDSIZE;
			AudioBufferDescriptor *desc = (AudioBufferDescriptor*)(&enc->buffer[offset]);

			uint32_t f = desc->stCommon.uiFlags;

			GST_LOG_OBJECT (self, "descriptors_count=%d, descriptors_available=%u\tuiOffset=%u, uiLength=%"G_GSIZE_FORMAT"", self->descriptors_count, self->descriptors_available, desc->stCommon.uiOffset, desc->stCommon.uiLength);

			if (f & CDB_FLAG_METADATA) {
				GST_LOG_OBJECT (self, "CDB_FLAG_METADATA... skip outdated packet");
				self->descriptors_count = self->descriptors_available;
				continue;
			}

			struct _bufferdebug * bdg = NULL;
			GDestroyNotify buffer_free_func = NULL;
			if ( gst_debug_category_get_threshold (dreamaudiosource_debug) >= GST_LEVEL_TRACE)
			{
				bdg = malloc(sizeof(struct _bufferdebug));
				buffer_free_func = (GDestroyNotify) gst_dreamaudiosource_free_buffer;
			}

			*outbuf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, enc->cdb, AMMAPSIZE, desc->stCommon.uiOffset, desc->stCommon.uiLength, bdg, buffer_free_func);

			if (bdg)
			{
				bdg->self = self;
				bdg->buffer = *outbuf;
				self->buffers_list = g_list_append(self->buffers_list, *outbuf);
			}

			if (f & CDB_FLAG_PTS_VALID)
			{
				if (G_UNLIKELY (self->base_pts == GST_CLOCK_TIME_NONE))
				{
					if (self->dreamvideosrc)
					{
						g_mutex_lock (&self->mutex);
						guint64 videosource_base_pts;
						g_signal_emit_by_name(self->dreamvideosrc, "get-base-pts", &videosource_base_pts);
						if (videosource_base_pts != GST_CLOCK_TIME_NONE)
						{
							GST_DEBUG_OBJECT (self, "use DREAMVIDEOSOURCE's base_pts=%" GST_TIME_FORMAT "", GST_TIME_ARGS (videosource_base_pts) );
							self->base_pts = videosource_base_pts;
						}
						g_mutex_unlock (&self->mutex);
					}
					else
					{
						self->base_pts = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS);
						GST_DEBUG_OBJECT (self, "use mpeg stream pts as base_pts=%" GST_TIME_FORMAT"", GST_TIME_ARGS (self->base_pts) );
					}
				}
				GstClockTime buffer_time = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS);
				GST_INFO_OBJECT (self, "f & CDB_FLAG_PTS_VALID buffer_time=%" GST_TIME_FORMAT " condition?%i", GST_TIME_ARGS (buffer_time), buffer_time > self->base_pts);
				if (self->base_pts != GST_CLOCK_TIME_NONE && buffer_time > self->base_pts )
				{
					buffer_time -= self->base_pts;
					GST_BUFFER_PTS(*outbuf) = buffer_time;
					GST_BUFFER_DTS(*outbuf) = buffer_time;
				}
				if (bdg)
					bdg->buffer_pts = buffer_time;
			}

#ifdef dump
			int wret = write(self->dumpfd, (unsigned char*)(enc->cdb + desc->stCommon.uiOffset), desc->stCommon.uiLength);
			dumpoffset += wret;
			GST_LOG_OBJECT (self, "read %"G_GSIZE_FORMAT" dumped %i total 0x%08X", desc->stCommon.uiLength, wret, dumpoffset );
#endif
			self->descriptors_count++;

			break;
		}

		if (self->descriptors_count == self->descriptors_available) {
			GST_LOG_OBJECT (self, "self->descriptors_count == self->descriptors_available -> release %i consumed descriptors", self->descriptors_count);
			/* release consumed descs */
			if (write(enc->fd, &self->descriptors_count, sizeof(self->descriptors_count)) != sizeof(self->descriptors_count)) {
				GST_WARNING_OBJECT (self, "release consumed descs write error!");
				return GST_FLOW_ERROR;
			}
			self->descriptors_available = 0;
		}

		if (*outbuf)
		{
			GST_DEBUG_OBJECT (self, "pushing %" GST_PTR_FORMAT "", *outbuf );
			return GST_FLOW_OK;
		}
	}

	return GST_FLOW_ERROR;
}

static GstStateChangeReturn gst_dreamaudiosource_change_state (GstElement * element, GstStateChange transition)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (element);
	GstStateChangeReturn sret = GST_STATE_CHANGE_SUCCESS;
	int ret;
	GST_OBJECT_LOCK (self);
	switch (transition) {
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_PAUSED_TO_PLAYING");
			self->base_pts = GST_CLOCK_TIME_NONE;
			ret = ioctl(self->encoder->fd, AENC_START);
			if ( ret != 0 )
				goto fail;
			self->descriptors_available = 0;
			CLEAR_COMMAND (self);
			GST_INFO_OBJECT (self, "started encoder!");
			break;
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			GST_DEBUG_OBJECT (self, "GST_STATE_CHANGE_PLAYING_TO_PAUSED self->descriptors_count=%i self->descriptors_available=%i", self->descriptors_count, self->descriptors_available);
			while (self->descriptors_count < self->descriptors_available)
				GST_LOG_OBJECT (self, "flushing self->descriptors_count=%i", self->descriptors_count++);
			if (self->descriptors_count)
				write(self->encoder->fd, &self->descriptors_count, sizeof(self->descriptors_count));
			ret = ioctl(self->encoder->fd, AENC_STOP);
			if ( ret != 0 )
				goto fail;
			GST_INFO_OBJECT (self, "stopped encoder!");
			break;
		default:
			break;
	}
	GST_OBJECT_UNLOCK (self);
	if (GST_ELEMENT_CLASS (parent_class)->change_state)
		sret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
	return sret;
fail:
	GST_ERROR_OBJECT(self,"can't perform encoder ioctl!");
	GST_OBJECT_UNLOCK (self);
	return GST_STATE_CHANGE_FAILURE;
}

static gboolean
gst_dreamaudiosource_start (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	self->dreamvideosrc = gst_bin_get_by_name_recurse_up(GST_BIN(GST_ELEMENT_PARENT(self)), "dreamvideosource0");

	int control_sock[2];
	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
	{
		GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE, (NULL), GST_ERROR_SYSTEM);
		return FALSE;
	}
	READ_SOCKET (self) = control_sock[0];
	WRITE_SOCKET (self) = control_sock[1];
	fcntl (READ_SOCKET (self), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (self), F_SETFL, O_NONBLOCK);

	GST_DEBUG_OBJECT (self, "started. reference to dreamvideosource=%" GST_PTR_FORMAT "", self->dreamvideosrc);
	return TRUE;
}

static gboolean
gst_dreamaudiosource_stop (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	if (self->dreamvideosrc)
		gst_object_unref(self->dreamvideosrc);
	close (READ_SOCKET (self));
	close (WRITE_SOCKET (self));
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;
	GST_DEBUG_OBJECT (self, "stop");
	return TRUE;
}

static void
gst_dreamaudiosource_dispose (GObject * gobject)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (gobject);
	if (self->encoder) {
		if (self->encoder->buffer)
			free(self->encoder->buffer);
		if (self->encoder->cdb)
			munmap(self->encoder->cdb, AMMAPSIZE);
		if (self->encoder->fd)
			close(self->encoder->fd);
		free(self->encoder);
	}
#ifdef dump
	close(self->dumpfd);
#endif
	g_list_free(self->buffers_list);
	g_mutex_clear (&self->mutex);
	GST_DEBUG_OBJECT (self, "disposed");
	G_OBJECT_CLASS (parent_class)->dispose (gobject);
}
