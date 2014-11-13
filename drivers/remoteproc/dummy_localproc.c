/*
 * Dummy Remote Processor resource table
 *
 * Copyright (C) 2014 Huawei Technologies
 *
 * Author: Veaceslav Falico <veaceslav.falico@huawei.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/remoteproc.h>
#include <linux/virtio_ids.h>
#include <asm/cacheflush.h>
#include <linux/memblock.h>
#include <asm/desc.h>
#include <asm/hw_irq.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>

#include "dummy_proc.h"

struct dummy_rproc_resourcetable {
	struct resource_table		main_hdr;
	u32				offset[2];
	/* We'd need some physical mem */
	struct fw_rsc_hdr		rsc_hdr_mem;
	struct fw_rsc_carveout		rsc_mem;
	/* And some rpmsg rings */
	struct fw_rsc_hdr		rsc_hdr_vdev;
	struct fw_rsc_vdev		rsc_vdev;
	struct fw_rsc_vdev_vring	rsc_ring0;
	struct fw_rsc_vdev_vring	rsc_ring1;
};

struct dummy_rproc_resourcetable dummy_remoteproc_resourcetable
	__attribute__((section(".resource_table"), aligned(PAGE_SIZE))) =
{
	.main_hdr = {
		.ver =		1,			/* version */
		.num =		2,			/* we have 2 entries - mem and rpmsg */
		.reserved =	{ 0, 0 },		/* reserved - must be 0 */
	},
	.offset = {					/* offsets to our resource entries */
		offsetof(struct dummy_rproc_resourcetable, rsc_hdr_mem),
		offsetof(struct dummy_rproc_resourcetable, rsc_hdr_vdev),
	},
	.rsc_hdr_mem = {
		.type =		RSC_CARVEOUT,		/* mem resource */
	},
	.rsc_mem = {
		.da =		CONFIG_PHYSICAL_START,	/* we don't care about the dev address */
		.pa =		CONFIG_PHYSICAL_START,	/* we actually need to be here */
		.len =		VMLINUX_FIRMWARE_SIZE,	/* size please */
		.flags =	0,			/* TODO flags */
		.reserved =	0,			/* reserved - 0 */
		.name =		"dummy-rproc-mem",
	},
	.rsc_hdr_vdev = {
		.type =		RSC_VDEV,		/* vdev resource */
	},
	.rsc_vdev = {
		.id =		VIRTIO_ID_RPMSG,	/* found in virtio_ids.h */
		.notifyid =	0,			/* magic number for IPC */
		.dfeatures =	0,			/* features - none (??) */
		.gfeatures =	0,			/* negotiated features - blank */
		.config_len =	0,			/* config len - none (??) */
		.status =	0,			/* status - updated by bsp */
		.num_of_vrings=	2,			/* we have 2 rings */
		.reserved =	{ 0, 0},		/* reserved */
	},
	.rsc_ring0 = {
		.da =		0,			/* we don't (??) care about the da */
		.align =	PAGE_SIZE,		/* alignment */
		.num =		512,			/* number of buffers */
		.notifyid =	0,			/* magic number for IPC */
		.reserved =	0,			/* reserved - 0 */
	},
	.rsc_ring1 = {
		.da =		0,			/* we don't (??) care about the da */
		.align =	PAGE_SIZE,		/* alignment */
		.num =		512,			/* number of buffers */
		.notifyid =	0,			/* magic number for IPC */
		.reserved =	0,			/* reserved - 0 */
	},
};

struct dummy_rproc_resourcetable *lproc = &dummy_remoteproc_resourcetable;
bool is_bsp = false;
unsigned char *x86_trampoline_bsp_base;

void dummy_lproc_kick_bsp()
{
	if (is_bsp)
		return;

	printk(KERN_INFO "Kicking BSP.\n");
	apic->send_IPI_single(0, DUMMY_RPROC_VECTOR);
}
late_initcall(dummy_lproc_kick_bsp);

void smp_dummy_lproc_kicked()
{
	ack_APIC_irq();
	irq_enter();

	printk(KERN_INFO "AP got kicked.\n");

	irq_exit();
}

/* this function should later find out the correct rproc who kicked us,
 * but, as we now have only one - we just find an rproc :)
 */
int dummy_rproc_match(struct device *dev, void *data)
{
	return (dev->driver && !strcmp(dev->driver->name, DRV_NAME));
}

void smp_dummy_rproc_kicked()
{
	struct device *dev;
	struct rproc *rproc = NULL;
	ack_APIC_irq();
	irq_enter();

	dev = device_find_child(&platform_bus, NULL, dummy_rproc_match);
	if (dev)
		rproc = dev_get_drvdata(dev);

	printk(KERN_INFO "BSP got kicked, rproc %s\n",
	       rproc ? rproc->name : "NOT FOUND");

	irq_exit();
}

void dummy_proc_setup_intr(void)
{
	if (!is_bsp) {
		alloc_intr_gate(DUMMY_LPROC_VECTOR, dummy_lproc_kicked);
		printk(KERN_INFO "Registered AP interrupt vector %d\n", DUMMY_LPROC_VECTOR);
	} else {
		alloc_intr_gate(DUMMY_RPROC_VECTOR, dummy_rproc_kicked);
		printk(KERN_INFO "Registered BSP interrupt vector %d\n", DUMMY_RPROC_VECTOR);
	}
}
pure_initcall(dummy_proc_setup_intr);

static int __init dummy_lproc_configure_trampoline()
{
	size_t size;

	if (!is_bsp)
		return 0;

	size = PAGE_ALIGN(x86_trampoline_bsp_end - x86_trampoline_bsp_start);

	set_memory_x((unsigned long)x86_trampoline_bsp_base, size >> PAGE_SHIFT);
	return 0;
}
arch_initcall(dummy_lproc_configure_trampoline);

static void __init dummy_lproc_setup_trampoline()
{
	phys_addr_t mem;
	size_t size = PAGE_ALIGN(x86_trampoline_bsp_end - x86_trampoline_bsp_start);

	/* Has to be in very low memory so we can execute real-mode AP code. */
	mem = memblock_find_in_range(0, 1<<20, size, PAGE_SIZE);
	if (mem == MEMBLOCK_ERROR)
		panic("Cannot allocate trampoline\n");

	x86_trampoline_bsp_base = __va(mem);
	memblock_x86_reserve_range(mem, mem + size, "TRAMPOLINE_BSP");

	printk(KERN_DEBUG "Base memory trampoline BSP at [%p] %llx size %zu\n",
	       x86_trampoline_bsp_base, (unsigned long long)mem, size);

	memcpy(x86_trampoline_bsp_base, x86_trampoline_bsp_start, size);
}

static int __init dummy_lproc_init(void)
{

	if (!lproc->rsc_ring0.da) {
		printk(KERN_INFO "%s: we're the BSP\n", __func__);
		is_bsp = true;

		dummy_lproc_setup_trampoline();

		return 0;
	}

	printk(KERN_INFO "%s: We're the AP, vring0 pa 0x%p vring1 pa 0x%p\n",
	       __func__, lproc->rsc_ring0.da, lproc->rsc_ring1.da);

	return 0;

}
early_initcall(dummy_lproc_init);
