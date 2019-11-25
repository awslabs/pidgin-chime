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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xcbimageutil.h"

#include <X11/Xlib-xcb.h>
#include <xcb/shm.h>

GType
gst_meta_xcbimage_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] = { "memory", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstMetaXcbImageSrcAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static gboolean
gst_meta_xcbimage_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstMetaXcbImage *emeta = (GstMetaXcbImage *) meta;

  emeta->parent = NULL;
  emeta->ximage = NULL;
#ifdef HAVE_XSHM
  emeta->SHMInfo.shmaddr = ((void *) -1);
  emeta->SHMInfo.shmid = -1;
  emeta->SHMInfo.readOnly = TRUE;
#endif
  emeta->width = emeta->height = emeta->size = 0;
  emeta->return_func = NULL;

  return TRUE;
}

const GstMetaInfo *
gst_meta_xcbimage_get_info (void)
{
  static const GstMetaInfo *meta_xcbimage_info = NULL;

  if (g_once_init_enter (&meta_xcbimage_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (gst_meta_xcbimage_api_get_type (), "GstMetaXcbImageSrc",
        sizeof (GstMetaXcbImage), (GstMetaInitFunction) gst_meta_xcbimage_init,
        (GstMetaFreeFunction) NULL, (GstMetaTransformFunction) NULL);
    g_once_init_leave (&meta_xcbimage_info, meta);
  }
  return meta_xcbimage_info;
}

#ifdef HAVE_XSHM
static gboolean error_caught = FALSE;

static int
xcbimageutil_handle_xerror (Display * display, XErrorEvent * xevent)
{
  char error_msg[1024];

  XGetErrorText (display, xevent->error_code, error_msg, 1024);
  GST_DEBUG ("xcbimageutil failed to use XShm calls. error: %s", error_msg);
  error_caught = TRUE;
  return 0;
}

/* This function checks that it is actually really possible to create an image
   using XShm */
gboolean
xcbimageutil_check_xshm_calls (GstXContext * xcontext)
{
  XImage *xcbimage;
  XShmSegmentInfo SHMInfo;
  size_t size;
  int (*handler) (Display *, XErrorEvent *);
  gboolean result = FALSE;
  gboolean did_attach = FALSE;

  g_return_val_if_fail (xcontext != NULL, FALSE);

  /* Sync to ensure any older errors are already processed */
  XSync (xcontext->disp, FALSE);

  /* Set defaults so we don't free these later unnecessarily */
  SHMInfo.shmaddr = ((void *) -1);
  SHMInfo.shmid = -1;

  /* Setting an error handler to catch failure */
  error_caught = FALSE;
  handler = XSetErrorHandler (xcbimageutil_handle_xerror);

  /* Trying to create a 1x1 xcbimage */
  GST_DEBUG ("XShmCreateImage of 1x1");

  xcbimage = XShmCreateImage (xcontext->disp, xcontext->visual,
      xcontext->depth, ZPixmap, NULL, &SHMInfo, 1, 1);

  /* Might cause an error, sync to ensure it is noticed */
  XSync (xcontext->disp, FALSE);
  if (!xcbimage || error_caught) {
    GST_WARNING ("could not XShmCreateImage a 1x1 image");
    goto beach;
  }
  size = xcbimage->height * xcbimage->bytes_per_line;

  SHMInfo.shmid = shmget (IPC_PRIVATE, size, IPC_CREAT | 0777);
  if (SHMInfo.shmid == -1) {
    GST_WARNING ("could not get shared memory of %" G_GSIZE_FORMAT " bytes",
        size);
    goto beach;
  }

  SHMInfo.shmaddr = shmat (SHMInfo.shmid, 0, 0);
  if (SHMInfo.shmaddr == ((void *) -1)) {
    GST_WARNING ("Failed to shmat: %s", g_strerror (errno));
    goto beach;
  }

  /* Delete the SHM segment. It will actually go away automatically
   * when we detach now */
  shmctl (SHMInfo.shmid, IPC_RMID, 0);

  xcbimage->data = SHMInfo.shmaddr;
  SHMInfo.readOnly = FALSE;

  if (XShmAttach (xcontext->disp, &SHMInfo) == 0) {
    GST_WARNING ("Failed to XShmAttach");
    goto beach;
  }

  /* Sync to ensure we see any errors we caused */
  XSync (xcontext->disp, FALSE);

  if (!error_caught) {
    did_attach = TRUE;
    /* store whether we succeeded in result */
    result = TRUE;
  }
beach:
  /* Sync to ensure we swallow any errors we caused and reset error_caught */
  XSync (xcontext->disp, FALSE);
  error_caught = FALSE;
  XSetErrorHandler (handler);

  if (did_attach) {
    XShmDetach (xcontext->disp, &SHMInfo);
    XSync (xcontext->disp, FALSE);
  }
  if (SHMInfo.shmaddr != ((void *) -1))
    shmdt (SHMInfo.shmaddr);
  if (xcbimage)
    XDestroyImage (xcbimage);
  return result;
}
#endif /* HAVE_XSHM */

static xcb_connection_t *
get_xcb_connection (Display *dpy)
{
  xcb_connection_t *conn;

  conn = XGetXCBConnection (dpy);
  if (xcb_connection_has_error (conn)) {
    g_warning ("Could not get XCB connection");
    return NULL;
  }

  return conn;
}

/* This function gets the X Display and global info about it. Everything is
   stored in our object and will be cleaned when the object is disposed. Note
   here that caps for supported format are generated without any window or
   image creation */
GstXContext *
xcbimageutil_xcontext_get (GstElement * parent, const gchar * display_name)
{
  GstXContext *xcontext = NULL;
  XPixmapFormatValues *px_formats = NULL;
  gint nb_formats = 0, i;

  xcontext = g_new0 (GstXContext, 1);

  xcontext->disp = XOpenDisplay (display_name);
  GST_DEBUG_OBJECT (parent, "opened display %p", xcontext->disp);
  if (!xcontext->disp) {
    g_free (xcontext);
    return NULL;
  }
  xcontext->conn = get_xcb_connection (xcontext->disp);
  xcontext->screen = DefaultScreenOfDisplay (xcontext->disp);
  xcontext->visual = DefaultVisualOfScreen (xcontext->screen);
  xcontext->root = RootWindowOfScreen (xcontext->screen);
  xcontext->white = WhitePixelOfScreen (xcontext->screen);
  xcontext->black = BlackPixelOfScreen (xcontext->screen);
  xcontext->depth = DefaultDepthOfScreen (xcontext->screen);

  xcontext->width = WidthOfScreen (xcontext->screen);
  xcontext->height = HeightOfScreen (xcontext->screen);

  xcontext->widthmm = WidthMMOfScreen (xcontext->screen);
  xcontext->heightmm = HeightMMOfScreen (xcontext->screen);

  xcontext->caps = NULL;

  GST_DEBUG_OBJECT (parent, "X reports %dx%d pixels and %d mm x %d mm",
      xcontext->width, xcontext->height, xcontext->widthmm, xcontext->heightmm);
  xcbimageutil_calculate_pixel_aspect_ratio (xcontext);

  /* We get supported pixmap formats at supported depth */
  px_formats = XListPixmapFormats (xcontext->disp, &nb_formats);

  if (!px_formats) {
    XCloseDisplay (xcontext->disp);
    g_free (xcontext);
    return NULL;
  }

  /* We get bpp value corresponding to our running depth */
  for (i = 0; i < nb_formats; i++) {
    if (px_formats[i].depth == xcontext->depth)
      xcontext->bpp = px_formats[i].bits_per_pixel;
  }

  XFree (px_formats);

  xcontext->endianness =
      (ImageByteOrder (xcontext->disp) ==
      LSBFirst) ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

#ifdef HAVE_XSHM
  /* Search for XShm extension support */
  if (xcb_get_extension_data (xcontext->conn, &xcb_shm_id)->present) {
    xcontext->use_xshm = TRUE;
    GST_DEBUG ("xcbimageutil is using XShm extension");
  } else {
    xcontext->use_xshm = FALSE;
    GST_DEBUG ("xcbimageutil is not using XShm extension");
  }
#endif /* HAVE_XSHM */

  /* our caps system handles 24/32bpp RGB as big-endian. */
  if ((xcontext->bpp == 24 || xcontext->bpp == 32) &&
      xcontext->endianness == G_LITTLE_ENDIAN) {
    xcontext->endianness = G_BIG_ENDIAN;
    xcontext->r_mask_output = GUINT32_TO_BE (xcontext->visual->red_mask);
    xcontext->g_mask_output = GUINT32_TO_BE (xcontext->visual->green_mask);
    xcontext->b_mask_output = GUINT32_TO_BE (xcontext->visual->blue_mask);
    if (xcontext->bpp == 24) {
      xcontext->r_mask_output >>= 8;
      xcontext->g_mask_output >>= 8;
      xcontext->b_mask_output >>= 8;
    }
  } else {
    xcontext->r_mask_output = xcontext->visual->red_mask;
    xcontext->g_mask_output = xcontext->visual->green_mask;
    xcontext->b_mask_output = xcontext->visual->blue_mask;
  }

  return xcontext;
}

/* This function cleans the X context. Closing the Display and unrefing the
   caps for supported formats. */
void
xcbimageutil_xcontext_clear (GstXContext * xcontext)
{
  g_return_if_fail (xcontext != NULL);

  if (xcontext->caps != NULL)
    gst_caps_unref (xcontext->caps);

  XCloseDisplay (xcontext->disp);

  g_free (xcontext);
}

/* This function calculates the pixel aspect ratio based on the properties
 * in the xcontext structure and stores it there. */
void
xcbimageutil_calculate_pixel_aspect_ratio (GstXContext * xcontext)
{
  gint par[][2] = {
    {1, 1},                     /* regular screen */
    {16, 15},                   /* PAL TV */
    {11, 10},                   /* 525 line Rec.601 video */
    {54, 59}                    /* 625 line Rec.601 video */
  };
  gint i;
  gint index;
  gdouble ratio;
  gdouble delta;

#define DELTA(idx) (ABS (ratio - ((gdouble) par[idx][0] / par[idx][1])))

  /* first calculate the "real" ratio based on the X values;
   * which is the "physical" w/h divided by the w/h in pixels of the display */
  ratio = (gdouble) (xcontext->widthmm * xcontext->height)
      / (xcontext->heightmm * xcontext->width);

  /* DirectFB's X in 720x576 reports the physical dimensions wrong, so
   * override here */
  if (xcontext->width == 720 && xcontext->height == 576) {
    ratio = 4.0 * 576 / (3.0 * 720);
  }
  GST_DEBUG ("calculated pixel aspect ratio: %f", ratio);

  /* now find the one from par[][2] with the lowest delta to the real one */
  delta = DELTA (0);
  index = 0;

  for (i = 1; i < sizeof (par) / (sizeof (gint) * 2); ++i) {
    gdouble this_delta = DELTA (i);

    if (this_delta < delta) {
      index = i;
      delta = this_delta;
    }
  }

  GST_DEBUG ("Decided on index %d (%d/%d)", index,
      par[index][0], par[index][1]);

  xcontext->par_n = par[index][0];
  xcontext->par_d = par[index][1];
  GST_DEBUG ("set xcontext PAR to %d/%d\n", xcontext->par_n, xcontext->par_d);
}

static gboolean
gst_xcbimagesrc_buffer_dispose (GstBuffer * xcbimage)
{
  GstElement *parent;
  GstMetaXcbImage *meta;
  gboolean ret = TRUE;

  meta = GST_META_XCBIMAGE_GET (xcbimage);

  parent = meta->parent;
  if (parent == NULL) {
    g_warning ("XcbImageSrcBuffer->xcbimagesrc == NULL");
    goto beach;
  }

  if (meta->return_func)
    ret = meta->return_func (parent, xcbimage);

beach:
  return ret;
}

void
gst_xcbimage_buffer_free (GstBuffer * xcbimage)
{
  GstMetaXcbImage *meta;

  meta = GST_META_XCBIMAGE_GET (xcbimage);

  /* make sure it is not recycled */
  meta->width = -1;
  meta->height = -1;
  gst_buffer_unref (xcbimage);
}

/* This function handles GstXcbImageSrcBuffer creation depending on XShm availability */
GstBuffer *
gst_xcbimageutil_xcbimage_new (GstXContext * xcontext,
    GstElement * parent, int width, int height, BufferReturnFunc return_func)
{
  GstBuffer *xcbimage = NULL;
  GstMetaXcbImage *meta;
  gboolean succeeded = FALSE;

  xcbimage = gst_buffer_new ();
  GST_MINI_OBJECT_CAST (xcbimage)->dispose =
      (GstMiniObjectDisposeFunction) gst_xcbimagesrc_buffer_dispose;

  meta = GST_META_XCBIMAGE_ADD (xcbimage);
  meta->width = width;
  meta->height = height;

#ifdef HAVE_XSHM
  meta->SHMInfo.shmaddr = ((void *) -1);
  meta->SHMInfo.shmid = -1;

  if (xcontext->use_xshm) {
    meta->ximage = XShmCreateImage (xcontext->disp,
        xcontext->visual, xcontext->depth,
        ZPixmap, NULL, &meta->SHMInfo, meta->width, meta->height);
    if (!meta->ximage) {
      GST_WARNING_OBJECT (parent,
          "could not XShmCreateImage a %dx%d image", meta->width, meta->height);

      /* Retry without XShm */
      xcontext->use_xshm = FALSE;
      goto no_xshm;
    }

    /* we have to use the returned bytes_per_line for our shm size */
    meta->size = meta->ximage->bytes_per_line * meta->ximage->height;
    meta->SHMInfo.shmid = shmget (IPC_PRIVATE, meta->size, IPC_CREAT | 0777);
    if (meta->SHMInfo.shmid == -1)
      goto beach;

    meta->SHMInfo.shmaddr = shmat (meta->SHMInfo.shmid, 0, 0);
    if (meta->SHMInfo.shmaddr == ((void *) -1))
      goto beach;

    /* Delete the SHM segment. It will actually go away automatically
     * when we detach now */
    shmctl (meta->SHMInfo.shmid, IPC_RMID, 0);

    meta->ximage->data = meta->SHMInfo.shmaddr;
    meta->SHMInfo.readOnly = FALSE;

    if (XShmAttach (xcontext->disp, &meta->SHMInfo) == 0)
      goto beach;

    XSync (xcontext->disp, FALSE);
  } else
  no_xshm:
#endif /* HAVE_XSHM */
  {
    meta->ximage = XCreateImage (xcontext->disp,
        xcontext->visual,
        xcontext->depth,
        ZPixmap, 0, NULL, meta->width, meta->height, xcontext->bpp, 0);
    if (!meta->ximage)
      goto beach;

    /* we have to use the returned bytes_per_line for our image size */
    meta->size = meta->ximage->bytes_per_line * meta->ximage->height;
    meta->ximage->data = g_malloc (meta->size);

    XSync (xcontext->disp, FALSE);
  }
  succeeded = TRUE;

  gst_buffer_append_memory (xcbimage,
      gst_memory_new_wrapped (GST_MEMORY_FLAG_NO_SHARE, meta->ximage->data,
          meta->size, 0, meta->size, NULL, NULL));

  /* Keep a ref to our src */
  meta->parent = gst_object_ref (parent);
  meta->return_func = return_func;
beach:
  if (!succeeded) {
    gst_xcbimage_buffer_free (xcbimage);
    xcbimage = NULL;
  }

  return xcbimage;
}

/* This function destroys a GstXcbImageBuffer handling XShm availability */
void
gst_xcbimageutil_xcbimage_destroy (GstXContext * xcontext, GstBuffer * xcbimage)
{
  GstMetaXcbImage *meta;

  meta = GST_META_XCBIMAGE_GET (xcbimage);

  /* We might have some buffers destroyed after changing state to NULL */
  if (!xcontext)
    goto beach;

  g_return_if_fail (xcbimage != NULL);

#ifdef HAVE_XSHM
  if (xcontext->use_xshm) {
    if (meta->SHMInfo.shmaddr != ((void *) -1)) {
      XShmDetach (xcontext->disp, &meta->SHMInfo);
      XSync (xcontext->disp, 0);
      shmdt (meta->SHMInfo.shmaddr);
    }
    if (meta->ximage)
      XDestroyImage (meta->ximage);

  } else
#endif /* HAVE_XSHM */
  {
    if (meta->ximage) {
      XDestroyImage (meta->ximage);
    }
  }

  XSync (xcontext->disp, FALSE);
beach:
  if (meta->parent) {
    /* Release the ref to our parent */
    gst_object_unref (meta->parent);
    meta->parent = NULL;
  }

  return;
}
