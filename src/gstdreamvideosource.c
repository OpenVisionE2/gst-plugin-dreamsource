/*
 * GStreamer dreamvideosource
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
#include "gstdreamvideosource.h"

GST_DEBUG_CATEGORY_STATIC (dreamvideosource_debug);
#define GST_CAT_DEFAULT dreamvideosource_debug

GType gst_dreamvideosource_input_mode_get_type (void)
{
	static volatile gsize input_mode_type = 0;
	static const GEnumValue input_mode[] = {
		{GST_DREAMVIDEOSOURCE_INPUT_MODE_LIVE, "GST_DREAMVIDEOSOURCE_INPUT_MODE_LIVE", "live"},
		{GST_DREAMVIDEOSOURCE_INPUT_MODE_HDMI_IN, "GST_DREAMVIDEOSOURCE_INPUT_MODE_HDMI_IN", "hdmi_in"},
		{GST_DREAMVIDEOSOURCE_INPUT_MODE_BACKGROUND, "GST_DREAMVIDEOSOURCE_INPUT_MODE_BACKGROUND", "background"},
		{0, NULL, NULL},
	};

	if (g_once_init_enter (&input_mode_type)) {
		GType tmp = g_enum_register_static ("GstDreamVideoSourceInputMode", input_mode);
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
	ARG_CAPS,
	ARG_BITRATE,
	ARG_INPUT_MODE
};

static guint gst_dreamvideosource_signals[LAST_SIGNAL] = { 0 };

#define DEFAULT_BITRATE    2048
#define DEFAULT_FRAMERATE  25
#define DEFAULT_WIDTH      1280
#define DEFAULT_HEIGHT     720
#define DEFAULT_INPUT_MODE GST_DREAMVIDEOSOURCE_INPUT_MODE_LIVE

static GstStaticPadTemplate srctemplate =
    GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS	("video/x-h264, "
	"width = { 720, 1280, 1920 }, "
	"height = { 576, 720, 1080 }, "
	"framerate = { 25/1, 30/1, 50/1, 60/1 }, "
	"pixel-aspect-ratio = { 5/4, 16/9 }, "
	"stream-format = (string) byte-stream, "
	"profile = (string) main")
    );

#define gst_dreamvideosource_parent_class parent_class
G_DEFINE_TYPE (GstDreamVideoSource, gst_dreamvideosource, GST_TYPE_PUSH_SRC);

static GstCaps *gst_dreamvideosource_getcaps (GstBaseSrc * psrc, GstCaps * filter);
static gboolean gst_dreamvideosource_setcaps (GstBaseSrc * bsrc, GstCaps * caps);
static GstCaps *gst_dreamvideosource_fixate (GstBaseSrc * bsrc, GstCaps * caps);

static gboolean gst_dreamvideosource_start (GstBaseSrc * bsrc);
static gboolean gst_dreamvideosource_stop (GstBaseSrc * bsrc);
static gboolean gst_dreamvideosource_unlock (GstBaseSrc * bsrc);
static void gst_dreamvideosource_dispose (GObject * gobject);
static GstFlowReturn gst_dreamvideosource_create (GstPushSrc * psrc, GstBuffer ** outbuf);

static void gst_dreamvideosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_dreamvideosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_dreamvideosource_change_state (GstElement * element, GstStateChange transition);
static gint64 gst_dreamvideosource_get_base_pts (GstDreamVideoSource *self);

static void
gst_dreamvideosource_class_init (GstDreamVideoSourceClass * klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;
	GstBaseSrcClass *gstbsrc_class;
	GstPushSrcClass *gstpush_src_class;

	gobject_class = (GObjectClass *) klass;
	gstelement_class = (GstElementClass *) klass;
	gstbsrc_class = (GstBaseSrcClass *) klass;
	gstpush_src_class = (GstPushSrcClass *) klass;

	gobject_class->set_property = gst_dreamvideosource_set_property;
	gobject_class->get_property = gst_dreamvideosource_get_property;
	gobject_class->dispose = gst_dreamvideosource_dispose;

	gst_element_class_add_pad_template (gstelement_class,
					    gst_static_pad_template_get (&srctemplate));

	gst_element_class_set_static_metadata (gstelement_class,
	    "Dream Video source", "Source/Video",
	    "Provide an h.264 video elementary stream from Dreambox encoder device",
	    "Andreas Frisch <fraxinas@opendreambox.org>");

	gstelement_class->change_state = gst_dreamvideosource_change_state;

	gstbsrc_class->get_caps = gst_dreamvideosource_getcaps;
 	gstbsrc_class->set_caps = gst_dreamvideosource_setcaps;
 	gstbsrc_class->fixate = gst_dreamvideosource_fixate;
	gstbsrc_class->start = gst_dreamvideosource_start;
	gstbsrc_class->stop = gst_dreamvideosource_stop;
	gstbsrc_class->unlock = gst_dreamvideosource_unlock;

	gstpush_src_class->create = gst_dreamvideosource_create;

	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
	  g_param_spec_int ("bitrate", "Bitrate (kb/s)",
	    "Bitrate in kbit/sec", 16, 200000, DEFAULT_BITRATE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, ARG_CAPS,
	  g_param_spec_boxed ("caps", "Caps",
	    "The caps for the source stream", GST_TYPE_CAPS,
	    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, ARG_INPUT_MODE,
	  g_param_spec_enum ("input-mode", "Input Mode",
	    "Select the input source of the video stream",
	    GST_TYPE_DREAMVIDEOSOURCE_INPUT_MODE, DEFAULT_INPUT_MODE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_dreamvideosource_signals[SIGNAL_GET_BASE_PTS] =
		g_signal_new ("get-base-pts",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDreamVideoSourceClass, get_base_pts),
		NULL, NULL, gst_dreamsource_marshal_INT64__VOID, G_TYPE_INT64, 0);

	klass->get_base_pts = gst_dreamvideosource_get_base_pts;
}

static gint64
gst_dreamvideosource_get_base_pts (GstDreamVideoSource *self)
{
	GST_DEBUG_OBJECT (self, "gst_dreamvideosource_get_base_pts %" GST_TIME_FORMAT"", GST_TIME_ARGS (self->base_pts) );
	return self->base_pts;
}

static void gst_dreamvideosource_set_bitrate (GstDreamVideoSource * self, uint32_t bitrate)
{
	if (!self->encoder || !self->encoder->fd)
		return;
	g_mutex_lock (&self->mutex);
	uint32_t vbr = bitrate*1000;
	int ret = ioctl(self->encoder->fd, VENC_SET_BITRATE, &vbr);
	if (ret != 0)
	{
		GST_WARNING_OBJECT (self, "can't set video bitrate to %i bytes/s!", vbr);
		g_mutex_unlock (&self->mutex);
		return;
	}
	GST_INFO_OBJECT (self, "set video bitrate to %i kBytes/s", bitrate);
	self->video_info.bitrate = vbr;
	g_mutex_unlock (&self->mutex);
}

static gboolean gst_dreamvideosource_set_format (GstDreamVideoSource * self, VideoFormatInfo * info)
{
	if (!self->encoder || !self->encoder->fd)
	{
		GST_ERROR_OBJECT (self, "can't set format because encoder device not opened!");
		return FALSE;
	}

	GST_OBJECT_LOCK (self);

	if (info->fps_n > 0)
	{
		int venc_fps = 0;
		switch (info->fps_n) {
			case 25:
				venc_fps = rate_25;
				break;
			case 30:
				venc_fps = rate_30;
				break;
			case 50:
				venc_fps = rate_50;
				break;
			case 60:
				venc_fps = rate_60;
				break;
			default:
				GST_ERROR_OBJECT (self, "invalid framerate %d/%d", info->fps_n, info->fps_d);
				goto fail;
		}
		if (!ioctl(self->encoder->fd, VENC_SET_FRAMERATE, &venc_fps))
			GST_INFO_OBJECT (self, "set framerate to %d/%d -> ioctrl(%d, VENC_SET_FRAMERATE, &%d)", info->fps_n, info->fps_d, self->encoder->fd, venc_fps);
		else
		{
			GST_WARNING_OBJECT (self, "can't set framerate to %d/%d -> ioctrl(%d, VENC_SET_FRAMERATE, &%d)", info->fps_n, info->fps_d, self->encoder->fd, venc_fps);
			goto fail;
		}
	}

	if (info->width && info->height)
	{
		int venc_size = 0;
		if ( info->width == 720 && info->height == 576 )
			venc_size = fmt_720x576;
		else if ( info->width == 1280 && info->height == 720)
			venc_size = fmt_1280x720;
		else if ( info->width == 1920 && info->height == 1080)
			venc_size = fmt_1920x1080;
		else
		{
			GST_ERROR_OBJECT (self, "invalid resolution %dx%d", info->width, info->height);
			goto fail;
		}
		if (!ioctl(self->encoder->fd, VENC_SET_RESOLUTION, &venc_size))
			GST_INFO_OBJECT (self, "set resolution to %dx%d -> ioctrl(%d, VENC_SET_RESOLUTION, &%d)", info->width, info->height, self->encoder->fd, venc_size);
		else
		{
			GST_WARNING_OBJECT (self, "can't set resolution to %dx%d -> ioctrl(%d, VENC_SET_RESOLUTION, &%d)", info->width, info->height, self->encoder->fd, venc_size);
			goto fail;
		}
	}

	self->video_info = *info;
	GST_OBJECT_UNLOCK (self);
	return TRUE;

fail:
	GST_OBJECT_UNLOCK (self);
	return FALSE;
}

void gst_dreamvideosource_set_input_mode (GstDreamVideoSource *self, GstDreamVideoSourceInputMode mode)
{
	g_return_if_fail (GST_IS_DREAMVIDEOSOURCE (self));
	GEnumValue *val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (GST_TYPE_DREAMVIDEOSOURCE_INPUT_MODE)), mode);
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
	int ret = ioctl(self->encoder->fd, VENC_SET_SOURCE, &int_mode);
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

GstDreamVideoSourceInputMode gst_dreamvideosource_get_input_mode (GstDreamVideoSource *self)
{
	GstDreamVideoSourceInputMode result;
	g_return_val_if_fail (GST_IS_DREAMVIDEOSOURCE (self), -1);
	GST_OBJECT_LOCK (self);
	result =self->input_mode;
	GST_OBJECT_UNLOCK (self);
	return result;
}

gboolean
gst_dreamvideosource_plugin_init (GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT (dreamvideosource_debug, "dreamvideosource", 0, "dreamvideosource");
	return gst_element_register (plugin, "dreamvideosource", GST_RANK_PRIMARY, GST_TYPE_DREAMVIDEOSOURCE);
}

static void
gst_dreamvideosource_init (GstDreamVideoSource * self)
{
	GstPadTemplate *pad_template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS(self), "src");
	self->current_caps = gst_pad_template_get_caps (pad_template);

	self->encoder = NULL;
	self->descriptors_available = 0;
	self->input_mode = DEFAULT_INPUT_MODE;

	self->buffers_in_use = 0;

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
	sprintf(fn_buf, "/dev/venc%d", 0);
	self->encoder->fd = open(fn_buf, O_RDWR | O_SYNC);
	if(self->encoder->fd <= 0) {
		GST_ERROR_OBJECT(self,"cannot open device %s (%s)", fn_buf, strerror(errno));
		free(self->encoder);
		self->encoder = NULL;
		return;
	}

	self->encoder->buffer = malloc(VBUFSIZE);
	if(!self->encoder->buffer) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer");
		return;
	}

	self->encoder->cdb = (unsigned char *)mmap(0, VMMAPSIZE, PROT_READ, MAP_PRIVATE, self->encoder->fd, 0);

	if(!self->encoder->cdb) {
		GST_ERROR_OBJECT(self,"cannot mmap cdb");
		return;
	}

#ifdef dump
	self->dumpfd = open("/media/hdd/movie/dreamvideosource.dump", O_WRONLY | O_CREAT | O_TRUNC);
	GST_DEBUG_OBJECT (self, "dumpfd = %i (%s)", self->dumpfd, (self->dumpfd > 0) ? "OK" : strerror(errno));
#endif
}

static void
gst_dreamvideosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (object);

	switch (prop_id) {
		case ARG_CAPS:
		{
			GstCaps *caps = gst_caps_copy (gst_value_get_caps (value));
			gst_dreamvideosource_setcaps(GST_BASE_SRC(object), caps);
			gst_caps_unref (caps);
			break;
		}
		case ARG_BITRATE:
			gst_dreamvideosource_set_bitrate(self, g_value_get_int (value));
			break;
		case ARG_INPUT_MODE:
			gst_dreamvideosource_set_input_mode (self, g_value_get_enum (value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
gst_dreamvideosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (object);

	switch (prop_id) {
		case ARG_CAPS:
			g_value_take_boxed (value, gst_dreamvideosource_getcaps (GST_BASE_SRC(object), GST_CAPS_ANY));
			break;
		case ARG_BITRATE:
			g_value_set_int (value, self->video_info.bitrate/1000);
			break;
		case ARG_INPUT_MODE:
			g_value_set_enum (value, gst_dreamvideosource_get_input_mode (self));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static GstCaps *
gst_dreamvideosource_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	GstCaps *caps = gst_caps_copy(self->current_caps);

	GST_LOG_OBJECT (self, "gst_dreamvideosource_getcaps %" GST_PTR_FORMAT " filter %" GST_PTR_FORMAT, caps, filter);

	if (filter) {
		GstCaps *intersection;
		intersection = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
		gst_caps_unref (caps);
		caps = intersection;
	}

	GST_DEBUG_OBJECT (self, "return caps %" GST_PTR_FORMAT, caps);
	return caps;
}

static gboolean
gst_dreamvideosource_setcaps (GstBaseSrc * bsrc, GstCaps * caps)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	GstBaseSrcClass *bclass = GST_BASE_SRC_GET_CLASS (bsrc);
	GstCaps *current_caps;
	const GstStructure *structure;
	VideoFormatInfo info;
	gboolean ret;
	int width, height;
	const GValue *framerate, *par;
	structure = gst_caps_get_structure (caps, 0);

	current_caps = gst_pad_get_current_caps (GST_BASE_SRC_PAD (bsrc));
	if (current_caps && gst_caps_is_equal (current_caps, caps)) {
		GST_DEBUG_OBJECT (self, "New caps equal to old ones: %" GST_PTR_FORMAT, caps);
		ret = TRUE;
	} else {
		GstState state;
		gst_element_get_state (GST_ELEMENT(self), &state, NULL, 1*GST_MSECOND);
		if (state == GST_STATE_PLAYING)
		{
			GST_WARNING_OBJECT (self, "can't change caps while in PLAYING state %" GST_PTR_FORMAT, caps);
			return TRUE;
		}
		else if (gst_structure_has_name (structure, "video/x-h264"))
		{
			memset (&info, 0, sizeof(VideoFormatInfo));
			ret = gst_structure_get_int (structure, "width", &info.width);
			ret &= gst_structure_get_int (structure, "height", &info.height);
			framerate = gst_structure_get_value (structure, "framerate");
			if (GST_VALUE_HOLDS_FRACTION(framerate)) {
				info.fps_n = gst_value_get_fraction_numerator (framerate);
				info.fps_d = gst_value_get_fraction_denominator (framerate);
			}
			GST_INFO_OBJECT (self, "set caps %" GST_PTR_FORMAT, caps);
			gst_caps_replace (&self->current_caps, caps);

			if (gst_dreamvideosource_set_format(self, &info) && gst_caps_is_fixed(caps))
				ret = gst_pad_push_event (bsrc->srcpad, gst_event_new_caps (caps));
		}
		else {
			GST_WARNING_OBJECT (self, "unsupported caps: %" GST_PTR_FORMAT, caps);
			ret = FALSE;
		}
	}
	if (current_caps)
		gst_caps_unref (current_caps);
	return ret;
}

static GstCaps *
gst_dreamvideosource_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	GstStructure *structure;

	caps = gst_caps_make_writable (caps);
	structure = gst_caps_get_structure (caps, 0);

	gst_structure_fixate_field_nearest_int (structure, "width", DEFAULT_WIDTH);
	gst_structure_fixate_field_nearest_int (structure, "height", DEFAULT_HEIGHT);
	gst_structure_fixate_field_nearest_fraction (structure, "framerate", DEFAULT_FRAMERATE, 1);
	gst_structure_fixate_field_nearest_fraction (structure, "pixel-aspect-ratio", DEFAULT_WIDTH, DEFAULT_HEIGHT);

	caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);
	GST_DEBUG_OBJECT (bsrc, "fixate caps: %" GST_PTR_FORMAT, caps);
	return caps;
}

static gboolean gst_dreamvideosource_unlock (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	GST_DEBUG_OBJECT (self, "stop creating buffers");
	SEND_COMMAND (self, CONTROL_STOP);
	return TRUE;
}

static void
gst_dreamvideosource_free_buffer (GstDreamVideoSource * self)
{
	self->buffers_in_use--;
	GST_TRACE_OBJECT (self, "gst_dreamvideosource_free_buffer buffer still in use: %i", self->buffers_in_use);
}

static GstFlowReturn
gst_dreamvideosource_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (psrc);
	EncoderInfo *enc = self->encoder;

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
				int rlen = read(enc->fd, enc->buffer, VBUFSIZE);
				if (rlen <= 0 || rlen % VBDSIZE ) {
					if ( errno == 512 )
						return GST_FLOW_FLUSHING;
					GST_WARNING_OBJECT (self, "read error %s (%i)", strerror(errno), errno);
					return GST_FLOW_ERROR;
				}
				self->descriptors_available = rlen / VBDSIZE;
				GST_LOG_OBJECT (self, "encoder buffer was empty, %d descriptors available", self->descriptors_available);
			}
		}

		while (self->descriptors_count < self->descriptors_available) {
			off_t offset = self->descriptors_count * VBDSIZE;
			VideoBufferDescriptor *desc = (VideoBufferDescriptor*)(&enc->buffer[offset]);

			uint32_t f = desc->stCommon.uiFlags;

			GST_LOG_OBJECT (self, "descriptors_count=%d, descriptors_available=%d\tuiOffset=%d, uiLength=%d", self->descriptors_count, self->descriptors_available, desc->stCommon.uiOffset, desc->stCommon.uiLength);

			if (G_UNLIKELY (f & CDB_FLAG_METADATA))
			{
				GST_LOG_OBJECT (self, "CDB_FLAG_METADATA... skip outdated packet");
				self->descriptors_count = self->descriptors_available;
				continue;
			}

			if (f & VBD_FLAG_DTS_VALID && desc->uiDTS)
			{
				if (G_UNLIKELY (self->base_pts == GST_CLOCK_TIME_NONE))
				{
					if (self->dreamaudiosrc)
					{
						g_mutex_lock (&self->mutex);
						guint64 audiosource_base_pts;
						g_signal_emit_by_name(self->dreamaudiosrc, "get-base-pts", &audiosource_base_pts);
						if (audiosource_base_pts != GST_CLOCK_TIME_NONE)
						{
							GST_DEBUG_OBJECT (self, "use DREAMAUDIOSOURCE's base_pts=%" GST_TIME_FORMAT "", GST_TIME_ARGS (audiosource_base_pts) );
							self->base_pts = audiosource_base_pts;
						}
						g_mutex_unlock (&self->mutex);
					}
					if (self->base_pts == GST_CLOCK_TIME_NONE)
					{
						self->base_pts = MPEGTIME_TO_GSTTIME(desc->uiDTS);
						GST_DEBUG_OBJECT (self, "use mpeg stream pts as base_pts=%" GST_TIME_FORMAT" (%lld)", GST_TIME_ARGS (self->base_pts), desc->uiDTS);
					}
				}
			}

			*outbuf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, enc->cdb, VMMAPSIZE, desc->stCommon.uiOffset, desc->stCommon.uiLength, self, (GDestroyNotify)gst_dreamvideosource_free_buffer);

			if (f & CDB_FLAG_PTS_VALID)
			{
				GstClockTime buffer_time = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS);
				if (self->base_pts != GST_CLOCK_TIME_NONE && buffer_time > self->base_pts )
				{
					buffer_time -= self->base_pts;
					GST_BUFFER_PTS(*outbuf) = buffer_time;
					GST_BUFFER_DTS(*outbuf) = buffer_time;
				}
			}
#ifdef dump
			int wret = write(self->dumpfd, (unsigned char*)(enc->cdb + desc->stCommon.uiOffset), desc->stCommon.uiLength);
			GST_LOG_OBJECT (self, "read %i dumped %i total %" G_GSIZE_FORMAT " ", desc->stCommon.uiLength, wret, gst_buffer_get_size (*outbuf) );
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
			self->buffers_in_use++;
			GST_DEBUG_OBJECT (self, "pushing %" GST_PTR_FORMAT "", *outbuf );
			return GST_FLOW_OK;
		}

	}
	return GST_FLOW_ERROR;
}

static GstStateChangeReturn gst_dreamvideosource_change_state (GstElement * element, GstStateChange transition)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (element);
	GstStateChangeReturn sret = GST_STATE_CHANGE_SUCCESS;
	int ret;
	GST_OBJECT_LOCK (self);
	switch (transition) {
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_PAUSED_TO_PLAYING");
			self->base_pts = GST_CLOCK_TIME_NONE;
			ret = ioctl(self->encoder->fd, VENC_START);
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
			ret = ioctl(self->encoder->fd, VENC_STOP);
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
gst_dreamvideosource_start (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	self->dreamaudiosrc = gst_bin_get_by_name_recurse_up(GST_BIN(GST_ELEMENT_PARENT(self)), "dreamaudiosource0");

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

	GST_DEBUG_OBJECT (self, "started. reference to dreamaudiosource=%" GST_PTR_FORMAT"", self->dreamaudiosrc);
	return TRUE;
}

static gboolean
gst_dreamvideosource_stop (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	if (self->dreamaudiosrc)
		gst_object_unref(self->dreamaudiosrc);
	close (READ_SOCKET (self));
	close (WRITE_SOCKET (self));
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;
	GST_DEBUG_OBJECT (self, "stop");
	return TRUE;
}

static void
gst_dreamvideosource_dispose (GObject * gobject)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (gobject);
	if (self->encoder) {
		if (self->encoder->buffer)
			free(self->encoder->buffer);
		if (self->encoder->cdb)
			munmap(self->encoder->cdb, VMMAPSIZE);
		if (self->encoder->fd)
			close(self->encoder->fd);
		free(self->encoder);
	}
#ifdef dump
	close(self->dumpfd);
#endif
	if (self->current_caps)
		gst_caps_unref(self->current_caps);
	g_mutex_clear (&self->mutex);
	GST_DEBUG_OBJECT (self, "disposed");
	G_OBJECT_CLASS (parent_class)->dispose (gobject);
}
