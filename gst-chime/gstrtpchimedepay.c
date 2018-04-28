/*
 * Chime Depayloader Gst Element
 *
 *   @author: Danilo Cesar Lemes de Paula <danilo.cesar@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/audio/audio.h>
#include "gstrtpchimedepay.h"

GST_DEBUG_CATEGORY_STATIC (rtpchimedepay_debug);
#define GST_CAT_DEFAULT (rtpchimedepay_debug)

static GstStaticPadTemplate gst_rtp_chime_depay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp, "
        "media = (string) \"audio\", "
        "payload = (int) " GST_RTP_PAYLOAD_DYNAMIC_STRING ","
        "clock-rate = (int) 16000, "
        "encoding-name = (string) \"CHIME\"")
    );

static GstStaticPadTemplate gst_rtp_chime_depay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-opus, channel-mapping-family = (int) 0")
    );

static GstBuffer *gst_rtp_chime_depay_process (GstRTPBaseDepayload * depayload,
    GstRTPBuffer * rtp_buffer);
static gboolean gst_rtp_chime_depay_setcaps (GstRTPBaseDepayload * depayload,
    GstCaps * caps);

G_DEFINE_TYPE (GstRTPChimeDepay, gst_rtp_chime_depay,
    GST_TYPE_RTP_BASE_DEPAYLOAD);

static void
gst_rtp_chime_depay_class_init (GstRTPChimeDepayClass * klass)
{
  GstRTPBaseDepayloadClass *gstbasertpdepayload_class;
  GstElementClass *element_class;

  element_class = GST_ELEMENT_CLASS (klass);
  gstbasertpdepayload_class = (GstRTPBaseDepayloadClass *) klass;

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rtp_chime_depay_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_rtp_chime_depay_sink_template));
  gst_element_class_set_static_metadata (element_class,
      "RTP Chime packet depayloader", "Codec/Depayloader/Network/RTP",
      "Extracts Chime audio from RTP packets",
      "Danilo Cesar Lemes de Paula <danilo.cesar@collabora.co.uk>");

  gstbasertpdepayload_class->process_rtp_packet = gst_rtp_chime_depay_process;
  gstbasertpdepayload_class->set_caps = gst_rtp_chime_depay_setcaps;

  GST_DEBUG_CATEGORY_INIT (rtpchimedepay_debug, "rtpchimedepay", 0,
      "Chime RTP Depayloader");
}

static void
gst_rtp_chime_depay_init (GstRTPChimeDepay * rtpchimedepay)
{

}

static gboolean
gst_rtp_chime_depay_setcaps (GstRTPBaseDepayload * depayload, GstCaps * caps)
{
  GstCaps *srccaps;
  GstStructure *s;
  gboolean ret;
  const gchar *sprop_stereo, *sprop_maxcapturerate;

  srccaps =
      gst_caps_new_simple ("audio/x-opus", "channel-mapping-family", G_TYPE_INT,
      0, NULL);

  s = gst_caps_get_structure (caps, 0);
  if ((sprop_stereo = gst_structure_get_string (s, "sprop-stereo"))) {
    if (strcmp (sprop_stereo, "0") == 0)
      gst_caps_set_simple (srccaps, "channels", G_TYPE_INT, 1, NULL);
    else if (strcmp (sprop_stereo, "1") == 0)
      gst_caps_set_simple (srccaps, "channels", G_TYPE_INT, 2, NULL);
    else
      GST_WARNING_OBJECT (depayload, "Unknown sprop-stereo value '%s'",
          sprop_stereo);
  }

  if ((sprop_maxcapturerate =
          gst_structure_get_string (s, "sprop-maxcapturerate"))) {
    gulong rate;
    gchar *tailptr;

    rate = strtoul (sprop_maxcapturerate, &tailptr, 10);
    if (rate > INT_MAX || *tailptr != '\0') {
      GST_WARNING_OBJECT (depayload,
          "Failed to parse sprop-maxcapturerate value '%s'",
          sprop_maxcapturerate);
    } else {
      gst_caps_set_simple (srccaps, "rate", G_TYPE_INT, rate, NULL);
    }
  }

  ret = gst_pad_set_caps (GST_RTP_BASE_DEPAYLOAD_SRCPAD (depayload), srccaps);

  GST_DEBUG_OBJECT (depayload,
      "set caps on source: %" GST_PTR_FORMAT " (ret=%d)", srccaps, ret);
  gst_caps_unref (srccaps);

  depayload->clock_rate = 16000;

  return ret;
}

static gboolean
foreach_metadata (GstBuffer * inbuf, GstMeta ** meta, gpointer user_data)
{
  GstRTPChimeDepay *depay = user_data;
  const GstMetaInfo *info = (*meta)->info;
  const gchar *const *tags = gst_meta_api_type_get_tags (info->api);

  if (!tags || (g_strv_length ((gchar **) tags) == 1
          && gst_meta_api_type_has_tag (info->api,
              g_quark_from_string (GST_META_TAG_AUDIO_STR)))) {
    GST_DEBUG_OBJECT (depay, "keeping metadata %s", g_type_name (info->api));
  } else {
    GST_DEBUG_OBJECT (depay, "dropping metadata %s", g_type_name (info->api));
    *meta = NULL;
  }

  return TRUE;
}

static GstBuffer *
gst_rtp_chime_depay_process (GstRTPBaseDepayload * depayload,
    GstRTPBuffer * rtp_buffer)
{
  GstBuffer *outbuf;

  outbuf = gst_rtp_buffer_get_payload_buffer (rtp_buffer);

  /* Filter away all metas that are not sensible to copy */
  gst_buffer_foreach_meta (outbuf, foreach_metadata, depayload);

  return outbuf;
}

gboolean
gst_rtp_chime_depay_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "rtpchimedepay",
      GST_RANK_PRIMARY, GST_TYPE_RTP_CHIME_DEPAY);
}
