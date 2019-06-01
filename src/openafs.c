/**
 * collectd - src/openafs.c
 * Copyright (C) 2019       Sine Nomine Associates
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Marcio Barbosa <mbarbosa@sinenomine.net>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <stdatomic.h>

#define OPENAFS_STR_MAX_LEN 256

struct openafs_class_s {
    int fd;				/* file descriptor */
    char *class;			/* identifier */
    char **labels;			/* name of each stat */
    volatile unsigned int *counters;	/* stats */
    unsigned int n_counters;		/* number of stats */
};
typedef struct openafs_class_s openafs_class_t;

openafs_class_t *openafs_class_lst = NULL;
unsigned int openafs_class_lst_len = 0;

static inline void
_openafs_release_labels(openafs_class_t *a_class)
{
    if (!a_class->labels) {
	return;
    }
    for (int i = 0; i < a_class->n_counters; i++) {
	free(a_class->labels[i]);
    }
}

/**
 * Release the resources allocated by the plugin.
 *
 * @return success
 */
static int
openafs_destroy(void)
{
    openafs_class_t *classp;

    for (int i = 0; i < openafs_class_lst_len; i++) {
	classp = &openafs_class_lst[i];
	munmap((void *)classp->counters, classp->n_counters * sizeof(unsigned int));
	close(classp->fd);
	free(classp->class);
	_openafs_release_labels(classp);
    }
    free(openafs_class_lst);

    return 0;
}

/**
 * Create shared-memory region for stats and register labels.
 *
 * @param[in]  a_ci     conf tree
 * @param[in]  a_class  name of class
 *
 * @return 0 on success
 */
static inline int
_openafs_add_stats(oconfig_item_t *a_ci, openafs_class_t *a_class)
{
    int status = 0;
    char buffer[OPENAFS_STR_MAX_LEN];

    if (strlen(a_class->class) + 2 > OPENAFS_STR_MAX_LEN) {
	return -1;
    }
    snprintf(buffer, sizeof(buffer), "/%s", a_class->class);

    a_class->fd = shm_open(buffer, O_RDONLY, 0666);
    if (a_class->fd < 0) {
	return -1;
    }
    a_class->n_counters = a_ci->children_num;

    a_class->counters =
	(unsigned int *)mmap(0, a_class->n_counters * sizeof(unsigned int),
			     PROT_READ, MAP_SHARED, a_class->fd, 0);

    a_class->labels = (char **)calloc(a_class->n_counters, sizeof(char *));
    if (!a_class->counters || !a_class->labels) {
	return -1;
    }

    for (int i = 0; i < a_ci->children_num && !status; i++) {
	oconfig_item_t *child = a_ci->children + i;

	if (strcasecmp("Counter", child->key) == 0) {
	    status = cf_util_get_string(child, &a_class->labels[i]);
	}
    }
    return status;
}

/**
 * Record and process each class.
 *
 * @param[in]  a_ci  config tree
 *
 * @return 0 on success
 */
static inline int
_openafs_process_class(oconfig_item_t *a_ci, openafs_class_t *a_class)
{
    int status = 0;

    if (strcasecmp("Class", a_ci->key) == 0) {
	status = cf_util_get_string(a_ci, &a_class->class);
	if (status == 0) {
	    status = _openafs_add_stats(a_ci, a_class);
	}
    }
    return status;
}

/**
 * Configuration callback used by collectd.
 *
 * @param[in]  a_ci  config tree
 *
 * @return 0 on success
 */
static int
openafs_config(oconfig_item_t *a_ci)
{
    int status = 0;

    openafs_class_lst =
	(openafs_class_t *)calloc(a_ci->children_num, sizeof(*openafs_class_lst));

    if (!openafs_class_lst) {
	return -1;
    }
    openafs_class_lst_len = a_ci->children_num;

    for (int i = 0; i < a_ci->children_num && !status; i++) {
	oconfig_item_t *child = a_ci->children + i;
	status = _openafs_process_class(child, &openafs_class_lst[i]);
    }
    if (status) {
	openafs_destroy();
    }
    return status;
}

/**
 * Export stats from class received as an argument.
 *
 * @param[in]  a_class  name of class
 *
 * @return none
 */
static inline void
_openafs_export_class(openafs_class_t *a_class)
{
    char buffer[OPENAFS_STR_MAX_LEN];
    unsigned int counter;

    value_list_t vl = VALUE_LIST_INIT;
    vl.values_len = 1;
    sstrncpy(vl.plugin, "openafs", sizeof(vl.plugin));

    for (int i = 0; i < a_class->n_counters; i++) {
	snprintf(buffer, sizeof(buffer), "%s_%s", a_class->class, a_class->labels[i]);
	sstrncpy(vl.type, buffer, sizeof(vl.type));
	counter = atomic_load_explicit(&a_class->counters[i], memory_order_relaxed);
	vl.values = &(value_t){.gauge = counter};
	plugin_dispatch_values(&vl);
    }
}

/**
 * Read callback used by collectd.
 *
 * @return 0 on success
 */
static int
openafs_read(void)
{
    for (int i = 0; i < openafs_class_lst_len; i++) {
	_openafs_export_class(&openafs_class_lst[i]);
    }
    return 0;
}

void
module_register(void)
{
    plugin_register_complex_config("openafs", openafs_config);
    plugin_register_read("openafs", openafs_read);
    plugin_register_shutdown("openafs", openafs_destroy);
}
