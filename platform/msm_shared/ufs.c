/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <debug.h>
#include <reg.h>
#include <ufs_hw.h>
#include <utp.h>
#include <upiu.h>
#include <uic.h>
#include <ucs.h>
#include <dme.h>
#include <string.h>
#include <platform/iomap.h>
#include <kernel/mutex.h>

static int ufs_dev_init(struct ufs_dev *dev)
{
	/* Init the mutexes. */
	mutex_init(&(dev->uic_data.uic_mutex));
	mutex_init(&(dev->utrd_data.bitmap_mutex));
	mutex_init(&(dev->utmrd_data.bitmap_mutex));

	/* Initialize wait lists. */
	list_initialize(&(dev->utrd_data.list_head.list_node));
	list_initialize(&(dev->utmrd_data.list_head.list_node));

	/* Initialize the bitmaps. */
	dev->utrd_data.bitmap  = 0;
	dev->utmrd_data.bitmap = 0;

	/* Initialize task ids. */
	dev->utrd_data.task_id  = 0;
	dev->utmrd_data.task_id = 0;

	/* Allocate memory for lists. */
	dev->utrd_data.list_base_addr  = ufs_alloc_trans_req_list();
	dev->utmrd_data.list_base_addr = ufs_alloc_task_mgmt_req_list();

	if (!dev->utrd_data.list_base_addr || !dev->utmrd_data.list_base_addr)
		return -UFS_FAILURE;

	return UFS_SUCCESS;
}

static void ufs_setup_req_lists(struct ufs_dev *dev)
{
	uint32_t val;

	writel(dev->utmrd_data.list_base_addr, UFS_UTMRLBA(dev->base));
	writel(dev->utmrd_data.list_base_addr << 32, UFS_UTMRLBAU(dev->base));

	writel(dev->utrd_data.list_base_addr, UFS_UTRLBA(dev->base));
	writel(dev->utrd_data.list_base_addr << 32, UFS_UTRLBAU(dev->base));

	writel(1, UFS_UTMRLRSR(dev->base));
	writel(1, UFS_UTRLRSR(dev->base));

	/* Enable the required irqs. */
	val = UFS_IE_UTRCE | UFS_IE_UEE | UFS_IE_UTMRCE | UFS_IE_UCCE ;
	ufs_irq_enable(dev, val);
}

int ufs_read(struct ufs_dev* dev, uint64_t start_lba, addr_t buffer, uint32_t num_blocks)
{
	struct scsi_rdwr_req req;
	int                  ret;

	req.data_buffer_base = buffer;
	req.lun              = 0;
	req.num_blocks       = num_blocks;
	req.start_lba        = start_lba / dev->block_size;

	ret = ucs_do_scsi_read(dev, &req);
	if (ret)
	{
		dprintf(CRITICAL, "UFS read failed.\n");
	}

	return ret;
}

int ufs_write(struct ufs_dev* dev, uint64_t start_lba, addr_t buffer, uint32_t num_blocks)
{
	struct scsi_rdwr_req req;
	int                  ret;

	req.data_buffer_base = buffer;
	req.lun              = 0;
	req.num_blocks       = num_blocks;
	req.start_lba        = start_lba / dev->block_size;

	ret = ucs_do_scsi_write(dev, &req);
	if (ret)
	{
		dprintf(CRITICAL, "UFS write failed.\n");
	}

	return ret;
}
uint64_t ufs_get_dev_capacity(struct ufs_dev* dev)
{
	uint64_t capacity;
	int ret = 0;

	ret = dme_read_unit_desc(dev, 0, &capacity);
	if (ret)
	{
		dprintf(CRITICAL, "Failed to read unit descriptor\n");
	}

	return capacity;
}

uint32_t ufs_get_serial_num(struct ufs_dev* dev)
{
	int ret;

	ret = dme_read_device_desc(dev);
	if (ret)
	{
		dprintf(CRITICAL, "UFS get serial number failed.\n");
	}

	return dev->serial_num;
}

uint32_t ufs_get_page_size(struct ufs_dev* dev)
{
	return dev->block_size;
}

int ufs_init(struct ufs_dev *dev)
{
	uint32_t ret = UFS_SUCCESS;
	uint64_t cap;

	dev->block_size = 4096;

	/* Init dev struct. */
	ret = ufs_dev_init(dev);
	if (ret != UFS_SUCCESS)
	{
		dprintf(CRITICAL, "UFS init failed\n");
		goto ufs_init_err;
	}

	/* Perform Data link init. */
	ret = uic_init(dev);
	if (ret != UFS_SUCCESS)
	{
		dprintf(CRITICAL, "UFS init failed\n");
		goto ufs_init_err;
	}

	/* Setup request lists. */
	ufs_setup_req_lists(dev);

	/* Send NOP to check if device UTP layer is ready. */
	ret = dme_send_nop_query(dev);
	if (ret != UFS_SUCCESS)
	{
		dprintf(CRITICAL, "UFS init failed\n");
		goto ufs_init_err;
	}

	ret = dme_set_fdeviceinit(dev);
    if (ret != UFS_SUCCESS)
    {
        dprintf(CRITICAL, "UFS init failed\n");
        goto ufs_init_err;
    }

	ret = ucs_scsi_send_inquiry(dev);
    if (ret != UFS_SUCCESS)
    {
        dprintf(CRITICAL, "UFS init failed\n");
        goto ufs_init_err;
    }

ufs_init_err:
	return ret;
}
