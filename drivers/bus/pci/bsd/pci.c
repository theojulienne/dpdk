/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/queue.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/pciio.h>
#include <dev/pci/pcireg.h>

#if defined(RTE_ARCH_X86)
#include <machine/cpufunc.h>
#endif

#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>
#include <rte_debug.h>
#include <rte_devargs.h>

#include <eal_export.h>
#include "eal_filesystem.h"
#include "private.h"

/**
 * @file
 * PCI probing under BSD.
 */

/* Map pci device */
RTE_EXPORT_SYMBOL(rte_pci_map_device)
int
rte_pci_map_device(struct rte_pci_device *dev)
{
	int ret = -1;

	/* try mapping the NIC resources */
	switch (dev->kdrv) {
	case RTE_PCI_KDRV_NIC_UIO:
		/* map resources for devices that use uio */
		ret = pci_uio_map_resource(dev);
		break;
	default:
		PCI_LOG(DEBUG, "  Not managed by a supported kernel driver, skipped");
		ret = 1;
		break;
	}

	return ret;
}

/* Unmap pci device */
RTE_EXPORT_SYMBOL(rte_pci_unmap_device)
void
rte_pci_unmap_device(struct rte_pci_device *dev)
{
	/* try unmapping the NIC resources */
	switch (dev->kdrv) {
	case RTE_PCI_KDRV_NIC_UIO:
		/* unmap resources for devices that use uio */
		pci_uio_unmap_resource(dev);
		break;
	default:
		PCI_LOG(DEBUG, "  Not managed by a supported kernel driver, skipped");
		break;
	}
}

void
pci_uio_free_resource(struct rte_pci_device *dev,
		struct mapped_pci_resource *uio_res)
{
	rte_free(uio_res);

	if (rte_intr_fd_get(dev->intr_handle)) {
		close(rte_intr_fd_get(dev->intr_handle));
		rte_intr_fd_set(dev->intr_handle, -1);
		rte_intr_type_set(dev->intr_handle, RTE_INTR_HANDLE_UNKNOWN);
	}
}

int
pci_uio_alloc_resource(struct rte_pci_device *dev,
		struct mapped_pci_resource **uio_res)
{
	char devname[PATH_MAX]; /* contains the /dev/uioX */
	struct rte_pci_addr *loc;
	int fd;

	loc = &dev->addr;

	snprintf(devname, sizeof(devname), "/dev/uio@pci:%u:%u:%u",
			dev->addr.bus, dev->addr.devid, dev->addr.function);

	fd = open(devname, O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT) {
			PCI_LOG(WARNING, PCI_PRI_FMT" not managed by UIO driver, skipping",
					loc->domain, loc->bus, loc->devid, loc->function);
			return 1;
		}
		PCI_LOG(ERR, "Failed to open device file for " PCI_PRI_FMT " (%s)",
				loc->domain, loc->bus, loc->devid, loc->function, devname);
		return -1;
	}

	/* save fd if in primary process */
	if (rte_intr_fd_set(dev->intr_handle, fd)) {
		PCI_LOG(WARNING, "Failed to save fd");
		goto error;
	}

	if (rte_intr_fd_get(dev->intr_handle) < 0) {
		PCI_LOG(ERR, "Cannot open %s: %s", devname, strerror(errno));
		goto error;
	}

	if (rte_intr_type_set(dev->intr_handle, RTE_INTR_HANDLE_UIO))
		goto error;

	/* allocate the mapping details for secondary processes*/
	*uio_res = rte_zmalloc("UIO_RES", sizeof(**uio_res), 0);
	if (*uio_res == NULL) {
		PCI_LOG(ERR, "%s(): cannot store uio mmap details", __func__);
		goto error;
	}

	strlcpy((*uio_res)->path, devname, sizeof((*uio_res)->path));
	memcpy(&(*uio_res)->pci_addr, &dev->addr, sizeof((*uio_res)->pci_addr));

	return 0;

error:
	pci_uio_free_resource(dev, *uio_res);
	return -1;
}

int
pci_uio_map_resource_by_index(struct rte_pci_device *dev, int res_idx,
		struct mapped_pci_resource *uio_res, int map_idx)
{
	int fd;
	char *devname;
	void *mapaddr;
	uint64_t offset;
	uint64_t pagesz;
	struct pci_map *maps;

	maps = uio_res->maps;
	devname = uio_res->path;
	pagesz = sysconf(_SC_PAGESIZE);

	/* allocate memory to keep path */
	maps[map_idx].path = rte_malloc(NULL, strlen(devname) + 1, 0);
	if (maps[map_idx].path == NULL) {
		PCI_LOG(ERR, "Cannot allocate memory for path: %s", strerror(errno));
		return -1;
	}

	/*
	 * open resource file, to mmap it
	 */
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		PCI_LOG(ERR, "Cannot open %s: %s", devname, strerror(errno));
		goto error;
	}

	/* if matching map is found, then use it */
	offset = res_idx * pagesz;

	/*
	 * Use baseaddr as a hint to avoid mapping resources
	 * where malloc(3) et al. usually make allocations.
	 * This reduces mapping conflicts in secondary processes
	 * that make memory allocations before initializing EAL.
	 */
	mapaddr = pci_map_resource((void *)rte_eal_get_baseaddr(),
			fd, (off_t)offset,
			(size_t)dev->mem_resource[res_idx].len, 0);
	close(fd);
	if (mapaddr == NULL)
		goto error;

	maps[map_idx].phaddr = dev->mem_resource[res_idx].phys_addr;
	maps[map_idx].size = dev->mem_resource[res_idx].len;
	maps[map_idx].addr = mapaddr;
	maps[map_idx].offset = offset;
	strcpy(maps[map_idx].path, devname);
	dev->mem_resource[res_idx].addr = mapaddr;

	return 0;

error:
	rte_free(maps[map_idx].path);
	return -1;
}

static int
pci_scan_one(int dev_pci_fd, struct pci_conf *conf)
{
	struct rte_pci_device_internal *pdev;
	struct rte_pci_device *dev;
	struct pci_bar_io bar;
	unsigned i, max;

	pdev = malloc(sizeof(*pdev));
	if (pdev == NULL) {
		PCI_LOG(ERR, "Cannot allocate memory for internal pci device");
		return -1;
	}

	memset(pdev, 0, sizeof(*pdev));
	dev = &pdev->device;
	dev->device.bus = &rte_pci_bus.bus;

	dev->addr.domain = conf->pc_sel.pc_domain;
	dev->addr.bus = conf->pc_sel.pc_bus;
	dev->addr.devid = conf->pc_sel.pc_dev;
	dev->addr.function = conf->pc_sel.pc_func;

	/* get vendor id */
	dev->id.vendor_id = conf->pc_vendor;

	/* get device id */
	dev->id.device_id = conf->pc_device;

	/* get subsystem_vendor id */
	dev->id.subsystem_vendor_id = conf->pc_subvendor;

	/* get subsystem_device id */
	dev->id.subsystem_device_id = conf->pc_subdevice;

	/* get class id */
	dev->id.class_id = (conf->pc_class << 16) |
			   (conf->pc_subclass << 8) |
			   (conf->pc_progif);

	/* TODO: get max_vfs */
	dev->max_vfs = 0;

	/* FreeBSD has no NUMA support (yet) */
	dev->device.numa_node = SOCKET_ID_ANY;

	pci_common_set(dev);

	/* FreeBSD has only one pass through driver */
	dev->kdrv = RTE_PCI_KDRV_NIC_UIO;

	/* parse resources */
	switch (conf->pc_hdr & PCIM_HDRTYPE) {
	case PCIM_HDRTYPE_NORMAL:
		max = PCIR_MAX_BAR_0;
		break;
	case PCIM_HDRTYPE_BRIDGE:
		max = PCIR_MAX_BAR_1;
		break;
	case PCIM_HDRTYPE_CARDBUS:
		max = PCIR_MAX_BAR_2;
		break;
	default:
		goto skipdev;
	}

	for (i = 0; i <= max; i++) {
		bar.pbi_sel = conf->pc_sel;
		bar.pbi_reg = PCIR_BAR(i);
		if (ioctl(dev_pci_fd, PCIOCGETBAR, &bar) < 0)
			continue;

		dev->mem_resource[i].len = bar.pbi_length;
		if (PCI_BAR_IO(bar.pbi_base)) {
			dev->mem_resource[i].addr = (void *)(bar.pbi_base & ~((uint64_t)0xf));
			continue;
		}
		dev->mem_resource[i].phys_addr = bar.pbi_base & ~((uint64_t)0xf);
	}

	/* device is valid, add in list (sorted) */
	if (TAILQ_EMPTY(&rte_pci_bus.device_list)) {
		rte_pci_add_device(dev);
	}
	else {
		struct rte_pci_device *dev2 = NULL;
		int ret;

		TAILQ_FOREACH(dev2, &rte_pci_bus.device_list, next) {
			ret = rte_pci_addr_cmp(&dev->addr, &dev2->addr);
			if (ret > 0)
				continue;
			else if (ret < 0) {
				rte_pci_insert_device(dev2, dev);
			} else { /* already registered */
				dev2->kdrv = dev->kdrv;
				dev2->max_vfs = dev->max_vfs;
				pci_common_set(dev2);
				memmove(dev2->mem_resource,
					dev->mem_resource,
					sizeof(dev->mem_resource));
				pci_free(pdev);
			}
			return 0;
		}
		rte_pci_add_device(dev);
	}

	return 0;

skipdev:
	pci_free(pdev);
	return 0;
}

/*
 * Scan the content of the PCI bus, and add the devices in the devices
 * list. Call pci_scan_one() for each pci entry found.
 */
int
rte_pci_scan(void)
{
	int fd;
	unsigned dev_count = 0;
	struct pci_conf matches[16];
	struct pci_conf_io conf_io = {
			.pat_buf_len = 0,
			.num_patterns = 0,
			.patterns = NULL,
			.match_buf_len = sizeof(matches),
			.matches = &matches[0],
	};
	struct rte_pci_addr pci_addr;

	/* for debug purposes, PCI can be disabled */
	if (!rte_eal_has_pci())
		return 0;

	fd = open("/dev/pci", O_RDONLY);
	if (fd < 0) {
		PCI_LOG(ERR, "%s(): error opening /dev/pci", __func__);
		goto error;
	}

	do {
		unsigned i;
		if (ioctl(fd, PCIOCGETCONF, &conf_io) < 0) {
			PCI_LOG(ERR, "%s(): error with ioctl on /dev/pci: %s",
				__func__, strerror(errno));
			goto error;
		}

		for (i = 0; i < conf_io.num_matches; i++) {
			pci_addr.domain = matches[i].pc_sel.pc_domain;
			pci_addr.bus = matches[i].pc_sel.pc_bus;
			pci_addr.devid = matches[i].pc_sel.pc_dev;
			pci_addr.function = matches[i].pc_sel.pc_func;

			if (rte_pci_ignore_device(&pci_addr))
				continue;

			if (pci_scan_one(fd, &matches[i]) < 0)
				goto error;
		}

		dev_count += conf_io.num_matches;
	} while(conf_io.status == PCI_GETCONF_MORE_DEVS);

	close(fd);

	PCI_LOG(DEBUG, "PCI scan found %u devices", dev_count);
	return 0;

error:
	if (fd >= 0)
		close(fd);
	return -1;
}

bool
pci_device_iommu_support_va(__rte_unused const struct rte_pci_device *dev)
{
	return false;
}

enum rte_iova_mode
pci_device_iova_mode(const struct rte_pci_driver *pdrv __rte_unused,
		     const struct rte_pci_device *pdev)
{
	if (pdev->kdrv != RTE_PCI_KDRV_NIC_UIO)
		PCI_LOG(DEBUG, "Unsupported kernel driver? Defaulting to IOVA as 'PA'");

	return RTE_IOVA_PA;
}

/* Read PCI config space. */
RTE_EXPORT_SYMBOL(rte_pci_read_config)
int rte_pci_read_config(const struct rte_pci_device *dev,
		void *buf, size_t len, off_t offset)
{
	int fd = -1;
	int size;
	/* Copy Linux implementation's behaviour */
	const int return_len = len;
	struct pci_io pi = {
		.pi_sel = {
			.pc_domain = dev->addr.domain,
			.pc_bus = dev->addr.bus,
			.pc_dev = dev->addr.devid,
			.pc_func = dev->addr.function,
		},
		.pi_reg = offset,
	};

	fd = open("/dev/pci", O_RDWR);
	if (fd < 0) {
		PCI_LOG(ERR, "%s(): error opening /dev/pci", __func__);
		goto error;
	}

	while (len > 0) {
		size = (len >= 4) ? 4 : ((len >= 2) ? 2 : 1);
		pi.pi_width = size;

		if (ioctl(fd, PCIOCREAD, &pi) < 0)
			goto error;
		memcpy(buf, &pi.pi_data, size);

		buf = (char *)buf + size;
		pi.pi_reg += size;
		len -= size;
	}
	close(fd);

	return return_len;

 error:
	if (fd >= 0)
		close(fd);
	return -1;
}

/* Write PCI config space. */
RTE_EXPORT_SYMBOL(rte_pci_write_config)
int rte_pci_write_config(const struct rte_pci_device *dev,
		const void *buf, size_t len, off_t offset)
{
	int fd = -1;

	struct pci_io pi = {
		.pi_sel = {
			.pc_domain = dev->addr.domain,
			.pc_bus = dev->addr.bus,
			.pc_dev = dev->addr.devid,
			.pc_func = dev->addr.function,
		},
		.pi_reg = offset,
		.pi_width = len,
	};

	if (len == 3 || len > sizeof(pi.pi_data)) {
		PCI_LOG(ERR, "%s(): invalid pci read length", __func__);
		goto error;
	}

	memcpy(&pi.pi_data, buf, RTE_MIN(len, sizeof(pi.pi_data)));

	fd = open("/dev/pci", O_RDWR);
	if (fd < 0) {
		PCI_LOG(ERR, "%s(): error opening /dev/pci", __func__);
		goto error;
	}

	if (ioctl(fd, PCIOCWRITE, &pi) < 0)
		goto error;

	close(fd);
	return 0;

 error:
	if (fd >= 0)
		close(fd);
	return -1;
}

/* Read PCI MMIO space. */
RTE_EXPORT_EXPERIMENTAL_SYMBOL(rte_pci_mmio_read, 23.07)
int rte_pci_mmio_read(const struct rte_pci_device *dev, int bar,
		      void *buf, size_t len, off_t offset)
{
	if (bar >= PCI_MAX_RESOURCE || dev->mem_resource[bar].addr == NULL ||
			(uint64_t)offset + len > dev->mem_resource[bar].len)
		return -1;
	memcpy(buf, (uint8_t *)dev->mem_resource[bar].addr + offset, len);
	return len;
}

/* Write PCI MMIO space. */
RTE_EXPORT_EXPERIMENTAL_SYMBOL(rte_pci_mmio_write, 23.07)
int rte_pci_mmio_write(const struct rte_pci_device *dev, int bar,
		       const void *buf, size_t len, off_t offset)
{
	if (bar >= PCI_MAX_RESOURCE || dev->mem_resource[bar].addr == NULL ||
			(uint64_t)offset + len > dev->mem_resource[bar].len)
		return -1;
	memcpy((uint8_t *)dev->mem_resource[bar].addr + offset, buf, len);
	return len;
}

RTE_EXPORT_SYMBOL(rte_pci_ioport_map)
int
rte_pci_ioport_map(struct rte_pci_device *dev, int bar,
		struct rte_pci_ioport *p)
{
	int ret;

	switch (dev->kdrv) {
#if defined(RTE_ARCH_X86)
	case RTE_PCI_KDRV_NIC_UIO:
		if (rte_eal_iopl_init() != 0) {
			PCI_LOG(ERR, "%s(): insufficient ioport permissions for PCI device %s",
				__func__, dev->name);
			return -1;
		}
		if ((uintptr_t) dev->mem_resource[bar].addr <= UINT16_MAX) {
			p->base = (uintptr_t)dev->mem_resource[bar].addr;
			ret = 0;
		} else
			ret = -1;
		break;
#endif
	default:
		ret = -1;
		break;
	}

	if (!ret)
		p->dev = dev;

	return ret;
}

static void
pci_uio_ioport_read(struct rte_pci_ioport *p,
		void *data, size_t len, off_t offset)
{
#if defined(RTE_ARCH_X86)
	uint8_t *d;
	int size;
	unsigned short reg = p->base + offset;

	for (d = data; len > 0; d += size, reg += size, len -= size) {
		if (len >= 4) {
			size = 4;
			*(uint32_t *)d = inl(reg);
		} else if (len >= 2) {
			size = 2;
			*(uint16_t *)d = inw(reg);
		} else {
			size = 1;
			*d = inb(reg);
		}
	}
#else
	RTE_SET_USED(p);
	RTE_SET_USED(data);
	RTE_SET_USED(len);
	RTE_SET_USED(offset);
#endif
}

RTE_EXPORT_SYMBOL(rte_pci_ioport_read)
void
rte_pci_ioport_read(struct rte_pci_ioport *p,
		void *data, size_t len, off_t offset)
{
	switch (p->dev->kdrv) {
	case RTE_PCI_KDRV_NIC_UIO:
		pci_uio_ioport_read(p, data, len, offset);
		break;
	default:
		break;
	}
}

static void
pci_uio_ioport_write(struct rte_pci_ioport *p,
		const void *data, size_t len, off_t offset)
{
#if defined(RTE_ARCH_X86)
	const uint8_t *s;
	int size;
	unsigned short reg = p->base + offset;

	for (s = data; len > 0; s += size, reg += size, len -= size) {
		if (len >= 4) {
			size = 4;
			outl(reg, *(const uint32_t *)s);
		} else if (len >= 2) {
			size = 2;
			outw(reg, *(const uint16_t *)s);
		} else {
			size = 1;
			outb(reg, *s);
		}
	}
#else
	RTE_SET_USED(p);
	RTE_SET_USED(data);
	RTE_SET_USED(len);
	RTE_SET_USED(offset);
#endif
}

RTE_EXPORT_SYMBOL(rte_pci_ioport_write)
void
rte_pci_ioport_write(struct rte_pci_ioport *p,
		const void *data, size_t len, off_t offset)
{
	switch (p->dev->kdrv) {
	case RTE_PCI_KDRV_NIC_UIO:
		pci_uio_ioport_write(p, data, len, offset);
		break;
	default:
		break;
	}
}

RTE_EXPORT_SYMBOL(rte_pci_ioport_unmap)
int
rte_pci_ioport_unmap(struct rte_pci_ioport *p)
{
	int ret;

	switch (p->dev->kdrv) {
#if defined(RTE_ARCH_X86)
	case RTE_PCI_KDRV_NIC_UIO:
		ret = 0;
		break;
#endif
	default:
		ret = -1;
		break;
	}

	return ret;
}
