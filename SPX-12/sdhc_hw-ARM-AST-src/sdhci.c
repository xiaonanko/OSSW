/*
 *  linux/drivers/mmc/host/sdhci.c - Secure Digital Host Controller Interface driver
 *
 *  Copyright (C) 2005-2008 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Thanks to the following companies for their support:
 *
 *     - JMicron (hardware and technical support)
 */
/* Modified for AST SD Controller */
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>

#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/module.h>
#endif

#include <linux/leds.h>

#include <linux/mmc/host.h>
#include <linux/kernel.h>	
#include <linux/module.h>
#include <linux/version.h>
#include <linux/proc_fs.h>	
#include <linux/err.h>		
#include <linux/slab.h>		
#include <asm/io.h>		

#include "sdhci.h"

#define DRIVER_NAME "sdhci"

#define SCU_UNLOCK_MAGIC        0x1688A8A8
#define SD_CLOCK_BIT	(1 << 27)
#define SD_CLOCK_RUN	(1 << 15)
#define SD_RESET_BIT	(1 << 16)

#define SD_CLOCK_DIVIDER_MASK		(7 << 12) /* bits[14:12] */
#define SD_CLOCK_DIVIDER_SELECTION	3 /* SDCLK = H-PLL / 8 */
#define SDIO_ALL_SOFTWARE_RESET		0x01

#define AST_SDHC_VA_BASE    IO_ADDRESS(0x1E740000)
#define AST_SDHC_INFO		0x00
#define AST_SDHC_DEBONUCE	0x04
#define AST_SDHC1_CAP		0x140
#define AST_SDHC2_CAP		0x240

#define	AST_SCU_MULTI_PIN_SDHC1		0x01
#define	AST_SCU_MULTI_PIN_SDHC2		0x02

#define DBG(f, x...) \
	pr_debug(DRIVER_NAME " [%s()]: " f, __func__,## x)

static unsigned int debug_quirks = 0;

static unsigned int ast_scu_sdhc1_enabled = 0;
static unsigned int ast_scu_sdhc2_enabled = 0;
static unsigned int ast_sdhc_port1_enabled = 0;
static unsigned int ast_sdhc_port2_enabled = 0;
static unsigned int flag_interrupt_card_remove = 0;
int  g_sd_status = -1;


static void sdhci_prepare_data(struct sdhci_host *, struct mmc_data *);
static void sdhci_finish_data(struct sdhci_host *);

static void sdhci_send_command(struct sdhci_host *, struct mmc_command *);
static void sdhci_finish_command(struct sdhci_host *);

struct proc_dir_entry *AddProcEntry(struct proc_dir_entry *moduledir,char *KeyName,
		read_proc_t *read_proc, write_proc_t *write_proc,void *PrivateData);	

extern int sdinfo_read(char *buf, char **start, off_t offset, int count, int *eof, void *data);	

struct proc_dir_entry *rootdir = NULL;			
static struct proc_dir_entry *sdinfo_proc = NULL;	
static struct proc_dir_entry *moduledir = NULL;	
struct sdhci_host *sdhci_host = NULL;

static struct resource ast_sdhci_resource[] = {
	[0] = {
		.start = 0x1E740100,
		.end = 0x1E740100 + 0xFF,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = 26,
		.end = 26,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource ast_sdhci_resource2[] = {
	[0] = {
		.start = 0x1E740200,
		.end = 0x1E740200 + 0xFF,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = 26,
		.end = 26,
		.flags = IORESOURCE_IRQ,
	},
};

static void ast_sdhci_release(struct device * dev)
{
    return ;
}

static struct platform_device ast_sdhci_device = {
	.name	= "ast_sdhci",
	.resource = ast_sdhci_resource,
 	.dev = { 
 	    .release = ast_sdhci_release, 
 	}	
};

static struct platform_device ast_sdhci_device2 = {
	.name	= "ast_sdhci2",
	.resource = ast_sdhci_resource2,
 	.dev = { 
 	    .release = ast_sdhci_release, 
 	}	
};


static void sdhci_dumpregs(struct sdhci_host *host)
{
	printk(KERN_DEBUG DRIVER_NAME ": ============== REGISTER DUMP ==============\n");

	printk(KERN_DEBUG DRIVER_NAME ": Sys addr: 0x%08x | Version:  0x%08x\n",
		readl(host->ioaddr + SDHCI_DMA_ADDRESS),
		readw(host->ioaddr + SDHCI_HOST_VERSION));
	printk(KERN_DEBUG DRIVER_NAME ": Blk size: 0x%08x | Blk cnt:  0x%08x\n",
		readw(host->ioaddr + SDHCI_BLOCK_SIZE),
		readw(host->ioaddr + SDHCI_BLOCK_COUNT));
	printk(KERN_DEBUG DRIVER_NAME ": Argument: 0x%08x | Trn mode: 0x%08x\n",
		readl(host->ioaddr + SDHCI_ARGUMENT),
		readw(host->ioaddr + SDHCI_TRANSFER_MODE));
	printk(KERN_DEBUG DRIVER_NAME ": Present:  0x%08x | Host ctl: 0x%08x\n",
		readl(host->ioaddr + SDHCI_PRESENT_STATE),
		readb(host->ioaddr + SDHCI_HOST_CONTROL));
	printk(KERN_DEBUG DRIVER_NAME ": Power:    0x%08x | Blk gap:  0x%08x\n",
		readb(host->ioaddr + SDHCI_POWER_CONTROL),
		readb(host->ioaddr + SDHCI_BLOCK_GAP_CONTROL));
	printk(KERN_DEBUG DRIVER_NAME ": Wake-up:  0x%08x | Clock:    0x%08x\n",
		readb(host->ioaddr + SDHCI_WAKE_UP_CONTROL),
		readw(host->ioaddr + SDHCI_CLOCK_CONTROL));
	printk(KERN_DEBUG DRIVER_NAME ": Timeout:  0x%08x | Int stat: 0x%08x\n",
		readb(host->ioaddr + SDHCI_TIMEOUT_CONTROL),
		readl(host->ioaddr + SDHCI_INT_STATUS));
	printk(KERN_DEBUG DRIVER_NAME ": Int enab: 0x%08x | Sig enab: 0x%08x\n",
		readl(host->ioaddr + SDHCI_INT_ENABLE),
		readl(host->ioaddr + SDHCI_SIGNAL_ENABLE));
	printk(KERN_DEBUG DRIVER_NAME ": AC12 err: 0x%08x | Slot int: 0x%08x\n",
		readw(host->ioaddr + SDHCI_ACMD12_ERR),
		readw(host->ioaddr + SDHCI_SLOT_INT_STATUS));
	printk(KERN_DEBUG DRIVER_NAME ": Caps:     0x%08x | Max curr: 0x%08x\n",
		readl(host->ioaddr + SDHCI_CAPABILITIES),
		readl(host->ioaddr + SDHCI_MAX_CURRENT));

	printk(KERN_DEBUG DRIVER_NAME ": ===========================================\n");
}

/*****************************************************************************\
 *                                                                           *
 * Low level functions                                                       *
 *                                                                           *
\*****************************************************************************/

static void sdhci_reset(struct sdhci_host *host, u8 mask)
{
	unsigned long timeout;

	if (host->quirks & SDHCI_QUIRK_NO_CARD_NO_RESET) {
		if (!(readl(host->ioaddr + SDHCI_PRESENT_STATE) &
			SDHCI_CARD_PRESENT))
			return;
	}

	writeb(mask, host->ioaddr + SDHCI_SOFTWARE_RESET);

	if (mask & SDHCI_RESET_ALL)
		host->clock = 0;

	/* Wait max 100 ms */
	timeout = 100;

	/* hw clears the bit when it's done */
	while (readb(host->ioaddr + SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			printk(KERN_ERR "%s: Reset 0x%x never completed.\n",
				mmc_hostname(host->mmc), (int)mask);
			sdhci_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}
}

static void sdhci_init(struct sdhci_host *host)
{
	u32 intmask;

	sdhci_reset(host, SDHCI_RESET_ALL);

	intmask = SDHCI_INT_BUS_POWER | SDHCI_INT_DATA_END_BIT |
		SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_INDEX |
		SDHCI_INT_END_BIT | SDHCI_INT_CRC | SDHCI_INT_TIMEOUT |
		SDHCI_INT_CARD_REMOVE | SDHCI_INT_CARD_INSERT |
		SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL |
		SDHCI_INT_DMA_END | SDHCI_INT_DATA_END | SDHCI_INT_RESPONSE |
		SDHCI_INT_ADMA_ERROR;

	writel(intmask, host->ioaddr + SDHCI_INT_ENABLE);
	writel(intmask, host->ioaddr + SDHCI_SIGNAL_ENABLE);
}

static void sdhci_activate_led(struct sdhci_host *host)
{
	u8 ctrl;

	ctrl = readb(host->ioaddr + SDHCI_HOST_CONTROL);
	ctrl |= SDHCI_CTRL_LED;
	writeb(ctrl, host->ioaddr + SDHCI_HOST_CONTROL);
}

static void sdhci_deactivate_led(struct sdhci_host *host)
{
	u8 ctrl;

	ctrl = readb(host->ioaddr + SDHCI_HOST_CONTROL);
	ctrl &= ~SDHCI_CTRL_LED;
	writeb(ctrl, host->ioaddr + SDHCI_HOST_CONTROL);
}

#ifdef CONFIG_LEDS_CLASS
static void sdhci_led_control(struct led_classdev *led,
	enum led_brightness brightness)
{
	struct sdhci_host *host = container_of(led, struct sdhci_host, led);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	if (brightness == LED_OFF)
		sdhci_deactivate_led(host);
	else
		sdhci_activate_led(host);

	spin_unlock_irqrestore(&host->lock, flags);
}
#endif

/*****************************************************************************\
 *                                                                           *
 * Core functions                                                            *
 *                                                                           *
\*****************************************************************************/

static void sdhci_read_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 uninitialized_var(scratch);
	u8 *buf;

	DBG("PIO reading\n");

	blksize = host->data->blksz;
	chunk = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			if (chunk == 0) {
				scratch = readl(host->ioaddr + SDHCI_BUFFER);
				chunk = 4;
			}

			*buf = scratch & 0xFF;

			buf++;
			scratch >>= 8;
			chunk--;
			len--;
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void sdhci_write_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 scratch;
	u8 *buf;

	DBG("PIO writing\n");

	blksize = host->data->blksz;
	chunk = 0;
	scratch = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			scratch |= (u32)*buf << (chunk * 8);

			buf++;
			chunk++;
			len--;

			if ((chunk == 4) || ((len == 0) && (blksize == 0))) {
				writel(scratch, host->ioaddr + SDHCI_BUFFER);
				chunk = 0;
				scratch = 0;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void sdhci_transfer_pio(struct sdhci_host *host)
{
	u32 mask;

	BUG_ON(!host->data);

	if (host->blocks == 0)
		return;

	if (host->data->flags & MMC_DATA_READ)
		mask = SDHCI_DATA_AVAILABLE;
	else
		mask = SDHCI_SPACE_AVAILABLE;

	/*
	 * Some controllers (JMicron JMB38x) mess up the buffer bits
	 * for transfers < 4 bytes. As long as it is just one block,
	 * we can ignore the bits.
	 */
	if ((host->quirks & SDHCI_QUIRK_BROKEN_SMALL_PIO) &&
		(host->data->blocks == 1))
		mask = ~0;

	while (readl(host->ioaddr + SDHCI_PRESENT_STATE) & mask) {
		if (host->data->flags & MMC_DATA_READ)
			sdhci_read_block_pio(host);
		else
			sdhci_write_block_pio(host);

		host->blocks--;
		if (host->blocks == 0)
			break;
	}

	DBG("PIO transfer complete.\n");
}

static char *sdhci_kmap_atomic(struct scatterlist *sg, unsigned long *flags)
{
	local_irq_save(*flags);
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
	return kmap_atomic(sg_page(sg)) + sg->offset;
#else
	return kmap_atomic(sg_page(sg), KM_BIO_SRC_IRQ) + sg->offset;		
#endif
}

static void sdhci_kunmap_atomic(void *buffer, unsigned long *flags)
{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
	kunmap_atomic(buffer);
#else
	kunmap_atomic(buffer, KM_BIO_SRC_IRQ);
#endif
	local_irq_restore(*flags);
}

static int sdhci_adma_table_pre(struct sdhci_host *host,
	struct mmc_data *data)
{
	int direction;

	u8 *desc;
	u8 *align;
	dma_addr_t addr;
	dma_addr_t align_addr;
	int len, offset;

	struct scatterlist *sg;
	int i;
	char *buffer;
	unsigned long flags;

	/*
	 * The spec does not specify endianness of descriptor table.
	 * We currently guess that it is LE.
	 */

	if (data->flags & MMC_DATA_READ)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	/*
	 * The ADMA descriptor table is mapped further down as we
	 * need to fill it with data first.
	 */

	host->align_addr = dma_map_single(mmc_dev(host->mmc),
		host->align_buffer, 128 * 4, direction);
	if (dma_mapping_error(mmc_dev(host->mmc), host->align_addr))
		goto fail;
	BUG_ON(host->align_addr & 0x3);

	host->sg_count = dma_map_sg(mmc_dev(host->mmc),
		data->sg, data->sg_len, direction);
	if (host->sg_count == 0)
		goto unmap_align;

	desc = host->adma_desc;
	align = host->align_buffer;

	align_addr = host->align_addr;

	for_each_sg(data->sg, sg, host->sg_count, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);

		/*
		 * The SDHCI specification states that ADMA
		 * addresses must be 32-bit aligned. If they
		 * aren't, then we use a bounce buffer for
		 * the (up to three) bytes that screw up the
		 * alignment.
		 */
		offset = (4 - (addr & 0x3)) & 0x3;
		if (offset) {
			if (data->flags & MMC_DATA_WRITE) {
				buffer = sdhci_kmap_atomic(sg, &flags);
				WARN_ON(((long)buffer & PAGE_MASK) > (PAGE_SIZE - 3));
				memcpy(align, buffer, offset);
				sdhci_kunmap_atomic(buffer, &flags);
			}

			desc[7] = (align_addr >> 24) & 0xff;
			desc[6] = (align_addr >> 16) & 0xff;
			desc[5] = (align_addr >> 8) & 0xff;
			desc[4] = (align_addr >> 0) & 0xff;

			BUG_ON(offset > 65536);

			desc[3] = (offset >> 8) & 0xff;
			desc[2] = (offset >> 0) & 0xff;

			desc[1] = 0x00;
			desc[0] = 0x21; /* tran, valid */

			align += 4;
			align_addr += 4;

			desc += 8;

			addr += offset;
			len -= offset;
		}

		desc[7] = (addr >> 24) & 0xff;
		desc[6] = (addr >> 16) & 0xff;
		desc[5] = (addr >> 8) & 0xff;
		desc[4] = (addr >> 0) & 0xff;

		BUG_ON(len > 65536);

		desc[3] = (len >> 8) & 0xff;
		desc[2] = (len >> 0) & 0xff;

		desc[1] = 0x00;
		desc[0] = 0x21; /* tran, valid */

		desc += 8;

		/*
		 * If this triggers then we have a calculation bug
		 * somewhere. :/
		 */
		WARN_ON((desc - host->adma_desc) > (128 * 2 + 1) * 4);
	}

	/*
	 * Add a terminating entry.
	 */
	desc[7] = 0;
	desc[6] = 0;
	desc[5] = 0;
	desc[4] = 0;

	desc[3] = 0;
	desc[2] = 0;

	desc[1] = 0x00;
	desc[0] = 0x03; /* nop, end, valid */

	/*
	 * Resync align buffer as we might have changed it.
	 */
	if (data->flags & MMC_DATA_WRITE) {
		dma_sync_single_for_device(mmc_dev(host->mmc),
			host->align_addr, 128 * 4, direction);
	}

	host->adma_addr = dma_map_single(mmc_dev(host->mmc),
		host->adma_desc, (128 * 2 + 1) * 4, DMA_TO_DEVICE);
	if (dma_mapping_error(mmc_dev(host->mmc), host->adma_addr))
		goto unmap_entries;
	BUG_ON(host->adma_addr & 0x3);

	return 0;

unmap_entries:
	dma_unmap_sg(mmc_dev(host->mmc), data->sg,
		data->sg_len, direction);
unmap_align:
	dma_unmap_single(mmc_dev(host->mmc), host->align_addr,
		128 * 4, direction);
fail:
	return -EINVAL;
}

static void sdhci_adma_table_post(struct sdhci_host *host,
	struct mmc_data *data)
{
	int direction;

	struct scatterlist *sg;
	int i, size;
	u8 *align;
	char *buffer;
	unsigned long flags;

	if (data->flags & MMC_DATA_READ)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	dma_unmap_single(mmc_dev(host->mmc), host->adma_addr,
		(128 * 2 + 1) * 4, DMA_TO_DEVICE);

	dma_unmap_single(mmc_dev(host->mmc), host->align_addr,
		128 * 4, direction);

	if (data->flags & MMC_DATA_READ) {
		dma_sync_sg_for_cpu(mmc_dev(host->mmc), data->sg,
			data->sg_len, direction);

		align = host->align_buffer;

		for_each_sg(data->sg, sg, host->sg_count, i) {
			if (sg_dma_address(sg) & 0x3) {
				size = 4 - (sg_dma_address(sg) & 0x3);

				buffer = sdhci_kmap_atomic(sg, &flags);
				WARN_ON(((long)buffer & PAGE_MASK) > (PAGE_SIZE - 3));
				memcpy(buffer, align, size);
				sdhci_kunmap_atomic(buffer, &flags);

				align += 4;
			}
		}
	}

	dma_unmap_sg(mmc_dev(host->mmc), data->sg,
		data->sg_len, direction);
}

static u8 sdhci_calc_timeout(struct sdhci_host *host, struct mmc_data *data)
{
	u8 count;
	unsigned target_timeout, current_timeout;

	/*
	 * If the host controller provides us with an incorrect timeout
	 * value, just skip the check and use 0xE.  The hardware may take
	 * longer to time out, but that's much better than having a too-short
	 * timeout value.
	 */
	if ((host->quirks & SDHCI_QUIRK_BROKEN_TIMEOUT_VAL))
		return 0xE;

	/* timeout in us */
	target_timeout = data->timeout_ns / 1000 +
		data->timeout_clks / host->clock;

	/*
	 * Figure out needed cycles.
	 * We do this in steps in order to fit inside a 32 bit int.
	 * The first step is the minimum timeout, which will have a
	 * minimum resolution of 6 bits:
	 * (1) 2^13*1000 > 2^22,
	 * (2) host->timeout_clk < 2^16
	 *     =>
	 *     (1) / (2) > 2^6
	 */
	count = 0;
	current_timeout = (1 << 13) * 1000 / host->timeout_clk;
	while (current_timeout < target_timeout) {
		count++;
		current_timeout <<= 1;
		if (count >= 0xF)
			break;
	}

	if (count >= 0xF) {
		printk(KERN_WARNING "%s: Too large timeout requested!\n",
			mmc_hostname(host->mmc));
		count = 0xE;
	}

	return count;
}

static void sdhci_prepare_data(struct sdhci_host *host, struct mmc_data *data)
{
	u8 count;
	u8 ctrl;
	int ret;

	WARN_ON(host->data);

	if (data == NULL)
		return;

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > 65535);

	host->data = data;
	host->data_early = 0;

	count = sdhci_calc_timeout(host, data);
	writeb(count, host->ioaddr + SDHCI_TIMEOUT_CONTROL);

	if (host->flags & SDHCI_USE_DMA)
		host->flags |= SDHCI_REQ_USE_DMA;

	/*
	 * FIXME: This doesn't account for merging when mapping the
	 * scatterlist.
	 */
	if (host->flags & SDHCI_REQ_USE_DMA) {
		int broken, i;
		struct scatterlist *sg;

		broken = 0;
		if (host->flags & SDHCI_USE_ADMA) {
			if (host->quirks & SDHCI_QUIRK_32BIT_ADMA_SIZE)
				broken = 1;
		} else {
			if (host->quirks & SDHCI_QUIRK_32BIT_DMA_SIZE)
				broken = 1;
		}

		if (unlikely(broken)) {
			for_each_sg(data->sg, sg, data->sg_len, i) {
				if (sg->length & 0x3) {
					DBG("Reverting to PIO because of "
						"transfer size (%d)\n",
						sg->length);
					host->flags &= ~SDHCI_REQ_USE_DMA;
					break;
				}
			}
		}
	}

	/*
	 * The assumption here being that alignment is the same after
	 * translation to device address space.
	 */
	if (host->flags & SDHCI_REQ_USE_DMA) {
		int broken, i;
		struct scatterlist *sg;

		broken = 0;
		if (host->flags & SDHCI_USE_ADMA) {
			/*
			 * As we use 3 byte chunks to work around
			 * alignment problems, we need to check this
			 * quirk.
			 */
			if (host->quirks & SDHCI_QUIRK_32BIT_ADMA_SIZE)
				broken = 1;
		} else {
			if (host->quirks & SDHCI_QUIRK_32BIT_DMA_ADDR)
				broken = 1;
		}

		if (unlikely(broken)) {
			for_each_sg(data->sg, sg, data->sg_len, i) {
				if (sg->offset & 0x3) {
					DBG("Reverting to PIO because of "
						"bad alignment\n");
					host->flags &= ~SDHCI_REQ_USE_DMA;
					break;
				}
			}
		}
	}

	if (host->flags & SDHCI_REQ_USE_DMA) {
		if (host->flags & SDHCI_USE_ADMA) {
			ret = sdhci_adma_table_pre(host, data);
			if (ret) {
				/*
				 * This only happens when someone fed
				 * us an invalid request.
				 */
				WARN_ON(1);
				host->flags &= ~SDHCI_REQ_USE_DMA;
			} else {
				writel(host->adma_addr,
					host->ioaddr + SDHCI_ADMA_ADDRESS);
			}
		} else {
			int sg_cnt;

			sg_cnt = dma_map_sg(mmc_dev(host->mmc),
					data->sg, data->sg_len,
					(data->flags & MMC_DATA_READ) ?
						DMA_FROM_DEVICE :
						DMA_TO_DEVICE);
			if (sg_cnt == 0) {
				/*
				 * This only happens when someone fed
				 * us an invalid request.
				 */
				WARN_ON(1);
				host->flags &= ~SDHCI_REQ_USE_DMA;
			} else {
				WARN_ON(sg_cnt != 1);
				writel(sg_dma_address(data->sg),
					host->ioaddr + SDHCI_DMA_ADDRESS);
			}
		}
	}

	/*
	 * Always adjust the DMA selection as some controllers
	 * (e.g. JMicron) can't do PIO properly when the selection
	 * is ADMA.
	 */
	if (host->version >= SDHCI_SPEC_200) {
		ctrl = readb(host->ioaddr + SDHCI_HOST_CONTROL);
		ctrl &= ~SDHCI_CTRL_DMA_MASK;
		if ((host->flags & SDHCI_REQ_USE_DMA) &&
			(host->flags & SDHCI_USE_ADMA))
			ctrl |= SDHCI_CTRL_ADMA32;
		else
			ctrl |= SDHCI_CTRL_SDMA;
		writeb(ctrl, host->ioaddr + SDHCI_HOST_CONTROL);
	}

	if (!(host->flags & SDHCI_REQ_USE_DMA)) {
		sg_miter_start(&host->sg_miter,
			data->sg, data->sg_len, SG_MITER_ATOMIC);
		host->blocks = data->blocks;
	}

	/* We do not handle DMA boundaries, so set it to max (512 KiB) */
	writew(SDHCI_MAKE_BLKSZ(7, data->blksz),
		host->ioaddr + SDHCI_BLOCK_SIZE);
	writew(data->blocks, host->ioaddr + SDHCI_BLOCK_COUNT);
}

static void sdhci_set_transfer_mode(struct sdhci_host *host,
	struct mmc_data *data)
{
	u16 mode;

	if (data == NULL)
		return;

	WARN_ON(!host->data);

	mode = SDHCI_TRNS_BLK_CNT_EN;
	if (data->blocks > 1)
		mode |= SDHCI_TRNS_MULTI;
	if (data->flags & MMC_DATA_READ)
		mode |= SDHCI_TRNS_READ;
	if (host->flags & SDHCI_REQ_USE_DMA)
		mode |= SDHCI_TRNS_DMA;

	writew(mode, host->ioaddr + SDHCI_TRANSFER_MODE);
}

static void sdhci_finish_data(struct sdhci_host *host)
{
	struct mmc_data *data;

	BUG_ON(!host->data);

	data = host->data;
	host->data = NULL;

	if (host->flags & SDHCI_REQ_USE_DMA) {
		if (host->flags & SDHCI_USE_ADMA)
			sdhci_adma_table_post(host, data);
		else {
			dma_unmap_sg(mmc_dev(host->mmc), data->sg,
				data->sg_len, (data->flags & MMC_DATA_READ) ?
					DMA_FROM_DEVICE : DMA_TO_DEVICE);
		}
	}

	/*
	 * The specification states that the block count register must
	 * be updated, but it does not specify at what point in the
	 * data flow. That makes the register entirely useless to read
	 * back so we have to assume that nothing made it to the card
	 * in the event of an error.
	 */
	if (data->error)
		data->bytes_xfered = 0;
	else
		data->bytes_xfered = data->blksz * data->blocks;

	if (data->stop) {
		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error) {
			sdhci_reset(host, SDHCI_RESET_CMD);
			sdhci_reset(host, SDHCI_RESET_DATA);
		}

		sdhci_send_command(host, data->stop);
	} else
		tasklet_schedule(&host->finish_tasklet);
}

static void sdhci_send_command(struct sdhci_host *host, struct mmc_command *cmd)
{
	int flags;
	u32 mask;
	unsigned long timeout;

	WARN_ON(host->cmd);

	/* Wait max 10 ms */
	timeout = 10;

	mask = SDHCI_CMD_INHIBIT;
	if ((cmd->data != NULL) || (cmd->flags & MMC_RSP_BUSY))
		mask |= SDHCI_DATA_INHIBIT;

	/* We shouldn't wait for data inihibit for stop commands, even
	   though they might use busy signaling */
	if (host->mrq->data && (cmd == host->mrq->data->stop))
		mask &= ~SDHCI_DATA_INHIBIT;

	while (readl(host->ioaddr + SDHCI_PRESENT_STATE) & mask) {
		if (timeout == 0) {
			printk(KERN_ERR "%s: Controller never released "
				"inhibit bit(s).\n", mmc_hostname(host->mmc));
			sdhci_dumpregs(host);
			cmd->error = -EIO;
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
		timeout--;
		mdelay(1);
	}

	mod_timer(&host->timer, jiffies + 10 * HZ);

	host->cmd = cmd;

	sdhci_prepare_data(host, cmd->data);

	writel(cmd->arg, host->ioaddr + SDHCI_ARGUMENT);

	sdhci_set_transfer_mode(host, cmd->data);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		printk(KERN_ERR "%s: Unsupported response type!\n",
			mmc_hostname(host->mmc));
		cmd->error = -EINVAL;
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (!(cmd->flags & MMC_RSP_PRESENT))
		flags = SDHCI_CMD_RESP_NONE;
	else if (cmd->flags & MMC_RSP_136)
		flags = SDHCI_CMD_RESP_LONG;
	else if (cmd->flags & MMC_RSP_BUSY)
		flags = SDHCI_CMD_RESP_SHORT_BUSY;
	else
		flags = SDHCI_CMD_RESP_SHORT;

	if (cmd->flags & MMC_RSP_CRC)
		flags |= SDHCI_CMD_CRC;
	if (cmd->flags & MMC_RSP_OPCODE)
		flags |= SDHCI_CMD_INDEX;
	if (cmd->data)
		flags |= SDHCI_CMD_DATA;

	writew(SDHCI_MAKE_CMD(cmd->opcode, flags),
		host->ioaddr + SDHCI_COMMAND);
}

static void sdhci_finish_command(struct sdhci_host *host)
{
	int i;

	BUG_ON(host->cmd == NULL);

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0;i < 4;i++) {
				host->cmd->resp[i] = readl(host->ioaddr +
					SDHCI_RESPONSE + (3-i)*4) << 8;
				if (i != 3)
					host->cmd->resp[i] |=
						readb(host->ioaddr +
						SDHCI_RESPONSE + (3-i)*4-1);
			}
		} else {
			host->cmd->resp[0] = readl(host->ioaddr + SDHCI_RESPONSE);
		}
	}

	host->cmd->error = 0;

	if (host->data && host->data_early)
		sdhci_finish_data(host);

	if (!host->cmd->data)
		tasklet_schedule(&host->finish_tasklet);

	host->cmd = NULL;
}

static void sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	int div;
	u16 clk;
	unsigned long timeout;

	if (clock == host->clock)
		return;

	writew(0, host->ioaddr + SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		goto out;

	for (div = 1;div < 256;div *= 2) {
		if ((host->max_clk / div) <= clock)
			break;
	}
	div >>= 1;

	clk = div << SDHCI_DIVIDER_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	writew(clk, host->ioaddr + SDHCI_CLOCK_CONTROL);

	/* Wait max 10 ms */
	timeout = 10;
	while (!((clk = readw(host->ioaddr + SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			printk(KERN_ERR "%s: Internal clock never "
				"stabilised.\n", mmc_hostname(host->mmc));
			sdhci_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	writew(clk, host->ioaddr + SDHCI_CLOCK_CONTROL);

out:
	host->clock = clock;
}

static void sdhci_set_power(struct sdhci_host *host, unsigned short power)
{
	u8 pwr;

	if (host->power == power)
		return;

	if (power == (unsigned short)-1) {
		writeb(0, host->ioaddr + SDHCI_POWER_CONTROL);
		goto out;
	}

	/*
	 * Spec says that we should clear the power reg before setting
	 * a new value. Some controllers don't seem to like this though.
	 */
	if (!(host->quirks & SDHCI_QUIRK_SINGLE_POWER_WRITE))
		writeb(0, host->ioaddr + SDHCI_POWER_CONTROL);

	pwr = SDHCI_POWER_ON;

	switch (1 << power) {
	case MMC_VDD_165_195:
		pwr |= SDHCI_POWER_180;
		break;
	case MMC_VDD_29_30:
	case MMC_VDD_30_31:
		pwr |= SDHCI_POWER_300;
		break;
	case MMC_VDD_32_33:
	case MMC_VDD_33_34:
		pwr |= SDHCI_POWER_330;
		break;
	default:
		BUG();
	}

	/*
	 * At least the Marvell CaFe chip gets confused if we set the voltage
	 * and set turn on power at the same time, so set the voltage first.
	 */
	if ((host->quirks & SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER))
		writeb(pwr & ~SDHCI_POWER_ON,
				host->ioaddr + SDHCI_POWER_CONTROL);

	writeb(pwr, host->ioaddr + SDHCI_POWER_CONTROL);

out:
	host->power = power;
}

/*****************************************************************************\
 *                                                                           *
 * MMC callbacks                                                             *
 *                                                                           *
\*****************************************************************************/

static void sdhci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host;
	unsigned long flags;

	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

#ifndef CONFIG_LEDS_CLASS
	sdhci_activate_led(host);
#endif

	host->mrq = mrq;

	if (!(readl(host->ioaddr + SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT)
		|| (host->flags & SDHCI_DEVICE_DEAD)) {
		host->mrq->cmd->error = -ENOMEDIUM;
		tasklet_schedule(&host->finish_tasklet);
	} else
		sdhci_send_command(host, mrq->cmd);

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void sdhci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host;
	unsigned long flags;
	u8 ctrl;

	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	if (host->flags & SDHCI_DEVICE_DEAD)
		goto out;

	/*
	 * Reset the chip on each power off.
	 * Should clear out any weird states.
	 */
	if (ios->power_mode == MMC_POWER_OFF) {
		writel(0, host->ioaddr + SDHCI_SIGNAL_ENABLE);
		sdhci_init(host);

		if (flag_interrupt_card_remove == 1)
		{
			flag_interrupt_card_remove = 0;

		}
	}

	sdhci_set_clock(host, ios->clock);

	if (ios->power_mode == MMC_POWER_OFF)
		sdhci_set_power(host, -1);
	else
		sdhci_set_power(host, ios->vdd);

	ctrl = readb(host->ioaddr + SDHCI_HOST_CONTROL);

	if (ios->bus_width == MMC_BUS_WIDTH_4)
		ctrl |= SDHCI_CTRL_4BITBUS;
	else
		ctrl &= ~SDHCI_CTRL_4BITBUS;

	if (ios->timing == MMC_TIMING_SD_HS)
		ctrl |= SDHCI_CTRL_HISPD;
	else
		ctrl &= ~SDHCI_CTRL_HISPD;

	writeb(ctrl, host->ioaddr + SDHCI_HOST_CONTROL);

	/*
	 * Some (ENE) controllers go apeshit on some ios operation,
	 * signalling timeout and CRC errors even on CMD0. Resetting
	 * it on each ios seems to solve the problem.
	 */
	if(host->quirks & SDHCI_QUIRK_RESET_CMD_DATA_ON_IOS)
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

out:
	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static int sdhci_get_ro(struct mmc_host *mmc)
{
	struct sdhci_host *host;
	unsigned long flags;
	int present;

	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	if (host->flags & SDHCI_DEVICE_DEAD)
		present = 0;
	else
		present = readl(host->ioaddr + SDHCI_PRESENT_STATE);

	spin_unlock_irqrestore(&host->lock, flags);

	return !(present & SDHCI_WRITE_PROTECT);
}

static void sdhci_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct sdhci_host *host;
	unsigned long flags;
	u32 ier;

	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	if (host->flags & SDHCI_DEVICE_DEAD)
		goto out;

	ier = readl(host->ioaddr + SDHCI_INT_ENABLE);

	ier &= ~SDHCI_INT_CARD_INT;
	if (enable)
		ier |= SDHCI_INT_CARD_INT;

	writel(ier, host->ioaddr + SDHCI_INT_ENABLE);
	writel(ier, host->ioaddr + SDHCI_SIGNAL_ENABLE);

out:
	mmiowb();

	spin_unlock_irqrestore(&host->lock, flags);
}

static const struct mmc_host_ops sdhci_ops = {
	.request	= sdhci_request,
	.set_ios	= sdhci_set_ios,
	.get_ro		= sdhci_get_ro,
	.enable_sdio_irq = sdhci_enable_sdio_irq,
};

/*****************************************************************************\
 *                                                                           *
 * Tasklets                                                                  *
 *                                                                           *
\*****************************************************************************/

static void sdhci_tasklet_card(unsigned long param)
{
	struct sdhci_host *host;
	unsigned long flags;

	host = (struct sdhci_host*)param;

	spin_lock_irqsave(&host->lock, flags);

	if (!(readl(host->ioaddr + SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT)) {
		if (host->mrq) {
			printk(KERN_ERR "%s: Card removed during transfer!\n",
				mmc_hostname(host->mmc));
			printk(KERN_ERR "%s: Resetting controller.\n",
				mmc_hostname(host->mmc));

			sdhci_reset(host, SDHCI_RESET_CMD);
			sdhci_reset(host, SDHCI_RESET_DATA);

			host->mrq->cmd->error = -ENOMEDIUM;
			tasklet_schedule(&host->finish_tasklet);
		}
	}

	spin_unlock_irqrestore(&host->lock, flags);

	mmc_detect_change(host->mmc, msecs_to_jiffies(200));
}

static void sdhci_tasklet_finish(unsigned long param)
{
	struct sdhci_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct sdhci_host*)param;

	spin_lock_irqsave(&host->lock, flags);

	del_timer(&host->timer);

	mrq = host->mrq;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (!(host->flags & SDHCI_DEVICE_DEAD) &&
		(mrq->cmd->error ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error))) ||
		   (host->quirks & SDHCI_QUIRK_RESET_AFTER_REQUEST))) {

		/* Some controllers need this kick or reset won't work here */
		if (host->quirks & SDHCI_QUIRK_CLOCK_BEFORE_RESET) {
			unsigned int clock;

			/* This is to force an update */
			clock = host->clock;
			host->clock = 0;
			sdhci_set_clock(host, clock);
		}

		/* Spec says we should do both at the same time, but Ricoh
		   controllers do not like that. */
		sdhci_reset(host, SDHCI_RESET_CMD);
		sdhci_reset(host, SDHCI_RESET_DATA);
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

#ifndef CONFIG_LEDS_CLASS
	sdhci_deactivate_led(host);
#endif

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	mmc_request_done(host->mmc, mrq);
}

static void sdhci_timeout_timer(unsigned long data)
{
	struct sdhci_host *host;
	unsigned long flags;

	host = (struct sdhci_host*)data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->mrq) {
		printk(KERN_ERR "%s: Timeout waiting for hardware "
			"interrupt.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			sdhci_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			else
				host->mrq->cmd->error = -ETIMEDOUT;

			tasklet_schedule(&host->finish_tasklet);
		}
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

/*****************************************************************************\
 *                                                                           *
 * Interrupt handling                                                        *
 *                                                                           *
\*****************************************************************************/

static void sdhci_cmd_irq(struct sdhci_host *host, u32 intmask)
{
	BUG_ON(intmask == 0);

	if (!host->cmd) {
//		printk(KERN_ERR "%s: Got command interrupt 0x%08x even "
//			"though no command operation was in progress.\n",
//			mmc_hostname(host->mmc), (unsigned)intmask);
		sdhci_dumpregs(host);
		return;
	}

	if (intmask & SDHCI_INT_TIMEOUT)
		host->cmd->error = -ETIMEDOUT;
	else if (intmask & (SDHCI_INT_CRC | SDHCI_INT_END_BIT |
			SDHCI_INT_INDEX))
		host->cmd->error = -EILSEQ;

	if (host->cmd->error) {
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	/*
	 * The host can send and interrupt when the busy state has
	 * ended, allowing us to wait without wasting CPU cycles.
	 * Unfortunately this is overloaded on the "data complete"
	 * interrupt, so we need to take some care when handling
	 * it.
	 *
	 * Note: The 1.0 specification is a bit ambiguous about this
	 *       feature so there might be some problems with older
	 *       controllers.
	 */
	if (host->cmd->flags & MMC_RSP_BUSY) {
		if (host->cmd->data)
			DBG("Cannot wait for busy signal when also "
				"doing a data transfer");
		else if (!(host->quirks & SDHCI_QUIRK_NO_BUSY_IRQ))
			return;

		/* The controller does not support the end-of-busy IRQ,
		 * fall through and take the SDHCI_INT_RESPONSE */
	}

	if (intmask & SDHCI_INT_RESPONSE)
		sdhci_finish_command(host);
}

static void sdhci_data_irq(struct sdhci_host *host, u32 intmask)
{
	BUG_ON(intmask == 0);

	if (!host->data) {
		/*
		 * The "data complete" interrupt is also used to
		 * indicate that a busy state has ended. See comment
		 * above in sdhci_cmd_irq().
		 */
		if (host->cmd && (host->cmd->flags & MMC_RSP_BUSY)) {
			if (intmask & SDHCI_INT_DATA_END) {
				sdhci_finish_command(host);
				return;
			}
		}

		printk(KERN_ERR "%s: Got data interrupt 0x%08x even "
			"though no data operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		sdhci_dumpregs(host);

		return;
	}

	if (intmask & SDHCI_INT_DATA_TIMEOUT)
		host->data->error = -ETIMEDOUT;
	else if (intmask & (SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT))
		host->data->error = -EILSEQ;
	else if (intmask & SDHCI_INT_ADMA_ERROR)
		host->data->error = -EIO;

	if (host->data->error)
		sdhci_finish_data(host);
	else {
		if (intmask & (SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL))
			sdhci_transfer_pio(host);

		/*
		 * We currently don't do anything fancy with DMA
		 * boundaries, but as we can't disable the feature
		 * we need to at least restart the transfer.
		 */
		if (intmask & SDHCI_INT_DMA_END)
			writel(readl(host->ioaddr + SDHCI_DMA_ADDRESS),
				host->ioaddr + SDHCI_DMA_ADDRESS);

		if (intmask & SDHCI_INT_DATA_END) {
			if (host->cmd) {
				/*
				 * Data managed to finish before the
				 * command completed. Make sure we do
				 * things in the proper order.
				 */
				host->data_early = 1;
			} else {
				sdhci_finish_data(host);
			}
		}
	}
}

/*
*@fn sdinfo_read
*@brief    sdinfo_read is a call back funtion to the sdInfo file. 
**         For every read to sdInfo proc file, provides SD card available status. 
**
**@param[in]
**      buf   : The buffer where the data is to be inserted.
**      start : A pointer to a pointer to characters.
**      offset: The current position in the file.
**      count : The size of the buffer in the first argument.  
**      eof   : for future use
**      data  : for future use
**
** @retval :  Returns number of bytes used in the buffer.     
**  
*/
int sdinfo_read(char *buf, char **start, off_t offset, int count, int *eof, void *data)
{
	return snprintf(buf,4,"%d",g_sd_status);
}

static irqreturn_t sdhci_irq(int irq, void *dev_id)
{
	irqreturn_t result;
	struct sdhci_host* host = dev_id;
	u32 intmask;
	int cardint = 0;

	spin_lock(&host->lock);

	intmask = readl(host->ioaddr + SDHCI_INT_STATUS);

	if (!intmask || intmask == 0xffffffff) {
		result = IRQ_NONE;
		goto out;
	}

	DBG("*** %s got interrupt: 0x%08x\n",
		mmc_hostname(host->mmc), intmask);

	if (intmask & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE)) {
		if (intmask & SDHCI_INT_CARD_REMOVE) flag_interrupt_card_remove = 1;
		writel(intmask & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE),
			host->ioaddr + SDHCI_INT_STATUS);
		tasklet_schedule(&host->card_tasklet);
		printk(KERN_INFO "SD Card %s!\n", (intmask & SDHCI_INT_CARD_INSERT)?"Insert":"Remove");

		//caching SD card INSERT and REMOVE states, providing info to higher applications by using proc entry
		if(intmask & SDHCI_INT_CARD_INSERT)	
		    g_sd_status=0;
		else
		    g_sd_status=-1;

	}

	intmask &= ~(SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE);

	if (intmask & SDHCI_INT_CMD_MASK) {
		writel(intmask & SDHCI_INT_CMD_MASK,
			host->ioaddr + SDHCI_INT_STATUS);
		sdhci_cmd_irq(host, intmask & SDHCI_INT_CMD_MASK);
	}

	if (intmask & SDHCI_INT_DATA_MASK) {
		writel(intmask & SDHCI_INT_DATA_MASK,
			host->ioaddr + SDHCI_INT_STATUS);
		sdhci_data_irq(host, intmask & SDHCI_INT_DATA_MASK);
	}

	intmask &= ~(SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK);

	intmask &= ~SDHCI_INT_ERROR;

	if (intmask & SDHCI_INT_BUS_POWER) {
		printk(KERN_ERR "%s: Card is consuming too much power!\n",
			mmc_hostname(host->mmc));
		writel(SDHCI_INT_BUS_POWER, host->ioaddr + SDHCI_INT_STATUS);
	}

	intmask &= ~SDHCI_INT_BUS_POWER;

	if (intmask & SDHCI_INT_CARD_INT)
		cardint = 1;

	intmask &= ~SDHCI_INT_CARD_INT;

	if (intmask) {
		printk(KERN_ERR "%s: Unexpected interrupt 0x%08x.\n",
			mmc_hostname(host->mmc), intmask);
		sdhci_dumpregs(host);

		writel(intmask, host->ioaddr + SDHCI_INT_STATUS);
	}

	result = IRQ_HANDLED;

	mmiowb();
out:
	spin_unlock(&host->lock);

	/*
	 * We have to delay this as it calls back into the driver.
	 */
	if (cardint)
		mmc_signal_sdio_irq(host->mmc);

	return result;
}

/*****************************************************************************\
 *                                                                           *
 * Suspend/resume                                                            *
 *                                                                           *
\*****************************************************************************/

#ifdef CONFIG_PM

int sdhci_suspend_host(struct sdhci_host *host, pm_message_t state)
{
	int ret;

	ret = mmc_suspend_host(host->mmc, state);
	if (ret)
		return ret;

	free_irq(host->irq, host);

	return 0;
}

EXPORT_SYMBOL_GPL(sdhci_suspend_host);

int sdhci_resume_host(struct sdhci_host *host)
{
	int ret;

	if (host->flags & SDHCI_USE_DMA) {
		#if 0
		if (host->ops->enable_dma)
			host->ops->enable_dma(host);
		#endif
	}

	ret = request_irq(host->irq, sdhci_irq, IRQF_SHARED,
			  mmc_hostname(host->mmc), host);
	if (ret)
		return ret;

	sdhci_init(host);
	mmiowb();

	ret = mmc_resume_host(host->mmc);
	if (ret)
		return ret;

	return 0;
}

EXPORT_SYMBOL_GPL(sdhci_resume_host);

#endif /* CONFIG_PM */

/*****************************************************************************\
 *                                                                           *
 * Device allocation/registration                                            *
 *                                                                           *
\*****************************************************************************/

struct sdhci_host *sdhci_alloc_host(struct device *dev,
	size_t priv_size)
{
	struct mmc_host *mmc;
	struct sdhci_host *host;

	WARN_ON(dev == NULL);
	
	mmc = mmc_alloc_host(sizeof(struct sdhci_host) + priv_size, dev);
	if (!mmc)
		return ERR_PTR(-ENOMEM);

	host = mmc_priv(mmc);
	host->mmc = mmc;

	return host;
}

EXPORT_SYMBOL_GPL(sdhci_alloc_host);

static int sdhci_ast_hw_init(struct sdhci_host *host)
{

	if ((ast_scu_sdhc1_enabled == 1) && (ast_sdhc_port1_enabled == 0)) {
		host->ioaddr = ioremap(ast_sdhci_device.resource[0].start, SZ_4K);
		host->irq = ast_sdhci_device.resource[1].start;
		host->hw_name = ast_sdhci_device.name;
		ast_sdhc_port1_enabled = 1;
	}
	else if ((ast_scu_sdhc2_enabled == 1) && (ast_sdhc_port2_enabled == 0)){
		host->ioaddr = ioremap(ast_sdhci_device2.resource[0].start, SZ_4K);
		host->irq = ast_sdhci_device2.resource[1].start;
		host->hw_name = ast_sdhci_device2.name;
		ast_sdhc_port2_enabled = 1;
	} 
	if ((ast_sdhc_port1_enabled == 0) && (ast_sdhc_port2_enabled == 0)) {
		printk (KERN_ERR "Both hardware SD ports can not be enabled now\n");
		return -ENODEV;
	}

	return 0;
}

int sdhci_add_host(struct sdhci_host *host)
{
	struct mmc_host *mmc;
	unsigned int caps;
	int ret;

	WARN_ON(host == NULL);
	if (host == NULL)
		return -EINVAL;
		
	ret = sdhci_ast_hw_init(host);
	if(ret)
		return -EINVAL;

	mmc = host->mmc;

	if (debug_quirks)
		host->quirks = debug_quirks;

	sdhci_reset(host, SDHCI_RESET_ALL);

	host->version = readw(host->ioaddr + SDHCI_HOST_VERSION);
	host->version = (host->version & SDHCI_SPEC_VER_MASK)
				>> SDHCI_SPEC_VER_SHIFT;
	if (host->version > SDHCI_SPEC_200) {
		printk(KERN_ERR "%s: Unknown controller version (%d). "
			"You may experience problems.\n", mmc_hostname(mmc),
			host->version);
	}

	caps = readl(host->ioaddr + SDHCI_CAPABILITIES);

	if (host->quirks & SDHCI_QUIRK_FORCE_DMA)
		host->flags |= SDHCI_USE_DMA;
	else if (!(caps & SDHCI_CAN_DO_DMA))
		DBG("Controller doesn't have DMA capability\n");
	else
		host->flags |= SDHCI_USE_DMA;

	if ((host->quirks & SDHCI_QUIRK_BROKEN_DMA) &&
		(host->flags & SDHCI_USE_DMA)) {
		DBG("Disabling DMA as it is marked broken\n");
		host->flags &= ~SDHCI_USE_DMA;
	}

	if (host->flags & SDHCI_USE_DMA) {
		if ((host->version >= SDHCI_SPEC_200) &&
				(caps & SDHCI_CAN_DO_ADMA2))
			host->flags |= SDHCI_USE_ADMA;
	}

	if ((host->quirks & SDHCI_QUIRK_BROKEN_ADMA) &&
		(host->flags & SDHCI_USE_ADMA)) {
		DBG("Disabling ADMA as it is marked broken\n");
		host->flags &= ~SDHCI_USE_ADMA;
	}

	if (host->flags & SDHCI_USE_DMA) {
		#if 0
		if (host->ops->enable_dma) {
			if (host->ops->enable_dma(host)) {
				printk(KERN_WARNING "%s: No suitable DMA "
					"available. Falling back to PIO.\n",
					mmc_hostname(mmc));
				host->flags &= ~(SDHCI_USE_DMA | SDHCI_USE_ADMA);
			}
		}
		#endif
	}

	if (host->flags & SDHCI_USE_ADMA) {
		/*
		 * We need to allocate descriptors for all sg entries
		 * (128) and potentially one alignment transfer for
		 * each of those entries.
		 */
		host->adma_desc = kmalloc((128 * 2 + 1) * 4, GFP_KERNEL);
		host->align_buffer = kmalloc(128 * 4, GFP_KERNEL);
		if (!host->adma_desc || !host->align_buffer) {
			kfree(host->adma_desc);
			kfree(host->align_buffer);
			printk(KERN_WARNING "%s: Unable to allocate ADMA "
				"buffers. Falling back to standard DMA.\n",
				mmc_hostname(mmc));
			host->flags &= ~SDHCI_USE_ADMA;
		}
	}

	/*
	 * If we use DMA, then it's up to the caller to set the DMA
	 * mask, but PIO does not need the hw shim so we set a new
	 * mask here in that case.
	 */
	if (!(host->flags & SDHCI_USE_DMA)) {
		host->dma_mask = DMA_BIT_MASK(64);
		mmc_dev(host->mmc)->dma_mask = &host->dma_mask;
	}

	#if 0
	host->max_clk =
		(caps & SDHCI_CLOCK_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT;
	#endif
	//Because value 0 is not allowed in SDIO12C D[15:8]: Host Control Settings #1 Register, we have to increase the maximum
	//host's clock in case that system will not ask host to set 1 in the sdhci_set_clock() function
	//software should read SCU08 D[14:12] to get SDCLK = H-PLL / x
	host->max_clk = 100;
	if (host->max_clk == 0) {
		printk(KERN_ERR "%s: Hardware doesn't specify base clock "
			"frequency.\n", mmc_hostname(mmc));
		return -ENODEV;
	}
	host->max_clk *= 1000000;

	host->timeout_clk =
		(caps & SDHCI_TIMEOUT_CLK_MASK) >> SDHCI_TIMEOUT_CLK_SHIFT;
	if (host->timeout_clk == 0) {
		printk(KERN_ERR "%s: Hardware doesn't specify timeout clock "
			"frequency.\n", mmc_hostname(mmc));
		return -ENODEV;
	}
	if (caps & SDHCI_TIMEOUT_CLK_UNIT)
		host->timeout_clk *= 1000;

	/*
	 * Set host parameters.
	 */
	mmc->ops = &sdhci_ops;
	mmc->f_min = host->max_clk / 256;
	mmc->f_max = host->max_clk;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_SDIO_IRQ;

	if ((caps & SDHCI_CAN_DO_HISPD) ||
		(host->quirks & SDHCI_QUIRK_FORCE_HIGHSPEED))
		mmc->caps |= MMC_CAP_SD_HIGHSPEED;

	mmc->ocr_avail = 0;
	if (caps & SDHCI_CAN_VDD_330)
		mmc->ocr_avail |= MMC_VDD_32_33|MMC_VDD_33_34;
	if (caps & SDHCI_CAN_VDD_300)
		mmc->ocr_avail |= MMC_VDD_29_30|MMC_VDD_30_31;
	if (caps & SDHCI_CAN_VDD_180)
		mmc->ocr_avail |= MMC_VDD_165_195;

	if (mmc->ocr_avail == 0) {
		printk(KERN_ERR "%s: Hardware doesn't report any "
			"support voltages.\n", mmc_hostname(mmc));
		return -ENODEV;
	}

	spin_lock_init(&host->lock);

	/*
	 * Maximum number of segments. Depends on if the hardware
	 * can do scatter/gather or not.
	 */
	if (host->flags & SDHCI_USE_ADMA)
	{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
		mmc->max_segs = 128;
#else
		mmc->max_hw_segs = 128;
#endif
	}
	else if (host->flags & SDHCI_USE_DMA)
	{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
		mmc->max_segs = 1;
#else
		mmc->max_hw_segs = 1;
#endif
	}
	else /* PIO */
	{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,11)) 	
		mmc->max_segs = 128;
#else
		mmc->max_hw_segs = 128;
#endif
	}

#if (LINUX_VERSION_CODE <  KERNEL_VERSION(3,4,11)) 	
	mmc->max_phys_segs = 128;
#endif

	/*
	 * Maximum number of sectors in one transfer. Limited by DMA boundary
	 * size (512KiB).
	 */
	mmc->max_req_size = 524288;

	/*
	 * Maximum segment size. Could be one segment with the maximum number
	 * of bytes. When doing hardware scatter/gather, each entry cannot
	 * be larger than 64 KiB though.
	 */
	if (host->flags & SDHCI_USE_ADMA)
		mmc->max_seg_size = 65536;
	else
		mmc->max_seg_size = mmc->max_req_size;

	/*
	 * Maximum block size. This varies from controller to controller and
	 * is specified in the capabilities register.
	 */
	mmc->max_blk_size = (caps & SDHCI_MAX_BLOCK_MASK) >> SDHCI_MAX_BLOCK_SHIFT;
	if (mmc->max_blk_size >= 3) {
		printk(KERN_WARNING "%s: Invalid maximum block size, "
			"assuming 512 bytes\n", mmc_hostname(mmc));
		mmc->max_blk_size = 512;
	} else
		mmc->max_blk_size = 512 << mmc->max_blk_size;

	/*
	 * Maximum block count.
	 */
	mmc->max_blk_count = 65535;

	/*
	 * Init tasklets.
	 */
	tasklet_init(&host->card_tasklet,
		sdhci_tasklet_card, (unsigned long)host);
	tasklet_init(&host->finish_tasklet,
		sdhci_tasklet_finish, (unsigned long)host);

	setup_timer(&host->timer, sdhci_timeout_timer, (unsigned long)host);

	ret = request_irq(host->irq, sdhci_irq, IRQF_SHARED,
		mmc_hostname(mmc), host);
	if (ret)
		goto untasklet;

	sdhci_init(host);

	//Checking SD card present state 
	if((readl(host->ioaddr + SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT) == SDHCI_CARD_PRESENT)
		g_sd_status = 0;
	else
		g_sd_status = -1;

#ifdef CONFIG_MMC_DEBUG
	sdhci_dumpregs(host);
#endif

#ifdef CONFIG_LEDS_CLASS
	snprintf(host->led_name, sizeof(host->led_name),
		"%s::", mmc_hostname(mmc));
	host->led.name = host->led_name;
	host->led.brightness = LED_OFF;
	host->led.default_trigger = mmc_hostname(mmc);
	host->led.brightness_set = sdhci_led_control;

	ret = led_classdev_register(mmc_dev(mmc), &host->led);
	if (ret)
		goto reset;
#endif

	mmiowb();

	mmc_add_host(mmc);

	printk(KERN_INFO "%s: SDHCI controller on %s [%s] using %s%s\n",
		mmc_hostname(mmc), host->hw_name, dev_name(mmc_dev(mmc)),
		(host->flags & SDHCI_USE_ADMA)?"A":"",
		(host->flags & SDHCI_USE_DMA)?"DMA":"PIO");

	return 0;

#ifdef CONFIG_LEDS_CLASS
reset:
	sdhci_reset(host, SDHCI_RESET_ALL);
	free_irq(host->irq, host);
#endif
untasklet:
	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);

	return ret;
}

EXPORT_SYMBOL_GPL(sdhci_add_host);

void sdhci_remove_host(struct sdhci_host *host, int dead)
{
	unsigned long flags;

	if (dead) {
		spin_lock_irqsave(&host->lock, flags);

		host->flags |= SDHCI_DEVICE_DEAD;

		if (host->mrq) {
			printk(KERN_ERR "%s: Controller removed during "
				" transfer!\n", mmc_hostname(host->mmc));

			host->mrq->cmd->error = -ENOMEDIUM;
			tasklet_schedule(&host->finish_tasklet);
		}

		spin_unlock_irqrestore(&host->lock, flags);
	}

	mmc_remove_host(host->mmc);

#ifdef CONFIG_LEDS_CLASS
	led_classdev_unregister(&host->led);
#endif

	if (!dead)
		sdhci_reset(host, SDHCI_RESET_ALL);

	free_irq(host->irq, host);

	del_timer_sync(&host->timer);

	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);

	kfree(host->adma_desc);
	kfree(host->align_buffer);

	host->adma_desc = NULL;
	host->align_buffer = NULL;
}

EXPORT_SYMBOL_GPL(sdhci_remove_host);

void sdhci_free_host(struct sdhci_host *host)
{
	mmc_free_host(host->mmc);
}

EXPORT_SYMBOL_GPL(sdhci_free_host);

/*****************************************************************************\
 *                                                                           *
 * Driver init/exit                                                          *
 *                                                                           *
\*****************************************************************************/

static int __devexit sdhci_remove(struct platform_device *pdev)
{
	struct mmc_host		*mmc = platform_get_drvdata(pdev);
	struct sdhci_host *host;
	//u32 scratch;
	//int dead;

	if(mmc) {
		host = mmc_priv(mmc);

	readl(host->ioaddr + SDHCI_INT_STATUS);
	//	scratch = readl(host->ioaddr + SDHCI_INT_STATUS);
	//	if (scratch == (u32)-1)
	//		dead = 1;

		sdhci_remove_host(host, 1);
		iounmap(host->ioaddr);
        sdhci_free_host(host);
		platform_set_drvdata(pdev, NULL);
	}
	return 0;
}


static int __devinit sdhci_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	int ret;

	host = sdhci_alloc_host(&pdev->dev, sizeof(struct sdhci_host));
	platform_set_drvdata(pdev, host->mmc);
	ret = sdhci_add_host(host);

	return ret;
}


static struct platform_driver ast_sdhci_driver = {
	.driver.name = "ast_sdhci",
	.driver.owner = THIS_MODULE,
	.probe = sdhci_probe,
	.remove = __devexit_p(sdhci_remove),
	#ifdef CONFIG_PM
	.resume = sdhci_resume_host,
	.suspend = sdhci_suspend_host,
	#endif
};

static struct platform_driver ast_sdhci_driver2 = {
	.driver.name = "ast_sdhci2",
	.driver.owner = THIS_MODULE,
	.probe = sdhci_probe,
	.remove = __devexit_p(sdhci_remove),
	#ifdef CONFIG_PM
	.resume = sdhci_resume_host,
	.suspend = sdhci_suspend_host,
	#endif
};


static int __init sdhci_drv_init(void)
{
	uint32_t reg;

	printk(KERN_INFO DRIVER_NAME
		": Secure Digital Host Controller Interface driver\n");
	printk(KERN_INFO DRIVER_NAME ": Copyright(c) Pierre Ossman\n");
	
	
    iowrite32(SCU_UNLOCK_MAGIC, SCU_KEY_CONTROL_REG); /* unlock SCU */

    /* enable reset of SDHC */
	reg = ioread32(SCU_SYS_RESET_REG);
	reg |= SD_RESET_BIT;
	iowrite32(reg, SCU_SYS_RESET_REG);
	mdelay(10);

	/* enable SDHC clock */
	reg = ioread32(SCU_CLK_STOP_REG);
	reg &= ~SD_CLOCK_BIT;
	iowrite32(reg, SCU_CLK_STOP_REG);
	mdelay(10);

	/* set clock of SDHC */
	reg = ioread32(SCU_CLK_SELECT_REG);
	reg |= SD_CLOCK_RUN;
	iowrite32(reg, SCU_CLK_SELECT_REG);
	mdelay(10);

	reg = ioread32(SCU_CLK_SELECT_REG);
	reg &= ~SD_CLOCK_DIVIDER_MASK;
	reg |= (SD_CLOCK_DIVIDER_SELECTION << 12);
	iowrite32(reg, SCU_CLK_SELECT_REG);
	mdelay(10);

	/* disable reset of SDHC */
	reg = ioread32(SCU_SYS_RESET_REG);
	reg &= ~SD_RESET_BIT;
	iowrite32(reg, SCU_SYS_RESET_REG);

	iowrite32(0, SCU_KEY_CONTROL_REG); /* lock SCU */

    /* Both ports's capabilities are 0, software needs to reset SDIO */
	if ((ioread32(AST_SDHC_VA_BASE + AST_SDHC1_CAP) == 0) && (ioread32(AST_SDHC_VA_BASE + AST_SDHC2_CAP) == 0)) {	
		reg = ioread32(AST_SDHC_VA_BASE + AST_SDHC_INFO);
		reg |= SDIO_ALL_SOFTWARE_RESET;
		iowrite32(reg, AST_SDHC_VA_BASE + AST_SDHC_INFO);

		barrier();

		do {
			reg = ioread32(AST_SDHC_VA_BASE + AST_SDHC_INFO);
		} while (reg & SDIO_ALL_SOFTWARE_RESET);
	}
	iowrite32(0x1000, AST_SDHC_VA_BASE + AST_SDHC_DEBONUCE);

        
    /* Read SCU90 to check if SD is enabled */
	if ((ast_scu_sdhc1_enabled == 0) && (ast_scu_sdhc2_enabled == 0)) {
		reg = ioread32(AST_SCU_VA_BASE + 0x90);

		if (reg & AST_SCU_MULTI_PIN_SDHC1)
			ast_scu_sdhc1_enabled = 1;
		if (reg & AST_SCU_MULTI_PIN_SDHC2)
			ast_scu_sdhc2_enabled = 1;		
	}
	barrier();
	
	// Add Proc Entry for SDHC1 alone. Adding Proc Entry for SDHC2 will overwrite SD available status of SDHC1.
	if ((ast_scu_sdhc1_enabled == 1)) {
		moduledir = proc_mkdir("SD_STATUS",NULL);
		sdinfo_proc = AddProcEntry(moduledir,"sdInfo",sdinfo_read,NULL,NULL);
	}
	if (ast_scu_sdhc1_enabled == 1) {
		platform_device_register(&ast_sdhci_device);
		platform_driver_register(&ast_sdhci_driver);
	}

	if (ast_scu_sdhc2_enabled == 1) {
		platform_device_register(&ast_sdhci_device2);
		platform_driver_register(&ast_sdhci_driver2);
	}

	return 0;
}

static void __exit sdhci_drv_exit(void)
{
	if (ast_scu_sdhc1_enabled == 1) {
		platform_device_unregister(&ast_sdhci_device);
		platform_driver_unregister(&ast_sdhci_driver);
	}
	if (ast_scu_sdhc2_enabled == 1) {
		platform_device_unregister(&ast_sdhci_device2);
		platform_driver_unregister(&ast_sdhci_driver2);
	}
}

module_init(sdhci_drv_init);
module_exit(sdhci_drv_exit);

module_param(debug_quirks, uint, 0444);

MODULE_AUTHOR("Pierre Ossman <drzeus@drzeus.cx>");
MODULE_DESCRIPTION("Secure Digital Host Controller Interface core driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(debug_quirks, "Force certain quirks.");
