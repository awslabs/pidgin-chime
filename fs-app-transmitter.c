/*
 * Farstream - Farstream Shm UDP Transmitter
 *
 * Copyright 2007-2008 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007-2008 Nokia Corp.
 *
 * fs-app-transmitter.c - A Farstream shm UDP transmitter
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:fs-app-transmitter
 * @short_description: A transmitter for shm UDP
 *
 * This transmitter provides shm udp
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-app-transmitter.h"
#include "fs-app-stream-transmitter.h"

#include <farstream/fs-conference.h>
#include <farstream/fs-plugin.h>

#include <string.h>
#include <stdio.h>

GST_DEBUG_CATEGORY (fs_app_transmitter_debug);
#define GST_CAT_DEFAULT fs_app_transmitter_debug

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_GST_SINK,
  PROP_GST_SRC,
  PROP_COMPONENTS,
  PROP_DO_TIMESTAMP,
};

struct _FsAppTransmitterPrivate
{
  /* We hold references to this element */
  GstElement *gst_sink;
  GstElement *gst_src;

  /* We don't hold a reference to these elements, they are owned
     by the bins */
  /* They are tables of pointers, one per component */
  GstElement **funnels;
  GstElement **tees;

  gboolean do_timestamp;
};

#define FS_APP_TRANSMITTER_GET_PRIVATE(o)  \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_APP_TRANSMITTER,   \
      FsAppTransmitterPrivate))

static void fs_app_transmitter_class_init (
    FsAppTransmitterClass *klass);
static void fs_app_transmitter_init (FsAppTransmitter *self);
static void fs_app_transmitter_constructed (GObject *object);
static void fs_app_transmitter_dispose (GObject *object);
static void fs_app_transmitter_finalize (GObject *object);

static void fs_app_transmitter_get_property (GObject *object,
                                                guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec);
static void fs_app_transmitter_set_property (GObject *object,
                                                guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec);

static FsStreamTransmitter *fs_app_transmitter_new_stream_transmitter (
    FsTransmitter *transmitter, FsParticipant *participant,
    guint n_parameters, GParameter *parameters, GError **error);
static GType fs_app_transmitter_get_stream_transmitter_type (
    FsTransmitter *transmitter);


static GObjectClass *parent_class = NULL;
//static guint signals[LAST_SIGNAL] = { 0 };

/*
 * Private bin subclass
 */

enum {
  BIN_SIGNAL_READY,
  BIN_SIGNAL_DISCONNECTED,
  BIN_LAST_SIGNAL
};

static guint bin_signals[BIN_LAST_SIGNAL] = { 0 };
static GType shm_bin_type = 0;
gpointer shm_bin_parent_class = NULL;

typedef struct _FsShmBin
{
  GstBin parent;
} FsShmBin;

typedef struct _FsShmBinClass
{
  GstBinClass parent_class;
} FsShmBinClass;

static void fs_shm_bin_init (FsShmBin *self)
{
}

static GstElement *
fs_shm_bin_new (void)
{
  return g_object_new (shm_bin_type, NULL);
}

static void
fs_shm_bin_handle_message (GstBin *bin, GstMessage *message)
{
  GstState old, new, pending;
  GError *gerror;
  gchar *msg;

  switch (GST_MESSAGE_TYPE (message))
  {
    case GST_MESSAGE_STATE_CHANGED:
      gst_message_parse_state_changed (message, &old, &new, &pending);

      if (old == GST_STATE_PAUSED && new == GST_STATE_PLAYING)
        g_signal_emit (bin, bin_signals[BIN_SIGNAL_READY], 0,
            GST_MESSAGE_SRC (message));
      break;
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &gerror, &msg);

      if (g_error_matches (gerror, GST_RESOURCE_ERROR,
              GST_RESOURCE_ERROR_READ))
      {
        g_signal_emit (bin, bin_signals[BIN_SIGNAL_DISCONNECTED], 0,
            GST_MESSAGE_SRC (message));
        gst_message_unref (message);
        return;
      }
      break;
    default:
      break;
  }

  GST_BIN_CLASS (shm_bin_parent_class)->handle_message (bin, message);
}

static void fs_shm_bin_class_init (FsShmBinClass *klass)
{
  GstBinClass *bin_class = GST_BIN_CLASS (klass);

  shm_bin_parent_class = g_type_class_peek_parent (klass);

  bin_signals[BIN_SIGNAL_READY] =
    g_signal_new ("ready", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_ELEMENT);

  bin_signals[BIN_SIGNAL_DISCONNECTED] =
    g_signal_new ("disconnected", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_ELEMENT);

  bin_class->handle_message = GST_DEBUG_FUNCPTR (fs_shm_bin_handle_message);
}

/*
 * Lets register the plugin
 */

static GType type = 0;

GType
fs_app_transmitter_get_type (void)
{
  g_assert (type);
  return type;
}

GType
fs_app_transmitter_register_type (FsPlugin *module);
GType
fs_app_transmitter_register_type (FsPlugin *module)
{
  static const GTypeInfo info = {
    sizeof (FsAppTransmitterClass),
    NULL,
    NULL,
    (GClassInitFunc) fs_app_transmitter_class_init,
    NULL,
    NULL,
    sizeof (FsAppTransmitter),
    0,
    (GInstanceInitFunc) fs_app_transmitter_init
  };

  static const GTypeInfo bin_info = {
    sizeof (FsShmBinClass),
    NULL,
    NULL,
    (GClassInitFunc) fs_shm_bin_class_init,
    NULL,
    NULL,
    sizeof (FsShmBin),
    0,
    (GInstanceInitFunc) fs_shm_bin_init
  };


  GST_DEBUG_CATEGORY_INIT (fs_app_transmitter_debug,
      "fsshmtransmitter", 0,
      "Farstream shm UDP transmitter");

  fs_app_stream_transmitter_register_type (module);

  type = g_type_register_static (FS_TYPE_TRANSMITTER, "FsAppTransmitter",
      &info, 0);

  shm_bin_type = g_type_register_static (
    GST_TYPE_BIN, "FsShmBin", &bin_info, 0);

  return type;
}

static void
fs_app_transmitter_class_init (FsAppTransmitterClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  FsTransmitterClass *transmitter_class = FS_TRANSMITTER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = fs_app_transmitter_set_property;
  gobject_class->get_property = fs_app_transmitter_get_property;

  gobject_class->constructed = fs_app_transmitter_constructed;

  g_object_class_override_property (gobject_class, PROP_GST_SRC, "gst-src");
  g_object_class_override_property (gobject_class, PROP_GST_SINK, "gst-sink");
  g_object_class_override_property (gobject_class, PROP_COMPONENTS,
    "components");
  g_object_class_override_property (gobject_class, PROP_DO_TIMESTAMP,
    "do-timestamp");

  transmitter_class->new_stream_transmitter =
    fs_app_transmitter_new_stream_transmitter;
  transmitter_class->get_stream_transmitter_type =
    fs_app_transmitter_get_stream_transmitter_type;

  gobject_class->dispose = fs_app_transmitter_dispose;
  gobject_class->finalize = fs_app_transmitter_finalize;

  g_type_class_add_private (klass, sizeof (FsAppTransmitterPrivate));
}

static void
fs_app_transmitter_init (FsAppTransmitter *self)
{

  /* member init */
  self->priv = FS_APP_TRANSMITTER_GET_PRIVATE (self);

  self->components = 2;
  self->priv->do_timestamp = TRUE;
}

static void
fs_app_transmitter_constructed (GObject *object)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER_CAST (object);
  FsTransmitter *trans = FS_TRANSMITTER_CAST (self);
  GstPad *pad = NULL, *pad2 = NULL;
  GstPad *ghostpad = NULL;
  gchar *padname;
  GstPadLinkReturn ret;
  int c; /* component_id */


  /* We waste one space in order to have the index be the component_id */
  self->priv->funnels = g_new0 (GstElement *, self->components+1);
  self->priv->tees = g_new0 (GstElement *, self->components+1);

  /* First we need the src elemnet */

  self->priv->gst_src = fs_shm_bin_new ();

  if (!self->priv->gst_src) {
    trans->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not build the transmitter src bin");
    return;
  }

  gst_object_ref (self->priv->gst_src);


  /* Second, we do the sink element */

  self->priv->gst_sink = fs_shm_bin_new ();

  if (!self->priv->gst_sink) {
    trans->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not build the transmitter sink bin");
    return;
  }

  g_object_set (G_OBJECT (self->priv->gst_sink),
      "async-handling", TRUE,
      NULL);

  gst_object_ref (self->priv->gst_sink);

  for (c = 1; c <= self->components; c++) {
    GstElement *fakesink = NULL;

    /* Lets create the RTP source funnel */

    self->priv->funnels[c] = gst_element_factory_make ("funnel", NULL);

    if (!self->priv->funnels[c]) {
      trans->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not make the funnel element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->gst_src),
        self->priv->funnels[c])) {
      trans->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not add the funnel element to the transmitter src bin");
    }

    pad = gst_element_get_static_pad (self->priv->funnels[c], "src");
    padname = g_strdup_printf ("src_%u", c);
    ghostpad = gst_ghost_pad_new (padname, pad);
    g_free (padname);
    gst_object_unref (pad);

    gst_pad_set_active (ghostpad, TRUE);
    gst_element_add_pad (self->priv->gst_src, ghostpad);


    /* Lets create the RTP sink tee */

    self->priv->tees[c] = gst_element_factory_make ("tee", NULL);

    if (!self->priv->tees[c]) {
      trans->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not make the tee element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->gst_sink),
        self->priv->tees[c])) {
      trans->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not add the tee element to the transmitter sink bin");
    }

    pad = gst_element_get_static_pad (self->priv->tees[c], "sink");
    padname = g_strdup_printf ("sink_%u", c);
    ghostpad = gst_ghost_pad_new (padname, pad);
    g_free (padname);
    gst_object_unref (pad);

    gst_pad_set_active (ghostpad, TRUE);
    gst_element_add_pad (self->priv->gst_sink, ghostpad);

    fakesink = gst_element_factory_make ("fakesink", NULL);

    if (!fakesink) {
      trans->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not make the fakesink element");
      return;
    }

    g_object_set (fakesink,
        "async", FALSE,
        "sync" , FALSE,
        NULL);

    if (!gst_bin_add (GST_BIN (self->priv->gst_sink), fakesink))
    {
      gst_object_unref (fakesink);
      trans->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the fakesink element to the transmitter sink bin");
      return;
    }

    pad = gst_element_get_request_pad (self->priv->tees[c], "src_%u");
    pad2 = gst_element_get_static_pad (fakesink, "sink");

    ret = gst_pad_link (pad, pad2);

    gst_object_unref (pad2);
    gst_object_unref (pad);

    if (GST_PAD_LINK_FAILED(ret)) {
      trans->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not link the tee to the fakesink");
      return;
    }
  }

  GST_CALL_PARENT (G_OBJECT_CLASS, constructed, (object));
}

static void
fs_app_transmitter_dispose (GObject *object)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER (object);

  if (self->priv->gst_src) {
    gst_object_unref (self->priv->gst_src);
    self->priv->gst_src = NULL;
  }

  if (self->priv->gst_sink) {
    gst_object_unref (self->priv->gst_sink);
    self->priv->gst_sink = NULL;
  }

  parent_class->dispose (object);
}

static void
fs_app_transmitter_finalize (GObject *object)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER (object);

  if (self->priv->funnels) {
    g_free (self->priv->funnels);
    self->priv->funnels = NULL;
  }

  if (self->priv->tees) {
    g_free (self->priv->tees);
    self->priv->tees = NULL;
  }

  parent_class->finalize (object);
}

static void
fs_app_transmitter_get_property (GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER (object);

  switch (prop_id) {
    case PROP_GST_SINK:
      g_value_set_object (value, self->priv->gst_sink);
      break;
    case PROP_GST_SRC:
      g_value_set_object (value, self->priv->gst_src);
      break;
    case PROP_COMPONENTS:
      g_value_set_uint (value, self->components);
      break;
    case PROP_DO_TIMESTAMP:
      g_value_set_boolean (value, self->priv->do_timestamp);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_app_transmitter_set_property (GObject *object,
                                    guint prop_id,
                                    const GValue *value,
                                    GParamSpec *pspec)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER (object);

  switch (prop_id) {
    case PROP_COMPONENTS:
      self->components = g_value_get_uint (value);
      break;
    case PROP_DO_TIMESTAMP:
      self->priv->do_timestamp = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


/**
 * fs_app_transmitter_new_stream_app_transmitter:
 * @transmitter: a #FsTranmitter
 * @participant: the #FsParticipant for which the #FsStream using this
 * new #FsStreamTransmitter is created
 *
 * This function will create a new #FsStreamTransmitter element for a
 * specific participant for this #FsAppTransmitter
 *
 * Returns: a new #FsStreamTransmitter
 */

static FsStreamTransmitter *
fs_app_transmitter_new_stream_transmitter (FsTransmitter *transmitter,
  FsParticipant *participant, guint n_parameters, GParameter *parameters,
  GError **error)
{
  FsAppTransmitter *self = FS_APP_TRANSMITTER (transmitter);

  printf("New App transmitter\n");
  return FS_STREAM_TRANSMITTER (fs_app_stream_transmitter_newv (
        self, n_parameters, parameters, error));
}

static GType
fs_app_transmitter_get_stream_transmitter_type (
    FsTransmitter *transmitter)
{
  return FS_TYPE_APP_STREAM_TRANSMITTER;
}


struct _ShmSrc {
  guint component;
  gchar *path;
  GstElement *src;
  GstPad *funnelpad;

  got_buffer got_buffer_func;
  connection disconnected_func;
  gpointer cb_data;
  gulong buffer_probe;
};


static GstPadProbeReturn
src_buffer_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  ShmSrc *shm = user_data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  shm->got_buffer_func (buffer, shm->component, shm->cb_data);

  return TRUE;
}


static void
disconnected_cb (GstBin *bin, GstElement *elem, ShmSrc *shm)
{
  if (elem != shm->src)
    return;

  shm->disconnected_func (shm->component, 0, shm->cb_data);
}


ShmSrc *
fs_app_transmitter_get_shm_src (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    got_buffer got_buffer_func,
    connection disconnected_func,
    gpointer cb_data,
    GError **error)
{
  ShmSrc *shm = g_slice_new0 (ShmSrc);
  GstElement *elem;
  GstPad *pad;

  shm->component = component;
  shm->got_buffer_func = got_buffer_func;
  shm->disconnected_func = disconnected_func;
  shm->cb_data = cb_data;

  shm->path = g_strdup (path);

  elem = gst_element_factory_make ("shmsrc", NULL);
  if (!elem)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not make shmsrc");
    goto error;
  }

  g_object_set (elem,
      "socket-path", path,
      "do-timestamp", self->priv->do_timestamp,
      "is-live", TRUE,
      NULL);

  if (shm->disconnected_func)
    g_signal_connect (self->priv->gst_src, "disconnected",
        G_CALLBACK (disconnected_cb), shm);

  if (!gst_bin_add (GST_BIN (self->priv->gst_src), elem))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add recvonly filter to bin");
    gst_object_unref (elem);
    goto error;
  }

  shm->src = elem;

  shm->funnelpad = gst_element_get_request_pad (self->priv->funnels[component],
      "sink_%u");

  if (!shm->funnelpad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get funnelpad");
    goto error;
  }

  pad = gst_element_get_static_pad (shm->src, "src");
  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, shm->funnelpad)))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION, "Could not link tee"
        " and valve");
    gst_object_unref (pad);
    goto error;
  }

  gst_object_unref (pad);

  if (got_buffer_func)
    shm->buffer_probe = gst_pad_add_probe (shm->funnelpad,
        GST_PAD_PROBE_TYPE_BUFFER,
        src_buffer_probe_cb, shm, NULL);

  if (!gst_element_sync_state_with_parent (shm->src))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new shmsrc with its parent");
    goto error;
  }

  return shm;

 error:
  fs_app_transmitter_check_shm_src (self, shm, NULL);
  return NULL;
}

/*
 * Returns: %TRUE if the path is the same, other %FALSE and freeds the ShmSrc
 */

gboolean
fs_app_transmitter_check_shm_src (FsAppTransmitter *self, ShmSrc *shm,
    const gchar *path)
{
  if (path && !strcmp (path, shm->path))
    return TRUE;

  if (shm->buffer_probe)
    gst_pad_remove_probe (shm->funnelpad, shm->buffer_probe);
  shm->buffer_probe = 0;

  if (shm->funnelpad) {
    gst_element_release_request_pad (self->priv->funnels[shm->component],
        shm->funnelpad);
    gst_object_unref (shm->funnelpad);
  }
  shm->funnelpad = NULL;

  if (shm->src)
  {
    gst_element_set_locked_state (shm->src, TRUE);
    gst_element_set_state (shm->src, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_src), shm->src);
  }
  shm->src = NULL;

  g_free (shm->path);
  g_slice_free (ShmSrc, shm);

  return FALSE;
}



struct _ShmSink {
  guint component;
  gchar *path;
  GstElement *sink;
  GstElement *recvonly_filter;
  GstPad *teepad;

  ready ready_func;
  connection connected_func;
  gpointer cb_data;
};


static void
ready_cb (GstBin *bin, GstElement *elem, ShmSink *shm)
{
  gchar *path = NULL;

  if (elem != shm->sink)
    return;

  g_object_get (elem, "socket-path", &path, NULL);
  shm->ready_func (shm->component, path, shm->cb_data);
  g_free (path);
}


static void
connected_cb (GstBin *bin, gint id, ShmSink *shm)
{
  shm->connected_func (shm->component, id, shm->cb_data);
}

ShmSink *
fs_app_transmitter_get_shm_sink (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    ready ready_func,
    connection connected_func,
    gpointer cb_data,
    GError **error)
{
  ShmSink *shm = g_slice_new0 (ShmSink);
  GstElement *elem;
  GstPad *pad;

  GST_DEBUG ("Trying to add shm sink for c:%u path %s", component, path);

  shm->component = component;

  shm->path = g_strdup (path);

  shm->ready_func = ready_func;
  shm->connected_func = connected_func;
  shm->cb_data = cb_data;

  /* First add the sink */

  elem = gst_element_factory_make ("shmsink", NULL);
  if (!elem)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not make shmsink");
    goto error;
  }
  g_object_set (elem,
      "socket-path", path,
      "wait-for-connection", FALSE,
      "async", FALSE,
      "sync" , FALSE,
      NULL);

  if (ready_func)
    g_signal_connect (self->priv->gst_sink, "ready", G_CALLBACK (ready_cb),
        shm);

  if (connected_func)
    g_signal_connect (elem, "client-connected", G_CALLBACK (connected_cb), shm);

  if (!gst_bin_add (GST_BIN (self->priv->gst_sink), elem))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add shmsink to bin");
    gst_object_unref (elem);
    goto error;
  }

  shm->sink = elem;

  /* Second add the recvonly filter */

  elem = gst_element_factory_make ("valve", NULL);
  if (!elem)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not make valve");
    goto error;
  }

  if (!gst_bin_add (GST_BIN (self->priv->gst_sink), elem))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add recvonly filter to bin");
    gst_object_unref (elem);
    goto error;
  }

  shm->recvonly_filter = elem;

  /* Third connect these */

  if (!gst_element_link (shm->recvonly_filter, shm->sink))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not link recvonly filter and shmsink");
    goto error;
  }

  if (!gst_element_sync_state_with_parent (shm->sink))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new shmsink with its parent");
    goto error;
  }

  if (!gst_element_sync_state_with_parent (shm->recvonly_filter))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new recvonly filter  with its parent");
    goto error;
  }

  shm->teepad = gst_element_get_request_pad (self->priv->tees[component],
      "src_%u");

  if (!shm->teepad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get teepad");
    goto error;
  }

  pad = gst_element_get_static_pad (shm->recvonly_filter, "sink");
  if (GST_PAD_LINK_FAILED (gst_pad_link (shm->teepad, pad)))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION, "Could not link tee"
        " and valve");
    gst_object_unref (pad);
    goto error;
  }
  gst_object_unref (pad);

  return shm;

 error:
  fs_app_transmitter_check_shm_sink (self, shm, NULL);

  return NULL;
}

gboolean
fs_app_transmitter_check_shm_sink (FsAppTransmitter *self, ShmSink *shm,
    const gchar *path)
{
  if (path && !strcmp (path, shm->path))
    return TRUE;

  if (path)
    GST_DEBUG ("Replacing shm socket %s with %s", shm->path, path);
  else
    GST_DEBUG ("Freeing shm socket %s", shm->path);

  if (shm->teepad)
  {
    gst_element_release_request_pad (self->priv->tees[shm->component],
        shm->teepad);
    gst_object_unref (shm->teepad);
  }
  shm->teepad = NULL;

  if (shm->sink)
  {
    gst_element_set_locked_state (shm->sink, TRUE);
    gst_element_set_state (shm->sink, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_sink), shm->sink);
  }
  shm->sink = NULL;

  if (shm->recvonly_filter)
  {
    gst_element_set_locked_state (shm->recvonly_filter, TRUE);
    gst_element_set_state (shm->recvonly_filter, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_sink), shm->recvonly_filter);
  }
  shm->recvonly_filter = NULL;

  g_free (shm->path);
  g_slice_free (ShmSink, shm);

  return FALSE;
}


void
fs_app_transmitter_sink_set_sending (FsAppTransmitter *self, ShmSink *shm,
    gboolean sending)
{
  GObjectClass *klass = G_OBJECT_GET_CLASS (shm->recvonly_filter);

  if (g_object_class_find_property (klass, "drop"))
    g_object_set (shm->recvonly_filter, "drop", !sending, NULL);

  if (sending)
    gst_element_send_event (shm->sink,
        gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
            gst_structure_new ("GstForceKeyUnit",
              "all-headers", G_TYPE_BOOLEAN, TRUE,
              NULL)));
}
