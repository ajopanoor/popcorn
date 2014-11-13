/*
 * Dummy Remote Processor control driver
 *
 * Copyright (C) 2014 Huawei Technologies
 *
 * Author: Paul Mundt <paul.mundt@huawei.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/remoteproc.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/gfp.h>
#include <linux/smp.h>
#include <linux/pci.h>
#include <asm/bootparam.h>
#include <asm/setup.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include "dummy_proc.h"

extern struct boot_params boot_params;

char cmdline_override[COMMAND_LINE_SIZE];
module_param(cmdline_override, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(cmdline_override, "kernel boot paramters to pass to the second cpu, leave blank to auto-generate");

int boot_cpu = 1;
module_param(boot_cpu, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(boot_cpu, "cpu number to boot the firmware on");

char *pci_devices_handover = "";
module_param(pci_devices_handover, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(pci_devices_handover, "list of pci devices to disconnect/reconnect to the kernel, in the form of 0xVENDOR_ID_HEX:0xDEVICE_ID_HEX,...");

static void dummy_handle_pci_handover(struct rproc *rproc, char *cmdline)
{
	char pdev_desc[14], pdev_blacklist[16*10] = "pci_dev_flags=";
	struct pci_dev *pdev = NULL, *tmp;
	int c = 0;

	pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, NULL);

	while (pdev) {
		sprintf(pdev_desc, "0x%04x:0x%04x", pdev->vendor, pdev->device);
		dev_dbg(&rproc->dev, "found %x:%x pci (searching for %s)\n", pdev->vendor, pdev->device, pdev_desc);
		if (strstr(pci_devices_handover, pdev_desc)) {
			dev_info(&rproc->dev, "Found, disabling...\n");
			tmp = pdev;
			pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev);
			disable_irq(tmp->irq);
			pci_remove_bus_device(tmp);
			continue;
		} else {
			dev_dbg(&rproc->dev, "not found, blacklisting in the new kernel\n");
			sprintf(pdev_blacklist + strlen(pdev_blacklist), "%c%s:b",
				c ? ',' : pdev_desc[0], pdev_desc + (c ? 0 : 1));
			c++;
		}

		pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev);
	}

	if (c)
		sprintf(cmdline + strlen(cmdline), " %s", pdev_blacklist);
}

static int dummy_rproc_start(struct rproc *rproc)
{
	void *kernel_start_address = rproc->bootaddr, *initrd_dma;
	dma_addr_t dma_bp, dma_str, dma_initrd;
	const struct firmware *initrd;
	int apicid, ret = -ENOMEM;
	struct boot_params *bp;
	char *cmdline_str;

	dev_notice(&rproc->dev, "Powering up processor %d, params \"%s\"\n",
		   boot_cpu, cmdline_override);

	apicid = apic->cpu_present_to_apicid(boot_cpu);

	if (apicid != BAD_APICID) {
		dev_err(&rproc->dev, "The CPU%d is used by this kernel, apicid = %d\n",
			boot_cpu, apicid);
		return -ENOEXEC;
	}

	apicid = per_cpu(x86_bios_cpu_apicid, boot_cpu);

	bp = dma_alloc_coherent(rproc->dev.parent, sizeof(*bp), &dma_bp, GFP_KERNEL);
	if (!bp) {
		dev_err(&rproc->dev, "can't allocate cma for boot_params\n");
		return -ENOMEM;
	}

	memcpy(bp, &boot_params, sizeof(*bp));

	if (memcmp(&bp->hdr.header, "HdrS", 4) != 0) {
		dev_err(&rproc->dev, "struct boot_params is broken.\n");
		goto free_bp;
	}

	if (!*cmdline_override) {
		sprintf(cmdline_override, "console=ttyS1,115200n8 earlyprintk=ttyS1,115200n8 memblock=debug acpi_irq_nobalance lapic_timer=1000000 mklinux debug memmap=640K@0 cma=0@0 present_mask=%d memmap=0x2e90000$640K memmap=0xB0340000$0x4e800000 memmap=4G$0xfebf0000 memmap=500M@0x2f400000 apic=debug",
			1 << (boot_cpu - 1));
		dummy_handle_pci_handover(rproc, cmdline_override);
	}

	cmdline_str = dma_alloc_coherent(rproc->dev.parent, strlen(cmdline_override),
					 &dma_str, GFP_KERNEL);
	if (!cmdline_str) {
		dev_err(&rproc->dev, "can't allocate cma for cmdline\n");
		goto free_bp;
	}

	strcpy(cmdline_str, cmdline_override);
	bp->hdr.cmd_line_ptr = __pa(cmdline_str);

	ret = request_firmware(&initrd, "initrd", &rproc->dev);

	if (ret < 0) {
		dev_err(&rproc->dev, "request_firmware failed: %d\n", ret);
		goto free_str;
	}

	initrd_dma = dma_alloc_coherent(rproc->dev.parent, initrd->size,
					&dma_initrd, GFP_KERNEL);

	if (!initrd_dma) {
		dev_err(&rproc->dev, "failed to allocate cma for fw size %zd\n",
			initrd->size);
		ret = -ENOMEM;
		goto free_fw;
	}

	memcpy(initrd_dma, initrd->data, initrd->size);

	bp->hdr.ramdisk_image = __pa(initrd_dma);
	bp->hdr.ramdisk_size = initrd->size;
	bp->hdr.ramdisk_magic = 0;

	dev_info(&rproc->dev, "PAs:\n\tinitrd\t\t0x%p-0x%p (%db)\n\tcmdline\t\t0x%p-0x%p (%db)\n\tboot_params\t0x%p-0x%p (%db)\n\tkernel\t\t0x%p (fw name \"%s\")\n\tCmdline:\n\t\t%s\n\n",
		 dma_initrd, dma_initrd + initrd->size, initrd->size, dma_str,
		 dma_str + strlen(cmdline_override), strlen(cmdline_override),
		 dma_bp, dma_bp + sizeof(*bp), sizeof(*bp), kernel_start_address,
		 rproc->firmware, cmdline_str);

	release_firmware(initrd);

	ret = mkbsp_boot_cpu(apicid, boot_cpu, kernel_start_address, bp);

	if (ret)
		goto free_str;

	/* give it some time to boot, as we need to be able to send IPIs
	 * after we exit.
	 */
	msleep(2000);

	return 0;

free_fw:
	release_firmware(initrd);

free_str:
	dma_free_coherent(rproc->dev.parent, strlen(cmdline_override),
			  cmdline_str, dma_str);

free_bp:
	dma_free_coherent(rproc->dev.parent, sizeof(*bp), bp, dma_bp);

	return ret;
}

static int dummy_rproc_stop(struct rproc *rproc)
{
	dev_notice(&rproc->dev, "Powering off remote processor\n");
	return 0;
}

static void dummy_rproc_kick(struct rproc *rproc, int vqid)
{
	dev_notice(&rproc->dev, "Kicking virtqueue id #%d via apic %s\n", vqid, apic->name);
	apic->send_IPI_single(boot_cpu, DUMMY_LPROC_VECTOR);
}

static struct rproc_ops dummy_rproc_ops = {
	.start		= dummy_rproc_start,
	.stop		= dummy_rproc_stop,
	.kick		= dummy_rproc_kick,
};

static int dummy_rproc_probe(struct platform_device *pdev)
{
	struct rproc *rproc;
	int ret;

	rproc = rproc_alloc(&pdev->dev, DRV_NAME, &dummy_rproc_ops, NULL, 0);
	if (!rproc)
		return -ENOMEM;

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	platform_set_drvdata(pdev, rproc);

	ret = rproc_add(rproc);
	if (unlikely(ret))
		goto err;

	return 0;

err:
	rproc_put(rproc);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int dummy_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);
	rproc_put(rproc);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver dummy_rproc_driver = {
	.probe	= dummy_rproc_probe,
	.remove	= dummy_rproc_remove,
	.driver = {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
	},
};

static struct platform_device *dummy_rproc_device;

static int __init dummy_rproc_init(void)
{
	int ret = 0;

	/*
	 * Only support one dummy device for testing
	 */
	if (unlikely(dummy_rproc_device))
		return -EEXIST;

	ret = platform_driver_register(&dummy_rproc_driver);
	if (unlikely(ret))
		return ret;


	dummy_rproc_device = platform_device_register_simple(DRV_NAME, 0,
							     NULL, 0);
	if (IS_ERR(dummy_rproc_device)) {
		platform_driver_unregister(&dummy_rproc_driver);
		ret = PTR_ERR(dummy_rproc_device);
	}

	return ret;
}

static void __exit dummy_rproc_exit(void)
{
	platform_device_unregister(dummy_rproc_device);
	platform_driver_unregister(&dummy_rproc_driver);
}

module_init(dummy_rproc_init);
module_exit(dummy_rproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Dummy Remote Processor control driver");
