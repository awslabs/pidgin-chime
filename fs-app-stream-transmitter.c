/*
 * Farstream - Farstream Shared Memory Stream Transmitter
 *
 * Copyright 2009 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2009 Nokia Corp.
 *
 * fs-app-stream-transmitter.c - A Farstream Shared memory stream transmitter
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
 * SECTION:fs-app-stream-transmitter
 * @short_description: A stream transmitter object for Shared Memory
 *
 * The name of this transmitter is "shm".
 *
 * This transmitter is meant to send and received the data from another process
 * on the same system while minimizing the memory pressure associated with the
 * use of sockets.
 *
 * Two sockets are used to control the shared memory areas. One is used to
 * send data and one to receive data. The receiver always connects to the
 * sender. The sender socket must exist before the receiver connects to it.
 *
 * Negotiating the paths of the sockets can happen in two ways. If the
 * create-local-candidates is True then the transmitter will generate the
 * path of the local candidate and us it as the ip filed in #FsCandidate. The
 * transmitter will expect the path of the applications sender socket to be in
 * the "ip" field of the remote candidates #FsCandidate as well.
 *
 * Or alternatively, if create-local-candidates is false then
 * the sender socket can be created by giving the transmitter a candidate
 * with the path of the socket in the "ip" field of the #FsCandidate. This
 * #FsCandidate can be given to the #FsStreamTransmitter in two ways, either
 * by setting the #FsStreamTransmitter:preferred-local-candidates property
 * or by calling the fs_stream_transmitter_force_remote_candidates() function.
 * There can be only one single send socket per stream. When the send socket
 * is ready to be connected to, #FsStreamTransmitter::new-local-candidate signal
 * will be emitted.
 *
 * To connect the receive side to the other application, one must create a
 * #FsCandidate with the path of the sender's socket in the "username" field.
 * If the receiver can not connect to the sender,
 * the fs_stream_transmitter_force_remote_candidates() call will fail.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-app-stream-transmitter.h"
#include "fs-app-transmitter.h"

#include <farstream/fs-candidate.h>
#include <farstream/fs-conference.h>

#include <gst/gst.h>

#include <glib/gstdio.h>

#include <string.h>
#include <sys/types.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <stdlib.h>

GST_DEBUG_CATEGORY_EXTERN (fs_app_transmitter_debug);
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
  PROP_SENDING,
  PROP_PREFERRED_LOCAL_CANDIDATES,
  PROP_CREATE_LOCAL_CANDIDATES,
};

struct _FsAppStreamTransmitterPrivate
{
  /* We don't actually hold a ref to this,
   * But since our parent FsStream can not exist without its parent
   * FsSession, we should be safe
   */
  FsAppTransmitter *transmitter;

  GList *preferred_local_candidates;

  GMutex mutex;

  /* Protected by the mutex */
  gboolean sending;

  /* Protected by the mutex */
  FsCandidate **candidates;

  /* Whether we create the local candidate ourselves or rely on the remote end
   * to pass them to us as part of the candidate */
  gboolean create_local_candidates;

  /* temporary socket directy in case we made one */
  gchar *socket_dir;

  ShmSrc **shm_src;
  ShmSink **shm_sink;
};

#define FS_APP_STREAM_TRANSMITTER_GET_PRIVATE(o)  \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_APP_STREAM_TRANSMITTER, \
                                FsAppStreamTransmitterPrivate))

#define FS_APP_STREAM_TRANSMITTER_LOCK(s) \
  g_mutex_lock (&(s)->priv->mutex)
#define FS_APP_STREAM_TRANSMITTER_UNLOCK(s) \
  g_mutex_unlock (&(s)->priv->mutex)

static void fs_app_stream_transmitter_class_init (FsAppStreamTransmitterClass *klass);
static void fs_app_stream_transmitter_init (FsAppStreamTransmitter *self);
static void fs_app_stream_transmitter_dispose (GObject *object);
static void fs_app_stream_transmitter_finalize (GObject *object);

static void fs_app_stream_transmitter_get_property (GObject *object,
                                                guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec);
static void fs_app_stream_transmitter_set_property (GObject *object,
                                                guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec);

static gboolean fs_app_stream_transmitter_force_remote_candidates (
    FsStreamTransmitter *streamtransmitter, GList *candidates,
    GError **error);
static gboolean fs_app_stream_transmitter_gather_local_candidates (
    FsStreamTransmitter *streamtransmitter,
    GError **error);

static gboolean
fs_app_stream_transmitter_add_sink (FsAppStreamTransmitter *self,
    FsCandidate *candidate, GError **error);


static GObjectClass *parent_class = NULL;
// static guint signals[LAST_SIGNAL] = { 0 };

static GType type = 0;

GType
fs_app_stream_transmitter_get_type (void)
{
  return type;
}

GType
fs_app_stream_transmitter_register_type (FsPlugin *module G_GNUC_UNUSED)
{
  static const GTypeInfo info = {
    sizeof (FsAppStreamTransmitterClass),
    NULL,
    NULL,
    (GClassInitFunc) fs_app_stream_transmitter_class_init,
    NULL,
    NULL,
    sizeof (FsAppStreamTransmitter),
    0,
    (GInstanceInitFunc) fs_app_stream_transmitter_init
  };

  type = g_type_register_static (FS_TYPE_STREAM_TRANSMITTER,
      "FsAppStreamTransmitter", &info, 0);

  return type;
}

static void
fs_app_stream_transmitter_class_init (FsAppStreamTransmitterClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  FsStreamTransmitterClass *streamtransmitterclass =
    FS_STREAM_TRANSMITTER_CLASS (klass);
  GParamSpec *pspec;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = fs_app_stream_transmitter_set_property;
  gobject_class->get_property = fs_app_stream_transmitter_get_property;

  streamtransmitterclass->force_remote_candidates =
    fs_app_stream_transmitter_force_remote_candidates;
  streamtransmitterclass->gather_local_candidates =
    fs_app_stream_transmitter_gather_local_candidates;

  g_object_class_override_property (gobject_class, PROP_SENDING, "sending");
  g_object_class_override_property (gobject_class,
      PROP_PREFERRED_LOCAL_CANDIDATES, "preferred-local-candidates");

  pspec = g_param_spec_boolean ("create-local-candidates",
    "CreateLocalCandidates",
    "Whether the transmitter should automatically create local candidates",
    FALSE,
    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (gobject_class,
    PROP_CREATE_LOCAL_CANDIDATES,
    pspec);


  gobject_class->dispose = fs_app_stream_transmitter_dispose;
  gobject_class->finalize = fs_app_stream_transmitter_finalize;

  g_type_class_add_private (klass, sizeof (FsAppStreamTransmitterPrivate));
}

static void
fs_app_stream_transmitter_init (FsAppStreamTransmitter *self)
{
  /* member init */
  self->priv = FS_APP_STREAM_TRANSMITTER_GET_PRIVATE (self);

  self->priv->sending = TRUE;

  g_mutex_init (&self->priv->mutex);
}

static void
fs_app_stream_transmitter_dispose (GObject *object)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER (object);
  gint c; /* component_id */

  for (c = 1; c <= self->priv->transmitter->components; c++)
  {
    if (self->priv->shm_src[c])
    {
      fs_app_transmitter_check_shm_src (self->priv->transmitter,
          self->priv->shm_src[c], NULL);
    }
    self->priv->shm_src[c] = NULL;

    if (self->priv->shm_sink[c])
    {
      fs_app_transmitter_check_shm_sink (self->priv->transmitter,
          self->priv->shm_sink[c], NULL);
    }
    self->priv->shm_sink[c] = NULL;
  }

  if (self->priv->socket_dir != NULL)
    g_rmdir (self->priv->socket_dir);
  g_free (self->priv->socket_dir);
  self->priv->socket_dir = NULL;

  parent_class->dispose (object);
}

static void
fs_app_stream_transmitter_finalize (GObject *object)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER (object);

  fs_candidate_list_destroy (self->priv->preferred_local_candidates);

  g_free (self->priv->shm_src);
  g_free (self->priv->shm_sink);
  g_mutex_clear (&self->priv->mutex);

  parent_class->finalize (object);
}

static void
fs_app_stream_transmitter_get_property (GObject *object,
                                           guint prop_id,
                                           GValue *value,
                                           GParamSpec *pspec)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER (object);

  switch (prop_id)
  {
    case PROP_SENDING:
      FS_APP_STREAM_TRANSMITTER_LOCK (self);
      g_value_set_boolean (value, self->priv->sending);
      FS_APP_STREAM_TRANSMITTER_UNLOCK (self);
      break;
    case PROP_PREFERRED_LOCAL_CANDIDATES:
      g_value_set_boxed (value, self->priv->preferred_local_candidates);
      break;
    case PROP_CREATE_LOCAL_CANDIDATES:
      g_value_set_boolean (value, self->priv->create_local_candidates);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_app_stream_transmitter_set_property (GObject *object,
                                           guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER (object);

  switch (prop_id) {
    case PROP_SENDING:
      FS_APP_STREAM_TRANSMITTER_LOCK (self);
      self->priv->sending = g_value_get_boolean (value);
      if (self->priv->shm_sink[1])
        fs_app_transmitter_sink_set_sending (self->priv->transmitter,
            self->priv->shm_sink[1], self->priv->sending);
      FS_APP_STREAM_TRANSMITTER_UNLOCK (self);
      break;
    case PROP_PREFERRED_LOCAL_CANDIDATES:
      self->priv->preferred_local_candidates = g_value_dup_boxed (value);
      break;
    case PROP_CREATE_LOCAL_CANDIDATES:
      self->priv->create_local_candidates = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
fs_app_stream_transmitter_build (FsAppStreamTransmitter *self,
  GError **error)
{
  self->priv->shm_src = g_new0 (ShmSrc *,
      self->priv->transmitter->components + 1);
  self->priv->shm_sink = g_new0 (ShmSink *,
      self->priv->transmitter->components + 1);

  return TRUE;
}

static void
got_buffer_func (GstBuffer *buffer, guint component, gpointer data)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER_CAST (data);

  g_signal_emit_by_name (self, "known-source-packet-received", component,
      buffer);
}

static void ready_cb (guint component, gchar *path, gpointer data)
{
  FsAppStreamTransmitter *self = FS_APP_STREAM_TRANSMITTER_CAST (data);
  FsCandidate *candidate = fs_candidate_new (NULL, component,
      FS_CANDIDATE_TYPE_HOST, FS_NETWORK_PROTOCOL_UDP, path, 0);

  GST_DEBUG ("Emitting new local candidate with path %s", path);

  g_signal_emit_by_name (self, "new-local-candidate", candidate);
  g_signal_emit_by_name (self, "local-candidates-prepared");

  fs_candidate_destroy (candidate);
}

static void
connected_cb (guint component, gint id, gpointer data)
{
  FsAppStreamTransmitter *self = data;

  g_signal_emit_by_name (self, "state-changed", component,
      FS_STREAM_STATE_READY);
}

static gboolean
fs_app_stream_transmitter_add_sink (FsAppStreamTransmitter *self,
    FsCandidate *candidate, GError **error)
{
  if (self->priv->create_local_candidates)
    return TRUE;

  if (!candidate->ip || !candidate->ip[0])
    return TRUE;

  if (self->priv->shm_sink[candidate->component_id])
  {
    if (fs_app_transmitter_check_shm_sink (self->priv->transmitter,
            self->priv->shm_sink[candidate->component_id], candidate->ip))
      return TRUE;
    self->priv->shm_sink[candidate->component_id] = NULL;
  }

  self->priv->shm_sink[candidate->component_id] =
    fs_app_transmitter_get_shm_sink (self->priv->transmitter,
        candidate->component_id, candidate->ip, ready_cb, connected_cb,
        self, error);

  if (self->priv->shm_sink[candidate->component_id] == NULL)
    return FALSE;

  if (candidate->component_id == 1)
    fs_app_transmitter_sink_set_sending (self->priv->transmitter,
        self->priv->shm_sink[candidate->component_id], self->priv->sending);

  return TRUE;
}


static void
disconnected_cb (guint component, gint id, gpointer data)
{
  FsAppStreamTransmitter *self = data;

  g_signal_emit_by_name (self, "state-changed", component,
      FS_STREAM_STATE_FAILED);
}

static gboolean
fs_app_stream_transmitter_force_remote_candidate (
    FsAppStreamTransmitter *self, FsCandidate *candidate,
    GError **error)
{
  const gchar *path;
  if (!fs_app_stream_transmitter_add_sink (self, candidate, error))
    return FALSE;

  if (self->priv->create_local_candidates)
    path = candidate->ip;
  else
    path = candidate->username;

  if (path && path[0])
  {
    if (self->priv->shm_src[candidate->component_id])
    {
      if (fs_app_transmitter_check_shm_src (self->priv->transmitter,
              self->priv->shm_src[candidate->component_id], path))
        return TRUE;
      self->priv->shm_src[candidate->component_id] = NULL;
    }

    self->priv->shm_src[candidate->component_id] =
      fs_app_transmitter_get_shm_src (self->priv->transmitter,
          candidate->component_id, path, got_buffer_func, disconnected_cb,
          self, error);

    if (self->priv->shm_src[candidate->component_id] == NULL)
      return FALSE;
  }

  return TRUE;
}

/**
 * fs_app_stream_transmitter_force_remote_candidates
 */

static gboolean
fs_app_stream_transmitter_force_remote_candidates (
    FsStreamTransmitter *streamtransmitter, GList *candidates,
    GError **error)
{
  GList *item = NULL;
  FsAppStreamTransmitter *self =
    FS_APP_STREAM_TRANSMITTER (streamtransmitter);

  for (item = candidates; item; item = g_list_next (item))
  {
    FsCandidate *candidate = item->data;

    if (candidate->component_id == 0 ||
        candidate->component_id > self->priv->transmitter->components) {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The candidate passed has an invalid component id %u (not in [1,%u])",
          candidate->component_id, self->priv->transmitter->components);
      return FALSE;
    }

    if ((!candidate->ip || !candidate->ip[0]) &&
        (!candidate->username || !candidate->username[0]))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The candidate does not have a SINK shm segment in its ip"
          " or a SRC shm segment in its username");
      return FALSE;
    }
  }

  for (item = candidates; item; item = g_list_next (item))
    if (!fs_app_stream_transmitter_force_remote_candidate (self,
            item->data, error))
      return FALSE;


  return TRUE;
}


FsAppStreamTransmitter *
fs_app_stream_transmitter_newv (FsAppTransmitter *transmitter,
  guint n_parameters, GParameter *parameters, GError **error)
{
  FsAppStreamTransmitter *streamtransmitter = NULL;

  streamtransmitter = g_object_newv (FS_TYPE_APP_STREAM_TRANSMITTER,
    n_parameters, parameters);

  if (!streamtransmitter) {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not build the stream transmitter");
    return NULL;
  }

  streamtransmitter->priv->transmitter = transmitter;

  if (!fs_app_stream_transmitter_build (streamtransmitter, error)) {
    g_object_unref (streamtransmitter);
    return NULL;
  }

  return streamtransmitter;
}


static gboolean
fs_app_stream_transmitter_gather_local_candidates (
    FsStreamTransmitter *streamtransmitter,
    GError **error)
{
  FsAppStreamTransmitter *self =
    FS_APP_STREAM_TRANSMITTER (streamtransmitter);
  GList *item;

  if (self->priv->create_local_candidates)
  {
    guint c;
    gchar *socket_dir;

    socket_dir = g_build_filename (g_get_tmp_dir (),
      "farstream-shm-XXXXXX", NULL);

    if (g_mkdtemp (socket_dir) == NULL)
      return FALSE;

    self->priv->socket_dir = socket_dir;

    for (c = 1; c <= self->priv->transmitter->components; c++)
    {
      gchar *path = g_strdup_printf ("%s/shm-sink-socket-%d", socket_dir, c);

      self->priv->shm_sink[c] =
        fs_app_transmitter_get_shm_sink (self->priv->transmitter,
          c, path, ready_cb, connected_cb, self, error);
      g_free (path);

      if (self->priv->shm_sink[c] == NULL)
        return FALSE;

      if (c == 1)
        fs_app_transmitter_sink_set_sending (self->priv->transmitter,
            self->priv->shm_sink[c], self->priv->sending);
    }

    return TRUE;
  }

  for (item = self->priv->preferred_local_candidates;
       item;
       item = g_list_next (item))
  {
    FsCandidate *candidate = item->data;

    if (candidate->ip && candidate->ip[0])
      if (!fs_app_stream_transmitter_add_sink (self, candidate, error))
        return FALSE;
  }

  return TRUE;
}
