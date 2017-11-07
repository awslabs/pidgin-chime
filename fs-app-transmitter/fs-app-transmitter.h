/*
 * Farstream - Farstream Shared Memory Transmitter
 *
 * Copyright 2007-2008 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007-2008 Nokia Corp.
 *
 * fs-app-transmitter.h - A Farstream Shared Memory transmitter
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

#ifndef __FS_APP_TRANSMITTER_H__
#define __FS_APP_TRANSMITTER_H__

#include <farstream/fs-transmitter.h>

#include <gst/gst.h>

G_BEGIN_DECLS

/* TYPE MACROS */
#define FS_TYPE_APP_TRANSMITTER \
  (fs_app_transmitter_get_type ())
#define FS_APP_TRANSMITTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), FS_TYPE_APP_TRANSMITTER, \
    FsAppTransmitter))
#define FS_APP_TRANSMITTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), FS_TYPE_APP_TRANSMITTER, \
    FsAppTransmitterClass))
#define FS_IS_APP_TRANSMITTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), FS_TYPE_APP_TRANSMITTER))
#define FS_IS_APP_TRANSMITTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), FS_TYPE_APP_TRANSMITTER))
#define FS_APP_TRANSMITTER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), FS_TYPE_APP_TRANSMITTER, \
    FsAppTransmitterClass))
#define FS_APP_TRANSMITTER_CAST(obj) ((FsAppTransmitter *) (obj))

typedef struct _FsAppTransmitter FsAppTransmitter;
typedef struct _FsAppTransmitterClass FsAppTransmitterClass;
typedef struct _FsAppTransmitterPrivate FsAppTransmitterPrivate;

/**
 * FsAppTransmitterClass:
 * @parent_class: Our parent
 *
 * The Shared Memory transmitter class
 */

struct _FsAppTransmitterClass
{
  FsTransmitterClass parent_class;
};

/**
 * FsAppTransmitter:
 * @parent: Parent object
 *
 * All members are private, access them using methods and properties
 */
struct _FsAppTransmitter
{
  FsTransmitter parent;

  /* The number of components (READONLY) */
  gint components;

  /*< private >*/
  FsAppTransmitterPrivate *priv;
};

GType fs_app_transmitter_get_type (void);

typedef struct _AppSrc AppSrc;
typedef struct _AppSink AppSink;

typedef void (*got_buffer) (GstBuffer *buffer, guint component, gpointer data);
typedef void (*ready) (guint component, gchar *path, gpointer data);
typedef void (*connection) (guint component, gint id, gpointer data);

AppSrc *fs_app_transmitter_get_app_src (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    got_buffer got_buffer_func,
    connection disconnected_func,
    gpointer cb_data,
    GError **error);

gboolean fs_app_transmitter_check_app_src (FsAppTransmitter *self,
    AppSrc *app,
    const gchar *path);

AppSink *fs_app_transmitter_get_app_sink (FsAppTransmitter *self,
    guint component,
    const gchar *path,
    ready ready_func,
    connection connected_fubnc,
    gpointer cb_data,
    GError **error);

gboolean fs_app_transmitter_check_app_sink (FsAppTransmitter *self,
    AppSink *app,
    const gchar *path);

void fs_app_transmitter_sink_set_sending (FsAppTransmitter *self,
    AppSink *app, gboolean sending);



G_END_DECLS

#endif /* __FS_APP_TRANSMITTER_H__ */
