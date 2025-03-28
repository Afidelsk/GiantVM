/*
 * Support for RAM backed by mmaped host memory.
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/mmap-alloc.h"

#define HUGETLBFS_MAGIC       0x958458f6

#ifdef CONFIG_LINUX
#include <sys/vfs.h>
#include <numa.h>
#endif

int qemu_ram_mmap_populate = 0;

size_t qemu_fd_getpagesize(int fd)
{
#ifdef CONFIG_LINUX
    struct statfs fs;
    int ret;

    if (fd != -1) {
        do {
            ret = fstatfs(fd, &fs);
        } while (ret != 0 && errno == EINTR);

        if (ret == 0 && fs.f_type == HUGETLBFS_MAGIC) {
            return fs.f_bsize;
        }
    }
#endif

    return getpagesize();
}

static void *__qemu_ram_mmap(int fd, size_t size, size_t align, bool shared, int phys_node)
{
    /*
     * Note: this always allocates at least one extra page of virtual address
     * space, even if size is already aligned.
     */
    size_t total = size + align;
#if defined(__powerpc64__) && defined(__linux__)
    /* On ppc64 mappings in the same segment (aka slice) must share the same
     * page size. Since we will be re-allocating part of this segment
     * from the supplied fd, we should make sure to use the same page size, to
     * this end we mmap the supplied fd.  In this case, set MAP_NORESERVE to
     * avoid allocating backing store memory.
     * We do this unless we are using the system page size, in which case
     * anonymous memory is OK.
     */
    int anonfd = fd == -1 || qemu_fd_getpagesize(fd) == getpagesize() ? -1 : fd;
    int flags = anonfd == -1 ? MAP_ANONYMOUS : MAP_NORESERVE;
    void *ptr = mmap(0, total, PROT_NONE, flags | MAP_PRIVATE, anonfd, 0);
#else
    void *ptr = mmap(0, total, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif
    size_t offset = QEMU_ALIGN_UP((uintptr_t)ptr, align) - (uintptr_t)ptr;
    void *ptr1;

     if (ptr == MAP_FAILED) {
        return MAP_FAILED;
    }

    /* Make sure align is a power of 2 */
    assert(!(align & (align - 1)));
    /* Always align to host page size */
    assert(align >= getpagesize());

    ptr1 = mmap(ptr + offset, size, PROT_READ | PROT_WRITE,
                MAP_FIXED |
                (fd == -1 ? MAP_ANONYMOUS : 0) |
                (shared ? MAP_SHARED : MAP_PRIVATE),
                fd, 0);
    if (ptr1 == MAP_FAILED) {
        munmap(ptr, total);
        return MAP_FAILED;
    }

#ifdef CONFIG_LINUX
	if (phys_node > numa_max_node()) {
		fprintf(stderr, "ERROR: failed to numa-aware allocation: phys_node(%d) > max_node(%d)\n",
						phys_node, numa_max_node());
	} else if (phys_node >= 0) {
		numa_tonode_memory(ptr, total, phys_node); 
	}
#endif
	if (qemu_ram_mmap_populate)
		memset(ptr1, 0, size);

    ptr += offset;
    total -= offset;

    if (offset > 0) {
        munmap(ptr - offset, offset);
    }

    /*
     * Leave a single PROT_NONE page allocated after the RAM block, to serve as
     * a guard page guarding against potential buffer overflows.
     */
    if (total > size + getpagesize()) {
        munmap(ptr + size + getpagesize(), total - size - getpagesize());
    }

    return ptr;
}

inline void *qemu_ram_mmap_numa(size_t size, size_t align, bool shared, int phys_node) {
	return __qemu_ram_mmap(-1, size, align, shared, phys_node);
}

inline void *qemu_ram_mmap(int fd, size_t size, size_t align, bool shared) {
	return __qemu_ram_mmap(fd, size, align, shared, -1);
}

void qemu_ram_munmap(void *ptr, size_t size)
{
    if (ptr) {
        /* Unmap both the RAM block and the guard page */
        munmap(ptr, size + getpagesize());
    }
}
