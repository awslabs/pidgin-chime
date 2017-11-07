/*
 * Farstream - Farstream Shared Memory Stream Transmitter
 *
 * Copyright 2007-2008 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007-2008 Nokia Corp.
 *
 * fs-app-stream-transmitter.h - A Farstream Shared Memory stream transmitter
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

#ifndef __FS_APP_STREAM_TRANSMITTER_H__
#define __FS_APP_STREAM_TRANSMITTER_H__

#include <glib.h>
#include <glib-object.h>

#include <farstream/fs-stream-transmitter.h>
#include <farstream/fs-plugin.h>
#include "fs-app-transmitter.h"

G_BEGIN_DECLS

/* TYPE MACROS */
#define FS_TYPE_APP_STREAM_TRANSMITTER \
  (fs_app_stream_transmitter_get_type ())
#define FS_APP_STREAM_TRANSMITTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), FS_TYPE_APP_STREAM_TRANSMITTER, \
                              FsAppStreamTransmitter))
#define FS_APP_STREAM_TRANSMITTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), FS_TYPE_APP_STREAM_TRANSMITTER, \
                           FsAppStreamTransmitterClass))
#define FS_IS_APP_STREAM_TRANSMITTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), FS_TYPE_APP_STREAM_TRANSMITTER))
#define FS_IS_APP_STREAM_TRANSMITTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), FS_TYPE_APP_STREAM_TRANSMITTER))
#define FS_APP_STREAM_TRANSMITTER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), FS_TYPE_APP_STREAM_TRANSMITTER, \
                              FsAppStreamTransmitterClass))
#define FS_APP_STREAM_TRANSMITTER_CAST(obj) ((FsAppStreamTransmitter *) (obj))

typedef struct _FsAppStreamTransmitter FsAppStreamTransmitter;
typedef struct _FsAppStreamTransmitterClass FsAppStreamTransmitterClass;
typedef struct _FsAppStreamTransmitterPrivate FsAppStreamTransmitterPrivate;

/**
 * FsAppStreamTransmitterClass:
 * @parent_class: Our parent
 *
 * The Shared Memory stream transmitter class
 */

struct _FsAppStreamTransmitterClass
{
  FsStreamTransmitterClass parent_class;

  /*virtual functions */
  /*< private >*/
};

/**
 * FsAppStreamTransmitter:
 * @parent: Parent object
 *
 * All members are private, access them using methods and properties
 */
struct _FsAppStreamTransmitter
{
  FsStreamTransmitter parent;

  /*< private >*/
  FsAppStreamTransmitterPrivate *priv;
};

GType fs_app_stream_transmitter_register_type (FsPlugin *module);

GType fs_app_stream_transmitter_get_type (void);

FsAppStreamTransmitter *
fs_app_stream_transmitter_newv (FsAppTransmitter *transmitter,
  guint n_parameters, GParameter *parameters, GError **error);

G_END_DECLS

#endif /* __FS_APP_STREAM_TRANSMITTER_H__ */
