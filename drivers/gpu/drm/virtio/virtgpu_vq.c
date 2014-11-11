#include <drm/drmP.h>
#include "virtgpu_drv.h"
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>


int virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev, uint32_t *resid)
{
	int handle;

	idr_preload(GFP_KERNEL);
	spin_lock(&vgdev->resource_idr_lock);
	handle = idr_alloc(&vgdev->resource_idr, NULL, 1, 0, GFP_NOWAIT);
	spin_unlock(&vgdev->resource_idr_lock);
	idr_preload_end();
	*resid = handle;
	return 0;
}

void virtio_gpu_resource_id_put(struct virtio_gpu_device *vgdev, uint32_t id)
{
	spin_lock(&vgdev->resource_idr_lock);
	idr_remove(&vgdev->resource_idr, id);
	spin_unlock(&vgdev->resource_idr_lock);
}

void virtio_gpu_ctrl_ack(struct virtqueue *vq)
{
	struct drm_device *dev = vq->vdev->priv;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	schedule_work(&vgdev->ctrlq.dequeue_work);
}

void virtio_gpu_cursor_ack(struct virtqueue *vq)
{
	struct drm_device *dev = vq->vdev->priv;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	schedule_work(&vgdev->cursorq.dequeue_work);
}

static struct virtio_gpu_vbuffer*
virtio_gpu_allocate_vbuf(struct virtio_gpu_device *vgdev,
			 int size, int resp_size,
			 virtio_gpu_resp_cb resp_cb)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = kzalloc(sizeof(*vbuf) + size + resp_size, GFP_KERNEL);
	if (!vbuf)
		goto fail;

	vbuf->buf = (void *)vbuf + sizeof(*vbuf);
	vbuf->size = size;

	vbuf->resp_cb = resp_cb;
	if (resp_size)
		vbuf->resp_buf = (void *)vbuf->buf + size;
	else
		vbuf->resp_buf = NULL;
	vbuf->resp_size = resp_size;

	return vbuf;
fail:
	kfree(vbuf);
	return ERR_PTR(-ENOMEM);
}

static void *virtio_gpu_alloc_cmd(struct virtio_gpu_device *vgdev,
				  struct virtio_gpu_vbuffer **vbuffer_p,
				  int size)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = virtio_gpu_allocate_vbuf(vgdev, size,
				     sizeof(struct virtio_gpu_ctrl_hdr), NULL);
	if (IS_ERR(vbuf)) {
		*vbuffer_p = NULL;
		return ERR_CAST(vbuf);
	}
	*vbuffer_p = vbuf;
	return vbuf->buf;
}

static struct virtio_gpu_update_cursor*
virtio_gpu_alloc_cursor(struct virtio_gpu_device *vgdev,
			struct virtio_gpu_vbuffer **vbuffer_p)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = virtio_gpu_allocate_vbuf
		(vgdev, sizeof(struct virtio_gpu_update_cursor), 0, NULL);
	if (IS_ERR(vbuf)) {
		*vbuffer_p = NULL;
		return ERR_CAST(vbuf);
	}
	*vbuffer_p = vbuf;
	return (struct virtio_gpu_update_cursor *)vbuf->buf;
}

static void *virtio_gpu_alloc_cmd_resp(struct virtio_gpu_device *vgdev,
				       virtio_gpu_resp_cb cb,
				       struct virtio_gpu_vbuffer **vbuffer_p,
				       int cmd_size, int resp_size)
{
	struct virtio_gpu_vbuffer *vbuf;

	vbuf = virtio_gpu_allocate_vbuf(vgdev, cmd_size, resp_size, cb);
	if (IS_ERR(vbuf)) {
		*vbuffer_p = NULL;
		return ERR_CAST(vbuf);
	}
	*vbuffer_p = vbuf;
	return (struct virtio_gpu_command *)vbuf->buf;
}

static void free_vbuf(struct virtio_gpu_device *vgdev,
		      struct virtio_gpu_vbuffer *vbuf)
{
	kfree(vbuf->data_buf);
	kfree(vbuf);
}

static int reclaim_vbufs(struct virtqueue *vq, struct list_head *reclaim_list)
{
	struct virtio_gpu_vbuffer *vbuf;
	unsigned int len;
	int freed = 0;
	while ((vbuf = virtqueue_get_buf(vq, &len))) {
		list_add_tail(&vbuf->destroy_list, reclaim_list);
		freed++;
	}
	return freed;
}

void virtio_gpu_dequeue_ctrl_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
		container_of(work, struct virtio_gpu_device,
			     ctrlq.dequeue_work);
	int ret;
	struct list_head reclaim_list;
	struct virtio_gpu_vbuffer *entry, *tmp;
	struct virtio_gpu_ctrl_hdr *resp;
	u64 fence_id = 0;

	INIT_LIST_HEAD(&reclaim_list);
	spin_lock(&vgdev->ctrlq.qlock);
	do {
		virtqueue_disable_cb(vgdev->ctrlq.vq);
		ret = reclaim_vbufs(vgdev->ctrlq.vq, &reclaim_list);
		if (ret == 0)
			DRM_DEBUG("cleaned 0 buffers wierd\n");

	} while (!virtqueue_enable_cb(vgdev->ctrlq.vq));
	spin_unlock(&vgdev->ctrlq.qlock);

	list_for_each_entry_safe(entry, tmp, &reclaim_list, destroy_list) {
		resp = (struct virtio_gpu_ctrl_hdr *)entry->resp_buf;
		if (resp->type != cpu_to_le32(VIRTIO_GPU_RESP_OK_NODATA))
			DRM_DEBUG("response 0x%x\n", le32_to_cpu(resp->type));
		if (resp->flags & cpu_to_le32(VIRTIO_GPU_FLAG_FENCE)) {
			u64 f = le64_to_cpu(resp->fence_id);

			if (fence_id > f) {
				DRM_ERROR("%s: Oops: fence %llx -> %llx\n",
					  __func__, fence_id, f);
			} else {
				fence_id = f;
			}
		}
		if (entry->resp_cb)
			entry->resp_cb(vgdev, entry);

		list_del(&entry->destroy_list);
		free_vbuf(vgdev, entry);
	}
	wake_up(&vgdev->ctrlq.ack_queue);

	if (fence_id) {
		virtio_gpu_fence_event_process(vgdev, fence_id);
	}
}

void virtio_gpu_dequeue_cursor_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
		container_of(work, struct virtio_gpu_device,
			     cursorq.dequeue_work);
	struct virtqueue *vq = vgdev->cursorq.vq;
	struct list_head reclaim_list;
	struct virtio_gpu_vbuffer *entry, *tmp;
	unsigned int len;
	int ret;

	INIT_LIST_HEAD(&reclaim_list);
	spin_lock(&vgdev->cursorq.qlock);
	do {
		virtqueue_disable_cb(vgdev->cursorq.vq);
		ret = reclaim_vbufs(vgdev->cursorq.vq, &reclaim_list);
		if (ret == 0)
			DRM_DEBUG("cleaned 0 buffers wierd\n");
		while (virtqueue_get_buf(vq, &len))
			/* nothing */;
	} while (!virtqueue_enable_cb(vgdev->cursorq.vq));
	spin_unlock(&vgdev->cursorq.qlock);

	list_for_each_entry_safe(entry, tmp, &reclaim_list, destroy_list) {
		list_del(&entry->destroy_list);
		free_vbuf(vgdev, entry);
	}
	wake_up(&vgdev->cursorq.ack_queue);
}

static int virtio_gpu_queue_ctrl_buffer(struct virtio_gpu_device *vgdev,
					struct virtio_gpu_vbuffer *vbuf)
{
	struct virtqueue *vq = vgdev->ctrlq.vq;
	struct scatterlist *sgs[3], vcmd, vout, vresp;
	int outcnt = 0, incnt = 0;
	int ret;

	sg_init_one(&vcmd, vbuf->buf, vbuf->size);
	sgs[outcnt+incnt] = &vcmd;
	outcnt++;

	if (vbuf->data_buf) {
		sg_init_one(&vout, vbuf->data_buf, vbuf->data_size);
		sgs[outcnt+incnt] = &vout;
		outcnt++;
	}

	if (vbuf->resp_buf) {
		sg_init_one(&vresp, vbuf->resp_buf, vbuf->resp_size);
		sgs[outcnt+incnt] = &vresp;
		incnt++;
	}

	spin_lock(&vgdev->ctrlq.qlock);
retry:
	ret = virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
	if (ret == -ENOSPC) {
		spin_unlock(&vgdev->ctrlq.qlock);
		wait_event(vgdev->ctrlq.ack_queue, vq->num_free);
		spin_lock(&vgdev->ctrlq.qlock);
		goto retry;
	} else {
		virtqueue_kick(vq);
	}
	spin_unlock(&vgdev->ctrlq.qlock);

	if (!ret)
		ret = vq->num_free;
	return ret;
}

static int virtio_gpu_queue_cursor(struct virtio_gpu_device *vgdev,
				   struct virtio_gpu_vbuffer *vbuf)
{
	struct virtqueue *vq = vgdev->cursorq.vq;
	struct scatterlist *sgs[1], ccmd;
	int ret;
	int outcnt;

	sg_init_one(&ccmd, vbuf->buf, vbuf->size);
	sgs[0] = &ccmd;
	outcnt = 1;

	spin_lock(&vgdev->cursorq.qlock);
retry:
	ret = virtqueue_add_sgs(vq, sgs, outcnt, 0, vbuf, GFP_ATOMIC);
	if (ret == -ENOSPC) {
		spin_unlock(&vgdev->cursorq.qlock);
		wait_event(vgdev->cursorq.ack_queue, vq->num_free);
		spin_lock(&vgdev->cursorq.qlock);
		goto retry;
	} else {
		virtqueue_kick(vq);
	}

	spin_unlock(&vgdev->cursorq.qlock);

	if (!ret)
		ret = vq->num_free;
	return ret;
}

/* just create gem objects for userspace and long lived objects,
   just use dma_alloced pages for the queue objects? */

/* create a basic resource */
int virtio_gpu_cmd_create_resource(struct virtio_gpu_device *vgdev,
				   uint32_t resource_id,
				   uint32_t format,
				   uint32_t width,
				   uint32_t height)
{
	struct virtio_gpu_resource_create_2d *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->format = cpu_to_le32(format);
	cmd_p->width = cpu_to_le32(width);
	cmd_p->height = cpu_to_le32(height);

	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);

	return 0;
}

int virtio_gpu_cmd_unref_resource(struct virtio_gpu_device *vgdev,
				  uint32_t resource_id)
{
	struct virtio_gpu_resource_unref *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
	cmd_p->resource_id = cpu_to_le32(resource_id);

	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
	return 0;
}

int virtio_gpu_cmd_resource_inval_backing(struct virtio_gpu_device *vgdev,
					  uint32_t resource_id)
{
	struct virtio_gpu_resource_detach_backing *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	cmd_p->resource_id = cpu_to_le32(resource_id);

	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);

	return 0;
}

int virtio_gpu_cmd_set_scanout(struct virtio_gpu_device *vgdev,
			       uint32_t scanout_id, uint32_t resource_id,
			       uint32_t width, uint32_t height,
			       uint32_t x, uint32_t y)
{
	struct virtio_gpu_set_scanout *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->scanout_id = cpu_to_le32(scanout_id);
	cmd_p->r.width = cpu_to_le32(width);
	cmd_p->r.height = cpu_to_le32(height);
	cmd_p->r.x = cpu_to_le32(x);
	cmd_p->r.y = cpu_to_le32(y);

	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
	return 0;
}

int virtio_gpu_cmd_resource_flush(struct virtio_gpu_device *vgdev,
				  uint32_t resource_id,
				  uint32_t x, uint32_t y,
				  uint32_t width, uint32_t height)
{
	struct virtio_gpu_resource_flush *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->r.width = cpu_to_le32(width);
	cmd_p->r.height = cpu_to_le32(height);
	cmd_p->r.x = cpu_to_le32(x);
	cmd_p->r.y = cpu_to_le32(y);

	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);

	return 0;
}

int virtio_gpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
				       uint32_t resource_id, uint64_t offset,
				       __le32 width, __le32 height,
				       __le32 x, __le32 y,
				       struct virtio_gpu_fence **fence)
{
	struct virtio_gpu_transfer_to_host_2d *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->offset = cpu_to_le64(offset);
	cmd_p->r.width = width;
	cmd_p->r.height = height;
	cmd_p->r.x = x;
	cmd_p->r.y = y;

	if (fence)
		virtio_gpu_fence_emit(vgdev, &cmd_p->hdr, fence);
	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);

	return 0;
}

static int
virtio_gpu_cmd_resource_attach_backing(struct virtio_gpu_device *vgdev,
				       uint32_t resource_id,
				       struct virtio_gpu_mem_entry *ents,
				       uint32_t nents,
				       struct virtio_gpu_fence **fence)
{
	struct virtio_gpu_resource_attach_backing *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd(vgdev, &vbuf, sizeof(*cmd_p));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	cmd_p->resource_id = cpu_to_le32(resource_id);
	cmd_p->nr_entries = cpu_to_le32(nents);

	vbuf->data_buf = ents;
	vbuf->data_size = sizeof(*ents) * nents;

	if (fence)
		virtio_gpu_fence_emit(vgdev, &cmd_p->hdr, fence);
	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);

	return 0;
}

static void virtio_gpu_cmd_get_display_info_cb(struct virtio_gpu_device *vgdev,
					       struct virtio_gpu_vbuffer *vbuf)
{
	struct virtio_gpu_resp_display_info *resp =
		(struct virtio_gpu_resp_display_info *)vbuf->resp_buf;
	int i;

	spin_lock(&vgdev->display_info_lock);
	for (i = 0; i < vgdev->num_scanouts; i++) {
		vgdev->outputs[i].info = resp->pmodes[i];
		if (resp->pmodes[i].enabled) {
			DRM_DEBUG("output %d: %dx%d+%d+%d", i,
				  le32_to_cpu(resp->pmodes[i].r.width),
				  le32_to_cpu(resp->pmodes[i].r.height),
				  le32_to_cpu(resp->pmodes[i].r.x),
				  le32_to_cpu(resp->pmodes[i].r.y));
		} else {
			DRM_DEBUG("output %d: disabled", i);
		}
	}

	spin_unlock(&vgdev->display_info_lock);
	wake_up(&vgdev->resp_wq);

	if (!drm_helper_hpd_irq_event(vgdev->ddev)) {
		drm_kms_helper_hotplug_event(vgdev->ddev);
	}
}

int virtio_gpu_cmd_get_display_info(struct virtio_gpu_device *vgdev)
{
	struct virtio_gpu_ctrl_hdr *cmd_p;
	struct virtio_gpu_vbuffer *vbuf;

	cmd_p = virtio_gpu_alloc_cmd_resp
		(vgdev, &virtio_gpu_cmd_get_display_info_cb, &vbuf,
		 sizeof(*cmd_p), sizeof(struct virtio_gpu_resp_display_info));
	memset(cmd_p, 0, sizeof(*cmd_p));

	cmd_p->type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
	virtio_gpu_queue_ctrl_buffer(vgdev, vbuf);
	return 0;
}

int virtio_gpu_object_attach(struct virtio_gpu_device *vgdev,
			     struct virtio_gpu_object *obj,
			     uint32_t resource_id,
			     struct virtio_gpu_fence **fence)
{
	struct virtio_gpu_mem_entry *ents;
	struct scatterlist *sg;
	int si;

	if (!obj->pages) {
		int ret;
		ret = virtio_gpu_object_get_sg_table(vgdev, obj);
		if (ret)
			return ret;
	}

	/* gets freed when the ring has consumed it */
	ents = kmalloc_array(obj->pages->nents,
			     sizeof(struct virtio_gpu_mem_entry),
			     GFP_KERNEL);
	if (!ents) {
		DRM_ERROR("failed to allocate ent list\n");
		return -ENOMEM;
	}

	for_each_sg(obj->pages->sgl, sg, obj->pages->nents, si) {
		ents[si].addr = cpu_to_le64(sg_phys(sg));
		ents[si].length = cpu_to_le32(sg->length);
		ents[si].padding = 0;
	}

	virtio_gpu_cmd_resource_attach_backing(vgdev, resource_id,
					       ents, obj->pages->nents,
					       fence);
	obj->hw_res_handle = resource_id;
	return 0;
}

void virtio_gpu_cursor_ping(struct virtio_gpu_device *vgdev,
			    struct virtio_gpu_output *output)
{
	struct virtio_gpu_vbuffer *vbuf;
	struct virtio_gpu_update_cursor *cur_p;

	output->cursor.pos.scanout_id = cpu_to_le32(output->index);
	cur_p = virtio_gpu_alloc_cursor(vgdev, &vbuf);
	memcpy(cur_p, &output->cursor, sizeof(output->cursor));
	virtio_gpu_queue_cursor(vgdev, vbuf);
}
