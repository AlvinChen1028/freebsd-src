/*-
 * Copyright (c) 2022 Netflix, Inc
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/efi.h>
#include <machine/metadata.h>
#include <sys/linker.h>
#include <fdt_platform.h>
#include <libfdt.h>

#include "kboot.h"
#include "bootstrap.h"

/*
 * Info from dtb about the EFI system
 */
vm_paddr_t efi_systbl_phys;
struct efi_map_header *efi_map_hdr;
uint32_t efi_map_size;
vm_paddr_t efi_map_phys_src;	/* From DTB */
vm_paddr_t efi_map_phys_dst;	/* From our memory map metadata module */

static bool
do_memory_from_fdt(int fd)
{
	struct stat sb;
	char *buf = NULL;
	int len, offset;
	uint32_t sz, ver, esz, efisz;
	uint64_t mmap_pa;
	const uint32_t *u32p;
	const uint64_t *u64p;
	struct efi_map_header *efihdr;
	struct efi_md *map;

	if (fstat(fd, &sb) < 0)
		return false;
	buf = malloc(sb.st_size);
	if (buf == NULL)
		return false;
	len = read(fd, buf, sb.st_size);
	/* NB: we're reading this from sysfs, so mismatch OK */
	if (len <= 0)
		goto errout;

	/*
	 * Look for /chosen to find these values:
	 * linux,uefi-system-table	PA of the UEFI System Table.
	 * linux,uefi-mmap-start	PA of the UEFI memory map
	 * linux,uefi-mmap-size		Size of mmap
	 * linux,uefi-mmap-desc-size	Size of each entry of mmap
	 * linux,uefi-mmap-desc-ver	Format version, should be 1
	 */
	offset = fdt_path_offset(buf, "/chosen");
	if (offset <= 0)
		goto errout;
	u64p = fdt_getprop(buf, offset, "linux,uefi-system-table", &len);
	if (u64p == NULL)
		goto errout;
	efi_systbl_phys = fdt64_to_cpu(*u64p);
	u32p = fdt_getprop(buf, offset, "linux,uefi-mmap-desc-ver", &len);
	if (u32p == NULL)
		goto errout;
	ver = fdt32_to_cpu(*u32p);
	u32p = fdt_getprop(buf, offset, "linux,uefi-mmap-desc-size", &len);
	if (u32p == NULL)
		goto errout;
	esz = fdt32_to_cpu(*u32p);
	u32p = fdt_getprop(buf, offset, "linux,uefi-mmap-size", &len);
	if (u32p == NULL)
		goto errout;
	sz = fdt32_to_cpu(*u32p);
	u64p = fdt_getprop(buf, offset, "linux,uefi-mmap-start", &len);
	if (u64p == NULL)
		goto errout;
	mmap_pa = fdt64_to_cpu(*u64p);
	free(buf);

	printf("UEFI MMAP: Ver %d Ent Size %d Tot Size %d PA %#lx\n",
	    ver, esz, sz, mmap_pa);

	/*
	 * We have no ability to read the PA that this map is in, so
	 * pass the address to FreeBSD via a rather odd flag entry as
	 * the first map so early boot can copy the memory map into
	 * this space and have the rest of the code cope.
	 */
	efisz = (sizeof(*efihdr) + 0xf) & ~0xf;
	buf = malloc(sz + efisz);
	if (buf == NULL)
		return false;
	efihdr = (struct efi_map_header *)buf;
	map = (struct efi_md *)((uint8_t *)efihdr + efisz);
	bzero(map, sz);
	efihdr->memory_size = sz;
	efihdr->descriptor_size = esz;
	efihdr->descriptor_version = ver;
	efi_map_phys_src = mmap_pa;
	efi_map_hdr = efihdr;
	efi_map_size = sz + efisz;

	return true;
errout:
	free(buf);
	return false;
}

bool
enumerate_memory_arch(void)
{
	int fd = -1;
	bool rv = false;

	fd = open("host:/sys/firmware/fdt", O_RDONLY);
	if (fd != -1) {
		rv = do_memory_from_fdt(fd);
		close(fd);
		/*
		 * So, we have physaddr to the memory table. However, we can't
		 * open /dev/mem on some platforms to get the actual table. So
		 * we have to fall through to get it from /proc/iomem.
		 */
	}
	if (!rv) {
		printf("Could not obtain UEFI memory tables, expect failure\n");
	}

	populate_avail_from_iomem();

	print_avail();

	return true;
}

uint64_t
kboot_get_phys_load_segment(void)
{
#define HOLE_SIZE	(64ul << 20)
#define KERN_ALIGN	(2ul << 20)
	static uint64_t	s = 0;

	if (s != 0)
		return (s);

	s = first_avail(KERN_ALIGN, HOLE_SIZE, SYSTEM_RAM);
	if (s != 0)
		return (s);
	s = 0x40000000 | 0x4200000;	/* should never get here */
	printf("Falling back to crazy address %#lx\n", s);
	return (s);
}
