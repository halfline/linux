#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <drm/drmP.h>
#include "virtgpu_drv.h"

static void virtio_gpu_config_changed_work_func(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
		container_of(work, struct virtio_gpu_device,
			     config_changed_work);
	__le32 event_read, event_clear = 0;

	/* read the config space */
	vgdev->vdev->config->get(vgdev->vdev,
				 offsetof(struct virtio_gpu_config,
					  events_read),
				 &event_read, sizeof(event_read));
	if (event_read & cpu_to_le32(VIRTIO_GPU_EVENT_DISPLAY)) {
		virtio_gpu_cmd_get_display_info(vgdev);
		drm_helper_hpd_irq_event(vgdev->ddev);
		event_clear |= cpu_to_le32(VIRTIO_GPU_EVENT_DISPLAY);
	}
	vgdev->vdev->config->set(vgdev->vdev,
				 offsetof(struct virtio_gpu_config,
					  events_clear),
				 &event_clear, sizeof(event_clear));
}

static void virtio_gpu_init_vq(struct virtio_gpu_queue *vgvq,
			       void (*work_func)(struct work_struct *work))
{
	spin_lock_init(&vgvq->qlock);
	init_waitqueue_head(&vgvq->ack_queue);
	INIT_WORK(&vgvq->dequeue_work, work_func);
}

int virtio_gpu_driver_load(struct drm_device *dev, unsigned long flags)
{
	static vq_callback_t *callbacks[] = {
		virtio_gpu_ctrl_ack, virtio_gpu_cursor_ack
	};
	static const char *names[] = { "control", "cursor" };

	struct virtio_gpu_device *vgdev;
	/* this will expand later */
	struct virtqueue *vqs[2];
	__le32 num_scanouts;
	int ret;

	vgdev = kzalloc(sizeof(struct virtio_gpu_device), GFP_KERNEL);
	if (!vgdev)
		return -ENOMEM;

	vgdev->ddev = dev;
	dev->dev_private = vgdev;
	vgdev->vdev = dev->virtdev;
	vgdev->dev = dev->dev;

	spin_lock_init(&vgdev->display_info_lock);
	spin_lock_init(&vgdev->ctx_id_idr_lock);
	idr_init(&vgdev->ctx_id_idr);
	spin_lock_init(&vgdev->resource_idr_lock);
	idr_init(&vgdev->resource_idr);
	init_waitqueue_head(&vgdev->resp_wq);
	virtio_gpu_init_vq(&vgdev->ctrlq, virtio_gpu_dequeue_ctrl_func);
	virtio_gpu_init_vq(&vgdev->cursorq, virtio_gpu_dequeue_cursor_func);

	spin_lock_init(&vgdev->fence_drv.lock);
	INIT_LIST_HEAD(&vgdev->fence_drv.fences);
	INIT_WORK(&vgdev->config_changed_work,
		  virtio_gpu_config_changed_work_func);

	ret = vgdev->vdev->config->find_vqs(vgdev->vdev, 2, vqs,
					    callbacks, names);
	if (ret) {
		DRM_ERROR("failed to find virt queues\n");
		kfree(vgdev);
		return ret;
	}

	vgdev->ctrlq.vq = vqs[0];
	vgdev->cursorq.vq = vqs[1];

	ret = virtio_gpu_ttm_init(vgdev);
	if (ret) {
		DRM_ERROR("failed to init ttm %d\n", ret);
		kfree(vgdev);
		return ret;
	}

	/* get display info */
	vgdev->vdev->config->get(vgdev->vdev,
				 offsetof(struct virtio_gpu_config,
					  num_scanouts),
				 &num_scanouts, sizeof(num_scanouts));
	vgdev->num_scanouts = min_t(uint32_t, le32_to_cpu(num_scanouts),
				    VIRTIO_GPU_MAX_SCANOUTS);
	if (!vgdev->num_scanouts) {
		DRM_ERROR("num_scanouts is zero\n");
		kfree(vgdev);
		return -EINVAL;
	}

	ret = virtio_gpu_modeset_init(vgdev);
	if (ret) {
		kfree(vgdev);
		return ret;
	}

	virtio_gpu_cmd_get_display_info(vgdev);
	return 0;
}

int virtio_gpu_driver_unload(struct drm_device *dev)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;

	virtio_gpu_modeset_fini(vgdev);
	virtio_gpu_ttm_fini(vgdev);
	kfree(vgdev);
	return 0;
}
