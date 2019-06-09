/*
 * Copyright (C) 2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2011 LunarG Inc.
 *
 * Based on xf86-video-nouveau, which has
 *
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2008 Maarten Maathuis
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

#define LOG_TAG "GRALLOC-TEGRA"

#include <cutils/log.h>
#include <stdlib.h>
#include <errno.h>
#include <drm.h>
#include <tegra.h>

#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

struct tegra_info {
	struct gralloc_drm_drv_t base;

	struct drm_tegra *drm;
	int fd;
};

struct tegra_buffer {
	struct gralloc_drm_bo_t base;

	struct drm_tegra_bo *bo;
};

static void tegra_init_kms_features(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_t *drm)
{
	switch (drm->primary->fb_format) {
	case HAL_PIXEL_FORMAT_BGRA_8888:
	case HAL_PIXEL_FORMAT_RGB_565:
		break;
	default:
		drm->primary->fb_format = HAL_PIXEL_FORMAT_BGRA_8888;
		break;
	}

	drm->mode_quirk_vmwgfx = 0;
	drm->swap_mode = DRM_SWAP_FLIP;
	drm->mode_sync_flip = 1;
	drm->swap_interval = 1;
	drm->vblank_secondary = 0;
}

static int tegra_map(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo, int x, int y, int w, int h,
		int enable_write, void **addr)
{
	struct tegra_buffer *tegra_buf = (struct tegra_buffer *) bo;
	void *map;

	if(drm_tegra_bo_map(tegra_buf->bo, &map))
		return 0;

	return -errno;
}

static struct drm_tegra_bo *alloc_bo(struct drm_tegra *drm, 
		int width, int height, int cpp, int usage, int *pitch)
{
	struct drm_tegra_bo *tegra_bo;
	int flags = 0, size;

	*pitch = ALIGN(width * cpp, 64);
	size = *pitch * height;
	if (drm_tegra_bo_new(&tegra_bo, drm, flags, size))
		return tegra_bo;
	else
		return NULL;
}

static struct gralloc_drm_bo_t *
tegra_alloc(struct gralloc_drm_drv_t *drv, struct gralloc_drm_handle_t *handle)
{
	struct tegra_info *info = (struct tegra_info *) drv;
	struct tegra_buffer *tegra_buf;
	int cpp, width, height, pitch;
	uint32_t handle_;

	cpp = gralloc_drm_get_bpp(handle->format);
	if (!cpp) {
		ALOGE("unrecognized format 0x%x", handle->format);
		return NULL;
	}

	tegra_buf = calloc(1, sizeof(*tegra_buf));
	if (!tegra_buf)
		return NULL;

	if (handle->name) {
		drm_tegra_bo_from_name(&tegra_buf->bo, info->drm, handle->name, 0);
		if (!tegra_buf->bo) {
			ALOGE("failed to create fd bo from name %u",
					handle->name);
			free(tegra_buf);
			return NULL;
		}
	}
	else {
		width = handle->width;
		height = handle->height;
		gralloc_drm_align_geometry(handle->format, &width, &height);

		tegra_buf->bo = alloc_bo(info->drm, width, height,
				cpp, handle->usage, &pitch);
		if (!tegra_buf->bo) {
			ALOGE("failed to allocate fd bo %dx%dx%d",
					handle->width, handle->height, cpp);
			free(tegra_buf);
			return NULL;
		}

		if (drm_tegra_bo_get_name(tegra_buf->bo, (uint32_t *) &handle->name)) {
			ALOGE("failed to flink tegra bo");
			drm_tegra_bo_unref(tegra_buf->bo);
			free(tegra_buf);
			return NULL;
		}

		handle->stride = pitch;
	}

	if (handle->usage & GRALLOC_USAGE_HW_FB) {
		drm_tegra_bo_get_handle(tegra_buf->bo, &handle_);
		tegra_buf->base.fb_handle = (int) handle_;
	}

	tegra_buf->base.handle = handle;

	return &tegra_buf->base;
}

static void tegra_free(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo)
{
	struct tegra_buffer *tb = (struct tegra_buffer *) bo;

	drm_tegra_bo_unref(tb->bo);
	free(tb);
}

static void tegra_unmap(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo)
{
	struct tegra_buffer *tegra_buf = (struct tegra_buffer *) bo;

	drm_tegra_bo_unmap(tegra_buf->bo);
}

static void tegra_destroy(struct gralloc_drm_drv_t *drv)
{
	struct tegra_info *info = (struct tegra_info *) drv;

	drm_tegra_close(info->drm);
	free(info);
}

struct gralloc_drm_drv_t *
gralloc_drm_drv_create_for_tegra(int fd)
{
	struct tegra_info *info;
	struct drm_tegra *drm;

	info = calloc(1, sizeof(*info));
	if (!info)
		return NULL;

	if (drm_tegra_new(&drm, fd)) {
		ALOGE("failed to wrap existing tegra device");
		free(info);
		return NULL;
	}

	info->fd = fd;
	info->drm = drm;

	info->base.destroy = tegra_destroy;
	info->base.init_kms_features = tegra_init_kms_features;
	info->base.alloc = tegra_alloc;
	info->base.free = tegra_free;
	info->base.map = tegra_map;
	info->base.unmap = tegra_unmap;

	return &info->base;
}
