#include <drm/drmP.h>
#include "virtgpu_drv.h"

struct virtio_gpu_fence_event {

	struct drm_device *dev;
	struct drm_file *file;
	struct virtio_gpu_fence *fence;
	struct list_head head;

	void (*seq_done)(struct virtio_gpu_fence_event *event);
	void (*cleanup)(struct virtio_gpu_fence_event *event);
	struct list_head fpriv_head;
	struct drm_pending_vblank_event *event;
};

static void virtio_gpu_fence_destroy(struct kref *kref)
{
	struct virtio_gpu_fence *fence;

	fence = container_of(kref, struct virtio_gpu_fence, kref);
	kfree(fence);
}

struct virtio_gpu_fence *virtio_gpu_fence_ref(struct virtio_gpu_fence *fence)
{
	kref_get(&fence->kref);
	return fence;
}

void virtio_gpu_fence_unref(struct virtio_gpu_fence **fence)
{
	struct virtio_gpu_fence *tmp = *fence;

	*fence = NULL;
	if (tmp)
		kref_put(&tmp->kref, virtio_gpu_fence_destroy);
}

static bool virtio_gpu_fence_seq_signaled(struct virtio_gpu_device *vgdev,
					  u64 seq)
{
	if (atomic64_read(&vgdev->fence_drv.last_seq) >= seq)
		return true;
	return false;
}

static int virtio_gpu_fence_wait_seq(struct virtio_gpu_device *vgdev,
				     u64 target_seq, bool intr)
{
	uint64_t last_activity;
	uint64_t seq;
	unsigned long timeout;
	bool signaled;
	int r;

	while (target_seq > atomic64_read(&vgdev->fence_drv.last_seq)) {

		timeout = jiffies - VIRTIO_GPU_FENCE_JIFFIES_TIMEOUT;
		if (time_after(vgdev->fence_drv.last_activity, timeout)) {
			/* the normal case, timeout is somewhere
			 * before last_activity */
			timeout = vgdev->fence_drv.last_activity - timeout;
		} else {
			/* either jiffies wrapped around, or no fence
			 * was signaled in the last 500ms anyway we
			 * will just wait for the minimum amount and
			 * then check for a lockup
			 */
			timeout = 1;
		}
		seq = atomic64_read(&vgdev->fence_drv.last_seq);
		/* Save current last activity valuee, used to check
		 * for GPU lockups */
		last_activity = vgdev->fence_drv.last_activity;

		if (intr) {
			r = wait_event_interruptible_timeout
				(vgdev->fence_queue,
				 (signaled = virtio_gpu_fence_seq_signaled
				  (vgdev, target_seq)),
				 timeout);
		} else {
			r = wait_event_timeout
				(vgdev->fence_queue,
				 (signaled = virtio_gpu_fence_seq_signaled
				  (vgdev, target_seq)),
				 timeout);
		}
		if (unlikely(r < 0))
			return r;

		if (unlikely(!signaled)) {
			/* we were interrupted for some reason and fence
			 * isn't signaled yet, resume waiting */
			if (r)
				continue;

			/* check if sequence value has changed since
			 * last_activity */
			if (seq != atomic64_read(&vgdev->fence_drv.last_seq))
				continue;

			/* test if somebody else has already decided
			 * that this is a lockup */
			if (last_activity != vgdev->fence_drv.last_activity)
				continue;
		}
	}
	return 0;
}

bool virtio_gpu_fence_signaled(struct virtio_gpu_fence *fence)
{
	if (!fence)
		return true;
	if (fence->seq == VIRTIO_GPU_FENCE_SIGNALED_SEQ)
		return true;
	if (virtio_gpu_fence_seq_signaled(fence->vgdev, fence->seq)) {
		fence->seq = VIRTIO_GPU_FENCE_SIGNALED_SEQ;
		return true;
	}
	return false;
}

int virtio_gpu_fence_wait(struct virtio_gpu_fence *fence, bool intr)
{
	int r;

	if (fence == NULL)
		return -EINVAL;

	r = virtio_gpu_fence_wait_seq(fence->vgdev, fence->seq,
				  intr);
	if (r)
		return r;

	fence->seq = VIRTIO_GPU_FENCE_SIGNALED_SEQ;

	return 0;

}

int virtio_gpu_fence_emit(struct virtio_gpu_device *vgdev,
			  struct virtio_gpu_ctrl_hdr *cmd_hdr,
			  struct virtio_gpu_fence **fence)
{
	*fence = kmalloc(sizeof(struct virtio_gpu_fence), GFP_KERNEL);
	if ((*fence) == NULL)
		return -ENOMEM;

	kref_init(&((*fence)->kref));
	(*fence)->vgdev = vgdev;
	(*fence)->seq = ++vgdev->fence_drv.sync_seq;

	cmd_hdr->flags |= cpu_to_le32(VIRTIO_GPU_FLAG_FENCE);
	cmd_hdr->fence_id = cpu_to_le64((*fence)->seq);

	return 0;
}

void virtio_gpu_fence_event_process(struct virtio_gpu_device *vdev,
				    u64 last_seq)
{
	unsigned long irq_flags;
	struct virtio_gpu_fence_event *fe, *tmp;

	spin_lock_irqsave(&vdev->fence_drv.event_lock, irq_flags);
	list_for_each_entry_safe(fe, tmp, &vdev->fence_drv.event_list, head) {
		if (virtio_gpu_fence_signaled(fe->fence)) {
			DRM_DEBUG_KMS("event signaled in irq\n");
			fe->seq_done(fe);
			list_del(&fe->head);
			fe->cleanup(fe);
		}
	}
	spin_unlock_irqrestore(&vdev->fence_drv.event_lock, irq_flags);
}
