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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gst/gst.h>

#include "gstdreamaudiosource.h"
#include "gstdreamvideosource.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean res = TRUE;
  res &= gst_dreamaudiosource_plugin_init (plugin);
  res &= gst_dreamvideosource_plugin_init (plugin);

  return res;
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	dreamsource,
	"Dreambox Audio/Video Source",
	plugin_init,
	VERSION,
	"Proprietary",
	"dreamsource",
	"https://schwerkraft.elitedvb.net/scm/browser.php?group_id=10"
)
