/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P2P_COMPAT_H_
#define P2P_COMPAT_H_

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/nvme.h>
#include <linux/version.h>

#include "dev.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)

#define P2P_LEGACY_KERNEL

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#error "XDS legacy NVMe request ABI requires Linux 5.15.x; this kernel needs a matching adapter"
#endif

/* Mirrors the common prefix of struct nvme_request in Linux 5.15. */
struct p2p_nvme_request {
	struct nvme_command *cmd;
	union nvme_result result;
	u8 genctr;
	u8 retries;
	u8 flags;
	u16 status;
};
typedef struct block_device p2p_bdev_handle;

#define P2P_BDEV_READ_MODE FMODE_READ

static inline struct request *p2p_alloc_nvme_request(struct request_queue *queue,
						     struct nvme_command *cmd)
{
	struct p2p_nvme_request *nvme_req;
	struct request *req;

	req = blk_mq_alloc_request(queue,
				   nvme_is_write(cmd) ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN, 0);
	if (IS_ERR(req))
		return req;

	req->cmd_flags |= REQ_FAILFAST_DRIVER;
	req->rq_flags |= RQF_DONTPREP;
	nvme_req = blk_mq_rq_to_pdu(req);
	if (WARN_ON_ONCE(!nvme_req->cmd)) {
		blk_mq_free_request(req);
		return ERR_PTR(-EOPNOTSUPP);
	}
	nvme_req->status = 0;
	nvme_req->retries = 0;
	nvme_req->flags = 0;
	memcpy(nvme_req->cmd, cmd, sizeof(*cmd));
	nvme_req->cmd->common.flags &= ~NVME_CMD_SGL_ALL;
	return req;
}

#ifdef P2P_HAVE_RQ_END_IO_RET
static inline enum rq_end_io_ret p2p_end_io(struct request *req, blk_status_t status)
#else
static inline void p2p_end_io(struct request *req, blk_status_t status)
#endif
{
	p2p_complete_io(req, status);
#ifdef P2P_HAVE_RQ_END_IO_RET
	return RQ_END_IO_FREE;
#else
	blk_mq_free_request(req);
#endif
}

static inline p2p_bdev_handle *p2p_bdev_open_by_dev(dev_t dev)
{
	return blkdev_get_by_dev(dev, FMODE_READ, NULL);
}

static inline void p2p_bdev_release(p2p_bdev_handle *handle)
{
	blkdev_put(handle, FMODE_READ);
}

static inline struct block_device *p2p_handle_to_bdev(p2p_bdev_handle *handle)
{
	return handle;
}

static inline struct block_device *p2p_bdev_whole(struct block_device *bdev)
{
#ifdef bdev_whole
	return bdev_whole(bdev);
#else
	return bdev->bd_contains;
#endif
}

static inline sector_t p2p_bdev_nr_sectors(struct block_device *bdev)
{
	return i_size_read(bdev->bd_inode) >> SECTOR_SHIFT;
}

static inline void p2p_execute_rq_nowait(struct request *req, struct gendisk *disk)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	blk_execute_rq_nowait(disk, req, true, req->end_io);
#else
	blk_execute_rq_nowait(req->q, disk, req, true, req->end_io);
#endif
}

static inline struct class *p2p_class_create(const char *name)
{
	return class_create(THIS_MODULE, name);
}

#else

extern void nvme_init_request(struct request *req, struct nvme_command *cmd);

typedef struct bdev_handle p2p_bdev_handle;

#define P2P_BDEV_READ_MODE BLK_OPEN_READ

static inline struct request *p2p_alloc_nvme_request(struct request_queue *queue,
						     struct nvme_command *cmd)
{
	struct request *req;

	req = blk_mq_alloc_request(queue, nvme_is_write(cmd) ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN, 0);
	if (!IS_ERR(req))
		nvme_init_request(req, cmd);
	return req;
}

static inline enum rq_end_io_ret p2p_end_io(struct request *req, blk_status_t status)
{
	p2p_complete_io(req, status);

	return RQ_END_IO_FREE;
}

static inline p2p_bdev_handle *p2p_bdev_open_by_dev(dev_t dev)
{
	return bdev_open_by_dev(dev, BLK_OPEN_READ, NULL, NULL);
}

static inline void p2p_bdev_release(p2p_bdev_handle *handle)
{
	bdev_release(handle);
}

static inline struct block_device *p2p_handle_to_bdev(p2p_bdev_handle *handle)
{
	return handle->bdev;
}

static inline struct block_device *p2p_bdev_whole(struct block_device *bdev)
{
	return bdev_whole(bdev);
}

static inline sector_t p2p_bdev_nr_sectors(struct block_device *bdev)
{
	return bdev_nr_sectors(bdev);
}

static inline void p2p_execute_rq_nowait(struct request *req, struct gendisk *disk)
{
	blk_execute_rq_nowait(req, true);
}

static inline struct class *p2p_class_create(const char *name)
{
	return class_create(name);
}

#endif

#endif
