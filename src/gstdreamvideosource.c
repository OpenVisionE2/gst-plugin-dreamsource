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
 * hardware which is licensed by Dream Multimedia GmbH.
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
};

static guint gst_dreamvideosource_signals[LAST_SIGNAL] = { 0 };


#define DEFAULT_BITRATE 2048
#define DEFAULT_FRAMERATE 25

static GstStaticPadTemplate srctemplate =
    GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS	("video/x-h264, "
	"width = { 720, 1280, 1920 }, "
	"height = { 576, 720, 1080 }, "
	"framerate = { 1/25, 1/30, 1/50, 1/60 }, "
	"pixel-aspect-ratio = { 5/4, 16/9 }, "
	"stream-format = (string) byte-stream, "
	"profile = (string) main")
    );

#define gst_dreamvideosource_parent_class parent_class
G_DEFINE_TYPE (GstDreamVideoSource, gst_dreamvideosource, GST_TYPE_PUSH_SRC);

static GstCaps *gst_dreamvideosource_getcaps (GstBaseSrc * psrc, GstCaps * filter);
static gboolean gst_dreamvideosource_setcaps (GstBaseSrc * bsrc, GstCaps * caps);
static GstCaps *gst_dreamvideosource_fixate (GstBaseSrc * bsrc, GstCaps * caps);
static gboolean gst_dreamvideosource_negotiate (GstBaseSrc * bsrc);

static gboolean gst_dreamvideosource_start (GstBaseSrc * bsrc);
static gboolean gst_dreamvideosource_stop (GstBaseSrc * bsrc);
static void gst_dreamvideosource_finalize (GObject * gobject);
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
	gobject_class->finalize = gst_dreamvideosource_finalize;

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
	gstbsrc_class->negotiate = gst_dreamvideosource_negotiate;
	gstbsrc_class->start = gst_dreamvideosource_start;
	gstbsrc_class->stop = gst_dreamvideosource_stop;

	gstpush_src_class->create = gst_dreamvideosource_create;
	
	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
	  g_param_spec_int ("bitrate", "Bitrate (kb/s)",
	    "Bitrate in kbit/sec", 16, 200000, DEFAULT_BITRATE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

 	g_object_class_install_property (gobject_class, ARG_CAPS,
 	  g_param_spec_boxed ("caps", "Caps",
 	    "The caps for the source stream", GST_TYPE_CAPS,
 	    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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
	GST_DEBUG_OBJECT (self, "gst_dreamvideosource_get_base_pts " GST_TIME_FORMAT"", GST_TIME_ARGS (self->base_pts) );
	return self->base_pts;
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
	self->encoder = NULL;
	self->descriptors_available = 0;
	self->video_info.width = 1280;
	self->video_info.height = 720;
	self->video_info.par_n = 16;
	self->video_info.par_d = 9;
	self->video_info.fps_n = 1;
	self->video_info.fps_d = 25;
	self->base_pts = GST_CLOCK_TIME_NONE;

	g_mutex_init (&self->mutex);
	
	gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
	gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
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
	
	GstState state;
	gst_element_get_state (GST_ELEMENT(self), &state, NULL, 1*GST_MSECOND);
	
	if (state != GST_STATE_PAUSED)
	{
		GST_ERROR_OBJECT (self, "can't set format in %s state. must be in PAUSED!", gst_element_state_get_name (state));
		return FALSE;
	}
	
	GST_OBJECT_LOCK (self);
// 	g_mutex_lock (&self->mutex);

	GST_INFO_OBJECT (self, "requested to set resolution to %dx%d, framerate to %d/%d aspect_ratio %d/%d", info->width, info->height, info->fps_n, info->fps_d, info->par_n, info->par_d);
	
	if ( (info->par_n == 5 && info->par_d == 4) || (info->par_n == 16 && info->par_d == 9) )
	{
		int venc_size = 0, venc_fps = 0;
		switch (info->fps_d) {
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
			
		if (!ioctl(self->encoder->fd, VENC_SET_FRAMERATE, &venc_fps))
			GST_INFO_OBJECT (self, "set framerate to %d/%d -> ioctrl(%d, VENC_SET_FRAMERATE, &%d)", info->fps_n, info->fps_d, self->encoder->fd, venc_fps);
		else
		{
			GST_WARNING_OBJECT (self, "can't set framerate to %d/%d -> ioctrl(%d, VENC_SET_FRAMERATE, &%d)", info->fps_n, info->fps_d, self->encoder->fd, venc_fps);
			goto fail;
		}

		if (!ioctl(self->encoder->fd, VENC_SET_RESOLUTION, &venc_size))
			GST_INFO_OBJECT (self, "set resolution to %dx%d -> ioctrl(%d, VENC_SET_RESOLUTION, &%d)", info->width, info->height, self->encoder->fd, venc_size);
		else
		{
			GST_WARNING_OBJECT (self, "can't set resolution to %dx%d -> ioctrl(%d, VENC_SET_RESOLUTION, &%d)", info->width, info->height, self->encoder->fd, venc_size);
			goto fail;
		}
		
		self->video_info = *info;
// 		g_mutex_unlock (&self->mutex);
		GST_OBJECT_UNLOCK (self);
		return TRUE;
	}
	
	GST_INFO_OBJECT (self, "invalid aspect!");
	
fail:
// 	g_mutex_unlock (&self->mutex);
	GST_OBJECT_UNLOCK (self);
	return FALSE;
}

static void
gst_dreamvideosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (object);
	
	switch (prop_id) {
		case ARG_CAPS:
		{
			GstCaps *caps = gst_caps_copy(gst_value_get_caps (value));
			gst_dreamvideosource_setcaps (GST_BASE_SRC(object), caps);
			gst_caps_unref(caps);
			break;
		}
		case ARG_BITRATE:
			gst_dreamvideosource_set_bitrate(self, g_value_get_int (value));
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
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static GstCaps *
gst_dreamvideosource_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	GstPadTemplate *pad_template;
	GstCaps *caps;

	pad_template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS(self), "src");
	g_return_val_if_fail (pad_template != NULL, NULL);
	
	GST_LOG_OBJECT (self, "gst_dreamvideosource_getcaps filter %" GST_PTR_FORMAT, caps);
	
	caps = gst_pad_template_get_caps (pad_template);
	
	if (self->encoder && self->video_info.width && self->video_info.height & self->video_info.fps_d)
	{
		caps = gst_caps_make_writable(gst_pad_template_get_caps (pad_template));
		gst_caps_set_simple(caps, "width", G_TYPE_INT, self->video_info.width, NULL);
		gst_caps_set_simple(caps, "height", G_TYPE_INT, self->video_info.height, NULL);
		gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, self->video_info.fps_n, self->video_info.fps_d, NULL);
		gst_caps_set_simple(caps, "pixel-aspect-ratio", GST_TYPE_FRACTION, self->video_info.par_n, self->video_info.par_d, NULL);
	}
	
	if (filter) {
		GstCaps *intersection;
		intersection = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
		gst_caps_unref (caps);
		caps = intersection;
	}	

// 	if (self->encoder == NULL)
// 	{
// 		caps = gst_pad_template_get_caps (pad_template);
// 		GST_LOG_OBJECT (self, "encoder not opened -> use template caps %" GST_PTR_FORMAT, caps);
// 		return caps;
// 	}
// 	else if (!self->video_info.width || !self->video_info.height || !self->video_info.fps_d)
// 	{
// 		caps = gst_pad_template_get_caps (pad_template);
// 		GST_LOG_OBJECT (self, "invalid video_info! -> use template caps %" GST_PTR_FORMAT, caps);
// 		return caps;
// 	}
// 	else


	GST_INFO_OBJECT (self, "return caps %" GST_PTR_FORMAT, caps);
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
		if (gst_structure_has_name (structure, "video/x-h264"))
		{
			memset (&info, 0, sizeof(VideoFormatInfo));
			ret = gst_structure_get_int (structure, "width", &info.width);
			ret &= gst_structure_get_int (structure, "height", &info.height);
			framerate = gst_structure_get_value (structure, "framerate");
			if (framerate) {
				info.fps_n = gst_value_get_fraction_numerator (framerate);
				info.fps_d = gst_value_get_fraction_denominator (framerate);
			}
			else {
				info.fps_n = 1;
				info.fps_d = DEFAULT_FRAMERATE;
			}
			par = gst_structure_get_value (structure, "pixel-aspect-ratio");
			if (par) {
				info.par_n = gst_value_get_fraction_numerator (par);
				info.par_d = gst_value_get_fraction_denominator (par);
			}
			else {
				info.par_n = 16;
				info.par_d = 9;
			}
			GST_INFO_OBJECT (self, "set caps %" GST_PTR_FORMAT, caps);
			if (gst_dreamvideosource_set_format(self, &info))
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
	GstStructure *structure;

	caps = gst_caps_make_writable (caps);
	structure = gst_caps_get_structure (caps, 0);

	gst_structure_fixate_field_nearest_int (structure, "width", 1280);
	gst_structure_fixate_field_nearest_int (structure, "height", 720);
	gst_structure_fixate_field_nearest_fraction (structure, "framerate", DEFAULT_FRAMERATE, 1);

	if (gst_structure_has_field (structure, "pixel-aspect-ratio"))
		gst_structure_fixate_field_nearest_fraction (structure, "pixel-aspect-ratio", 16, 9);
	else
		gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 16, 9, NULL);

	caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);
	GST_DEBUG_OBJECT (bsrc, "fixate caps: %" GST_PTR_FORMAT, caps);
	return caps;
}

static gboolean
gst_dreamvideosource_negotiate (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);

	GstState state;
	GstCaps *thiscaps;
	GstCaps *caps = NULL;
	GstCaps *peercaps = NULL;
	gboolean result = FALSE;
	
	gst_element_get_state (GST_ELEMENT(self), &state, NULL, 1*GST_MSECOND);
	
	if (state == GST_STATE_PLAYING)
		return TRUE;

	thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (bsrc), NULL);
	GST_DEBUG_OBJECT (bsrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
	
	if (thiscaps == NULL || gst_caps_is_any (thiscaps))
		goto no_nego_needed;
	
	peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (bsrc), NULL);
	GST_DEBUG_OBJECT (bsrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
	
	if (peercaps && !gst_caps_is_any (peercaps)) {
		GstCaps *icaps = NULL;
		int i;
		
		/* Prefer the first caps we are compatible with that the peer proposed */
		for (i = 0; i < gst_caps_get_size (peercaps); i++) {
			/* get intersection */
			GstCaps *ipcaps = gst_caps_copy_nth (peercaps, i);
			
			GST_DEBUG_OBJECT (bsrc, "peer: %" GST_PTR_FORMAT, ipcaps);
			
			icaps = gst_caps_intersect (thiscaps, ipcaps);
			gst_caps_unref (ipcaps);
			
			if (!gst_caps_is_empty (icaps))
				break;
			
			gst_caps_unref (icaps);
			icaps = NULL;
		}
		
		GST_DEBUG_OBJECT (bsrc, "intersect: %" GST_PTR_FORMAT, icaps);
		if (icaps) {
			/* If there are multiple intersections pick the one with the smallest
			 * resolution strictly bigger then the first peer caps */
			if (gst_caps_get_size (icaps) > 1) {
				GstStructure *s = gst_caps_get_structure (peercaps, 0);
				int best = 0;
				int twidth, theight;
				int width = G_MAXINT, height = G_MAXINT;
				
				if (gst_structure_get_int (s, "width", &twidth) && gst_structure_get_int (s, "height", &theight)) {
					
						/* Walk the structure backwards to get the first entry of the
						* smallest resolution bigger (or equal to) the preferred resolution)
						*/
						for (i = gst_caps_get_size (icaps) - 1; i >= 0; i--) {
							GstStructure *is = gst_caps_get_structure (icaps, i);
							int w, h;
							
							if (gst_structure_get_int (is, "width", &w) && gst_structure_get_int (is, "height", &h))
							{
								if (w >= twidth && w <= width && h >= theight && h <= height) {
									width = w;
									height = h;
									best = i;
								}
							}
						}
					}
					
					caps = gst_caps_copy_nth (icaps, best);
					gst_caps_unref (icaps);
			} else {
				caps = icaps;
			}
		}
		gst_caps_unref (thiscaps);
	} else {
		/* no peer or peer have ANY caps, work with our own caps then */
		caps = thiscaps;
	}
	if (peercaps)
		gst_caps_unref (peercaps);
	if (caps) {
		caps = gst_caps_truncate (caps);
		
		/* now fixate */
		if (!gst_caps_is_empty (caps)) {
			caps = gst_dreamvideosource_fixate (bsrc, caps);
			GST_DEBUG_OBJECT (bsrc, "fixated to: %" GST_PTR_FORMAT, caps);
			
			if (gst_caps_is_any (caps)) {
				/* hmm, still anything, so element can do anything and
				 * nego is not needed */
				result = TRUE;
			} else if (gst_caps_is_fixed (caps)) {
				/* yay, fixed caps, use those then */
				result = gst_base_src_set_caps (bsrc, caps);
			}
		}
		gst_caps_unref (caps);
	}
	return result;
	
	no_nego_needed:
	{
		GST_DEBUG_OBJECT (bsrc, "no negotiation needed");
		if (thiscaps)
			gst_caps_unref (thiscaps);
		return TRUE;
	}
}

static void
gst_dreamvideosource_free_buffer (GstDreamVideoSource * self)
{
	GST_LOG_OBJECT (self, "gst_dreamvideosource_free_buffer");
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
			int rlen = read(enc->fd, enc->buffer, VBUFSIZE);
			if (rlen <= 0 || rlen % VBDSIZE ) {
				GST_WARNING_OBJECT (self, "read error %d (errno %i)", rlen, errno);
				return GST_FLOW_ERROR;
			}
			self->descriptors_available = rlen / VBDSIZE;
			GST_LOG_OBJECT (self, "encoder buffer was empty, %d descriptors available", self->descriptors_available);
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
						GST_DEBUG_OBJECT (self, "use mpeg stream pts as base_pts=%" GST_TIME_FORMAT"", GST_TIME_ARGS (self->base_pts) );
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
			if (write(enc->fd, &self->descriptors_count, 4) != 4) {
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

static GstStateChangeReturn gst_dreamvideosource_change_state (GstElement * element, GstStateChange transition)
{
	g_return_val_if_fail (GST_DREAMVIDEOSOURCE (element), FALSE);
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (element);
	int ret;

	switch (transition) {
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_PAUSED_TO_PLAYING");
			ret = ioctl(self->encoder->fd, VENC_START);
			if ( ret != 0 )
			{
				GST_ERROR_OBJECT(self,"can't start encoder ioctl!");
				return GST_STATE_CHANGE_FAILURE;
			}
			GST_INFO_OBJECT (self, "started encoder!");
			break;
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			GST_LOG_OBJECT (self, "GST_STATE_CHANGE_PLAYING_TO_PAUSED");
			ret = ioctl(self->encoder->fd, VENC_STOP);
			if ( ret != 0 )
			{
				GST_ERROR_OBJECT(self,"can't stop encoder ioctl!");
				return GST_STATE_CHANGE_FAILURE;
			}
			break;
		default:
			break;
	}

	if (GST_ELEMENT_CLASS (parent_class)->change_state)
		return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

	return GST_STATE_CHANGE_SUCCESS;
}
static gboolean
gst_dreamvideosource_start (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);

	char fn_buf[32];

	self->encoder = malloc(sizeof(EncoderInfo));

	if(!self->encoder) {
		GST_ERROR_OBJECT(self,"out of space");
		return FALSE;
	}

	sprintf(fn_buf, "/dev/venc%d", 0);
	self->encoder->fd = open(fn_buf, O_RDWR | O_SYNC);
	if(self->encoder->fd <= 0) {
		GST_ERROR_OBJECT(self,"cannot open device %s (%s)", fn_buf, strerror(errno));
		free(self->encoder);
		self->encoder = NULL;
		return FALSE;
	}

	self->encoder->buffer = malloc(VBUFSIZE);
	if(!self->encoder->buffer) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer");
		return FALSE;
	}

	self->encoder->cdb = (unsigned char *)mmap(0, VMMAPSIZE, PROT_READ, MAP_PRIVATE, self->encoder->fd, 0);

	if(!self->encoder->cdb) {
		GST_ERROR_OBJECT(self,"cannot mmap cdb");
		return FALSE;
	}
#ifdef dump
	self->dumpfd = open("/media/hdd/movie/dreamvideosource.dump", O_WRONLY | O_CREAT | O_TRUNC);
	GST_DEBUG_OBJECT (self, "dumpfd = %i (%s)", self->dumpfd, (self->dumpfd > 0) ? "OK" : strerror(errno));
#endif

	gst_dreamvideosource_set_bitrate(self, DEFAULT_BITRATE);

	self->dreamaudiosrc = gst_bin_get_by_name_recurse_up(GST_BIN(GST_ELEMENT_PARENT(self)), "dreamaudiosource0");

	return TRUE;
}

static gboolean
gst_dreamvideosource_stop (GstBaseSrc * bsrc)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (bsrc);
	if (self->encoder) {
		if (self->encoder->fd > 0)
			ioctl(self->encoder->fd, VENC_STOP);
		close(self->encoder->fd);
	}
#ifdef dump
	close(self->dumpfd);
#endif
	GST_DEBUG_OBJECT (self, "closed");
	return TRUE;
}

static void
gst_dreamvideosource_finalize (GObject * gobject)
{
	GstDreamVideoSource *self = GST_DREAMVIDEOSOURCE (gobject);
	if (self->encoder) {
		if (self->encoder->buffer)
			free(self->encoder->buffer);
		if (self->encoder->cdb) 
			munmap(self->encoder->cdb, VMMAPSIZE);
		free(self->encoder);
	}
	g_mutex_clear (&self->mutex);
	GST_DEBUG_OBJECT (self, "finalized");
	G_OBJECT_CLASS (parent_class)->finalize (gobject);
}
