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
#include "gstdreamaudiosource.h"

GST_DEBUG_CATEGORY_STATIC (dreamaudiosource_debug);
#define GST_CAT_DEFAULT dreamaudiosource_debug

enum
{
  ARG_0,
  ARG_BITRATE,
};

#define DEFAULT_BITRATE 128

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
static void gst_dreamaudiosource_finalize (GObject * gobject);
static GstFlowReturn gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf);

static void gst_dreamaudiosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_dreamaudiosource_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);

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
	gobject_class->finalize = gst_dreamaudiosource_finalize;

	gst_element_class_add_pad_template (gstelement_class,
					    gst_static_pad_template_get (&srctemplate));

	gst_element_class_set_static_metadata (gstelement_class,
	    "Dream Audio source", "Source/Audio",
	    "Provide an audio elementary stream from Dreambox encoder device",
	    "Andreas Frisch <fraxinas@opendreambox.org>");

	gstbasesrc_class->get_caps = gst_dreamaudiosource_getcaps;
	gstbasesrc_class->start = gst_dreamaudiosource_start;
	gstbasesrc_class->stop = gst_dreamaudiosource_stop;

	gstpush_src_class->create = gst_dreamaudiosource_create;

	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
	  g_param_spec_int ("bitrate", "Bitrate (kb/s)",
	    "Bitrate in kbit/sec", 16, 320, DEFAULT_BITRATE,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

}

gboolean
gst_dreamaudiosource_plugin_init (GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT (dreamaudiosource_debug, "dreamsource", 0, "dreamaudiosource");
	return gst_element_register (plugin, "dreamaudiosource", GST_RANK_PRIMARY, GST_TYPE_DREAMAUDIOSOURCE);
}

static void
gst_dreamaudiosource_init (GstDreamAudioSource * self)
{
	self->encoder = NULL;
	self->descriptors_available = 0;
	self->first_dts = GST_CLOCK_TIME_NONE;
	
	gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
	gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
}

static void
gst_dreamaudiosource_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (object);
	
	switch (prop_id) {
		case ARG_BITRATE:
			self->audio_info.bitrate = g_value_get_int (value);
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
			g_value_set_int (value, self->audio_info.bitrate);
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

	GST_INFO_OBJECT (self, "return caps %" GST_PTR_FORMAT, caps);
	return caps;
}

static void
gst_dreamaudiosource_free_buffer (GstDreamAudioSource * self)
{
	GST_LOG_OBJECT (self, "gst_dreamaudiosource_free_buffer");
}

static GstFlowReturn
gst_dreamaudiosource_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (psrc);
	EncoderInfo *enc = self->encoder;
		
	GstClock *clock;
	GstClockTime time = GST_CLOCK_TIME_NONE;
	GstClockTime running_time;
	clock = gst_element_get_clock (GST_ELEMENT (self));
	
	static int dumpoffset;

	GST_LOG_OBJECT (self, "new buffer requested");

	if (!enc) {
		GST_WARNING_OBJECT (self, "encoder device not opened!");
		return GST_FLOW_ERROR;
	}

	if (self->descriptors_available == 0)
	{
		self->descriptors_count = 0;
		int rlen = read(enc->fd, enc->buffer, ABUFSIZE);
		if (rlen <= 0 || rlen % ABDSIZE ) {
			GST_WARNING_OBJECT (self, "read error %d (errno %i)", rlen, errno);
			return GST_FLOW_ERROR;
		}
		self->descriptors_available = rlen / ABDSIZE;
		GST_LOG_OBJECT (self, "encoder buffer was empty, %d descriptors available", self->descriptors_available);
	}

	while (self->descriptors_count < self->descriptors_available) {
		off_t offset = self->descriptors_count * ABDSIZE;
		AudioBufferDescriptor *desc = (AudioBufferDescriptor*)(&enc->buffer[offset]);

		uint32_t f = desc->stCommon.uiFlags;

		GST_DEBUG_OBJECT (self, "descriptors_count=%d, descriptors_available=%u\tuiOffset=%u, uiLength=%"G_GSIZE_FORMAT"", self->descriptors_count, self->descriptors_available, desc->stCommon.uiOffset, desc->stCommon.uiLength);

		if (f & CDB_FLAG_METADATA) { 
			GST_LOG_OBJECT (self, "CDB_FLAG_METADATA... skip outdated packet");
			self->descriptors_count = self->descriptors_available;
			*outbuf = gst_buffer_new();
			continue;
		}

		*outbuf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, enc->cdb, AMMAPSIZE, desc->stCommon.uiOffset, desc->stCommon.uiLength, self, (GDestroyNotify)gst_dreamaudiosource_free_buffer);

		if (f & CDB_FLAG_PTS_VALID)
		{
			if (G_UNLIKELY (self->first_dts == GST_CLOCK_TIME_NONE))
			{
				self->first_dts = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS);
				GST_DEBUG_OBJECT (self, "self->first_dts=%" GST_TIME_FORMAT"", GST_TIME_ARGS (self->first_dts) );
			}
			
			if (clock != NULL) {
				GstClockTime base_time, buffer_time;
				time = gst_clock_get_time (clock);
				GST_INFO_OBJECT (self, "gst_clock_get_time=%" GST_TIME_FORMAT"", GST_TIME_ARGS(time));
				base_time = gst_element_get_base_time (GST_ELEMENT (self));
				GST_INFO_OBJECT (self, "gst_element_get_base_time=%" GST_TIME_FORMAT"",GST_TIME_ARGS( base_time));
				running_time = time - base_time;
				GST_INFO_OBJECT (self, "running_time=%" GST_TIME_FORMAT"", GST_TIME_ARGS(running_time));
				buffer_time = MPEGTIME_TO_GSTTIME(desc->stCommon.uiPTS)-self->first_dts/*+running_time*/;
				GST_BUFFER_PTS(*outbuf) = buffer_time;
				GST_BUFFER_DTS(*outbuf) = buffer_time;
				GST_INFO_OBJECT (self, "buffer_time=%" GST_TIME_FORMAT"", GST_TIME_ARGS(buffer_time));
				
			} else {
				GST_ERROR_OBJECT (self, "NO CLOCK!!!");
			}
		}

		int wret = 0;
#ifdef dump
		wret = write(self->dumpfd, (unsigned char*)(enc->cdb + desc->stCommon.uiOffset), desc->stCommon.uiLength);
		dumpoffset += wret;
		GST_LOG_OBJECT (self, "read %"G_GSIZE_FORMAT" dumped %i total 0x%08X", desc->stCommon.uiLength, wret, dumpoffset );
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

	GST_INFO_OBJECT (self, "comitting %" G_GSIZE_FORMAT " bytes", gst_buffer_get_size (*outbuf) );
	
	return GST_FLOW_OK;
}

static gboolean
gst_dreamaudiosource_start (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);

	GST_INFO_OBJECT (self, "start");

	char fn_buf[32];

	self->encoder = malloc(sizeof(EncoderInfo));

	if(!self->encoder) {
		GST_ERROR_OBJECT(self,"out of space");
		return FALSE;
	}

	sprintf(fn_buf, "/dev/aenc%d", 0);
	self->encoder->fd = open(fn_buf, O_RDWR | O_SYNC);
	if(self->encoder->fd <= 0) {
		GST_ERROR_OBJECT(self,"cannot open device %s (%s)", fn_buf, strerror(errno));
		free(self->encoder);
		self->encoder = NULL;
		return FALSE;
	}

	self->encoder->buffer = malloc(ABUFSIZE);
	if(!self->encoder->buffer) {
		GST_ERROR_OBJECT(self,"cannot alloc buffer");
		return FALSE;
	}

	self->encoder->cdb = (unsigned char *)mmap(0, AMMAPSIZE, PROT_READ, MAP_PRIVATE, self->encoder->fd, 0);

	if(!self->encoder->cdb || self->encoder->cdb== MAP_FAILED) {
		GST_ERROR_OBJECT(self,"cannot mmap cdb: %s (%d)", strerror(errno));
		return FALSE;
	}
#ifdef dump
	self->dumpfd = open("/testProgs/dump.audio", O_WRONLY | O_CREAT | O_TRUNC);
	GST_INFO_OBJECT (self, "dumpfd = %i (%s)", self->dumpfd, (self->dumpfd > 0) ? "OK" : strerror(errno));
#endif
	
	GST_INFO_OBJECT (self, "cdb buffer mapped to %08x", self->encoder->cdb);
	
	uint32_t abr = self->audio_info.bitrate*1000;
	int ret = ioctl(self->encoder->fd, AENC_SET_BITRATE, &abr);
	GST_INFO_OBJECT (self, "set bitrate to %i bytes/s ret=%i", abr, ret);
	
	int rlen = 0;
	do {
		rlen = read(self->encoder->fd, self->encoder->buffer, ABUFSIZE);
		GST_INFO_OBJECT (self, "flushed %d bytes", rlen);
	} while (rlen > 0);
	
	ret = ioctl(self->encoder->fd, AENC_START);
	GST_INFO_OBJECT (self, "started encoder! ret=%i", ret);
	
	return TRUE;
}

static gboolean
gst_dreamaudiosource_stop (GstBaseSrc * bsrc)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (bsrc);
	if (self->encoder) {
		if (self->encoder->fd > 0)
			ioctl(self->encoder->fd, AENC_STOP);
		close(self->encoder->fd);
	}
#ifdef dump
	close(self->dumpfd);
#endif
	GST_INFO_OBJECT (self, "closed");
	return TRUE;
}

static void
gst_dreamaudiosource_finalize (GObject * gobject)
{
	GstDreamAudioSource *self = GST_DREAMAUDIOSOURCE (gobject);
	if (self->encoder) {
		if (self->encoder->buffer)
			free(self->encoder->buffer);
		if (self->encoder->cdb) 
			munmap(self->encoder->cdb, AMMAPSIZE);
		free(self->encoder);
	}
	GST_DEBUG_OBJECT (self, "finalized");
	G_OBJECT_CLASS (parent_class)->finalize (gobject);
}
