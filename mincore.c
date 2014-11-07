/*
 * Copyright (C) 2013-2014 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include "stress-ng.h"

long int page_size;

/*
 * madvise_random()
 *	apply random madvise setting to a memory region
 */
int mincore_touch_pages(void *buf, size_t buf_len)
{
#if _BSD_SOURCE || _SVID_SOURCE
	unsigned char *vec;
	char *buffer;
	size_t vec_len, i;

	if (!(opt_flags & OPT_FLAGS_MMAP_MINCORE))
		return 0;

	if (!page_size)
		page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		return -1;

	vec_len = buf_len / page_size;
	if (vec_len < 1)
		return -1;

	vec = calloc(vec_len, 1);
	if (!vec)
		return -1;

	if (mincore(buf, buf_len, vec) < 0) {
		free(vec);
		return -1;
	}

	/* If page is not resident in memory, touch it */
	for (buffer = buf, i = 0; i < vec_len; i++, buffer += page_size)
		if (!(vec[i] & 1))
			(*buffer)++;

	/* And return it back */
	for (buffer = buf, i = 0; i < vec_len; i++, buffer += page_size)
		if (!(vec[i] & 1))
			(*buffer)--;

	free(vec);
#endif
	return 0;
}