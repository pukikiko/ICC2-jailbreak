/*
 * livi-stride-fix - preload shim for the bundled gstreamer's waylandsink (LIVI 8.x).
 *
 * waylandsink builds the wl_shm buffer from the negotiated caps video info (stride
 * 3200 for 800x480 RGBx) but the frame memory videoconvert hands it is row-padded:
 * the GstBuffer is 3328*480 bytes, i.e. a stride of 3328. the nested compositor
 * reads the wl_shm buffer with stride 3200, so every row shifts 128 bytes and the
 * video arrives as the sheared "h-sync failure" picture while everything else on
 * the panel is fine.
 *
 * this interposes gst_wl_shm_memory_construct_wl_buffer (libgstwayland) and, for
 * single-plane frames whose memory is larger than the caps geometry, calls the real
 * implementation with a copy of the video info carrying the memory's real stride.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gst/video/video.h>

typedef struct _GstWlDisplay GstWlDisplay;

typedef struct wl_buffer *(*construct_fn) (GstMemory * mem,
    GstWlDisplay * display, const GstVideoInfo * info);

static construct_fn resolve_real (void)
{
  construct_fn fn;
  void *handle;

  handle = dlopen ("libgstwayland-1.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
  if (handle) {
    fn = (construct_fn) dlsym (handle,
        "gst_wl_shm_memory_construct_wl_buffer");
    if (fn)
      return fn;
  }
  return (construct_fn) dlsym (RTLD_NEXT,
      "gst_wl_shm_memory_construct_wl_buffer");
}

struct wl_buffer *
gst_wl_shm_memory_construct_wl_buffer (GstMemory * mem, GstWlDisplay * display,
    const GstVideoInfo * info)
{
  static construct_fn real;
  gsize memsize;
  gint stride;

  if (!real)
    real = resolve_real ();
  if (!real)
    return NULL;

  if (!getenv ("LIVI_STRIDE_FIX_QUIET"))
    fprintf (stderr, "[stride-fix] mem=%zu finfo=%p planes=%d h=%d w=%d caps_stride=%d\n",
        (size_t) mem->size, (void *) info->finfo,
        info->finfo ? info->finfo->n_planes : -1, info->height, info->width,
        info->stride[0]);

  if (!info || !info->finfo || info->finfo->n_planes != 1
      || info->height <= 0 || info->width <= 0)
    return real (mem, display, info);

  memsize = mem->size;
  if (memsize % (gsize) info->height != 0)
    return real (mem, display, info);
  stride = (gint) (memsize / (gsize) info->height);

  if (stride > info->stride[0]) {
    GstVideoInfo fixed = *info;

    fixed.stride[0] = stride;
    fixed.size = memsize;
    if (!getenv ("LIVI_STRIDE_FIX_QUIET"))
      fprintf (stderr, "[stride-fix] correcting stride %d -> %d\n",
          info->stride[0], stride);
    return real (mem, display, &fixed);
  }
  return real (mem, display, info);
}
