#include "virtgpu_drv.h"

static void virtio_gpu_ttm_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct virtio_gpu_object *bo;
	struct virtio_gpu_device *vgdev;

	bo = container_of(tbo, struct virtio_gpu_object, tbo);
	vgdev = (struct virtio_gpu_device *)bo->gem_base.dev->dev_private;

	if (bo->hw_res_handle)
		virtio_gpu_cmd_unref_resource(vgdev, bo->hw_res_handle);
	if (bo->pages)
		virtio_gpu_object_free_sg_table(bo);
	drm_gem_object_release(&bo->gem_base);
	kfree(bo);
}

bool virtio_gpu_ttm_bo_is_virtio_gpu_object(struct ttm_buffer_object *bo)
{
	if (bo->destroy == &virtio_gpu_ttm_bo_destroy)
		return true;
	return false;
}

static void virtio_gpu_init_ttm_placement(struct virtio_gpu_object *vgbo,
					  bool pinned)
{
	u32 c = 1;
	u32 pflag = pinned ? TTM_PL_FLAG_NO_EVICT : 0;

	vgbo->placement.placement = &vgbo->placement_code;
	vgbo->placement.busy_placement = &vgbo->placement_code;
	vgbo->placement_code.fpfn = 0;
	vgbo->placement_code.lpfn = 0;
	vgbo->placement_code.flags =
		TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT | pflag;
	vgbo->placement.num_placement = c;
	vgbo->placement.num_busy_placement = c;

}

int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
			     unsigned long size, bool kernel, bool pinned,
			     struct virtio_gpu_object **bo_ptr)
{
	struct virtio_gpu_object *bo;
	enum ttm_bo_type type;
	size_t acc_size;
	int r;

	if (kernel)
		type = ttm_bo_type_kernel;
	else
		type = ttm_bo_type_device;
	*bo_ptr = NULL;

	acc_size = ttm_bo_dma_acc_size(&vgdev->mman.bdev, size,
				       sizeof(struct virtio_gpu_object));

	bo = kzalloc(sizeof(struct virtio_gpu_object), GFP_KERNEL);
	if (bo == NULL)
		return -ENOMEM;
	size = roundup(size, PAGE_SIZE);
	r = drm_gem_object_init(vgdev->ddev, &bo->gem_base, size);
	if (unlikely(r)) {
		kfree(bo);
		return r;
	}
	bo->dumb = false;

	virtio_gpu_init_ttm_placement(bo, pinned);
	r = ttm_bo_init(&vgdev->mman.bdev, &bo->tbo, size, type,
			&bo->placement, 0, !kernel, NULL, acc_size,
			NULL, NULL, &virtio_gpu_ttm_bo_destroy);
	if (unlikely(r != 0)) {
		if (r != -ERESTARTSYS)
			dev_err(vgdev->dev,
				"object_init %d failed for (%lu)\n", r,
				size);
		return r;
	}
	*bo_ptr = bo;
	return 0;
}

int virtio_gpu_object_kmap(struct virtio_gpu_object *bo, void **ptr)
{
	bool is_iomem;
	int r;

	if (bo->vmap) {
		if (ptr)
			*ptr = bo->vmap;
		return 0;
	}
	r = ttm_bo_kmap(&bo->tbo, 0, bo->tbo.num_pages, &bo->kmap);
	if (r)
		return r;
	bo->vmap = ttm_kmap_obj_virtual(&bo->kmap, &is_iomem);
	if (ptr)
		*ptr = bo->vmap;
	return 0;
}

#if 0
void virtio_gpu_object_force_delete(struct virtio_gpu_device *vgdev)
{
	struct virtio_gpu_object *bo, *n;


	dev_err(vgdev->dev, "Userspace still has active objects !\n");
	list_for_each_entry_safe(bo, n, &vgdev->gem.objects, list) {
		mutex_lock(&vgdev->ddev->struct_mutex);
		dev_err(vgdev->dev, "%p %p %lu %lu force free\n",
			&bo->gem_base, bo, (unsigned long)bo->gem_base.size,
			*((unsigned long *)&bo->gem_base.refcount));
		spin_lock(&vgdev->gem.lock);
		list_del_init(&bo->list);
		spin_unlock(&vgdev->gem.lock);
		/* this should unref the ttm bo */
		drm_gem_object_unreference(&bo->gem_base);
		mutex_unlock(&vgdev->ddev->struct_mutex);
	}
}
#endif

int virtio_gpu_object_get_sg_table(struct virtio_gpu_device *qdev,
				   struct virtio_gpu_object *bo)
{
	int ret;
	struct page **pages = bo->tbo.ttm->pages;
	int nr_pages = bo->tbo.num_pages;

	/* wtf swapping */
	if (bo->pages)
		return 0;

	if (bo->tbo.ttm->state == tt_unpopulated)
		bo->tbo.ttm->bdev->driver->ttm_tt_populate(bo->tbo.ttm);
	bo->pages = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!bo->pages)
		goto out;

	ret = sg_alloc_table_from_pages(bo->pages, pages, nr_pages, 0,
					nr_pages << PAGE_SHIFT, GFP_KERNEL);
	if (ret)
		goto out;
	return 0;
out:
	kfree(bo->pages);
	bo->pages = NULL;
	return -ENOMEM;
}

void virtio_gpu_object_free_sg_table(struct virtio_gpu_object *bo)
{
	sg_free_table(bo->pages);
	kfree(bo->pages);
	bo->pages = NULL;
}

int virtio_gpu_object_wait(struct virtio_gpu_object *bo, bool no_wait)
{
	int r;

	r = ttm_bo_reserve(&bo->tbo, true, no_wait, false, NULL);
	if (unlikely(r != 0))
		return r;
	r = ttm_bo_wait(&bo->tbo, true, true, no_wait);
	ttm_bo_unreserve(&bo->tbo);
	return r;
}

