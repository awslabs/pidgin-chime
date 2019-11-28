/* GStreamer
 * Copyright (C) <2005> Luca Ognibene <luogni@tin.it>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#ifndef __GST_XCBIMAGEUTIL_H__
#define __GST_XCBIMAGEUTIL_H__

#include <gst/gst.h>

#ifdef HAVE_XSHM
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif /* HAVE_XSHM */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef HAVE_XSHM
#include <X11/extensions/XShm.h>
#endif /* HAVE_XSHM */

#include <xcb/xcb.h>

#include <string.h>
#include <math.h>

G_BEGIN_DECLS

typedef struct _GstXContext GstXContext;
typedef struct _GstXWindow GstXWindow;
typedef struct _GstXcbImage GstXcbImage;
typedef struct _GstMetaXcbImage GstMetaXcbImage;

/* Global X Context stuff */
/**
 * GstXContext:
 * @disp: the X11 Display of this context
 * @screen: the default Screen of Display @disp
 * @visual: the default Visual of Screen @screen
 * @root: the root Window of Display @disp
 * @white: the value of a white pixel on Screen @screen
 * @black: the value of a black pixel on Screen @screen
 * @depth: the color depth of Display @disp
 * @bpp: the number of bits per pixel on Display @disp
 * @endianness: the endianness of image bytes on Display @disp
 * @width: the width in pixels of Display @disp
 * @height: the height in pixels of Display @disp
 * @widthmm: the width in millimeters of Display @disp
 * @heightmm: the height in millimeters of Display @disp
 * @par_n: the pixel aspect ratio numerator calculated from @width, @widthmm
 * and @height,
 * @par_d: the pixel aspect ratio denumerator calculated from @width, @widthmm
 * and @height,
 * @heightmm ratio
 * @use_xshm: used to known wether of not XShm extension is usable or not even
 * if the Extension is present
 * @caps: the #GstCaps that Display @disp can accept
 *
 * Structure used to store various information collected/calculated for a
 * Display.
 */
struct _GstXContext {
  Display *disp;
  xcb_connection_t *conn;

  xcb_screen_t *screen;

  Visual *visual;

  xcb_window_t root;

  gulong white, black;

  gint depth;
  gint bpp;
  gint endianness;

  gint width, height;
  gint widthmm, heightmm;

  /* these are the output masks
   * for buffers from xcbimagesrc
   * and are in big endian */
  guint32 r_mask_output, g_mask_output, b_mask_output;

  guint par_n;                  /* calculated pixel aspect ratio numerator */
  guint par_d;                  /* calculated pixel aspect ratio denumerator */

  gboolean use_xshm;

  GstCaps *caps;
};

/**
 * GstXWindow:
 * @win: the Window ID of this X11 window
 * @width: the width in pixels of Window @win
 * @height: the height in pixels of Window @win
 * @internal: used to remember if Window @win was created internally or passed
 * through the #GstXOverlay interface
 * @gc: the Graphical Context of Window @win
 *
 * Structure used to store information about a Window.
 */
struct _GstXWindow {
  Window win;
  gint width, height;
  gboolean internal;
  GC gc;
};

GstXContext *xcbimageutil_xcontext_get (GstElement *parent, 
    const gchar *display_name);
void xcbimageutil_xcontext_clear (GstXContext *xcontext);
void xcbimageutil_calculate_pixel_aspect_ratio (GstXContext * xcontext);

/* custom xcbimagesrc buffer, copied from xcbimagesink */

/* BufferReturnFunc is called when a buffer is finalised */
typedef gboolean (*BufferReturnFunc) (GstElement *parent, GstBuffer *buf);

/**
 * GstMetaXcbimage:
 * @parent: a reference to the element we belong to
 * @xcbimage: the Xcbimage of this buffer
 * @width: the width in pixels of Xcbimage @xcbimage
 * @height: the height in pixels of Xcbimage @xcbimage
 * @size: the size in bytes of Xcbimage @xcbimage
 *
 * Extra data attached to buffers containing additional information about an Xcbimage.
 */
struct _GstMetaXcbImage {
  GstMeta meta;

  /* Reference to the xcbimagesrc we belong to */
  GstElement *parent;

  XImage *ximage;

#ifdef HAVE_XSHM
  XShmSegmentInfo SHMInfo;
#endif /* HAVE_XSHM */

  gint width, height;
  size_t size;

  BufferReturnFunc return_func;
};

GType gst_meta_xcbimage_api_get_type (void);
const GstMetaInfo * gst_meta_xcbimage_get_info (void);
#define GST_META_XCBIMAGE_GET(buf) ((GstMetaXcbImage *)gst_buffer_get_meta(buf,gst_meta_xcbimage_api_get_type()))
#define GST_META_XCBIMAGE_ADD(buf) ((GstMetaXcbImage *)gst_buffer_add_meta(buf,gst_meta_xcbimage_get_info(),NULL))

GstBuffer *gst_xcbimageutil_xcbimage_new (GstXContext *xcontext,
  GstElement *parent, int width, int height, BufferReturnFunc return_func);

void gst_xcbimageutil_xcbimage_destroy (GstXContext *xcontext, 
  GstBuffer * xcbimage);
  
/* Call to manually release a buffer */
void gst_xcbimage_buffer_free (GstBuffer *xcbimage);

G_END_DECLS 

#endif /* __GST_XCBIMAGEUTIL_H__ */
