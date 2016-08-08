/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Based on platform_x11, which has
 *
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <dlfcn.h>
#include <xf86drm.h>

#if ANDROID_VERSION >= 0x402
#include <sync/sync.h>
#endif

#include "loader.h"
#include "egl_dri2.h"
#include "egl_dri2_fallbacks.h"
#include "gralloc_drm_handle.h"
#include "gralloc_drm.h"

static int
get_format_bpp(int native)
{
   int bpp;

   switch (native) {
   case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
      /*
       * HACK: Hardcode this to RGBX_8888 as per drm_gralloc hack.
       * TODO: Revert this once b/29886289 is fixed.
       */
   case HAL_PIXEL_FORMAT_RGBA_8888:
   case HAL_PIXEL_FORMAT_RGBX_8888:
   case HAL_PIXEL_FORMAT_BGRA_8888:
      bpp = 4;
      break;
   case HAL_PIXEL_FORMAT_RGB_888:
      bpp = 3;
      break;
   case HAL_PIXEL_FORMAT_RGB_565:
      bpp = 2;
      break;
   case HAL_PIXEL_FORMAT_YCbCr_420_888:
      /*
       * HACK: Hardcode this to YV12 as gralloc does for platforms using Mesa.
       * TODO: Revert this once b/29886289 is fixed.
       */
   case HAL_PIXEL_FORMAT_YV12:
      bpp = 1;
      break;
   default:
      bpp = 0;
      break;
   }

   return bpp;
}

static unsigned int get_fourcc_format(int hal_format)
{
   int format = 0;

   switch (hal_format) {
   case HAL_PIXEL_FORMAT_BGRA_8888:
      format = __DRI_IMAGE_FOURCC_ARGB8888;
      break;
   case HAL_PIXEL_FORMAT_RGB_565:
      format = __DRI_IMAGE_FOURCC_RGB565;
      break;
   case HAL_PIXEL_FORMAT_RGBA_8888:
      format = __DRI_IMAGE_FOURCC_ABGR8888;
      break;
   case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
      /*
       * HACK: Hardcode this to RGBX_8888 as per drm_gralloc hack.
       * TODO: Revert this once b/29886289 is fixed.
       */
   case HAL_PIXEL_FORMAT_RGBX_8888:
      format = __DRI_IMAGE_FOURCC_XBGR8888;
      break;
   case HAL_PIXEL_FORMAT_YCbCr_420_888:
      /*
       * HACK: Hardcode this to YV12 as gralloc does for platforms using Mesa.
       * TODO: Revert this once b/29886289 is fixed.
       */
   case HAL_PIXEL_FORMAT_YV12:
      format = __DRI_IMAGE_FOURCC_YUV420;
      break;
   default:
      break;
   }

   return format;
}

static int
get_native_buffer_fd(struct ANativeWindowBuffer *buf)
{
	struct gralloc_drm_handle_t *handle = gralloc_drm_handle(buf->handle);

	if (!handle)
		return -1;

	return handle->prime_fd;
}

static EGLBoolean
droid_window_dequeue_buffer(struct dri2_egl_surface *dri2_surf)
{
#if ANDROID_VERSION >= 0x0402
   int fence_fd;

   if (dri2_surf->window->dequeueBuffer(dri2_surf->window, &dri2_surf->buffer,
                                        &fence_fd))
      return EGL_FALSE;

   /* If access to the buffer is controlled by a sync fence, then block on the
    * fence.
    *
    * It may be more performant to postpone blocking until there is an
    * immediate need to write to the buffer. But doing so would require adding
    * hooks to the DRI2 loader.
    *
    * From the ANativeWindow::dequeueBuffer documentation:
    *
    *    The libsync fence file descriptor returned in the int pointed to by
    *    the fenceFd argument will refer to the fence that must signal
    *    before the dequeued buffer may be written to.  A value of -1
    *    indicates that the caller may access the buffer immediately without
    *    waiting on a fence.  If a valid file descriptor is returned (i.e.
    *    any value except -1) then the caller is responsible for closing the
    *    file descriptor.
    */
    if (fence_fd >= 0) {
       /* From the SYNC_IOC_WAIT documentation in <linux/sync.h>:
        *
        *    Waits indefinitely if timeout < 0.
        */
        int timeout = -1;
        sync_wait(fence_fd, timeout);
        close(fence_fd);
   }

   dri2_surf->buffer->common.incRef(&dri2_surf->buffer->common);
#else
   if (dri2_surf->window->dequeueBuffer(dri2_surf->window, &dri2_surf->buffer))
      return EGL_FALSE;

   dri2_surf->buffer->common.incRef(&dri2_surf->buffer->common);
   dri2_surf->window->lockBuffer(dri2_surf->window, dri2_surf->buffer);
#endif

   return EGL_TRUE;
}

static EGLBoolean
droid_window_enqueue_buffer(struct dri2_egl_surface *dri2_surf)
{
#if ANDROID_VERSION >= 0x0402
   /* Queue the buffer without a sync fence. This informs the ANativeWindow
    * that it may access the buffer immediately.
    *
    * From ANativeWindow::dequeueBuffer:
    *
    *    The fenceFd argument specifies a libsync fence file descriptor for
    *    a fence that must signal before the buffer can be accessed.  If
    *    the buffer can be accessed immediately then a value of -1 should
    *    be used.  The caller must not use the file descriptor after it
    *    is passed to queueBuffer, and the ANativeWindow implementation
    *    is responsible for closing it.
    */
   int fence_fd = -1;
   dri2_surf->window->queueBuffer(dri2_surf->window, dri2_surf->buffer,
                                  fence_fd);
#else
   dri2_surf->window->queueBuffer(dri2_surf->window, dri2_surf->buffer);
#endif

   dri2_surf->buffer->common.decRef(&dri2_surf->buffer->common);
   dri2_surf->buffer = NULL;

   return EGL_TRUE;
}

static void
droid_window_cancel_buffer(struct dri2_egl_surface *dri2_surf)
{
   /* no cancel buffer? */
   droid_window_enqueue_buffer(dri2_surf);
}

static __DRIbuffer *
droid_alloc_local_buffer(struct dri2_egl_surface *dri2_surf,
                         unsigned int att, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (att >= ARRAY_SIZE(dri2_surf->local_buffers))
      return NULL;

   if (!dri2_surf->local_buffers[att]) {
      dri2_surf->local_buffers[att] =
         dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen, att, format,
               dri2_surf->base.Width, dri2_surf->base.Height);
   }

   return dri2_surf->local_buffers[att];
}

static void
droid_free_local_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->local_buffers); i++) {
      if (dri2_surf->local_buffers[i]) {
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
               dri2_surf->local_buffers[i]);
         dri2_surf->local_buffers[i] = NULL;
      }
   }
}

static _EGLSurface *
droid_create_surface(_EGLDriver *drv, _EGLDisplay *disp, EGLint type,
		    _EGLConfig *conf, void *native_window,
		    const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   struct ANativeWindow *window = native_window;
   const __DRIconfig *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "droid_create_surface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, type, conf, attrib_list))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      int format;

      if (!window || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "droid_create_surface");
         goto cleanup_surface;
      }
      if (window->query(window, NATIVE_WINDOW_FORMAT, &format)) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "droid_create_surface");
         goto cleanup_surface;
      }

      if (format != dri2_conf->base.NativeVisualID) {
         _eglLog(_EGL_WARNING, "Native format mismatch: 0x%x != 0x%x",
               format, dri2_conf->base.NativeVisualID);
      }

      window->query(window, NATIVE_WINDOW_WIDTH, &dri2_surf->base.Width);
      window->query(window, NATIVE_WINDOW_HEIGHT, &dri2_surf->base.Height);
   }

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);

   dri2_surf->dri_drawable =
      (*dri2_dpy->dri2->createNewDrawable)(dri2_dpy->dri_screen, config,
                                           dri2_surf);
   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
      goto cleanup_surface;
   }

   if (window) {
      window->common.incRef(&window->common);
      dri2_surf->window = window;
   }

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
droid_create_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                            _EGLConfig *conf, void *native_window,
                            const EGLint *attrib_list)
{
   return droid_create_surface(drv, disp, EGL_WINDOW_BIT, conf,
                               native_window, attrib_list);
}

static _EGLSurface *
droid_create_pbuffer_surface(_EGLDriver *drv, _EGLDisplay *disp,
			    _EGLConfig *conf, const EGLint *attrib_list)
{
   return droid_create_surface(drv, disp, EGL_PBUFFER_BIT, conf,
			      NULL, attrib_list);
}

static EGLBoolean
droid_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   droid_free_local_buffers(dri2_surf);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (dri2_surf->buffer)
         droid_window_cancel_buffer(dri2_surf);

      dri2_surf->window->common.decRef(&dri2_surf->window->common);
   }

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   free(dri2_surf);

   return EGL_TRUE;
}

static EGLBoolean
droid_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   _EGLContext *ctx;

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return EGL_TRUE;

   if (dri2_drv->glFlush) {
      ctx = _eglGetCurrentContext();
      if (ctx && ctx->DrawSurface == &dri2_surf->base)
         dri2_drv->glFlush();
   }

   dri2_flush_drawable_for_swapbuffers(disp, draw);

   if (dri2_surf->buffer) {
      /* To avoid blocking other EGL calls, release the display mutex before
       * we enter droid_window_enqueue_buffer() and re-acquire the mutex upon
       * return.
       */
      mtx_unlock(&disp->Mutex);
      droid_window_enqueue_buffer(dri2_surf);
      mtx_lock(&disp->Mutex);
   }

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   return EGL_TRUE;
}

static EGLBoolean
droid_query_surface(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                    EGLint attribute, EGLint *value)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   switch (attribute) {
      case EGL_WIDTH:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
            dri2_surf->window->query(dri2_surf->window,
                                     NATIVE_WINDOW_DEFAULT_WIDTH, value);
            return EGL_TRUE;
         }
         break;
      case EGL_HEIGHT:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
            dri2_surf->window->query(dri2_surf->window,
                                     NATIVE_WINDOW_DEFAULT_HEIGHT, value);
            return EGL_TRUE;
         }
         break;
      default:
         break;
   }
   return _eglQuerySurface(drv, dpy, surf, attribute, value);
}

static _EGLImage *
dri2_create_image_android_native_buffer(_EGLDriver *drv, _EGLDisplay *disp,
					_EGLContext *ctx,
                                        struct ANativeWindowBuffer *buf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img;
   unsigned int total_planes = 1;
   unsigned int fourcc_format = 0;
   unsigned int offsets[3] = { 0, 0, 0 };
   unsigned int strides[3] = { 0, 0, 0 };
   /* This is ok for now as we will have only one fd
    * for all currently supported formats.
    */
   int fd, p, index;

   if (ctx != NULL) {
      /* From the EGL_ANDROID_image_native_buffer spec:
       *
       *     * If <target> is EGL_NATIVE_BUFFER_ANDROID and <ctx> is not
       *       EGL_NO_CONTEXT, the error EGL_BAD_CONTEXT is generated.
       */
      _eglError(EGL_BAD_CONTEXT, "eglCreateEGLImageKHR: for "
                "EGL_NATIVE_BUFFER_ANDROID, the context must be "
                "EGL_NO_CONTEXT");
      return NULL;
   }

   if (!buf || buf->common.magic != ANDROID_NATIVE_BUFFER_MAGIC ||
       buf->common.version != sizeof(*buf)) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   fd = get_native_buffer_fd(buf);
   if (fd < 0) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   fourcc_format = get_fourcc_format(buf->format);
   if (!fourcc_format) {
      _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%x", buf->format);
      return NULL;
   }

   dri2_img = calloc(1, sizeof(*dri2_img));
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "droid_create_image_mesa_drm");
      return NULL;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      free(dri2_img);
      return NULL;
   }

   strides[0] = buf->stride * get_format_bpp(buf->format);

   switch (buf->format) {
   case HAL_PIXEL_FORMAT_YCbCr_420_888:
      /*
       * HACK: Hardcode this to YV12 as gralloc does for platforms using Mesa.
       * TODO: Revert this once b/29886289 is fixed.
       */
   case HAL_PIXEL_FORMAT_YV12:
      /* Y plane is at offset 0 and fds[0] is already set. */
      /* Cr plane is located after Y plane */
      offsets[2] = offsets[0] + strides[0] * buf->height;
      strides[2] = ALIGN(strides[0] / 2, 16);
      /* Cb plane is located after Cr plane */
      offsets[1] = offsets[2] + strides[2] * buf->height / 2;
      strides[1] = strides[2];
      total_planes = 3;
      break;
   default:
      break;
   }

   /* TODO: Do we need to pass in the following or defaults suffice:
    * EGL_YUV_COLOR_SPACE_HINT_EXT,
    * EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,
    * EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT
    */
   EGLint plane_fd_attr[] = {
      EGL_DMA_BUF_PLANE0_FD_EXT,
      EGL_DMA_BUF_PLANE1_FD_EXT,
      EGL_DMA_BUF_PLANE2_FD_EXT,
   };

   EGLint plane_pitch_attr[] = {
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      EGL_DMA_BUF_PLANE1_PITCH_EXT,
      EGL_DMA_BUF_PLANE2_PITCH_EXT,
   };

   EGLint plane_offset_attr[] = {
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      EGL_DMA_BUF_PLANE1_OFFSET_EXT,
      EGL_DMA_BUF_PLANE2_OFFSET_EXT,
   };

   EGLint attr_list[25];
   attr_list[0] = EGL_WIDTH;
   attr_list[1] = buf->width;
   attr_list[2] = EGL_HEIGHT;
   attr_list[3] = buf->height;
   attr_list[4] = EGL_LINUX_DRM_FOURCC_EXT;
   attr_list[5] = fourcc_format;
   index = 5;

   for (p = 0; p < total_planes; ++p) {
      attr_list[++index] = plane_fd_attr[p];
      attr_list[++index] = fd;
      attr_list[++index] = plane_pitch_attr[p];
      attr_list[++index] = strides[p];
      attr_list[++index] = plane_offset_attr[p];
      attr_list[++index] = offsets[p];
   }

   attr_list[++index] = EGL_NONE;

   return dri2_create_image_khr(drv, disp, ctx, EGL_LINUX_DMA_BUF_EXT,
      NULL, attr_list);
}

static _EGLImage *
droid_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
		       _EGLContext *ctx, EGLenum target,
		       EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_BUFFER_ANDROID:
      return dri2_create_image_android_native_buffer(drv, disp, ctx,
            (struct ANativeWindowBuffer *) buffer);
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static void
droid_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
}

static int
droid_get_buffers_parse_attachments(struct dri2_egl_surface *dri2_surf,
                                    unsigned int *attachments, int count)
{
   int num_buffers = 0, i;

   /* fill dri2_surf->buffers */
   for (i = 0; i < count * 2; i += 2) {
      __DRIbuffer *buf, *local;

      assert(num_buffers < ARRAY_SIZE(dri2_surf->buffers));
      buf = &dri2_surf->buffers[num_buffers];

      switch (attachments[i]) {
      case __DRI_BUFFER_BACK_LEFT:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
            buf->attachment = attachments[i];
            buf->name = 0;
            buf->fd = get_native_buffer_fd(dri2_surf->buffer);
            buf->cpp = get_format_bpp(dri2_surf->buffer->format);
            buf->pitch = dri2_surf->buffer->stride * buf->cpp;
            buf->flags = 0;

            if (buf->name || buf->fd >= 0)
               num_buffers++;

            break;
         }
         /* fall through for pbuffers */
      case __DRI_BUFFER_DEPTH:
      case __DRI_BUFFER_STENCIL:
      case __DRI_BUFFER_ACCUM:
      case __DRI_BUFFER_DEPTH_STENCIL:
      case __DRI_BUFFER_HIZ:
         local = droid_alloc_local_buffer(dri2_surf,
               attachments[i], attachments[i + 1]);

         if (local) {
            *buf = *local;
            num_buffers++;
         }
         break;
      case __DRI_BUFFER_FRONT_LEFT:
      case __DRI_BUFFER_FRONT_RIGHT:
      case __DRI_BUFFER_FAKE_FRONT_LEFT:
      case __DRI_BUFFER_FAKE_FRONT_RIGHT:
      case __DRI_BUFFER_BACK_RIGHT:
      default:
         /* no front or right buffers */
         break;
      }
   }

   return num_buffers;
}

static __DRIbuffer *
droid_get_buffers_with_format(__DRIdrawable * driDrawable,
			     int *width, int *height,
			     unsigned int *attachments, int count,
			     int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      /* try to dequeue the next back buffer */
      if (!dri2_surf->buffer && !droid_window_dequeue_buffer(dri2_surf))
         return NULL;

      /* free outdated buffers and update the surface size */
      if (dri2_surf->base.Width != dri2_surf->buffer->width ||
          dri2_surf->base.Height != dri2_surf->buffer->height) {
         droid_free_local_buffers(dri2_surf);
         dri2_surf->base.Width = dri2_surf->buffer->width;
         dri2_surf->base.Height = dri2_surf->buffer->height;
      }
   }

   dri2_surf->buffer_count =
      droid_get_buffers_parse_attachments(dri2_surf, attachments, count);

   if (width)
      *width = dri2_surf->base.Width;
   if (height)
      *height = dri2_surf->base.Height;

   *out_count = dri2_surf->buffer_count;;

   return dri2_surf->buffers;
}

static EGLBoolean
droid_add_configs_for_visuals(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   const struct {
      int format;
      unsigned int rgba_masks[4];
      EGLBoolean fb_target;
   } visuals[] = {
      { HAL_PIXEL_FORMAT_RGBA_8888, { 0xff, 0xff00, 0xff0000, 0xff000000 }, EGL_TRUE },
      { HAL_PIXEL_FORMAT_RGBX_8888, { 0xff, 0xff00, 0xff0000, 0x0 }, EGL_FALSE },
      { HAL_PIXEL_FORMAT_RGB_888,   { 0xff, 0xff00, 0xff0000, 0x0 }, EGL_FALSE },
      { HAL_PIXEL_FORMAT_RGB_565,   { 0xf800, 0x7e0, 0x1f, 0x0 }, EGL_FALSE },
      { HAL_PIXEL_FORMAT_BGRA_8888, { 0xff0000, 0xff00, 0xff, 0xff000000 }, EGL_FALSE },
      { 0, { 0, 0, 0, 0 }, 0 }
   };
   EGLint config_attrs[] = {
     EGL_NATIVE_VISUAL_ID,   0,
     EGL_NATIVE_VISUAL_TYPE, 0,
     EGL_FRAMEBUFFER_TARGET_ANDROID, 0,
     EGL_RECORDABLE_ANDROID, EGL_TRUE,
     EGL_MAX_PBUFFER_WIDTH, 4096,
     EGL_MAX_PBUFFER_HEIGHT, 4096,
     EGL_NONE
   };
   int count, i, j;

   count = 0;
   for (i = 0; visuals[i].format; i++) {
      int format_count = 0;

      config_attrs[1] = visuals[i].format;
      config_attrs[3] = visuals[i].format;
      config_attrs[5] = visuals[i].fb_target;

      for (j = 0; dri2_dpy->driver_configs[j]; j++) {
         const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
         struct dri2_egl_config *dri2_conf;
         unsigned int double_buffered = 0;

         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[j],
            __DRI_ATTRIB_DOUBLE_BUFFER, &double_buffered);

         /* support only double buffered configs */
         if (!double_buffered)
            continue;

         dri2_conf = dri2_add_config(dpy, dri2_dpy->driver_configs[j],
               count + 1, surface_type, config_attrs, visuals[i].rgba_masks);
         if (dri2_conf) {
            count++;
            format_count++;
         }
      }

      if (!format_count) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format 0x%x",
               visuals[i].format);
      }
   }

   /* post-process configs */
   for (i = 0; i < dpy->Configs->Size; i++) {
      struct dri2_egl_config *dri2_conf = dri2_egl_config(dpy->Configs->Elements[i]);

      /* there is no front buffer so no OpenGL */
      dri2_conf->base.RenderableType &= ~EGL_OPENGL_BIT;
      dri2_conf->base.Conformant &= ~EGL_OPENGL_BIT;
   }

   return (count != 0);
}

/* support versions < JellyBean */
#ifndef ALOGW
#define ALOGW LOGW
#endif
#ifndef ALOGD
#define ALOGD LOGD
#endif
#ifndef ALOGI
#define ALOGI LOGI
#endif

static void
droid_log(EGLint level, const char *msg)
{
   switch (level) {
   case _EGL_DEBUG:
      ALOGD("%s", msg);
      break;
   case _EGL_INFO:
      ALOGI("%s", msg);
      break;
   case _EGL_WARNING:
      ALOGW("%s", msg);
      break;
   case _EGL_FATAL:
      LOG_FATAL("%s", msg);
      break;
   default:
      break;
   }
}

static struct dri2_egl_display_vtbl droid_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = droid_create_window_surface,
   .create_pixmap_surface = dri2_fallback_create_pixmap_surface,
   .create_pbuffer_surface = droid_create_pbuffer_surface,
   .destroy_surface = droid_destroy_surface,
   .create_image = droid_create_image_khr,
   .swap_interval = dri2_fallback_swap_interval,
   .swap_buffers = droid_swap_buffers,
   .swap_buffers_with_damage = dri2_fallback_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = dri2_fallback_query_buffer_age,
   .query_surface = droid_query_surface,
   .create_wayland_buffer_from_image = dri2_fallback_create_wayland_buffer_from_image,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

#define DRM_RENDER_DEV_NAME  "%s/renderD%d"

EGLBoolean
dri2_initialize_android(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy;
   int driver_loaded = 0;
   const char *err;
   int i;

   _eglSetLogProc(droid_log);

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof(*dri2_dpy));
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   dpy->DriverData = (void *) dri2_dpy;

   const int limit = 64;
   const int base = 128;
   for (i = 0; i < limit; ++i) {
      char *card_path;
      if (asprintf(&card_path, DRM_RENDER_DEV_NAME, DRM_DIR_NAME, base + i) < 0)
         continue;

      dri2_dpy->fd = loader_open_device(card_path);

      free(card_path);
      if (dri2_dpy->fd < 0)
         continue;

      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd, 0);
      if (dri2_dpy->driver_name) {
         if (dri2_load_driver(dpy)) {
            driver_loaded = 1;
            break;
         }
         free(dri2_dpy->driver_name);
      }
      close(dri2_dpy->fd);
      dri2_dpy->fd = -1;
   }

   if (!driver_loaded) {
      dri2_dpy->driver_name = strdup("swrast");
      if (!dri2_load_driver_swrast(dpy))
      {
         err = "DRI2: failed to load driver";
         free(dri2_dpy->driver_name);
         goto cleanup_display;
      }
   }

   dri2_dpy->dri2_loader_extension.base.name = __DRI_DRI2_LOADER;
   dri2_dpy->dri2_loader_extension.base.version = 3;
   dri2_dpy->dri2_loader_extension.getBuffers = NULL;
   dri2_dpy->dri2_loader_extension.flushFrontBuffer = droid_flush_front_buffer;
   dri2_dpy->dri2_loader_extension.getBuffersWithFormat =
      droid_get_buffers_with_format;

   dri2_dpy->extensions[0] = &dri2_dpy->dri2_loader_extension.base;
   dri2_dpy->extensions[1] = &image_lookup_extension.base;
   dri2_dpy->extensions[2] = &use_invalidate.base;
   dri2_dpy->extensions[3] = NULL;

   if (!dri2_create_screen(dpy)) {
      err = "DRI2: failed to create screen";
      goto cleanup_driver;
   }

   if (!droid_add_configs_for_visuals(drv, dpy)) {
      err = "DRI2: failed to add configs";
      goto cleanup_screen;
   }

   dpy->Extensions.ANDROID_framebuffer_target = EGL_TRUE;
   dpy->Extensions.ANDROID_image_native_buffer = EGL_TRUE;
   dpy->Extensions.ANDROID_recordable = EGL_TRUE;
   dpy->Extensions.KHR_image_base = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &droid_display_vtbl;

   return EGL_TRUE;

cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
cleanup_driver:
   dlclose(dri2_dpy->driver);
cleanup_driver_name:
   free(dri2_dpy->driver_name);
cleanup_device:
   close(dri2_dpy->fd);
cleanup_display:
   free(dri2_dpy);

   return _eglError(EGL_NOT_INITIALIZED, err);
}
