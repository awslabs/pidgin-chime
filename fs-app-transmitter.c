/*
 * Farstream - Farstream App UDP Transmitter
 *
 * Copyright 2007-2008 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007-2008 Nokia Corp.
 *
 * fs-app-transmitter.c - A Farstream appsink/appsrc transmitter
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
 * @short_description: A transmitter for appsink/appsrc I/O
 *
 * This transmitter provides appsink/appsrc I/O
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
static GType app_bin_type = 0;
gpointer app_bin_parent_class = NULL;

typedef struct _FsAppBin
{
  GstBin parent;
} FsAppBin;

typedef struct _FsAppBinClass
{
  GstBinClass parent_class;
} FsAppBinClass;

static void fs_app_bin_init (FsAppBin *self)
{
}

static GstElement *
fs_app_bin_new (void)
{
  return g_object_new (app_bin_type, NULL);
}

static void
fs_app_bin_handle_message (GstBin *bin, GstMessage *message)
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

  GST_BIN_CLASS (app_bin_parent_class)->handle_message (bin, message);
}

static void fs_app_bin_class_init (FsAppBinClass *klass)
{
  GstBinClass *bin_class = GST_BIN_CLASS (klass);

  app_bin_parent_class = g_type_class_peek_parent (klass);

  bin_signals[BIN_SIGNAL_READY] =
    g_signal_new ("ready", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_ELEMENT);

  bin_signals[BIN_SIGNAL_DISCONNECTED] =
    g_signal_new ("disconnected", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_ELEMENT);

  bin_class->handle_message = GST_DEBUG_FUNCPTR (fs_app_bin_handle_message);
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
    sizeof (FsAppBinClass),
    NULL,
    NULL,
    (GClassInitFunc) fs_app_bin_class_init,
    NULL,
    NULL,
    sizeof (FsAppBin),
    0,
    (GInstanceInitFunc) fs_app_bin_init
  };


  GST_DEBUG_CATEGORY_INIT (fs_app_transmitter_debug,
      "fsapptransmitter", 0,
      "Farstream app UDP transmitter");

  fs_app_stream_transmitter_register_type (module);

  type = g_type_register_static (FS_TYPE_TRANSMITTER, "FsAppTransmitter",
      &info, 0);

  app_bin_type = g_type_register_static (
    GST_TYPE_BIN, "FsAppBin", &bin_info, 0);

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

  self->priv->gst_src = fs_app_bin_new ();

  if (!self->priv->gst_src) {
    trans->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not build the transmitter src bin");
    return;
  }

  gst_object_ref (self->priv->gst_src);


  /* Second, we do the sink element */

  self->priv->gst_sink = fs_app_bin_new ();

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


struct _AppSrc {
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
  AppSrc *app = user_data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  app->got_buffer_func (buffer, app->component, app->cb_data);

  return TRUE;
}


static void
disconnected_cb (GstBin *bin, GstElement *elem, AppSrc *app)
{
  if (elem != app->src)
    return;

  app->disconnected_func (app->component, 0, app->cb_data);
}


AppSrc *
fs_app_transmitter_get_app_src (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    got_buffer got_buffer_func,
    connection disconnected_func,
    gpointer cb_data,
    GError **error)
{
  AppSrc *app = g_slice_new0 (AppSrc);
  GstElement *elem;
  GstPad *pad;

  app->component = component;
  app->got_buffer_func = got_buffer_func;
  app->disconnected_func = disconnected_func;
  app->cb_data = cb_data;

  app->path = g_strdup (path);

  elem = gst_element_factory_make ("audiotestsrc", NULL);
  if (!elem)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not make appsrc");
    goto error;
  }

  g_object_set (elem,
      "socket-path", path,
      "do-timestamp", self->priv->do_timestamp,
      "is-live", TRUE,
      NULL);

  if (app->disconnected_func)
    g_signal_connect (self->priv->gst_src, "disconnected",
        G_CALLBACK (disconnected_cb), app);

  if (!gst_bin_add (GST_BIN (self->priv->gst_src), elem))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add recvonly filter to bin");
    gst_object_unref (elem);
    goto error;
  }

  app->src = elem;

  app->funnelpad = gst_element_get_request_pad (self->priv->funnels[component],
      "sink_%u");

  if (!app->funnelpad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get funnelpad");
    goto error;
  }

  pad = gst_element_get_static_pad (app->src, "src");
  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, app->funnelpad)))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION, "Could not link tee"
        " and valve");
    gst_object_unref (pad);
    goto error;
  }

  gst_object_unref (pad);

  if (got_buffer_func)
    app->buffer_probe = gst_pad_add_probe (app->funnelpad,
        GST_PAD_PROBE_TYPE_BUFFER,
        src_buffer_probe_cb, app, NULL);

  if (!gst_element_sync_state_with_parent (app->src))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new appsrc with its parent");
    goto error;
  }

  return app;

 error:
  fs_app_transmitter_check_app_src (self, app, NULL);
  return NULL;
}

/*
 * Returns: %TRUE if the path is the same, other %FALSE and freeds the AppSrc
 */

gboolean
fs_app_transmitter_check_app_src (FsAppTransmitter *self, AppSrc *app,
    const gchar *path)
{
  if (path && !strcmp (path, app->path))
    return TRUE;

  if (app->buffer_probe)
    gst_pad_remove_probe (app->funnelpad, app->buffer_probe);
  app->buffer_probe = 0;

  if (app->funnelpad) {
    gst_element_release_request_pad (self->priv->funnels[app->component],
        app->funnelpad);
    gst_object_unref (app->funnelpad);
  }
  app->funnelpad = NULL;

  if (app->src)
  {
    gst_element_set_locked_state (app->src, TRUE);
    gst_element_set_state (app->src, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_src), app->src);
  }
  app->src = NULL;

  g_free (app->path);
  g_slice_free (AppSrc, app);

  return FALSE;
}



struct _AppSink {
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
ready_cb (GstBin *bin, GstElement *elem, AppSink *app)
{
  gchar *path = NULL;

  if (elem != app->sink)
    return;

  g_object_get (elem, "socket-path", &path, NULL);
  app->ready_func (app->component, path, app->cb_data);
  g_free (path);
}


static void
connected_cb (GstBin *bin, gint id, AppSink *app)
{
  app->connected_func (app->component, id, app->cb_data);
}

AppSink *
fs_app_transmitter_get_app_sink (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    ready ready_func,
    connection connected_func,
    gpointer cb_data,
    GError **error)
{
  AppSink *app = g_slice_new0 (AppSink);
  GstElement *elem;
  GstPad *pad;

  GST_DEBUG ("Trying to add app sink for c:%u path %s", component, path);

  app->component = component;

  app->path = g_strdup (path);

  app->ready_func = ready_func;
  app->connected_func = connected_func;
  app->cb_data = cb_data;

  /* First add the sink */

  elem = gst_element_factory_make ("filesink", NULL);
  if (!elem)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not make appsink");
    goto error;
  }
  g_object_set (elem,
      "location", "/tmp/pidgin.s16",
      "async", FALSE,
      "sync" , FALSE,
      NULL);

  if (ready_func)
    g_signal_connect (self->priv->gst_sink, "ready", G_CALLBACK (ready_cb),
        app);

  if (connected_func)
    g_signal_connect (elem, "client-connected", G_CALLBACK (connected_cb), app);

  if (!gst_bin_add (GST_BIN (self->priv->gst_sink), elem))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add appsink to bin");
    gst_object_unref (elem);
    goto error;
  }

  app->sink = elem;

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

  app->recvonly_filter = elem;

  /* Third connect these */

  if (!gst_element_link (app->recvonly_filter, app->sink))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not link recvonly filter and appsink");
    goto error;
  }

  if (!gst_element_sync_state_with_parent (app->sink))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new appsink with its parent");
    goto error;
  }

  if (!gst_element_sync_state_with_parent (app->recvonly_filter))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the new recvonly filter  with its parent");
    goto error;
  }

  app->teepad = gst_element_get_request_pad (self->priv->tees[component],
      "src_%u");

  if (!app->teepad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get teepad");
    goto error;
  }

  pad = gst_element_get_static_pad (app->recvonly_filter, "sink");
  if (GST_PAD_LINK_FAILED (gst_pad_link (app->teepad, pad)))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION, "Could not link tee"
        " and valve");
    gst_object_unref (pad);
    goto error;
  }
  gst_object_unref (pad);

  return app;

 error:
  fs_app_transmitter_check_app_sink (self, app, NULL);

  return NULL;
}

gboolean
fs_app_transmitter_check_app_sink (FsAppTransmitter *self, AppSink *app,
    const gchar *path)
{
  if (path && !strcmp (path, app->path))
    return TRUE;

  if (path)
    GST_DEBUG ("Replacing app socket %s with %s", app->path, path);
  else
    GST_DEBUG ("Freeing app socket %s", app->path);

  if (app->teepad)
  {
    gst_element_release_request_pad (self->priv->tees[app->component],
        app->teepad);
    gst_object_unref (app->teepad);
  }
  app->teepad = NULL;

  if (app->sink)
  {
    gst_element_set_locked_state (app->sink, TRUE);
    gst_element_set_state (app->sink, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_sink), app->sink);
  }
  app->sink = NULL;

  if (app->recvonly_filter)
  {
    gst_element_set_locked_state (app->recvonly_filter, TRUE);
    gst_element_set_state (app->recvonly_filter, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->gst_sink), app->recvonly_filter);
  }
  app->recvonly_filter = NULL;

  g_free (app->path);
  g_slice_free (AppSink, app);

  return FALSE;
}


void
fs_app_transmitter_sink_set_sending (FsAppTransmitter *self, AppSink *app,
    gboolean sending)
{
  GObjectClass *klass = G_OBJECT_GET_CLASS (app->recvonly_filter);

  if (g_object_class_find_property (klass, "drop"))
    g_object_set (app->recvonly_filter, "drop", !sending, NULL);

  if (sending)
    gst_element_send_event (app->sink,
        gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
            gst_structure_new ("GstForceKeyUnit",
              "all-headers", G_TYPE_BOOLEAN, TRUE,
              NULL)));
}
