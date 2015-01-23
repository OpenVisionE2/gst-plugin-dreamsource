/*
 * GStreamer dreamsource
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

#ifndef __GST_DREAMSOURCE_H__
#define __GST_DREAMSOURCE_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "gstdreamsource-marshal.h"

G_BEGIN_DECLS

typedef struct _CompressedBufferDescriptor CompressedBufferDescriptor;
typedef struct _EncoderInfo                EncoderInfo;

#define MPEGTIME_TO_GSTTIME(time) (gst_util_uint64_scale ((time), GST_MSECOND/10, 9LL))

/* validity flags */
#define CDB_FLAG_ORIGINALPTS_VALID         0x00000001
#define CDB_FLAG_PTS_VALID                 0x00000002
#define CDB_FLAG_ESCR_VALID                0x00000004
#define CDB_FLAG_TICKSPERBIT_VALID         0x00000008
#define CDB_FLAG_SHR_VALID                 0x00000010
#define CDB_FLAG_STCSNAPSHOT_VALID         0x00000020

/* indicator flags */
#define CDB_FLAG_FRAME_START               0x00010000
#define CDB_FLAG_EOS                       0x00020000
#define CDB_FLAG_EMPTY_FRAME               0x00040000
#define CDB_FLAG_FRAME_END                 0x00080000
#define CDB_FLAG_EOC                       0x00100000

#define CDB_FLAG_METADATA                  0x40000000
#define CDB_FLAG_EXTENDED                  0x80000000

struct _CompressedBufferDescriptor
{
	uint32_t uiFlags;

	/* Timestamp Parameters */
	uint32_t uiOriginalPTS;      /* 32-bit original PTS value (in 45 Khz or 27Mhz?) */
	uint64_t uiPTS;              /* 33-bit PTS value (in 90 Khz) */
	uint64_t uiSTCSnapshot;      /* 42-bit STC snapshot when frame received by the encoder (in 27Mhz) */

	/* Transmission Parameters */
	uint32_t uiESCR;             /* Expected mux transmission start time for the first bit of the data (in 27 Mhz) */

	uint16_t uiTicksPerBit;
	int16_t iSHR;

	/* Buffer Parameters */
	unsigned uiOffset;           /* REQUIRED: offset of frame data from frame buffer base address (in bytes) */
	size_t uiLength;             /* REQUIRED: 0 if fragment is empty, e.g. for EOS entry (in bytes) */
	unsigned uiReserved;         /* Unused field */
};

struct _EncoderInfo {
	int fd;

	/* descriptor space */
	unsigned char *buffer;

	/* mmapp'ed data buffer */
	unsigned char *cdb;
};

G_END_DECLS

#endif /* __GST_DREAMSOURCE_H__ */
