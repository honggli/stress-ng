/*
 * Copyright (C) 2013-2016 Canonical, Ltd.
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
#include "stress-ng.h"

#if defined(__linux__) &&		\
    defined(__NR_get_mempolicy) &&	\
    defined(__NR_mbind) &&		\
    defined(__NR_migrate_pages) &&	\
    defined(__NR_move_pages) &&		\
    defined(__NR_set_mempolicy)

#define NUMA_LONG_BITS		(sizeof(unsigned long) * 8)

#define MPOL_DEFAULT		(0)
#define MPOL_PREFERRED		(1)
#define MPOL_BIND		(2)
#define MPOL_INTERLEAVE		(3)

#define MPOL_F_NODE		(1 << 0)
#define MPOL_F_ADDR		(1 << 1)
#define MPOL_F_MEMS_ALLOWED	(1 << 2)

#define MPOL_MF_STRICT		(1 << 0)
#define MPOL_MF_MOVE		(1 << 1)
#define MPOL_MF_MOVE_ALL	(1 << 2)

#define SYS_NODE_PATH	"/sys/devices/system/node"
#define MMAP_SZ			(4 * MB)

typedef struct node {
	uint32_t	node_id;
	struct node	*next;
} node_t;

/*
 *  stress_numa_free_nodes()
 *	free circular list of node info
 */
static void stress_numa_free_nodes(node_t *nodes)
{
	node_t *n = nodes;

	while (n) {
		node_t *next = n->next;

		free(n);
		n = next;

		if (n == nodes)
			break;
	}
}

/*
 *  stress_numa_get_nodes(void)
 *	collect number of nodes, add them to a
 *	circular linked list
 */
static int stress_numa_get_nodes(node_t **node_ptr)
{
	DIR *dir;
	struct dirent *entry;
	unsigned long n = 0;
	node_t *tail = NULL;
	*node_ptr = NULL;

	dir = opendir(SYS_NODE_PATH);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		uint32_t node_id;
		node_t *node;

		if (strncmp(entry->d_name, "node", 4))
			continue;
		if (!isdigit(entry->d_name[4]))
			continue;
		if (sscanf(&entry->d_name[4], "%u10", &node_id) != 1)
			continue;

		node = calloc(1, sizeof(*node));
		if (!node) {
			break;
		}
		node->node_id = node_id;
		node->next = *node_ptr;
		*node_ptr = node;
		if (!tail)
			tail = node;
		tail->next = node;
		n++;
	}
	(void)closedir(dir);

	return n;
}

/*
 *  stress_numa()
 *	stress the Linux NUMA interfaces
 */
int stress_numa(
        uint64_t *const counter,
        const uint32_t instance,
        const uint64_t max_ops,
        const char *name)
{
	long numa_nodes;
	unsigned long max_nodes, nbits, lbits = 8 * sizeof(unsigned long);
	uint8_t *buf;
	const pid_t mypid = getpid();
	const unsigned long page_sz = stress_get_pagesize();
	const unsigned long num_pages = MMAP_SZ / page_sz;
	node_t *n;
	int rc = EXIT_FAILURE;

	(void)instance;

	numa_nodes = stress_numa_get_nodes(&n);
	if (numa_nodes <= 1) {
		pr_inf(stderr, "%s: multiple NUMA nodes not found, "
			"aborting test.\n", name);
		rc = EXIT_SUCCESS;
		goto numa_free;
	}
	nbits = (numa_nodes + lbits - 1) / lbits;
	max_nodes = nbits * lbits;

	/*
	 *  We need a buffer to migrate around NUMA nodes
	 */
	buf = mmap(NULL, MMAP_SZ, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (buf == MAP_FAILED) {
		rc = exit_status(errno);
		pr_fail(stderr, "%s: mmap'd region of %zu bytes failed",
			name, (size_t)MMAP_SZ);
		goto numa_free;
	}

	do {
		int j, mode, ret, status[num_pages], dest_nodes[num_pages];
		unsigned long i, node_mask[lbits], old_node_mask[lbits];
		void *pages[num_pages];
		uint8_t *ptr;
		node_t *n_tmp;
		unsigned cpu, curr_node;

		/*
		 *  Fetch memory policy
		 */
		ret = shim_get_mempolicy(&mode, node_mask, max_nodes,
			(unsigned long)buf, MPOL_F_ADDR);
		if (ret < 0) {
			pr_fail_err(name, "get_mempolicy");
		}
		if (!opt_do_run)
			break;

		memset(node_mask, 0, sizeof(node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		ret = shim_set_mempolicy(MPOL_PREFERRED, node_mask, max_nodes);
		if (ret < 0) {
			pr_fail_err(name, "set_mempolicy");
		}
		memset(buf, 0xff, MMAP_SZ);
		if (!opt_do_run)
			break;

		/*
		 *  Fetch CPU and node, we just waste some cycled
		 *  doing this for stress reasons only
		 */
		(void)shim_getcpu(&cpu, &curr_node, NULL);

		/*
		 *  mbind the buffer, first try MPOL_STRICT which
		 *  may fail with EIO
		 */
		memset(node_mask, 0, sizeof(node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		ret = shim_mbind(buf, MMAP_SZ, MPOL_BIND, node_mask,
			max_nodes, MPOL_MF_STRICT);
		if (ret < 0) {
			if (errno != EIO)
				pr_fail_err(name, "mbind");
		} else {
			memset(buf, 0xaa, MMAP_SZ);
		}
		if (!opt_do_run)
			break;

		/*
		 *  mbind the buffer, now try MPOL_DEFAULT
		 */
		memset(node_mask, 0, sizeof(node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		ret = shim_mbind(buf, MMAP_SZ, MPOL_BIND, node_mask,
			max_nodes, MPOL_DEFAULT);
		if (ret < 0) {
			if (errno != EIO)
				pr_fail_err(name, "mbind");
		} else {
			memset(buf, 0x5c, MMAP_SZ);
		}
		if (!opt_do_run)
			break;

		/* Move to next node */
		n = n->next;

		/*
		 *  Migrate all this processes pages to the current new node
		 */
		memset(old_node_mask, 0xff, sizeof(old_node_mask));
		memset(node_mask, 0, sizeof(node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		ret = shim_migrate_pages(mypid, max_nodes,
			old_node_mask, node_mask);
		if (ret < 0) {
			pr_fail_err(name, "migrate_pages");
		}
		if (!opt_do_run)
			break;

		n_tmp = n;
		for (j = 0; j < 16; j++) {
			/*
			 *  Now move pages to lots of different numa nodes
			 */
			for (ptr = buf, i = 0; i < num_pages; i++, ptr += page_sz, n_tmp = n_tmp->next) {
				pages[i] = ptr;
				dest_nodes[i] = n_tmp->node_id;
			}
			memset(status, 0, sizeof(status));
			ret = shim_move_pages(mypid, num_pages, pages,
				dest_nodes, status, MPOL_MF_MOVE);
			if (ret < 0) {
				pr_fail_err(name, "move_pages");
			}
			memset(buf, j, MMAP_SZ);
			if (!opt_do_run)
				break;
		}
		(*counter)++;
	} while (opt_do_run && (!max_ops || *counter < max_ops));

	rc = EXIT_SUCCESS;
	munmap(buf, MMAP_SZ);
numa_free:
	stress_numa_free_nodes(n);

	return rc;
}
#else
int stress_numa(
        uint64_t *const counter,
        const uint32_t instance,
        const uint64_t max_ops,
        const char *name)
{
	return stress_not_implemented(counter, instance, max_ops, name);
}
#endif
