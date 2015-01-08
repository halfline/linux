/*
 * Copyright 2013 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Dave Airlie
 *          Alon Levy
 */

#include "virtgpu_drv.h"
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>

static void virtio_gpu_crtc_gamma_set(struct drm_crtc *crtc,
				      u16 *red, u16 *green, u16 *blue,
				      uint32_t start, uint32_t size)
{
	/* TODO */
}

static void
virtio_gpu_hide_cursor(struct virtio_gpu_device *vgdev,
		       struct virtio_gpu_output *output)
{
	output->cursor.hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_UPDATE_CURSOR);
	output->cursor.resource_id = 0;
	virtio_gpu_cursor_ping(vgdev, output);
}

static int virtio_gpu_crtc_cursor_set(struct drm_crtc *crtc,
				      struct drm_file *file_priv,
				      uint32_t handle,
				      uint32_t width,
				      uint32_t height,
				      int32_t hot_x, int32_t hot_y)
{
	struct virtio_gpu_device *vgdev = crtc->dev->dev_private;
	struct virtio_gpu_output *output =
		container_of(crtc, struct virtio_gpu_output, crtc);
	struct drm_gem_object *gobj = NULL;
	struct virtio_gpu_object *qobj = NULL;
	struct virtio_gpu_fence *fence = NULL;
	int ret = 0;

	if (handle == 0) {
		virtio_gpu_hide_cursor(vgdev, output);
		return 0;
	}

	/* lookup the cursor */
	gobj = drm_gem_object_lookup(crtc->dev, file_priv, handle);
	if (gobj == NULL)
		return -ENOENT;

	qobj = gem_to_virtio_gpu_obj(gobj);

	if (!qobj->hw_res_handle) {
		ret = -EINVAL;
		goto out;
	}

	ret = virtio_gpu_cmd_transfer_to_host_2d(vgdev, qobj->hw_res_handle, 0,
						 cpu_to_le32(64),
						 cpu_to_le32(64),
						 0, 0, &fence);
	if (!ret) {
		reservation_object_add_excl_fence(qobj->tbo.resv,
						  &fence->f);
		virtio_gpu_object_wait(qobj, false);
	}

	output->cursor.hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_UPDATE_CURSOR);
	output->cursor.resource_id = cpu_to_le32(qobj->hw_res_handle);
	output->cursor.hot_x = cpu_to_le32(hot_x);
	output->cursor.hot_y = cpu_to_le32(hot_y);
	virtio_gpu_cursor_ping(vgdev, output);
out:
	drm_gem_object_unreference_unlocked(gobj);
	return ret;
}

static int virtio_gpu_crtc_cursor_move(struct drm_crtc *crtc,
				    int x, int y)
{
	struct virtio_gpu_device *vgdev = crtc->dev->dev_private;
	struct virtio_gpu_output *output =
		container_of(crtc, struct virtio_gpu_output, crtc);

	output->cursor.hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_MOVE_CURSOR);
	output->cursor.pos.x = cpu_to_le32(x);
	output->cursor.pos.y = cpu_to_le32(y);
	virtio_gpu_cursor_ping(vgdev, output);
	return 0;
}

static int virtio_gpu_crtc_page_flip(struct drm_crtc *crtc,
				     struct drm_framebuffer *fb,
				     struct drm_pending_vblank_event *event,
				     uint32_t flags)
{
	struct drm_device *dev = crtc->dev;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_output *output = drm_crtc_to_virtio_gpu_output(crtc);
	struct virtio_gpu_framebuffer *vgfb = to_virtio_gpu_framebuffer(fb);
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(vgfb->obj);
	unsigned long irqflags;

	crtc->primary->fb = fb;
	virtio_gpu_cmd_set_scanout(vgdev, output->index, bo->hw_res_handle,
				   output->mode_hdisplay, output->mode_vdisplay,
				   0, 0);
#if 1
	/*
	 * Hmm, I think we should not need this, userspace should have
	 * tagged updated areas via drm_framebuffer_funcs->dirty()
	 * before invoking a page flip I think, and doing this here
	 * may bring us into trouble in virgl mode.
	 *
	 * kmscon doesn't display anything without this tough ...
	 */
	virtio_gpu_cmd_transfer_to_host_2d(vgdev, bo->hw_res_handle, 0,
					   cpu_to_le32(output->mode_hdisplay),
					   cpu_to_le32(output->mode_vdisplay),
					   cpu_to_le32(0), cpu_to_le32(0),
					   NULL);
#endif
	virtio_gpu_cmd_resource_flush(vgdev, bo->hw_res_handle, 0, 0,
				      output->mode_hdisplay,
				      output->mode_vdisplay);
	if (event) {
		/* FIXME: fence cmd instead */
		spin_lock_irqsave(&dev->event_lock, irqflags);
		drm_send_vblank_event(dev, -1, event);
		spin_unlock_irqrestore(&dev->event_lock, irqflags);
	}
	return 0;
}


static const struct drm_crtc_funcs virtio_gpu_crtc_funcs = {
	.cursor_set2 = virtio_gpu_crtc_cursor_set,
	.cursor_move = virtio_gpu_crtc_cursor_move,
	.gamma_set = virtio_gpu_crtc_gamma_set,
	.set_config = drm_crtc_helper_set_config,
	.page_flip = virtio_gpu_crtc_page_flip,
	.destroy = drm_crtc_cleanup,
};

static void virtio_gpu_user_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct virtio_gpu_framebuffer *virtio_gpu_fb
		= to_virtio_gpu_framebuffer(fb);

	if (virtio_gpu_fb->obj)
		drm_gem_object_unreference_unlocked(virtio_gpu_fb->obj);
	drm_framebuffer_cleanup(fb);
	kfree(virtio_gpu_fb);
}

static int
virtio_gpu_framebuffer_surface_dirty(struct drm_framebuffer *fb,
				     struct drm_file *file_priv,
				     unsigned flags, unsigned color,
				     struct drm_clip_rect *clips,
				     unsigned num_clips)
{
	struct virtio_gpu_framebuffer *virtio_gpu_fb
		= to_virtio_gpu_framebuffer(fb);

	return virtio_gpu_surface_dirty(virtio_gpu_fb, clips, num_clips);
}

static const struct drm_framebuffer_funcs virtio_gpu_fb_funcs = {
	.destroy = virtio_gpu_user_framebuffer_destroy,
	.dirty = virtio_gpu_framebuffer_surface_dirty,
};

int
virtio_gpu_framebuffer_init(struct drm_device *dev,
			    struct virtio_gpu_framebuffer *vgfb,
			    struct drm_mode_fb_cmd2 *mode_cmd,
			    struct drm_gem_object *obj)
{
	int ret;
	struct virtio_gpu_object *bo;
	vgfb->obj = obj;

	bo = gem_to_virtio_gpu_obj(obj);

	ret = drm_framebuffer_init(dev, &vgfb->base, &virtio_gpu_fb_funcs);
	if (ret) {
		vgfb->obj = NULL;
		return ret;
	}
	drm_helper_mode_fill_fb_struct(&vgfb->base, mode_cmd);

	spin_lock_init(&vgfb->dirty_lock);
	vgfb->x1 = vgfb->y1 = INT_MAX;
	vgfb->x2 = vgfb->y2 = 0;
	return 0;
}

static void virtio_gpu_crtc_dpms(struct drm_crtc *crtc, int mode)
{
}

static bool virtio_gpu_crtc_mode_fixup(struct drm_crtc *crtc,
				       const struct drm_display_mode *mode,
				       struct drm_display_mode *adjusted_mode)
{
	return true;
}

static int virtio_gpu_crtc_mode_set(struct drm_crtc *crtc,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted_mode,
				    int x, int y,
				    struct drm_framebuffer *old_fb)
{
	struct drm_device *dev = crtc->dev;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_framebuffer *vgfb;
	struct virtio_gpu_object *bo, *old_bo = NULL;
	struct virtio_gpu_output *output = drm_crtc_to_virtio_gpu_output(crtc);

	if (!crtc->primary->fb) {
		DRM_DEBUG_KMS("No FB bound\n");
		return 0;
	}

	if (old_fb) {
		vgfb = to_virtio_gpu_framebuffer(old_fb);
		old_bo = gem_to_virtio_gpu_obj(vgfb->obj);
	}
	vgfb = to_virtio_gpu_framebuffer(crtc->primary->fb);
	bo = gem_to_virtio_gpu_obj(vgfb->obj);
	DRM_DEBUG("+%d+%d (%d,%d) => (%d,%d)\n",
		  x, y,
		  mode->hdisplay, mode->vdisplay,
		  adjusted_mode->hdisplay,
		  adjusted_mode->vdisplay);

	output->mode_hdisplay = mode->hdisplay;
	output->mode_vdisplay = mode->vdisplay;
	virtio_gpu_cmd_set_scanout(vgdev, output->index, bo->hw_res_handle,
				   mode->hdisplay, mode->vdisplay, x, y);

	return 0;
}

static void virtio_gpu_crtc_prepare(struct drm_crtc *crtc)
{
	DRM_DEBUG("current: %dx%d+%d+%d (%d).\n",
		  crtc->mode.hdisplay, crtc->mode.vdisplay,
		  crtc->x, crtc->y, crtc->enabled);
}

static void virtio_gpu_crtc_commit(struct drm_crtc *crtc)
{
	DRM_DEBUG("\n");
}

static void virtio_gpu_crtc_load_lut(struct drm_crtc *crtc)
{
}

static void virtio_gpu_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_output *output = drm_crtc_to_virtio_gpu_output(crtc);

	virtio_gpu_cmd_set_scanout(vgdev, output->index, 0, 0, 0, 0, 0);
}

static const struct drm_crtc_helper_funcs virtio_gpu_crtc_helper_funcs = {
	.disable = virtio_gpu_crtc_disable,
	.dpms = virtio_gpu_crtc_dpms,
	.mode_fixup = virtio_gpu_crtc_mode_fixup,
	.mode_set = virtio_gpu_crtc_mode_set,
	.prepare = virtio_gpu_crtc_prepare,
	.commit = virtio_gpu_crtc_commit,
	.load_lut = virtio_gpu_crtc_load_lut,
};

static void virtio_gpu_enc_dpms(struct drm_encoder *encoder, int mode)
{
}

static bool virtio_gpu_enc_mode_fixup(struct drm_encoder *encoder,
				      const struct drm_display_mode *mode,
				      struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void virtio_gpu_enc_prepare(struct drm_encoder *encoder)
{
}

static void virtio_gpu_enc_commit(struct drm_encoder *encoder)
{
}

static void virtio_gpu_enc_mode_set(struct drm_encoder *encoder,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted_mode)
{
}

static int virtio_gpu_conn_get_modes(struct drm_connector *connector)
{
	struct virtio_gpu_output *output =
		drm_connector_to_virtio_gpu_output(connector);
	struct drm_display_mode *mode = NULL;
	int count, width, height;

	width  = le32_to_cpu(output->info.r.width);
	height = le32_to_cpu(output->info.r.height);
	DRM_DEBUG("idx %d, %dx%d\n", output->index, width, height);
	count = drm_add_modes_noedid(connector, 8192, 8192);

	if (width == 0 || height == 0) {
		width = 1024;
		height = 768;
		drm_set_preferred_mode(connector, 1024, 768);
	} else {
		mode = drm_cvt_mode(connector->dev, width, height, 60,
				    false, false, false);
		mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static int virtio_gpu_conn_mode_valid(struct drm_connector *connector,
				      struct drm_display_mode *mode)
{
	return MODE_OK;
}

static struct drm_encoder*
virtio_gpu_best_encoder(struct drm_connector *connector)
{
	struct virtio_gpu_output *virtio_gpu_output =
		drm_connector_to_virtio_gpu_output(connector);

	return &virtio_gpu_output->enc;
}


static const struct drm_encoder_helper_funcs virtio_gpu_enc_helper_funcs = {
	.dpms = virtio_gpu_enc_dpms,
	.mode_fixup = virtio_gpu_enc_mode_fixup,
	.prepare = virtio_gpu_enc_prepare,
	.mode_set = virtio_gpu_enc_mode_set,
	.commit = virtio_gpu_enc_commit,
};

static const struct drm_connector_helper_funcs virtio_gpu_conn_helper_funcs = {
	.get_modes = virtio_gpu_conn_get_modes,
	.mode_valid = virtio_gpu_conn_mode_valid,
	.best_encoder = virtio_gpu_best_encoder,
};

static void virtio_gpu_conn_save(struct drm_connector *connector)
{
	DRM_DEBUG("\n");
}

static void virtio_gpu_conn_restore(struct drm_connector *connector)
{
	DRM_DEBUG("\n");
}

static enum drm_connector_status virtio_gpu_conn_detect(
			struct drm_connector *connector,
			bool force)
{
	struct virtio_gpu_output *output =
		drm_connector_to_virtio_gpu_output(connector);

	if (output->info.enabled)
		return connector_status_connected;
	else
		return connector_status_disconnected;
}

static int virtio_gpu_conn_set_property(struct drm_connector *connector,
				   struct drm_property *property,
				   uint64_t value)
{
	DRM_DEBUG("\n");
	return 0;
}

static void virtio_gpu_conn_destroy(struct drm_connector *connector)
{
	struct virtio_gpu_output *virtio_gpu_output =
		drm_connector_to_virtio_gpu_output(connector);

	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(virtio_gpu_output);
}

static const struct drm_connector_funcs virtio_gpu_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.save = virtio_gpu_conn_save,
	.restore = virtio_gpu_conn_restore,
	.detect = virtio_gpu_conn_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.set_property = virtio_gpu_conn_set_property,
	.destroy = virtio_gpu_conn_destroy,
};

static const struct drm_encoder_funcs virtio_gpu_enc_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int vgdev_output_init(struct virtio_gpu_device *vgdev, int index)
{
	struct drm_device *dev = vgdev->ddev;
	struct virtio_gpu_output *output = vgdev->outputs + index;
	struct drm_connector *connector = &output->conn;
	struct drm_encoder *encoder = &output->enc;
	struct drm_crtc *crtc = &output->crtc;

	output->index = index;
	if (index == 0) {
		output->info.enabled = cpu_to_le32(true);
		output->info.r.width = cpu_to_le32(1024);
		output->info.r.height = cpu_to_le32(768);
	}

	drm_crtc_init(dev, crtc, &virtio_gpu_crtc_funcs);
	drm_mode_crtc_set_gamma_size(crtc, 256);
	drm_crtc_helper_add(crtc, &virtio_gpu_crtc_helper_funcs);

	drm_connector_init(dev, connector, &virtio_gpu_connector_funcs,
			   DRM_MODE_CONNECTOR_VIRTUAL);
	connector->polled = DRM_CONNECTOR_POLL_HPD;
	drm_encoder_init(dev, encoder, &virtio_gpu_enc_funcs,
			 DRM_MODE_ENCODER_VIRTUAL);

	encoder->possible_crtcs = 1 << index;
	drm_mode_connector_attach_encoder(connector, encoder);
	drm_encoder_helper_add(encoder, &virtio_gpu_enc_helper_funcs);
	drm_connector_helper_add(connector, &virtio_gpu_conn_helper_funcs);
	drm_connector_register(connector);
	return 0;
}

static struct drm_framebuffer *
virtio_gpu_user_framebuffer_create(struct drm_device *dev,
				   struct drm_file *file_priv,
				   struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj = NULL;
	struct virtio_gpu_framebuffer *virtio_gpu_fb;
	int ret;

	/* lookup object associated with res handle */
	obj = drm_gem_object_lookup(dev, file_priv, mode_cmd->handles[0]);
	if (!obj)
		return ERR_PTR(-EINVAL);

	virtio_gpu_fb = kzalloc(sizeof(*virtio_gpu_fb), GFP_KERNEL);
	if (virtio_gpu_fb == NULL)
		return ERR_PTR(-ENOMEM);

	ret = virtio_gpu_framebuffer_init(dev, virtio_gpu_fb, mode_cmd, obj);
	if (ret) {
		kfree(virtio_gpu_fb);
		if (obj)
			drm_gem_object_unreference_unlocked(obj);
		return NULL;
	}

	return &virtio_gpu_fb->base;
}

static const struct drm_mode_config_funcs virtio_gpu_mode_funcs = {
	.fb_create = virtio_gpu_user_framebuffer_create,
};

int virtio_gpu_modeset_init(struct virtio_gpu_device *vgdev)
{
	int i;
	int ret;

	drm_mode_config_init(vgdev->ddev);
	vgdev->ddev->mode_config.funcs = (void *)&virtio_gpu_mode_funcs;

	/* modes will be validated against the framebuffer size */
	vgdev->ddev->mode_config.min_width = 320;
	vgdev->ddev->mode_config.min_height = 200;
	vgdev->ddev->mode_config.max_width = 8192;
	vgdev->ddev->mode_config.max_height = 8192;

	for (i = 0 ; i < vgdev->num_scanouts; ++i)
		vgdev_output_init(vgdev, i);

	/* primary surface must be created by this point, to allow
	 * issuing command queue commands and having them read by
	 * spice server. */
	ret = virtio_gpu_fbdev_init(vgdev);
	if (ret)
		return ret;

	ret = drm_vblank_init(vgdev->ddev, vgdev->num_scanouts);

	drm_kms_helper_poll_init(vgdev->ddev);
	return ret;
}

void virtio_gpu_modeset_fini(struct virtio_gpu_device *vgdev)
{
	virtio_gpu_fbdev_fini(vgdev);
	drm_mode_config_cleanup(vgdev->ddev);
}
