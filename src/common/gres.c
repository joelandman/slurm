/*****************************************************************************\
 *  gres.c - driver for gres plugin
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2014-2019 SchedMD LLC
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE

#ifdef __FreeBSD__
#  include <sys/param.h>
#  include <sys/cpuset.h>
typedef cpuset_t cpu_set_t;
#endif

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef MAJOR_IN_MKDEV
#  include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#  include <sys/sysmacros.h>
#endif

#include <math.h>

#ifdef __NetBSD__
#define CPU_ZERO(c) cpuset_zero(*(c))
#define CPU_ISSET(i,c) cpuset_isset((i),*(c))
#define sched_getaffinity sched_getaffinity_np
#endif

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"
#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/gres.h"
#include "src/common/job_resources.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/node_conf.h"
#include "src/common/node_select.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/strlcpy.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define MAX_GRES_BITMAP 1024

strong_alias(gres_find_id, slurm_gres_find_id);
strong_alias(gres_find_sock_by_job_state, slurm_gres_find_sock_by_job_state);
strong_alias(gres_get_node_used, slurm_gres_get_node_used);
strong_alias(gres_get_system_cnt, slurm_gres_get_system_cnt);
strong_alias(gres_get_value_by_type, slurm_gres_get_value_by_type);
strong_alias(gres_get_job_info, slurm_gres_get_job_info);
strong_alias(gres_get_step_info, slurm_gres_get_step_info);
strong_alias(gres_device_major, slurm_gres_device_major);
strong_alias(gres_sock_delete, slurm_gres_sock_delete);
strong_alias(destroy_gres_device, slurm_destroy_gres_device);
strong_alias(destroy_gres_slurmd_conf, slurm_destroy_gres_slurmd_conf);

/* Gres symbols provided by the plugin */
typedef struct slurm_gres_ops {
	int		(*node_config_load)	( List gres_conf_list,
						  node_config_load_t *node_conf);
	void		(*job_set_env)		( char ***job_env_ptr,
						  bitstr_t *gres_bit_alloc,
						  uint64_t gres_cnt,
						  gres_internal_flags_t flags);
	void		(*step_set_env)		( char ***step_env_ptr,
						  bitstr_t *gres_bit_alloc,
						  uint64_t gres_cnt,
						  gres_internal_flags_t flags);
	void		(*task_set_env)		( char ***step_env_ptr,
						  bitstr_t *gres_bit_alloc,
						  uint64_t gres_cnt,
						  bitstr_t *usable_gres,
						  gres_internal_flags_t flags);
	void		(*send_stepd)		( buf_t *buffer );
	void		(*recv_stepd)		( buf_t *buffer );
	int		(*job_info)		( gres_job_state_t *job_gres_data,
						  uint32_t node_inx,
						  enum gres_job_data_type data_type,
						  void *data);
	int		(*step_info)		( gres_step_state_t *step_gres_data,
						  uint32_t node_inx,
						  enum gres_step_data_type data_type,
						  void *data);
	List            (*get_devices)		( void );
	void            (*step_hardware_init)	( bitstr_t *, char * );
	void            (*step_hardware_fini)	( void );
	gres_epilog_info_t *(*epilog_build_env)(gres_job_state_t *gres_job_ptr);
	void            (*epilog_set_env)	( char ***epilog_env_ptr,
						  gres_epilog_info_t *epilog_info,
						  int node_inx );
} slurm_gres_ops_t;

/*
 * Gres plugin context, one for each gres type.
 * Add to gres_context through _add_gres_context().
 */
typedef struct slurm_gres_context {
	plugin_handle_t	cur_plugin;
	uint8_t		config_flags;		/* See GRES_CONF_* in gres.h */
	char *		gres_name;		/* name (e.g. "gpu") */
	char *		gres_name_colon;	/* name + colon (e.g. "gpu:") */
	int		gres_name_colon_len;	/* size of gres_name_colon */
	char *		gres_type;		/* plugin name (e.g. "gres/gpu") */
	slurm_gres_ops_t ops;			/* pointers to plugin symbols */
	uint32_t	plugin_id;		/* key for searches */
	plugrack_t	*plugin_list;		/* plugrack info */
	uint64_t        total_cnt;		/* Total GRES across all nodes */
} slurm_gres_context_t;

typedef struct {
	slurm_gres_context_t *context_ptr;
	int new_has_file;
	int new_has_type;
	int rec_count;
} foreach_gres_conf_t;

typedef struct {
	uint64_t gres_cnt;
	bool ignore_alloc;
	gres_key_t *job_search_key;
	slurm_step_id_t *step_id;
} foreach_gres_cnt_t;

/* Pointers to functions in src/slurmd/common/xcpuinfo.h that we may use */
typedef struct xcpuinfo_funcs {
	int (*xcpuinfo_abs_to_mac) (char *abs, char **mac);
} xcpuinfo_funcs_t;
xcpuinfo_funcs_t xcpuinfo_ops;

/* Local variables */
static int gres_context_cnt = -1;
static uint32_t gres_cpu_cnt = 0;
static slurm_gres_context_t *gres_context = NULL;
static char *gres_node_name = NULL;
static char *local_plugins_str = NULL;
static pthread_mutex_t gres_context_lock = PTHREAD_MUTEX_INITIALIZER;
static List gres_conf_list = NULL;
static bool init_run = false;
static bool have_gpu = false, have_mps = false;
static uint32_t gpu_plugin_id = NO_VAL, mps_plugin_id = NO_VAL;
static volatile uint32_t autodetect_flags = GRES_AUTODETECT_UNSET;
static uint32_t select_plugin_type = NO_VAL;
static buf_t *gres_context_buf = NULL;
static buf_t *gres_conf_buf = NULL;

/* Local functions */
static void _add_gres_context(char *gres_name);
static gres_node_state_t *_build_gres_node_state(void);
static void	_build_node_gres_str(List *gres_list, char **gres_str,
				     int cores_per_sock, int sock_per_node);
static bitstr_t *_core_bitmap_rebuild(bitstr_t *old_core_bitmap, int new_size);
static void	_epilog_list_del(void *x);
static void	_get_gres_cnt(gres_node_state_t *gres_data, char *orig_config,
			      char *gres_name, char *gres_name_colon,
			      int gres_name_colon_len);
static uint64_t _get_job_gres_list_cnt(List gres_list, char *gres_name,
				       char *gres_type);
static uint64_t	_get_tot_gres_cnt(uint32_t plugin_id, uint64_t *topo_cnt,
				  int *config_type_cnt);
static void	_job_state_delete(void *gres_data);
static void *	_job_state_dup(void *gres_data);
static void *	_job_state_dup2(void *gres_data, int node_index);
static void	_job_state_log(void *gres_data, uint32_t job_id,
			       uint32_t plugin_id);
static uint32_t _job_test(void *job_gres_data, void *node_gres_data,
			  bool use_total_gres, bitstr_t *core_bitmap,
			  int core_start_bit, int core_end_bit, bool *topo_set,
			  uint32_t job_id, char *node_name, char *gres_name,
			  uint32_t plugin_id, bool disable_binding);
static int	_load_plugin(slurm_gres_context_t *plugin_context);
static int	_log_gres_slurmd_conf(void *x, void *arg);
static void	_my_stat(char *file_name);
static int	_node_config_init(char *node_name, char *orig_config,
				  slurm_gres_context_t *context_ptr,
				  gres_state_t *gres_ptr);
static char *	_node_gres_used(void *gres_data, char *gres_name);
static int	_node_reconfig(char *node_name, char *new_gres, char **gres_str,
			       gres_state_t *gres_ptr, bool config_overrides,
			       slurm_gres_context_t *context_ptr,
			       bool *updated_gpu_cnt);
static int	_node_reconfig_test(char *node_name, char *new_gres,
				    gres_state_t *gres_ptr,
				    slurm_gres_context_t *context_ptr);
static void	_node_state_dealloc(gres_state_t *gres_ptr);
static void *	_node_state_dup(void *gres_data);
static void	_node_state_log(void *gres_data, char *node_name,
				char *gres_name);
static int	_parse_gres_config(void **dest, slurm_parser_enum_t type,
				   const char *key, const char *value,
				   const char *line, char **leftover);
static int	_parse_gres_config2(void **dest, slurm_parser_enum_t type,
				    const char *key, const char *value,
				    const char *line, char **leftover);
static void *	_step_state_dup(void *gres_data);
static void *	_step_state_dup2(void *gres_data, int node_index);
static void	_step_state_log(void *gres_data, slurm_step_id_t *step_id,
				char *gres_name);
static void	_sync_node_mps_to_gpu(gres_state_t *mps_gres_ptr,
				      gres_state_t *gpu_gres_ptr);
static int	_unload_plugin(slurm_gres_context_t *plugin_context);
static void	_validate_slurm_conf(List slurm_conf_list,
				     slurm_gres_context_t *context_ptr);
static void	_validate_gres_conf(List gres_conf_list,
				    slurm_gres_context_t *context_ptr);
static int	_validate_file(char *path_name, char *gres_name);
static void	_validate_links(gres_slurmd_conf_t *p);
static int	_valid_gres_type(char *gres_name, gres_node_state_t *gres_data,
				 bool config_overrides, char **reason_down);

extern uint32_t gres_build_id(char *name)
{
	int i, j;
	uint32_t id = 0;

	if (!name)
		return id;

	for (i = 0, j = 0; name[i]; i++) {
		id += (name[i] << j);
		j = (j + 8) % 32;
	}

	return id;
}

extern int gres_find_id(void *x, void *key)
{
	uint32_t *plugin_id = (uint32_t *)key;
	gres_state_t *state_ptr = (gres_state_t *) x;
	if (state_ptr->plugin_id == *plugin_id)
		return 1;
	return 0;
}

/* Find job record with matching name and type */
extern int gres_find_job_by_key_exact_type(void *x, void *key)
{
	gres_state_t *state_ptr = (gres_state_t *) x;
	gres_key_t *job_key = (gres_key_t *) key;
	gres_job_state_t *gres_data_ptr;
	gres_data_ptr = (gres_job_state_t *)state_ptr->gres_data;

	if ((state_ptr->plugin_id == job_key->plugin_id) &&
	    (gres_data_ptr->type_id == job_key->type_id))
		return 1;
	return 0;
}

/* Find job record with matching name and type */
extern int gres_find_job_by_key(void *x, void *key)
{
	gres_state_t *state_ptr = (gres_state_t *) x;
	gres_key_t *job_key = (gres_key_t *) key;
	gres_job_state_t *gres_data_ptr;
	gres_data_ptr = (gres_job_state_t *)state_ptr->gres_data;

	if ((state_ptr->plugin_id == job_key->plugin_id) &&
	    ((job_key->type_id == NO_VAL) ||
	     (gres_data_ptr->type_id == job_key->type_id)))
		return 1;
	return 0;
}

/* Find job record with matching name and type */
extern int gres_find_job_by_key_with_cnt(void *x, void *key)
{
	gres_state_t *state_ptr = (gres_state_t *) x;
	gres_key_t *job_key = (gres_key_t *) key;
	gres_job_state_t *gres_data_ptr;
	gres_data_ptr = (gres_job_state_t *)state_ptr->gres_data;

	if (!gres_find_job_by_key(x, key))
		return 0;
	/* ignore count on no_consume gres */
	if (!gres_data_ptr->node_cnt ||
	    gres_data_ptr->gres_cnt_node_alloc[job_key->node_offset])
		return 1;
	return 0;
}

extern int gres_find_step_by_key(void *x, void *key)
{
	gres_state_t *state_ptr = (gres_state_t *) x;
	gres_key_t *step_key = (gres_key_t *) key;
	gres_step_state_t *gres_data_ptr;
	gres_data_ptr = (gres_step_state_t *)state_ptr->gres_data;

	if ((state_ptr->plugin_id == step_key->plugin_id) &&
	    (gres_data_ptr->type_id == step_key->type_id))
		return 1;
	return 0;
}

static int _load_plugin(slurm_gres_context_t *plugin_context)
{
	/*
	 * Must be synchronized with slurm_gres_ops_t above.
	 */
	static const char *syms[] = {
		"gres_p_node_config_load",
		"gres_p_job_set_env",
		"gres_p_step_set_env",
		"gres_p_task_set_env",
		"gres_p_send_stepd",
		"gres_p_recv_stepd",
		"gres_p_get_job_info",
		"gres_p_get_step_info",
		"gres_p_get_devices",
		"gres_p_step_hardware_init",
		"gres_p_step_hardware_fini",
		"gres_p_epilog_build_env",
		"gres_p_epilog_set_env"
	};
	int n_syms = sizeof(syms) / sizeof(char *);

	/* Find the correct plugin */
	if (plugin_context->config_flags & GRES_CONF_COUNT_ONLY) {
		debug("Plugin of type %s only tracks gres counts",
		      plugin_context->gres_type);
		return SLURM_SUCCESS;
	}

	plugin_context->cur_plugin = plugin_load_and_link(
		plugin_context->gres_type,
		n_syms, syms,
		(void **) &plugin_context->ops);
	if (plugin_context->cur_plugin != PLUGIN_INVALID_HANDLE)
		return SLURM_SUCCESS;

	if (errno != EPLUGIN_NOTFOUND) {
		error("Couldn't load specified plugin name for %s: %s",
		      plugin_context->gres_type, plugin_strerror(errno));
		return SLURM_ERROR;
	}

	debug("gres: Couldn't find the specified plugin name for %s looking "
	      "at all files", plugin_context->gres_type);

	/* Get plugin list */
	if (plugin_context->plugin_list == NULL) {
		plugin_context->plugin_list = plugrack_create("gres");
		plugrack_read_dir(plugin_context->plugin_list,
				  slurm_conf.plugindir);
	}

	plugin_context->cur_plugin = plugrack_use_by_type(
		plugin_context->plugin_list,
		plugin_context->gres_type );
	if (plugin_context->cur_plugin == PLUGIN_INVALID_HANDLE) {
		debug("Cannot find plugin of type %s, just track gres counts",
		      plugin_context->gres_type);
		plugin_context->config_flags |= GRES_CONF_COUNT_ONLY;
		return SLURM_ERROR;
	}

	/* Dereference the API. */
	if (plugin_get_syms(plugin_context->cur_plugin,
			    n_syms, syms,
			    (void **) &plugin_context->ops ) < n_syms ) {
		error("Incomplete %s plugin detected",
		      plugin_context->gres_type);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static int _unload_plugin(slurm_gres_context_t *plugin_context)
{
	int rc;

	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if (plugin_context->plugin_list)
		rc = plugrack_destroy(plugin_context->plugin_list);
	else {
		rc = SLURM_SUCCESS;
		plugin_unload(plugin_context->cur_plugin);
	}
	xfree(plugin_context->gres_name);
	xfree(plugin_context->gres_name_colon);
	xfree(plugin_context->gres_type);

	return rc;
}

/*
 * Add new gres context to gres_context array and load the plugin.
 * Must hold gres_context_lock before calling.
 */
static void _add_gres_context(char *gres_name)
{
	slurm_gres_context_t *plugin_context;

	if (!gres_name || !gres_name[0])
		fatal("%s: invalid empty gres_name", __func__);

	xrecalloc(gres_context, (gres_context_cnt + 1),
		  sizeof(slurm_gres_context_t));

	plugin_context = &gres_context[gres_context_cnt];
	plugin_context->gres_name = xstrdup(gres_name);
	plugin_context->plugin_id = gres_build_id(gres_name);
	plugin_context->gres_type = xstrdup_printf("gres/%s", gres_name);
	plugin_context->plugin_list = NULL;
	plugin_context->cur_plugin = PLUGIN_INVALID_HANDLE;

	gres_context_cnt++;
}

/*
 * Initialize the GRES plugins.
 *
 * Returns a Slurm errno.
 */
extern int gres_init(void)
{
	int i, j, rc = SLURM_SUCCESS;
	char *last = NULL, *names, *one_name, *full_name;
	char *sorted_names = NULL, *sep = "";
	bool append_mps = false;

	if (init_run && (gres_context_cnt >= 0))
		return rc;

	slurm_mutex_lock(&gres_context_lock);

	if (gres_context_cnt >= 0)
		goto fini;

	local_plugins_str = xstrdup(slurm_conf.gres_plugins);
	gres_context_cnt = 0;
	if ((local_plugins_str == NULL) || (local_plugins_str[0] == '\0'))
		goto fini;

	/* Ensure that "gres/mps" follows "gres/gpu" */
	have_gpu = false;
	have_mps = false;
	names = xstrdup(local_plugins_str);
	one_name = strtok_r(names, ",", &last);
	while (one_name) {
		bool skip_name = false;
		if (!xstrcmp(one_name, "mps")) {
			have_mps = true;
			if (!have_gpu) {
				append_mps = true; /* "mps" must follow "gpu" */
				skip_name = true;
			}
			mps_plugin_id = gres_build_id("mps");
		} else if (!xstrcmp(one_name, "gpu")) {
			have_gpu = true;
			gpu_plugin_id = gres_build_id("gpu");
		}
		if (!skip_name) {
			xstrfmtcat(sorted_names, "%s%s", sep, one_name);
			sep = ",";
		}
		one_name = strtok_r(NULL, ",", &last);
	}
	if (append_mps) {
		if (!have_gpu)
			fatal("GresTypes: gres/mps requires that gres/gpu also be configured");
		xstrfmtcat(sorted_names, "%s%s", sep, "mps");
	}
	xfree(names);

	gres_context_cnt = 0;
	one_name = strtok_r(sorted_names, ",", &last);
	while (one_name) {
		full_name = xstrdup("gres/");
		xstrcat(full_name, one_name);
		for (i = 0; i < gres_context_cnt; i++) {
			if (!xstrcmp(full_name, gres_context[i].gres_type))
				break;
		}
		xfree(full_name);
		if (i < gres_context_cnt) {
			error("Duplicate plugin %s ignored",
			      gres_context[i].gres_type);
		} else {
			_add_gres_context(one_name);
		}
		one_name = strtok_r(NULL, ",", &last);
	}
	xfree(sorted_names);

	/* Ensure that plugin_id is valid and unique */
	for (i = 0; i < gres_context_cnt; i++) {
		for (j = i + 1; j < gres_context_cnt; j++) {
			if (gres_context[i].plugin_id !=
			    gres_context[j].plugin_id)
				continue;
			fatal("Gres: Duplicate plugin_id %u for %s and %s, "
			      "change gres name for one of them",
			      gres_context[i].plugin_id,
			      gres_context[i].gres_type,
			      gres_context[j].gres_type);
		}
		xassert(gres_context[i].gres_name);

		gres_context[i].gres_name_colon =
			xstrdup_printf("%s:", gres_context[i].gres_name);
		gres_context[i].gres_name_colon_len =
			strlen(gres_context[i].gres_name_colon);
	}

fini:
	if ((select_plugin_type == NO_VAL) &&
	    (select_g_get_info_from_plugin(SELECT_CR_PLUGIN, NULL,
					   &select_plugin_type) != SLURM_SUCCESS)) {
		select_plugin_type = NO_VAL;	/* error */
	}
	if (have_mps && running_in_slurmctld() &&
	    (select_plugin_type != SELECT_TYPE_CONS_TRES)) {
		fatal("Use of gres/mps requires the use of select/cons_tres");
	}

	init_run = true;
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

extern int gres_get_gres_cnt(void)
{
	static int cnt = -1;

	if (cnt != -1)
		return cnt;

	gres_init();

	slurm_mutex_lock(&gres_context_lock);
	cnt = gres_context_cnt;
	slurm_mutex_unlock(&gres_context_lock);

	return cnt;
}

/*
 * Add a GRES record. This is used by the node_features plugin after the
 * slurm.conf file is read and the initial GRES records are built by
 * gres_init().
 */
extern void gres_add(char *gres_name)
{
	int i;

	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (!xstrcmp(gres_context[i].gres_name, gres_name))
			goto fini;
	}

	_add_gres_context(gres_name);
fini:	slurm_mutex_unlock(&gres_context_lock);
}

/* Given a gres_name, return its context index or -1 if not found */
static int _gres_name_context(char *gres_name)
{
	int i;

	for (i = 0; i < gres_context_cnt; i++) {
		if (!xstrcmp(gres_context[i].gres_name, gres_name))
			return i;
	}

	return -1;
}

/*
 * Takes a GRES config line (typically from slurm.conf) and remove any
 * records for GRES which are not defined in GresTypes.
 * RET string of valid GRES, Release memory using xfree()
 */
extern char *gres_name_filter(char *orig_gres, char *nodes)
{
	char *new_gres = NULL, *save_ptr = NULL;
	char *colon, *sep = "", *tmp, *tok, *name;

	slurm_mutex_lock(&gres_context_lock);
	if (!orig_gres || !orig_gres[0] || !gres_context_cnt) {
		slurm_mutex_unlock(&gres_context_lock);
		return new_gres;
	}

	tmp = xstrdup(orig_gres);
	tok = strtok_r(tmp, ",", &save_ptr);
	while (tok) {
		name = xstrdup(tok);
		if ((colon = strchr(name, ':')))
			colon[0] = '\0';
		if (_gres_name_context(name) != -1) {
			xstrfmtcat(new_gres, "%s%s", sep, tok);
			sep = ",";
		} else {
			/* Logging may not be initialized at this point */
			error("Invalid GRES configured on node %s: %s", nodes,
			      tok);
		}
		xfree(name);
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	slurm_mutex_unlock(&gres_context_lock);
	xfree(tmp);

	return new_gres;
}

/*
 * Terminate the gres plugin. Free memory.
 *
 * Returns a Slurm errno.
 */
extern int gres_fini(void)
{
	int i, j, rc = SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
	xfree(gres_node_name);
	if (gres_context_cnt < 0)
		goto fini;

	init_run = false;
	for (i = 0; i < gres_context_cnt; i++) {
		j = _unload_plugin(gres_context + i);
		if (j != SLURM_SUCCESS)
			rc = j;
	}
	xfree(gres_context);
	xfree(local_plugins_str);
	FREE_NULL_LIST(gres_conf_list);
	FREE_NULL_BUFFER(gres_context_buf);
	FREE_NULL_BUFFER(gres_conf_buf);
	gres_context_cnt = -1;

fini:	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

/*
 **************************************************************************
 *                          P L U G I N   C A L L S                       *
 **************************************************************************
 */

/*
 * Return a plugin-specific help message for salloc, sbatch and srun
 * Result must be xfree()'d.
 *
 * NOTE: GRES "type" (e.g. model) information is only available from slurmctld
 * after slurmd registers. It is not readily available from srun (as used here).
 */
extern char *gres_help_msg(void)
{
	int i;
	char *msg = xstrdup("Valid gres options are:\n");

	gres_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		xstrcat(msg, gres_context[i].gres_name);
		xstrcat(msg, "[[:type]:count]\n");
	}
	slurm_mutex_unlock(&gres_context_lock);

	return msg;
}

/*
 * Perform reconfig, re-read any configuration files
 * OUT did_change - set if gres configuration changed
 */
extern int gres_reconfig(void)
{
	int rc = SLURM_SUCCESS;
	bool plugin_change;

	slurm_mutex_lock(&gres_context_lock);

	if (xstrcmp(slurm_conf.gres_plugins, local_plugins_str))
		plugin_change = true;
	else
		plugin_change = false;
	slurm_mutex_unlock(&gres_context_lock);

	if (plugin_change) {
		error("GresPlugins changed from %s to %s ignored",
		      local_plugins_str, slurm_conf.gres_plugins);
		error("Restart the slurmctld daemon to change GresPlugins");
#if 0
		/* This logic would load new plugins, but we need the old
		 * plugins to persist in order to process old state
		 * information. */
		rc = gres_fini();
		if (rc == SLURM_SUCCESS)
			rc = gres_init();
#endif
	}

	return rc;
}

/* Return 1 if a gres_conf record is the correct plugin_id and has no file */
static int _find_fileless_gres(void *x, void *arg)
{
	gres_slurmd_conf_t *gres_conf = (gres_slurmd_conf_t *)x;
	uint32_t plugin_id = *(uint32_t *)arg;

	if ((gres_conf->plugin_id == plugin_id) && !gres_conf->file) {
		debug("Removing file-less GPU %s:%s from final GRES list",
		      gres_conf->name, gres_conf->type_name);
		return 1;
	}
	return 0;

}

/*
 * Log the contents of a gres_slurmd_conf_t record
 */
static int _log_gres_slurmd_conf(void *x, void *arg)
{
	gres_slurmd_conf_t *p;
	char *links = NULL;
	int index = -1, offset, mult = 1;

	p = (gres_slurmd_conf_t *) x;
	xassert(p);

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_GRES)) {
		verbose("Gres Name=%s Type=%s Count=%"PRIu64,
			p->name, p->type_name, p->count);
		return 0;
	}

	if (p->file) {
		index = 0;
		offset = strlen(p->file);
		while (offset > 0) {
			offset--;
			if ((p->file[offset] < '0') || (p->file[offset] > '9'))
				break;
			index += (p->file[offset] - '0') * mult;
			mult *= 10;
		}
	}

	if (p->links)
		xstrfmtcat(links, "Links=%s", p->links);
	if (p->cpus && (index != -1)) {
		info("Gres Name=%s Type=%s Count=%"PRIu64" Index=%d ID=%u "
		     "File=%s Cores=%s CoreCnt=%u %s",
		     p->name, p->type_name, p->count, index, p->plugin_id,
		     p->file, p->cpus, p->cpu_cnt, links);
	} else if (index != -1) {
		info("Gres Name=%s Type=%s Count=%"PRIu64" Index=%d ID=%u File=%s %s",
		     p->name, p->type_name, p->count, index, p->plugin_id,
		     p->file, links);
	} else if (p->file) {
		info("Gres Name=%s Type=%s Count=%"PRIu64" ID=%u File=%s %s",
		     p->name, p->type_name, p->count, p->plugin_id, p->file,
		     links);
	} else {
		info("Gres Name=%s Type=%s Count=%"PRIu64" ID=%u %s", p->name,
		     p->type_name, p->count, p->plugin_id, links);
	}
	xfree(links);

	return 0;
}

/* Make sure that specified file name exists, wait up to 20 seconds or generate
 * fatal error and exit. */
static void _my_stat(char *file_name)
{
	struct stat config_stat;
	bool sent_msg = false;
	int i;

	if (!running_in_slurmd_stepd())
		return;

	for (i = 0; i < 20; i++) {
		if (i)
			sleep(1);
		if (stat(file_name, &config_stat) == 0) {
			if (sent_msg)
				info("gres.conf file %s now exists", file_name);
			return;
		}

		if (errno != ENOENT)
			break;

		if (!sent_msg) {
			error("Waiting for gres.conf file %s", file_name);
			sent_msg = true;
		}
	}
	fatal("can't stat gres.conf file %s: %m", file_name);
	return;
}

static int _validate_file(char *filenames, char *gres_name)
{
	char *one_name;
	hostlist_t hl;
	int file_count = 0;

	if (!(hl = hostlist_create(filenames)))
		fatal("can't parse File=%s", filenames);

	while ((one_name = hostlist_shift(hl))) {
		_my_stat(one_name);
		file_count++;
		free(one_name);
	}

	hostlist_destroy(hl);

	return file_count;
}

/*
 * Check that we have a comma-delimited list of numbers
 */
static void _validate_links(gres_slurmd_conf_t *p)
{
	char *tmp, *tok, *save_ptr = NULL, *end_ptr = NULL;
	long int val;

	if (!p->links)
		return;
	if (p->links[0] == '\0') {
		xfree(p->links);
		return;
	}

	tmp = xstrdup(p->links);
	tok = strtok_r(tmp, ",", &save_ptr);
	while (tok) {
		val = strtol(tok, &end_ptr, 10);
		if ((val < -2) || (val > GRES_MAX_LINK) || (val == LONG_MIN) ||
		    (end_ptr[0] != '\0')) {
			error("gres.conf: Ignoring invalid Link (%s) for Name=%s",
			      tok, p->name);
			xfree(p->links);
			break;
		}
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp);
}

/*
 * Return true if count can be greater than 1 for a given file.
 * For example, each GPU can have arbitrary count of MPS elements.
 */
static bool _multi_count_per_file(char *name)
{
	if (!xstrcmp(name, "mps"))
		return true;
	return false;
}

static char *_get_autodetect_flags_str(void)
{
	char *flags = NULL;

	if (!(autodetect_flags & GRES_AUTODETECT_GPU_FLAGS))
		xstrfmtcat(flags, "%sunset", flags ? "," : "");
	else {
		if (autodetect_flags & GRES_AUTODETECT_GPU_NVML)
			xstrfmtcat(flags, "%snvml", flags ? "," : "");
		else if (autodetect_flags & GRES_AUTODETECT_GPU_RSMI)
			xstrfmtcat(flags, "%srsmi", flags ? "," : "");
		else if (autodetect_flags & GRES_AUTODETECT_GPU_OFF)
			xstrfmtcat(flags, "%soff", flags ? "," : "");
	}

	return flags;
}

static uint32_t _handle_autodetect_flags(char *str)
{
	uint32_t flags = 0;

	/* Set the node-local gpus value of autodetect_flags */
	if (xstrcasestr(str, "nvml"))
		flags |= GRES_AUTODETECT_GPU_NVML;
	else if (xstrcasestr(str, "rsmi"))
		flags |= GRES_AUTODETECT_GPU_RSMI;
	else if (!xstrcmp(str, "off"))
		flags |= GRES_AUTODETECT_GPU_OFF;

	return flags;
}

static void _handle_local_autodetect(char *str)
{
	uint32_t autodetect_flags_local = _handle_autodetect_flags(str);

	/* Only set autodetect_flags once locally, unless it's the same val */
	if ((autodetect_flags != GRES_AUTODETECT_UNSET) &&
	    (autodetect_flags != autodetect_flags_local)) {
		fatal("gres.conf: duplicate node-local AutoDetect specification does not match the first");
		return;
	}

	/* Set the node-local gpus value of autodetect_flags */
	autodetect_flags |= autodetect_flags_local;

	if (slurm_conf.debug_flags & DEBUG_FLAG_GRES) {
		char *flags = _get_autodetect_flags_str();
		log_flag(GRES, "Using node-local AutoDetect=%s(%d)",
			 flags, autodetect_flags);
		xfree(flags);
	}
}

static void _handle_global_autodetect(char *str)
{
	/* If GPU flags exist, node-local value was already specified */
	if (autodetect_flags & GRES_AUTODETECT_GPU_FLAGS)
		debug2("gres.conf: AutoDetect GPU flags were locally set, so ignoring global flags");
	else
		autodetect_flags |= _handle_autodetect_flags(str);

	if (slurm_conf.debug_flags & DEBUG_FLAG_GRES) {
		char *flags = _get_autodetect_flags_str();
		log_flag(GRES, "Global AutoDetect=%s(%d)",
			 flags, autodetect_flags);
		xfree(flags);
	}
}

/*
 * Build gres_slurmd_conf_t record based upon a line from the gres.conf file
 */
static int _parse_gres_config(void **dest, slurm_parser_enum_t type,
			      const char *key, const char *value,
			      const char *line, char **leftover)
{
	static s_p_options_t _gres_options[] = {
		{"AutoDetect", S_P_STRING},
		{"Count", S_P_STRING},	/* Number of Gres available */
		{"CPUs" , S_P_STRING},	/* CPUs to bind to Gres resource
					 * (deprecated, use Cores) */
		{"Cores", S_P_STRING},	/* Cores to bind to Gres resource */
		{"File",  S_P_STRING},	/* Path to Gres device */
		{"Files", S_P_STRING},	/* Path to Gres device */
		{"Flags", S_P_STRING},	/* GRES Flags */
		{"Link",  S_P_STRING},	/* Communication link IDs */
		{"Links", S_P_STRING},	/* Communication link IDs */
		{"MultipleFiles", S_P_STRING}, /* list of GRES device files */
		{"Name",  S_P_STRING},	/* Gres name */
		{"Type",  S_P_STRING},	/* Gres type (e.g. model name) */
		{NULL}
	};
	int i;
	s_p_hashtbl_t *tbl;
	gres_slurmd_conf_t *p;
	uint64_t tmp_uint64, mult;
	char *tmp_str, *last;
	bool cores_flag = false, cpus_flag = false;
	char *type_str = NULL;
	char *autodetect_string = NULL;
	bool autodetect = false;

	tbl = s_p_hashtbl_create(_gres_options);
	s_p_parse_line(tbl, *leftover, leftover);

	p = xmalloc(sizeof(gres_slurmd_conf_t));

	/*
	 * Detect and set the node-local AutoDetect option only if
	 * NodeName is specified.
	 */
	if (s_p_get_string(&autodetect_string, "AutoDetect", tbl)) {
		if (value)
			error("gres.conf: In-line AutoDetect requires NodeName to take effect");
		else {
			_handle_local_autodetect(autodetect_string);
			/* AutoDetect was specified w/ NodeName */
			autodetect = true;
		}
		xfree(autodetect_string);
	}

	if (!value) {
		if (!s_p_get_string(&p->name, "Name", tbl)) {
			if (!autodetect)
				error("Invalid GRES data, no type name (%s)",
				      line);
			xfree(p);
			s_p_hashtbl_destroy(tbl);
			return 0;
		}
	} else {
		p->name = xstrdup(value);
	}

	p->cpu_cnt = gres_cpu_cnt;
	if (s_p_get_string(&p->cpus, "Cores", tbl)) {
		cores_flag = true;
		type_str = "Cores";
	} else if (s_p_get_string(&p->cpus, "CPUs", tbl)) {
		cpus_flag = true;
		type_str = "CPUs";
	}
	if (cores_flag || cpus_flag) {
		char *local_cpus = NULL;
		if (xcpuinfo_ops.xcpuinfo_abs_to_mac) {
			i = (xcpuinfo_ops.xcpuinfo_abs_to_mac)
				(p->cpus, &local_cpus);
			if (i != SLURM_SUCCESS) {
				error("Invalid GRES data for %s, %s=%s",
				      p->name, type_str, p->cpus);
			}
		} else {
			/*
			 * Not converting Cores into machine format is only for
			 * testing or if we don't care about cpus_bitmap. The
			 * slurmd should always convert to machine format.
			 */
			debug("%s: %s=%s is not being converted to machine-local format",
			      __func__, type_str, p->cpus);
			local_cpus = xstrdup(p->cpus);
			i = SLURM_SUCCESS;
		}
		if (i == SLURM_SUCCESS) {
			p->cpus_bitmap = bit_alloc(gres_cpu_cnt);
			if (!bit_size(p->cpus_bitmap) ||
			    bit_unfmt(p->cpus_bitmap, local_cpus)) {
				fatal("Invalid GRES data for %s, %s=%s (only %u CPUs are available)",
				      p->name, type_str, p->cpus, gres_cpu_cnt);
			}
		}
		xfree(local_cpus);
	}

	if (s_p_get_string(&p->file, "File", tbl) ||
	    s_p_get_string(&p->file, "Files", tbl)) {
		p->count = _validate_file(p->file, p->name);
		p->config_flags |= GRES_CONF_HAS_FILE;
	}

	if (s_p_get_string(&p->file, "MultipleFiles", tbl)) {
		if (p->config_flags & GRES_CONF_HAS_FILE)
			fatal("File and MultipleFiles options are mutually exclusive");
		p->count = 1;
		_validate_file(p->file, p->name);
		p->config_flags |= GRES_CONF_HAS_FILE;
	}

	if (s_p_get_string(&tmp_str, "Flags", tbl)) {
		if (xstrcasestr(tmp_str, "CountOnly"))
			p->config_flags |= GRES_CONF_COUNT_ONLY;
		xfree(tmp_str);
	}

	if (s_p_get_string(&p->links, "Link",  tbl) ||
	    s_p_get_string(&p->links, "Links", tbl)) {
		_validate_links(p);
	}

	if (s_p_get_string(&p->type_name, "Type", tbl)) {
		p->config_flags |= GRES_CONF_HAS_TYPE;
	}

	if (s_p_get_string(&tmp_str, "Count", tbl)) {
		tmp_uint64 = strtoll(tmp_str, &last, 10);
		if ((tmp_uint64 == LONG_MIN) || (tmp_uint64 == LONG_MAX)) {
			fatal("Invalid GRES record for %s, invalid count %s",
			      p->name, tmp_str);
		}
		if ((mult = suffix_mult(last)) != NO_VAL64) {
			tmp_uint64 *= mult;
		} else {
			fatal("Invalid GRES record for %s, invalid count %s",
			      p->name, tmp_str);
		}
		/*
		 * Some GRES can have count > 1 for a given file. For example,
		 * each GPU can have arbitrary count of MPS elements.
		 */
		if (p->count && (p->count != tmp_uint64) &&
		    !_multi_count_per_file(p->name)) {
			fatal("Invalid GRES record for %s, count does not match File value",
			      p->name);
		}
		if (tmp_uint64 >= NO_VAL64) {
			fatal("GRES %s has invalid count value %"PRIu64,
			      p->name, tmp_uint64);
		}
		p->count = tmp_uint64;
		xfree(tmp_str);
	} else if (p->count == 0)
		p->count = 1;

	s_p_hashtbl_destroy(tbl);

	for (i = 0; i < gres_context_cnt; i++) {
		if (xstrcasecmp(p->name, gres_context[i].gres_name) == 0)
			break;
	}
	if (i >= gres_context_cnt) {
		error("Ignoring gres.conf record, invalid name: %s", p->name);
		destroy_gres_slurmd_conf(p);
		return 0;
	}
	p->plugin_id = gres_context[i].plugin_id;
	*dest = (void *)p;
	return 1;
}
static int _parse_gres_config2(void **dest, slurm_parser_enum_t type,
			       const char *key, const char *value,
			       const char *line, char **leftover)
{
	static s_p_options_t _gres_options[] = {
		{"AutoDetect", S_P_STRING},
		{"Count", S_P_STRING},	/* Number of Gres available */
		{"CPUs" , S_P_STRING},	/* CPUs to bind to Gres resource */
		{"Cores", S_P_STRING},	/* Cores to bind to Gres resource */
		{"File",  S_P_STRING},	/* Path to Gres device */
		{"Files",  S_P_STRING},	/* Path to Gres device */
		{"Flags", S_P_STRING},	/* GRES Flags */
		{"Link",  S_P_STRING},	/* Communication link IDs */
		{"Links", S_P_STRING},	/* Communication link IDs */
		{"MultipleFiles", S_P_STRING}, /* list of GRES device files */
		{"Name",  S_P_STRING},	/* Gres name */
		{"Type",  S_P_STRING},	/* Gres type (e.g. model name) */
		{NULL}
	};
	s_p_hashtbl_t *tbl;

	if (gres_node_name && value) {
		bool match = false;
		hostlist_t hl;
		hl = hostlist_create(value);
		if (hl) {
			match = (hostlist_find(hl, gres_node_name) >= 0);
			hostlist_destroy(hl);
		}
		if (!match) {
			debug("skipping GRES for NodeName=%s %s", value, line);
			tbl = s_p_hashtbl_create(_gres_options);
			s_p_parse_line(tbl, *leftover, leftover);
			s_p_hashtbl_destroy(tbl);
			return 0;
		}
	}
	return _parse_gres_config(dest, type, key, NULL, line, leftover);
}

static int _foreach_slurm_conf(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *)x;
	slurm_gres_context_t *context_ptr = (slurm_gres_context_t *)arg;
	gres_node_state_t *slurm_gres;
	uint64_t tmp_count = 0;

	/* Only look at the GRES under the current plugin (same name) */
	if (gres_ptr->plugin_id != context_ptr->plugin_id)
		return 0;

	slurm_gres = (gres_node_state_t *)gres_ptr->gres_data;

	/*
	 * gres_cnt_config should equal the combined count from
	 * type_cnt_avail if there are no untyped GRES
	 */
	for (uint16_t i = 0; i < slurm_gres->type_cnt; i++)
		tmp_count += slurm_gres->type_cnt_avail[i];

	/* Forbid mixing typed and untyped GRES under the same name */
	if (slurm_gres->type_cnt &&
	    slurm_gres->gres_cnt_config > tmp_count)
		fatal("%s: Some %s GRES in slurm.conf have a type while others do not (slurm_gres->gres_cnt_config (%"PRIu64") > tmp_count (%"PRIu64"))",
		      __func__, context_ptr->gres_name,
		      slurm_gres->gres_cnt_config, tmp_count);
	return 1;
}

static void _validate_slurm_conf(List slurm_conf_list,
				 slurm_gres_context_t *context_ptr)
{
	if (!slurm_conf_list)
		return;

	(void)list_for_each_nobreak(slurm_conf_list, _foreach_slurm_conf,
				    context_ptr);
}

static int _foreach_gres_conf(void *x, void *arg)
{
	gres_slurmd_conf_t *gres_slurmd_conf = (gres_slurmd_conf_t *)x;
	foreach_gres_conf_t *foreach_gres_conf = (foreach_gres_conf_t *)arg;
	slurm_gres_context_t *context_ptr = foreach_gres_conf->context_ptr;
	bool orig_has_file, orig_has_type;

	/* Only look at the GRES under the current plugin (same name) */
	if (gres_slurmd_conf->plugin_id != context_ptr->plugin_id)
		return 0;

	/*
	 * If any plugin of this type has this set it will virally set
	 * any other to be the same as we use the context_ptr from here
	 * on out.
	 */
	if (gres_slurmd_conf->config_flags & GRES_CONF_COUNT_ONLY)
		context_ptr->config_flags |= GRES_CONF_COUNT_ONLY;

	/*
	 * Since there could be multiple types of the same plugin we
	 * need to only make sure we load it once.
	 */
	if (!(context_ptr->config_flags & GRES_CONF_LOADED)) {
		/*
		 * Ignore return code, as we will still support the gres
		 * with or without the plugin.
		 */
		if (_load_plugin(context_ptr) == SLURM_SUCCESS)
			context_ptr->config_flags |= GRES_CONF_LOADED;
	}

	foreach_gres_conf->rec_count++;
	orig_has_file = gres_slurmd_conf->config_flags & GRES_CONF_HAS_FILE;
	if (foreach_gres_conf->new_has_file == -1) {
		if (gres_slurmd_conf->config_flags & GRES_CONF_HAS_FILE)
			foreach_gres_conf->new_has_file = 1;
		else
			foreach_gres_conf->new_has_file = 0;
	} else if ((foreach_gres_conf->new_has_file && !orig_has_file) ||
		   (!foreach_gres_conf->new_has_file && orig_has_file)) {
		fatal("gres.conf for %s, some records have \"File\" specification while others do not",
		      context_ptr->gres_name);
	}
	orig_has_type = gres_slurmd_conf->config_flags &
		GRES_CONF_HAS_TYPE;
	if (foreach_gres_conf->new_has_type == -1) {
		if (gres_slurmd_conf->config_flags &
		    GRES_CONF_HAS_TYPE) {
			foreach_gres_conf->new_has_type = 1;
		} else
			foreach_gres_conf->new_has_type = 0;
	} else if ((foreach_gres_conf->new_has_type && !orig_has_type) ||
		   (!foreach_gres_conf->new_has_type && orig_has_type)) {
		fatal("gres.conf for %s, some records have \"Type=\" specification while others do not",
		      context_ptr->gres_name);
	}

	if (!foreach_gres_conf->new_has_file &&
	    !foreach_gres_conf->new_has_type &&
	    (foreach_gres_conf->rec_count > 1)) {
		fatal("gres.conf duplicate records for %s",
		      context_ptr->gres_name);
	}

	if (foreach_gres_conf->new_has_file)
		context_ptr->config_flags |= GRES_CONF_HAS_FILE;

	return 0;
}

static void _validate_gres_conf(List gres_conf_list,
				slurm_gres_context_t *context_ptr)
{
	foreach_gres_conf_t gres_conf = {
		.context_ptr = context_ptr,
		.new_has_file = -1,
		.new_has_type = -1,
		.rec_count = 0,
	};

	(void)list_for_each_nobreak(gres_conf_list, _foreach_gres_conf,
				    &gres_conf);

	if (!(context_ptr->config_flags & GRES_CONF_LOADED)) {
		/*
		 * This means there was no gre.conf line for this gres found.
		 * We still need to try to load it for AutoDetect's sake.
		 * If we fail loading we will treat it as a count
		 * only GRES since the stepd will try to load it elsewise.
		 */
		if (_load_plugin(context_ptr) != SLURM_SUCCESS)
			context_ptr->config_flags |= GRES_CONF_COUNT_ONLY;
	} else
		/* Remove as this is only really used locally */
		context_ptr->config_flags &= (~GRES_CONF_LOADED);
}

/*
 * Keep track of which gres.conf lines have a count greater than expected
 * according to the current slurm.conf GRES. Modify the count of throw-away
 * records in gres_conf_list_tmp to keep track of this. Any gres.conf records
 * with a count > 0 means that slurm.conf did not account for it completely.
 *
 * gres_conf_list_tmp - (in/out) The temporary gres.conf list.
 * count              - (in) The count of the current slurm.conf GRES record.
 * type_name          - (in) The type of the current slurm.conf GRES record.
 */
static void _compare_conf_counts(List gres_conf_list_tmp, uint64_t count,
				 char *type_name)
{
	gres_slurmd_conf_t *gres_conf;
	ListIterator iter = list_iterator_create(gres_conf_list_tmp);
	while ((gres_conf = list_next(iter))) {
		/* Note: plugin type filter already applied */
		/* Check that type is the same */
		if (xstrcasecmp(gres_conf->type_name, type_name))
			continue;
		/* Keep track of counts */
		if (gres_conf->count > count) {
			gres_conf->count -= count;
			/* This slurm.conf GRES specification is now used up */
			list_iterator_destroy(iter);
			return;
		} else {
			count -= gres_conf->count;
			gres_conf->count = 0;
		}
	}
	list_iterator_destroy(iter);
}

/*
 * Loop through each entry in gres.conf and see if there is a corresponding
 * entry in slurm.conf. If so, see if the counts line up. If there are more
 * devices specified in gres.conf than in slurm.conf, emit errors.
 *
 * slurm_conf_list - (in) The slurm.conf GRES list.
 * gres_conf_list  - (in) The gres.conf GRES list.
 * context_ptr     - (in) Which GRES plugin we are currently working in.
 */
static void _check_conf_mismatch(List slurm_conf_list, List gres_conf_list,
				 slurm_gres_context_t *context_ptr)
{
	ListIterator iter;
	gres_slurmd_conf_t *gres_conf;
	gres_state_t *slurm_conf;
	List gres_conf_list_tmp;

	/* E.g. slurm_conf_list will be NULL in the case of --gpu-bind */
	if (!slurm_conf_list || !gres_conf_list)
		return;

	/*
	 * Duplicate the gres.conf list with records relevant to this GRES
	 * plugin only so we can mangle records. Only add records under the
	 * current plugin.
	 */
	gres_conf_list_tmp = list_create(destroy_gres_slurmd_conf);
	iter = list_iterator_create(gres_conf_list);
	while ((gres_conf = list_next(iter))) {
		gres_slurmd_conf_t *gres_conf_tmp;
		if (gres_conf->plugin_id != context_ptr->plugin_id)
			continue;

		gres_conf_tmp = xmalloc(sizeof(*gres_conf_tmp));
		gres_conf_tmp->name = xstrdup(gres_conf->name);
		gres_conf_tmp->type_name = xstrdup(gres_conf->type_name);
		gres_conf_tmp->count = gres_conf->count;
		list_append(gres_conf_list_tmp, gres_conf_tmp);
	}
	list_iterator_destroy(iter);

	/*
	 * Loop through the slurm.conf list and see if there are more gres.conf
	 * GRES than expected.
	 */
	iter = list_iterator_create(slurm_conf_list);
	while ((slurm_conf = list_next(iter))) {
		gres_node_state_t *slurm_gres;

		if (slurm_conf->plugin_id != context_ptr->plugin_id)
			continue;

		/* Determine if typed or untyped, and act accordingly */
		slurm_gres = (gres_node_state_t *)slurm_conf->gres_data;
		if (!slurm_gres->type_name) {
			_compare_conf_counts(gres_conf_list_tmp,
					     slurm_gres->gres_cnt_config, NULL);
			continue;
		}

		for (int i = 0; i < slurm_gres->type_cnt; ++i) {
			_compare_conf_counts(gres_conf_list_tmp,
					     slurm_gres->type_cnt_avail[i],
					     slurm_gres->type_name[i]);
		}
	}
	list_iterator_destroy(iter);

	/*
	 * Loop through gres_conf_list_tmp to print errors for gres.conf
	 * records that were not completely accounted for in slurm.conf.
	 */
	iter = list_iterator_create(gres_conf_list_tmp);
	while ((gres_conf = list_next(iter)))
		if (gres_conf->count > 0)
			info("WARNING: A line in gres.conf for GRES %s%s%s has %"PRIu64" more configured than expected in slurm.conf. Ignoring extra GRES.",
			     gres_conf->name,
			     (gres_conf->type_name) ? ":" : "",
			     (gres_conf->type_name) ? gres_conf->type_name : "",
			     gres_conf->count);
	list_iterator_destroy(iter);

	FREE_NULL_LIST(gres_conf_list_tmp);
}

/*
 * Match the type of a GRES from slurm.conf to a GRES in the gres.conf list. If
 * a match is found, pop it off the gres.conf list and return it.
 *
 * gres_conf_list - (in) The gres.conf list to search through.
 * gres_context   - (in) Which GRES plugin we are currently working in.
 * type_name      - (in) The type of the slurm.conf GRES record. If null, then
 *			 it's an untyped GRES.
 *
 * Returns the first gres.conf record from gres_conf_list with the same type
 * name as the slurm.conf record.
 */
static gres_slurmd_conf_t *_match_type(List gres_conf_list,
				       slurm_gres_context_t *gres_context,
				       char *type_name)
{
	ListIterator gres_conf_itr;
	gres_slurmd_conf_t *gres_conf = NULL;

	gres_conf_itr = list_iterator_create(gres_conf_list);
	while ((gres_conf = list_next(gres_conf_itr))) {
		if (gres_conf->plugin_id != gres_context->plugin_id)
			continue;

		/*
		 * If type_name is NULL we will take the first matching
		 * gres_conf that we find.  This means we also will remove the
		 * type from the gres_conf to match 18.08 stylings.
		 */
		if (!type_name)
			xfree(gres_conf->type_name);
		else if (xstrcasecmp(gres_conf->type_name, type_name))
			continue;

		/* We found a match, so remove from gres_conf_list and break */
		list_remove(gres_conf_itr);
		break;
	}
	list_iterator_destroy(gres_conf_itr);

	return gres_conf;
}

/*
 * Add a GRES conf record with count == 0 to gres_list.
 *
 * gres_list    - (in/out) The gres list to add to.
 * gres_context - (in) The GRES plugin to add a GRES record for.
 * cpu_cnt      - (in) The cpu count configured for the node.
 */
static void _add_gres_config_empty(List gres_list,
				   slurm_gres_context_t *gres_context,
				   uint32_t cpu_cnt)
{
	gres_slurmd_conf_t *gres_conf = xmalloc(sizeof(*gres_conf));
	gres_conf->cpu_cnt = cpu_cnt;
	gres_conf->name = xstrdup(gres_context->gres_name);
	gres_conf->plugin_id = gres_context->plugin_id;
	list_append(gres_list, gres_conf);
}

/*
 * Truncate the File hostrange string of a GRES record to be to be at most
 * new_count entries. The extra entries will be removed.
 *
 * gres_conf - (in/out) The GRES record to modify.
 * count     - (in) The new number of entries in File
 */
static void _set_file_subset(gres_slurmd_conf_t *gres_conf, uint64_t new_count)
{
	/* Convert file to hostrange */
	hostlist_t hl = hostlist_create(gres_conf->file);
	unsigned long old_count = hostlist_count(hl);

	if (new_count >= old_count) {
		hostlist_destroy(hl);
		/* Nothing to do */
		return;
	}

	/* Remove all but the first entries */
	for (int i = old_count; i > new_count; --i) {
		free(hostlist_pop(hl));
	}

	debug3("%s: Truncating %s:%s File from (%ld) %s", __func__,
	       gres_conf->name, gres_conf->type_name, old_count,
	       gres_conf->file);

	/* Set file to the new subset */
	xfree(gres_conf->file);
	gres_conf->file = hostlist_ranged_string_xmalloc(hl);

	debug3("%s: to (%"PRIu64") %s", __func__, new_count, gres_conf->file);
	hostlist_destroy(hl);
}

/*
 * A continuation of _merge_gres() depending on if the slurm.conf GRES is typed
 * or not.
 *
 * gres_conf_list - (in) The gres.conf list.
 * new_list       - (out) The new merged [slurm|gres].conf list.
 * count          - (in) The count of the slurm.conf GRES record.
 * type_name      - (in) The type of the slurm.conf GRES record, if it exists.
 * gres_context   - (in) Which GRES plugin we are working in.
 * cpu_cnt        - (in) A count of CPUs on the node.
 */
static void _merge_gres2(List gres_conf_list, List new_list, uint64_t count,
			 char *type_name, slurm_gres_context_t *gres_context,
			 uint32_t cpu_count)
{
	gres_slurmd_conf_t *gres_conf, *match;

	/* If slurm.conf count is initially 0, don't waste time on it */
	if (count == 0)
		return;

	/*
	 * There can be multiple gres.conf GRES lines contained within a
	 * single slurm.conf GRES line, due to different values of Cores
	 * and Links. Append them to the list where possible.
	 */
	while ((match = _match_type(gres_conf_list, gres_context, type_name))) {
		list_append(new_list, match);

		debug3("%s: From gres.conf, using %s:%s:%"PRIu64":%s", __func__,
		       match->name, match->type_name, match->count,
		       match->file);

		/* See if we need to merge with any more gres.conf records. */
		if (match->count > count) {
			/*
			 * Truncate excess count of gres.conf to match total
			 * count of slurm.conf.
			 */
			match->count = count;
			/*
			 * Truncate excess file of gres.conf to match total
			 * count of slurm.conf.
			 */
			if (match->file)
				_set_file_subset(match, count);
			/* Floor to 0 to break out of loop. */
			count = 0;
		} else
			/*
			 * Subtract this gres.conf line count from the
			 * slurm.conf total.
			 */
			count -= match->count;

		/*
		 * All devices outlined by this slurm.conf record have now been
		 * merged with gres.conf records and added to new_list, so exit.
		 */
		if (count == 0)
			break;
	}

	if (count == 0)
		return;

	/*
	 * There are leftover GRES specified in this slurm.conf record that are
	 * not accounted for in gres.conf that still need to be added.
	 */
	gres_conf = xmalloc(sizeof(*gres_conf));
	gres_conf->count = count;
	gres_conf->cpu_cnt = cpu_count;
	gres_conf->name = xstrdup(gres_context->gres_name);
	gres_conf->plugin_id = gres_context->plugin_id;
	if (type_name) {
		gres_conf->config_flags = GRES_CONF_HAS_TYPE;
		gres_conf->type_name = xstrdup(type_name);
	}

	if (gres_context->config_flags & GRES_CONF_COUNT_ONLY)
		gres_conf->config_flags |= GRES_CONF_COUNT_ONLY;

	list_append(new_list, gres_conf);
}

/*
 * Merge a single slurm.conf GRES specification with any relevant gres.conf
 * records and append the result to new_list.
 *
 * gres_conf_list - (in) The gres.conf list.
 * new_list       - (out) The new merged [slurm|gres].conf list.
 * ptr            - (in) A slurm.conf GRES record.
 * gres_context   - (in) Which GRES plugin we are working in.
 * cpu_cnt        - (in) A count of CPUs on the node.
 */
static void _merge_gres(List gres_conf_list, List new_list, gres_state_t *ptr,
			slurm_gres_context_t *gres_context, uint32_t cpu_cnt)
{
	gres_node_state_t *slurm_gres = (gres_node_state_t *)ptr->gres_data;

	/* If this GRES has no types, merge in the single untyped GRES */
	if (slurm_gres->type_cnt == 0) {
		_merge_gres2(gres_conf_list, new_list,
			     slurm_gres->gres_cnt_config, NULL, gres_context,
			     cpu_cnt);
		return;
	}

	/* If this GRES has types, merge in each typed GRES */
	for (int i = 0; i < slurm_gres->type_cnt; i++) {
		_merge_gres2(gres_conf_list, new_list,
			     slurm_gres->type_cnt_avail[i],
			     slurm_gres->type_name[i], gres_context, cpu_cnt);
	}
}

/*
 * Merge slurm.conf and gres.conf GRES configuration.
 * gres.conf can only work within what is outlined in slurm.conf. Every
 * gres.conf device that does not match up to a device in slurm.conf is
 * discarded with an error. If no gres conf found for what is specified in
 * slurm.conf, create a zero-count conf record.
 *
 * node_conf       - (in) node configuration info (cpu count).
 * gres_conf_list  - (in/out) GRES data from gres.conf. This becomes the new
 *		     merged slurm.conf/gres.conf list.
 * slurm_conf_list - (in) GRES data from slurm.conf.
 */
static void _merge_config(node_config_load_t *node_conf, List gres_conf_list,
			  List slurm_conf_list)
{
	int i;
	gres_state_t *gres_ptr;
	ListIterator iter;
	bool found;

	List new_gres_list = list_create(destroy_gres_slurmd_conf);

	for (i = 0; i < gres_context_cnt; i++) {
		/* Copy GRES configuration from slurm.conf */
		if (slurm_conf_list) {
			found = false;
			iter = list_iterator_create(slurm_conf_list);
			while ((gres_ptr = (gres_state_t *) list_next(iter))) {
				if (gres_ptr->plugin_id !=
				    gres_context[i].plugin_id)
					continue;
				found = true;
				_merge_gres(gres_conf_list, new_gres_list,
					    gres_ptr, &gres_context[i],
					    node_conf->cpu_cnt);
			}
			list_iterator_destroy(iter);
			if (found)
				continue;
		}

		/* Add GRES record with zero count */
		_add_gres_config_empty(new_gres_list, &gres_context[i],
				       node_conf->cpu_cnt);
	}
	/* Set gres_conf_list to be the new merged list */
	list_flush(gres_conf_list);
	list_transfer(gres_conf_list, new_gres_list);
	FREE_NULL_LIST(new_gres_list);
}

static void _pack_gres_context(slurm_gres_context_t *ctx, buf_t *buffer)
{
	/* ctx->cur_plugin: DON'T PACK will be filled in on the other side */
	pack8(ctx->config_flags, buffer);
	packstr(ctx->gres_name, buffer);
	packstr(ctx->gres_name_colon, buffer);
	pack32((uint32_t)ctx->gres_name_colon_len, buffer);
	packstr(ctx->gres_type, buffer);
	/* ctx->ops: DON'T PACK will be filled in on the other side */
	pack32(ctx->plugin_id, buffer);
	/* ctx->plugin_list: DON'T PACK will be filled in on the other side */
	pack64(ctx->total_cnt, buffer);
}

static int _unpack_gres_context(slurm_gres_context_t* ctx, buf_t *buffer)
{
	uint32_t uint32_tmp;

	/* ctx->cur_plugin: filled in later with _load_plugin() */
	safe_unpack8(&ctx->config_flags, buffer);
	safe_unpackstr_xmalloc(&ctx->gres_name, &uint32_tmp, buffer);
	safe_unpackstr_xmalloc(&ctx->gres_name_colon, &uint32_tmp, buffer);
	safe_unpack32(&uint32_tmp, buffer);
	ctx->gres_name_colon_len = (int)uint32_tmp;
	safe_unpackstr_xmalloc(&ctx->gres_type, &uint32_tmp, buffer);
	/* ctx->ops: filled in later with _load_plugin() */
	safe_unpack32(&ctx->plugin_id, buffer);
	/* ctx->plugin_list: filled in later with _load_plugin() */
	safe_unpack64(&ctx->total_cnt, buffer);
	return SLURM_SUCCESS;

unpack_error:
	error("%s: unpack_error", __func__);
	return SLURM_ERROR;
}

static void _pack_gres_slurmd_conf(void *in, uint16_t protocol_version,
				   buf_t *buffer)
{
	gres_slurmd_conf_t *gres_conf = (gres_slurmd_conf_t *)in;

	/* Pack gres_slurmd_conf_t */
	pack8(gres_conf->config_flags, buffer);
	pack64(gres_conf->count, buffer);
	pack32(gres_conf->cpu_cnt, buffer);
	packstr(gres_conf->cpus, buffer);
	pack_bit_str_hex(gres_conf->cpus_bitmap, buffer);
	packstr(gres_conf->file, buffer);
	packstr(gres_conf->links, buffer);
	packstr(gres_conf->name, buffer);
	packstr(gres_conf->type_name, buffer);
	pack32(gres_conf->plugin_id, buffer);
}

static int _unpack_gres_slurmd_conf(void **object, uint16_t protocol_version,
				    buf_t *buffer)
{
	uint32_t uint32_tmp;
	gres_slurmd_conf_t *gres_conf = xmalloc(sizeof(*gres_conf));

	/* Unpack gres_slurmd_conf_t */
	safe_unpack8(&gres_conf->config_flags, buffer);
	safe_unpack64(&gres_conf->count, buffer);
	safe_unpack32(&gres_conf->cpu_cnt, buffer);
	safe_unpackstr_xmalloc(&gres_conf->cpus, &uint32_tmp, buffer);
	unpack_bit_str_hex(&gres_conf->cpus_bitmap, buffer);
	safe_unpackstr_xmalloc(&gres_conf->file, &uint32_tmp, buffer);
	safe_unpackstr_xmalloc(&gres_conf->links, &uint32_tmp, buffer);
	safe_unpackstr_xmalloc(&gres_conf->name, &uint32_tmp, buffer);
	safe_unpackstr_xmalloc(&gres_conf->type_name, &uint32_tmp, buffer);
	safe_unpack32(&gres_conf->plugin_id, buffer);

	*object = gres_conf;
	return SLURM_SUCCESS;

unpack_error:
	destroy_gres_slurmd_conf(gres_conf);
	*object = NULL;
	return SLURM_ERROR;
}

/* gres_context_lock should be locked before this */
static void _pack_context_buf(void)
{
	FREE_NULL_BUFFER(gres_context_buf);

	gres_context_buf = init_buf(0);
	pack32(gres_context_cnt, gres_context_buf);
	if (gres_context_cnt <= 0) {
		debug3("%s: No GRES context count sent to stepd", __func__);
		return;
	}

	for (int i = 0; i < gres_context_cnt; i++) {
		slurm_gres_context_t *ctx = &gres_context[i];
		_pack_gres_context(ctx, gres_context_buf);
		if (ctx->ops.send_stepd)
			(*(ctx->ops.send_stepd))(gres_context_buf);
	}
}

static int _unpack_context_buf(buf_t *buffer)
{
	uint32_t cnt;
	safe_unpack32(&cnt, buffer);

	gres_context_cnt = cnt;

	if (!gres_context_cnt)
		return SLURM_SUCCESS;

	xrecalloc(gres_context, gres_context_cnt, sizeof(slurm_gres_context_t));
	for (int i = 0; i < gres_context_cnt; i++) {
		slurm_gres_context_t *ctx = &gres_context[i];
		if (_unpack_gres_context(ctx, buffer) != SLURM_SUCCESS)
			goto unpack_error;
		(void)_load_plugin(ctx);
		if (ctx->ops.recv_stepd)
			(*(ctx->ops.recv_stepd))(buffer);
	}
	return SLURM_SUCCESS;

unpack_error:
	error("%s: failed", __func__);
	return SLURM_ERROR;
}

/* gres_context_lock should be locked before this */
static void _pack_gres_conf(void)
{
	int len = 0;
	FREE_NULL_BUFFER(gres_conf_buf);

	gres_conf_buf = init_buf(0);
	pack32(autodetect_flags, gres_conf_buf);

	/* If there is no list to send, let the stepd know */
	if (!gres_conf_list || !(len = list_count(gres_conf_list))) {
		pack32(len, gres_conf_buf);
		return;
	}
	pack32(len, gres_conf_buf);

	if (slurm_pack_list(gres_conf_list, _pack_gres_slurmd_conf,
			    gres_conf_buf, SLURM_PROTOCOL_VERSION)
	    != SLURM_SUCCESS) {
		error("%s: Failed to pack gres_conf_list", __func__);
		return;
	}
}

static int _unpack_gres_conf(buf_t *buffer)
{
	uint32_t cnt;
	safe_unpack32(&cnt, buffer);
	autodetect_flags = cnt;
	safe_unpack32(&cnt, buffer);

	if (!cnt)
		return SLURM_SUCCESS;

	if (slurm_unpack_list(&gres_conf_list, _unpack_gres_slurmd_conf,
			      destroy_gres_slurmd_conf, buffer,
			      SLURM_PROTOCOL_VERSION) != SLURM_SUCCESS)
		goto unpack_error;

	return SLURM_SUCCESS;

unpack_error:
	error("%s: failed", __func__);
	return SLURM_ERROR;
}

/*
 * Load this node's configuration (how many resources it has, topology, etc.)
 * IN cpu_cnt - Number of CPUs configured on this node
 * IN node_name - Name of this node
 * IN gres_list - Node's GRES information as loaded from slurm.conf by slurmd
 * IN xcpuinfo_abs_to_mac - Pointer to xcpuinfo_abs_to_mac() funct. If
 *	specified, Slurm will convert gres_slurmd_conf->cpus_bitmap (a bitmap
 *	derived from gres.conf's "Cores" range string) into machine format
 *	(normal slrumd/stepd operation). If not, it will remain unconverted (for
 *	testing purposes or when unused).
 * IN xcpuinfo_mac_to_abs - Pointer to xcpuinfo_mac_to_abs() funct. Used to
 *	convert CPU affinities from machine format (as collected from NVML and
 *	others) into abstract format, for sanity checking purposes.
 * NOTE: Called from slurmd and slurmstepd
 */
extern int gres_g_node_config_load(uint32_t cpu_cnt, char *node_name,
				   List gres_list,
				   void *xcpuinfo_abs_to_mac,
				   void *xcpuinfo_mac_to_abs)
{
	static s_p_options_t _gres_options[] = {
		{"AutoDetect", S_P_STRING},
		{"Name",     S_P_ARRAY, _parse_gres_config,  NULL},
		{"NodeName", S_P_ARRAY, _parse_gres_config2, NULL},
		{NULL}
	};

	int count = 0, i, rc, rc2;
	struct stat config_stat;
	s_p_hashtbl_t *tbl;
	gres_slurmd_conf_t **gres_array;
	char *gres_conf_file;
	char *autodetect_string = NULL;

	node_config_load_t node_conf = {
		.cpu_cnt = cpu_cnt,
		.xcpuinfo_mac_to_abs = xcpuinfo_mac_to_abs
	};

	if (cpu_cnt == 0) {
		error("%s: Invalid cpu_cnt of 0 for node %s",
		      __func__, node_name);
		return ESLURM_INVALID_CPU_COUNT;
	}

	if (xcpuinfo_abs_to_mac)
		xcpuinfo_ops.xcpuinfo_abs_to_mac = xcpuinfo_abs_to_mac;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);

	if (gres_context_cnt == 0) {
		rc = SLURM_SUCCESS;
		goto fini;
	}

	FREE_NULL_LIST(gres_conf_list);
	gres_conf_list = list_create(destroy_gres_slurmd_conf);
	gres_conf_file = get_extra_conf_path("gres.conf");
	if (stat(gres_conf_file, &config_stat) < 0) {
		info("Can not stat gres.conf file (%s), using slurm.conf data",
		     gres_conf_file);
	} else {
		if (xstrcmp(gres_node_name, node_name)) {
			xfree(gres_node_name);
			gres_node_name = xstrdup(node_name);
		}

		gres_cpu_cnt = cpu_cnt;
		tbl = s_p_hashtbl_create(_gres_options);
		if (s_p_parse_file(tbl, NULL, gres_conf_file, false) ==
		    SLURM_ERROR)
			fatal("error opening/reading %s", gres_conf_file);

		/* Overwrite unspecified local AutoDetect with global default */
		if (s_p_get_string(&autodetect_string, "Autodetect", tbl)) {
			_handle_global_autodetect(autodetect_string);
			xfree(autodetect_string);
		}

		if (s_p_get_array((void ***) &gres_array,
				  &count, "Name", tbl)) {
			for (i = 0; i < count; i++) {
				list_append(gres_conf_list, gres_array[i]);
				gres_array[i] = NULL;
			}
		}
		if (s_p_get_array((void ***) &gres_array,
				  &count, "NodeName", tbl)) {
			for (i = 0; i < count; i++) {
				list_append(gres_conf_list, gres_array[i]);
				gres_array[i] = NULL;
			}
		}
		s_p_hashtbl_destroy(tbl);
	}
	xfree(gres_conf_file);

	/* Validate gres.conf and slurm.conf somewhat before merging */
	for (i = 0; i < gres_context_cnt; i++) {
		_validate_slurm_conf(gres_list, &gres_context[i]);
		_validate_gres_conf(gres_conf_list, &gres_context[i]);
		_check_conf_mismatch(gres_list, gres_conf_list,
				     &gres_context[i]);
	}

	/* Merge slurm.conf and gres.conf together into gres_conf_list */
	_merge_config(&node_conf, gres_conf_list, gres_list);

	for (i = 0; i < gres_context_cnt; i++) {
		if (gres_context[i].ops.node_config_load == NULL)
			continue;	/* No plugin */
		rc2 = (*(gres_context[i].ops.node_config_load))(gres_conf_list,
								&node_conf);
		if (rc == SLURM_SUCCESS)
			rc = rc2;

	}

	/* Postprocess gres_conf_list after all plugins' node_config_load */

	/* Remove every GPU with an empty File */
	(void) list_delete_all(gres_conf_list, _find_fileless_gres,
			       &gpu_plugin_id);

	list_for_each(gres_conf_list, _log_gres_slurmd_conf, NULL);

fini:
	_pack_context_buf();
	_pack_gres_conf();
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Pack this node's gres configuration into a buffer
 * IN/OUT buffer - message buffer to pack
 */
extern int gres_node_config_pack(buf_t *buffer)
{
	int rc;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0, version = SLURM_PROTOCOL_VERSION;
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	pack16(version, buffer);
	if (gres_conf_list)
		rec_cnt = list_count(gres_conf_list);
	pack16(rec_cnt, buffer);
	if (rec_cnt) {
		iter = list_iterator_create(gres_conf_list);
		while ((gres_slurmd_conf =
			(gres_slurmd_conf_t *) list_next(iter))) {
			pack32(magic, buffer);
			pack64(gres_slurmd_conf->count, buffer);
			pack32(gres_slurmd_conf->cpu_cnt, buffer);
			pack8(gres_slurmd_conf->config_flags, buffer);
			pack32(gres_slurmd_conf->plugin_id, buffer);
			packstr(gres_slurmd_conf->cpus, buffer);
			packstr(gres_slurmd_conf->links, buffer);
			packstr(gres_slurmd_conf->name, buffer);
			packstr(gres_slurmd_conf->type_name, buffer);
		}
		list_iterator_destroy(iter);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Unpack this node's configuration from a buffer (built/packed by slurmd)
 * IN/OUT buffer - message buffer to unpack
 * IN node_name - name of node whose data is being unpacked
 */
extern int gres_node_config_unpack(buf_t *buffer, char *node_name)
{
	int i, j, rc;
	uint32_t cpu_cnt = 0, magic = 0, plugin_id = 0, utmp32 = 0;
	uint64_t count64 = 0;
	uint16_t rec_cnt = 0, protocol_version = 0;
	uint8_t config_flags = 0;
	char *tmp_cpus = NULL, *tmp_links = NULL, *tmp_name = NULL;
	char *tmp_type = NULL;
	gres_slurmd_conf_t *p;

	rc = gres_init();

	FREE_NULL_LIST(gres_conf_list);
	gres_conf_list = list_create(destroy_gres_slurmd_conf);

	safe_unpack16(&protocol_version, buffer);

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;
	if (rec_cnt > NO_VAL16)
		goto unpack_error;

	slurm_mutex_lock(&gres_context_lock);
	if (protocol_version < SLURM_MIN_PROTOCOL_VERSION) {
		error("%s: protocol_version %hu not supported",
		      __func__, protocol_version);
		goto unpack_error;
	}
	for (i = 0; i < rec_cnt; i++) {
		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;

			safe_unpack64(&count64, buffer);
			safe_unpack32(&cpu_cnt, buffer);
			safe_unpack8(&config_flags, buffer);
			safe_unpack32(&plugin_id, buffer);
			safe_unpackstr_xmalloc(&tmp_cpus, &utmp32, buffer);
			safe_unpackstr_xmalloc(&tmp_links, &utmp32, buffer);
			safe_unpackstr_xmalloc(&tmp_name, &utmp32, buffer);
			safe_unpackstr_xmalloc(&tmp_type, &utmp32, buffer);
		}

		log_flag(GRES, "Node:%s Gres:%s Type:%s Flags:%s CPU_IDs:%s CPU#:%u Count:%"PRIu64" Links:%s",
			 node_name, tmp_name, tmp_type,
			 gres_flags2str(config_flags), tmp_cpus, cpu_cnt,
			 count64, tmp_links);

		for (j = 0; j < gres_context_cnt; j++) {
			bool new_has_file,  new_has_type;
			bool orig_has_file, orig_has_type;
			if (gres_context[j].plugin_id != plugin_id)
				continue;
			if (xstrcmp(gres_context[j].gres_name, tmp_name)) {
				/*
				 * Should have been caught in
				 * gres_init()
				 */
				error("%s: gres/%s duplicate plugin ID with %s, unable to process",
				      __func__, tmp_name,
				      gres_context[j].gres_name);
				continue;
			}
			new_has_file  = config_flags & GRES_CONF_HAS_FILE;
			orig_has_file = gres_context[j].config_flags &
				GRES_CONF_HAS_FILE;
			if (orig_has_file && !new_has_file && count64) {
				error("%s: gres/%s lacks \"File=\" parameter for node %s",
				      __func__, tmp_name, node_name);
				config_flags |= GRES_CONF_HAS_FILE;
			}
			if (new_has_file && (count64 > MAX_GRES_BITMAP)) {
				/*
				 * Avoid over-subscribing memory with
				 * huge bitmaps
				 */
				error("%s: gres/%s has \"File=\" plus very large "
				      "\"Count\" (%"PRIu64") for node %s, "
				      "resetting value to %d",
				      __func__, tmp_name, count64,
				      node_name, MAX_GRES_BITMAP);
				count64 = MAX_GRES_BITMAP;
			}
			new_has_type  = config_flags & GRES_CONF_HAS_TYPE;
			orig_has_type = gres_context[j].config_flags &
				GRES_CONF_HAS_TYPE;
			if (orig_has_type && !new_has_type && count64) {
				error("%s: gres/%s lacks \"Type\" parameter for node %s",
				      __func__, tmp_name, node_name);
				config_flags |= GRES_CONF_HAS_TYPE;
			}
			gres_context[j].config_flags |= config_flags;

			/*
			 * On the slurmctld we need to load the plugins to
			 * correctly set env vars.  We want to call this only
			 * after we have the config_flags so we can tell if we
			 * are CountOnly or not.
			 */
			if (!(gres_context[j].config_flags &
			      GRES_CONF_LOADED)) {
				(void)_load_plugin(&gres_context[j]);
				gres_context[j].config_flags |=
					GRES_CONF_LOADED;
			}

			break;
		}
		if (j >= gres_context_cnt) {
			/*
			 * GresPlugins is inconsistently configured.
			 * Not a fatal error, but skip this data.
			 */
			error("%s: No plugin configured to process GRES data from node %s (Name:%s Type:%s PluginID:%u Count:%"PRIu64")",
			      __func__, node_name, tmp_name, tmp_type,
			      plugin_id, count64);
			xfree(tmp_cpus);
			xfree(tmp_links);
			xfree(tmp_name);
			xfree(tmp_type);
			continue;
		}
		p = xmalloc(sizeof(gres_slurmd_conf_t));
		p->config_flags = config_flags;
		p->count = count64;
		p->cpu_cnt = cpu_cnt;
		p->cpus = tmp_cpus;
		tmp_cpus = NULL;	/* Nothing left to xfree */
		p->links = tmp_links;
		tmp_links = NULL;	/* Nothing left to xfree */
		p->name = tmp_name;     /* Preserve for accounting! */
		p->type_name = tmp_type;
		tmp_type = NULL;	/* Nothing left to xfree */
		p->plugin_id = plugin_id;
		_validate_links(p);
		list_append(gres_conf_list, p);
	}

	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("%s: unpack error from node %s", __func__, node_name);
	xfree(tmp_cpus);
	xfree(tmp_links);
	xfree(tmp_name);
	xfree(tmp_type);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

static void _gres_state_delete_members(void *x)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;

	if (!gres_ptr)
		return;

	xfree(gres_ptr->gres_name);
	xassert(!gres_ptr->gres_data); /* This must be freed beforehand */
	xfree(gres_ptr);
}

static void _gres_node_state_delete_topo(gres_node_state_t *gres_node_ptr)
{
	int i;

	for (i = 0; i < gres_node_ptr->topo_cnt; i++) {
		if (gres_node_ptr->topo_gres_bitmap)
			FREE_NULL_BITMAP(gres_node_ptr->topo_gres_bitmap[i]);
		if (gres_node_ptr->topo_core_bitmap)
			FREE_NULL_BITMAP(gres_node_ptr->topo_core_bitmap[i]);
		xfree(gres_node_ptr->topo_type_name[i]);
	}
	xfree(gres_node_ptr->topo_gres_bitmap);
	xfree(gres_node_ptr->topo_core_bitmap);
	xfree(gres_node_ptr->topo_gres_cnt_alloc);
	xfree(gres_node_ptr->topo_gres_cnt_avail);
	xfree(gres_node_ptr->topo_type_id);
	xfree(gres_node_ptr->topo_type_name);
}

static void _gres_node_state_delete(gres_node_state_t *gres_node_ptr)
{
	int i;

	FREE_NULL_BITMAP(gres_node_ptr->gres_bit_alloc);
	xfree(gres_node_ptr->gres_used);
	if (gres_node_ptr->links_cnt) {
		for (i = 0; i < gres_node_ptr->link_len; i++)
			xfree(gres_node_ptr->links_cnt[i]);
		xfree(gres_node_ptr->links_cnt);
	}

	_gres_node_state_delete_topo(gres_node_ptr);

	for (i = 0; i < gres_node_ptr->type_cnt; i++) {
		xfree(gres_node_ptr->type_name[i]);
	}
	xfree(gres_node_ptr->type_cnt_alloc);
	xfree(gres_node_ptr->type_cnt_avail);
	xfree(gres_node_ptr->type_id);
	xfree(gres_node_ptr->type_name);
	xfree(gres_node_ptr);
}

/*
 * Delete an element placed on gres_list by _node_config_validate()
 * free associated memory
 */
static void _gres_node_list_delete(void *list_element)
{
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_ptr;

	gres_ptr = (gres_state_t *) list_element;
	gres_node_ptr = (gres_node_state_t *) gres_ptr->gres_data;
	_gres_node_state_delete(gres_node_ptr);
	gres_ptr->gres_data = NULL;
	_gres_state_delete_members(gres_ptr);
}

extern void gres_add_type(char *type, gres_node_state_t *gres_data,
			  uint64_t tmp_gres_cnt)
{
	int i;
	uint32_t type_id;

	if (!xstrcasecmp(type, "no_consume")) {
		gres_data->no_consume = true;
		return;
	}

	type_id = gres_build_id(type);
	for (i = 0; i < gres_data->type_cnt; i++) {
		if (gres_data->type_id[i] != type_id)
			continue;
		gres_data->type_cnt_avail[i] += tmp_gres_cnt;
		break;
	}

	if (i >= gres_data->type_cnt) {
		gres_data->type_cnt++;
		gres_data->type_cnt_alloc =
			xrealloc(gres_data->type_cnt_alloc,
				 sizeof(uint64_t) * gres_data->type_cnt);
		gres_data->type_cnt_avail =
			xrealloc(gres_data->type_cnt_avail,
				 sizeof(uint64_t) * gres_data->type_cnt);
		gres_data->type_id =
			xrealloc(gres_data->type_id,
				 sizeof(uint32_t) * gres_data->type_cnt);
		gres_data->type_name =
			xrealloc(gres_data->type_name,
				 sizeof(char *) * gres_data->type_cnt);
		gres_data->type_cnt_avail[i] += tmp_gres_cnt;
		gres_data->type_id[i] = type_id;
		gres_data->type_name[i] = xstrdup(type);
	}
}

/*
 * Compute the total GRES count for a particular gres_name.
 * Note that a given gres_name can appear multiple times in the orig_config
 * string for multiple types (e.g. "gres=gpu:kepler:1,gpu:tesla:2").
 * IN/OUT gres_data - set gres_cnt_config field in this structure
 * IN orig_config - gres configuration from slurm.conf
 * IN gres_name - name of the gres type (e.g. "gpu")
 * IN gres_name_colon - gres name with appended colon
 * IN gres_name_colon_len - size of gres_name_colon
 * RET - Total configured count for this GRES type
 */
static void _get_gres_cnt(gres_node_state_t *gres_data, char *orig_config,
			  char *gres_name, char *gres_name_colon,
			  int gres_name_colon_len)
{
	char *node_gres_config, *tok, *last_tok = NULL;
	char *sub_tok, *last_sub_tok = NULL;
	char *num, *paren, *last_num = NULL;
	uint64_t gres_config_cnt = 0, tmp_gres_cnt = 0, mult;
	int i;

	xassert(gres_data);
	if (orig_config == NULL) {
		gres_data->gres_cnt_config = 0;
		return;
	}

	for (i = 0; i < gres_data->type_cnt; i++) {
		gres_data->type_cnt_avail[i] = 0;
	}

	node_gres_config = xstrdup(orig_config);
	tok = strtok_r(node_gres_config, ",", &last_tok);
	while (tok) {
		if (!xstrcmp(tok, gres_name)) {
			gres_config_cnt = 1;
			break;
		}
		if (!xstrncmp(tok, gres_name_colon, gres_name_colon_len)) {
			paren = strrchr(tok, '(');
			if (paren)	/* Ignore socket binding info */
				paren[0] = '\0';
			num = strrchr(tok, ':');
			if (!num) {
				error("Bad GRES configuration: %s", tok);
				break;
			}
			tmp_gres_cnt = strtoll(num + 1, &last_num, 10);
			if ((num[1] < '0') || (num[1] > '9')) {
				/*
				 * Type name, no count (e.g. "gpu:tesla").
				 * assume count of 1.
				 */
				tmp_gres_cnt = 1;
			} else if ((mult = suffix_mult(last_num)) != NO_VAL64) {
				tmp_gres_cnt *= mult;
				num[0] = '\0';
			} else {
				error("Bad GRES configuration: %s", tok);
				break;
			}

			gres_config_cnt += tmp_gres_cnt;

			sub_tok = strtok_r(tok, ":", &last_sub_tok);
			if (sub_tok)	/* Skip GRES name */
				sub_tok = strtok_r(NULL, ":", &last_sub_tok);
			while (sub_tok) {
				gres_add_type(sub_tok, gres_data,
					      tmp_gres_cnt);
				sub_tok = strtok_r(NULL, ":", &last_sub_tok);
			}
		}
		tok = strtok_r(NULL, ",", &last_tok);
	}
	xfree(node_gres_config);

	gres_data->gres_cnt_config = gres_config_cnt;
}

static int _valid_gres_type(char *gres_name, gres_node_state_t *gres_data,
			    bool config_overrides, char **reason_down)
{
	int i, j;
	uint64_t model_cnt;

	if (gres_data->type_cnt == 0)
		return 0;

	for (i = 0; i < gres_data->type_cnt; i++) {
		model_cnt = 0;
		if (gres_data->type_cnt) {
			for (j = 0; j < gres_data->type_cnt; j++) {
				if (gres_data->type_id[i] ==
				    gres_data->type_id[j])
					model_cnt +=
						gres_data->type_cnt_avail[j];
			}
		} else {
			for (j = 0; j < gres_data->topo_cnt; j++) {
				if (gres_data->type_id[i] ==
				    gres_data->topo_type_id[j])
					model_cnt += gres_data->
						topo_gres_cnt_avail[j];
			}
		}
		if (config_overrides) {
			gres_data->type_cnt_avail[i] = model_cnt;
		} else if (model_cnt < gres_data->type_cnt_avail[i]) {
			if (reason_down) {
				xstrfmtcat(*reason_down,
					   "%s:%s count too low "
					   "(%"PRIu64" < %"PRIu64")",
					   gres_name, gres_data->type_name[i],
					   model_cnt,
					   gres_data->type_cnt_avail[i]);
			}
			return -1;
		}
	}
	return 0;
}

static gres_node_state_t *_build_gres_node_state(void)
{
	gres_node_state_t *gres_data;

	gres_data = xmalloc(sizeof(gres_node_state_t));
	gres_data->gres_cnt_config = NO_VAL64;
	gres_data->gres_cnt_found  = NO_VAL64;

	return gres_data;
}

/*
 * Build a node's gres record based only upon the slurm.conf contents
 */
static int _node_config_init(char *node_name, char *orig_config,
			     slurm_gres_context_t *context_ptr,
			     gres_state_t *gres_ptr)
{
	int rc = SLURM_SUCCESS;
	gres_node_state_t *gres_data;

	if (!gres_ptr->gres_data)
		gres_ptr->gres_data = _build_gres_node_state();
	gres_data = (gres_node_state_t *) gres_ptr->gres_data;

	/* If the resource isn't configured for use with this node */
	if ((orig_config == NULL) || (orig_config[0] == '\0')) {
		gres_data->gres_cnt_config = 0;
		return rc;
	}

	_get_gres_cnt(gres_data, orig_config,
		      context_ptr->gres_name,
		      context_ptr->gres_name_colon,
		      context_ptr->gres_name_colon_len);

	context_ptr->total_cnt += gres_data->gres_cnt_config;

	/* Use count from recovered state, if higher */
	gres_data->gres_cnt_avail = MAX(gres_data->gres_cnt_avail,
					gres_data->gres_cnt_config);
	if ((gres_data->gres_bit_alloc != NULL) &&
	    (gres_data->gres_cnt_avail >
	     bit_size(gres_data->gres_bit_alloc)) &&
	    !gres_id_shared(context_ptr->plugin_id)) {
		gres_data->gres_bit_alloc =
			bit_realloc(gres_data->gres_bit_alloc,
				    gres_data->gres_cnt_avail);
	}

	return rc;
}

/*
 * Build a node's gres record based only upon the slurm.conf contents
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT gres_list - List of Gres records for this node to track usage
 */
extern int gres_init_node_config(char *node_name, char *orig_config,
				 List *gres_list)
{
	int i, rc, rc2;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
	}
	for (i = 0; i < gres_context_cnt; i++) {
		/* Find or create gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_ptr->plugin_id == gres_context[i].plugin_id)
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = gres_context[i].plugin_id;
			gres_ptr->gres_name =
				xstrdup(gres_context[i].gres_name);
			gres_ptr->state_type = GRES_STATE_TYPE_NODE;
			list_append(*gres_list, gres_ptr);
		}

		rc2 = _node_config_init(node_name, orig_config,
					&gres_context[i], gres_ptr);
		if (rc == SLURM_SUCCESS)
			rc = rc2;
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Determine GRES availability on some node
 * plugin_id IN - plugin number to search for
 * topo_cnt OUT - count of gres.conf records of this ID found by slurmd
 *		  (each can have different topology)
 * config_type_cnt OUT - Count of records for this GRES found in configuration,
 *		  each of this represesents a different Type of of GRES with
 *		  with this name (e.g. GPU model)
 * RET - total number of GRES available of this ID on this node in (sum
 *	 across all records of this ID)
 */
static uint64_t _get_tot_gres_cnt(uint32_t plugin_id, uint64_t *topo_cnt,
				  int *config_type_cnt)
{
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
	uint32_t cpu_set_cnt = 0, rec_cnt = 0;
	uint64_t gres_cnt = 0;

	xassert(config_type_cnt);
	xassert(topo_cnt);
	*config_type_cnt = 0;
	*topo_cnt = 0;
	if (gres_conf_list == NULL)
		return gres_cnt;

	iter = list_iterator_create(gres_conf_list);
	while ((gres_slurmd_conf = (gres_slurmd_conf_t *) list_next(iter))) {
		if (gres_slurmd_conf->plugin_id != plugin_id)
			continue;
		gres_cnt += gres_slurmd_conf->count;
		rec_cnt++;
		if (gres_slurmd_conf->cpus || gres_slurmd_conf->type_name)
			cpu_set_cnt++;
	}
	list_iterator_destroy(iter);
	*config_type_cnt = rec_cnt;
	if (cpu_set_cnt)
		*topo_cnt = rec_cnt;
	return gres_cnt;
}

/* Convert comma-delimited array of link counts to an integer array */
static void _links_str2array(char *links, char *node_name,
			     gres_node_state_t *gres_data,
			     int gres_inx, int gres_cnt)
{
	char *start_ptr, *end_ptr = NULL;
	int i = 0;

	if (!links)	/* No "Links=" data */
		return;
	if (gres_inx >= gres_data->link_len) {
		error("%s: Invalid GRES index (%d >= %d)", __func__, gres_inx,
		      gres_cnt);
		return;
	}

	start_ptr = links;
	while (1) {
		gres_data->links_cnt[gres_inx][i] =
			strtol(start_ptr, &end_ptr, 10);
		if (gres_data->links_cnt[gres_inx][i] < -2) {
			error("%s: Invalid GRES Links value (%s) on node %s:"
			      "Link value '%d' < -2", __func__, links,
			      node_name, gres_data->links_cnt[gres_inx][i]);
			gres_data->links_cnt[gres_inx][i] = 0;
			return;
		}
		if (end_ptr[0] == '\0')
			return;
		if (end_ptr[0] != ',') {
			error("%s: Invalid GRES Links value (%s) on node %s:"
			      "end_ptr[0]='%c' != ','", __func__, links,
			      node_name, end_ptr[0]);
			return;
		}
		if (++i >= gres_data->link_len) {
			error("%s: Invalid GRES Links value (%s) on node %s:"
			      "i=%d >= link_len=%d", __func__, links, node_name,
			      i, gres_data->link_len);
			return;
		}
		start_ptr = end_ptr + 1;
	}
}

static bool _valid_gres_types(char *gres_name, gres_node_state_t *gres_data,
			      char **reason_down)
{
	bool rc = true;
	uint64_t gres_cnt_found = 0, gres_sum;
	int topo_inx, type_inx;

	if ((gres_data->type_cnt == 0) || (gres_data->topo_cnt == 0))
		return rc;

	for (type_inx = 0; type_inx < gres_data->type_cnt; type_inx++) {
		gres_cnt_found = 0;
		for (topo_inx = 0; topo_inx < gres_data->topo_cnt; topo_inx++) {
			if (gres_data->topo_type_id[topo_inx] !=
			    gres_data->type_id[type_inx])
				continue;
			gres_sum = gres_cnt_found +
				gres_data->topo_gres_cnt_avail[topo_inx];
			if (gres_sum > gres_data->type_cnt_avail[type_inx]) {
				gres_data->topo_gres_cnt_avail[topo_inx] -=
					(gres_sum -
					 gres_data->type_cnt_avail[type_inx]);
			}
			gres_cnt_found +=
				gres_data->topo_gres_cnt_avail[topo_inx];
		}
		if (gres_cnt_found < gres_data->type_cnt_avail[type_inx]) {
			rc = false;
			break;
		}
	}
	if (!rc && reason_down && (*reason_down == NULL)) {
		xstrfmtcat(*reason_down,
			   "%s:%s count too low (%"PRIu64" < %"PRIu64")",
			   gres_name, gres_data->type_name[type_inx],
			   gres_cnt_found, gres_data->type_cnt_avail[type_inx]);
	}

	return rc;
}

static void _gres_bit_alloc_resize(gres_node_state_t *gres_data,
				   uint64_t gres_bits)
{
	if (!gres_bits) {
		FREE_NULL_BITMAP(gres_data->gres_bit_alloc);
		return;
	}

	if (!gres_data->gres_bit_alloc)
		gres_data->gres_bit_alloc = bit_alloc(gres_bits);
	else if (gres_bits != bit_size(gres_data->gres_bit_alloc))
		gres_data->gres_bit_alloc =
			bit_realloc(gres_data->gres_bit_alloc, gres_bits);
}

static int _node_config_validate(char *node_name, char *orig_config,
				 gres_state_t *gres_ptr,
				 int cpu_cnt, int core_cnt, int sock_cnt,
				 bool config_overrides, char **reason_down,
				 slurm_gres_context_t *context_ptr)
{
	int cpus_config = 0, i, j, gres_inx, rc = SLURM_SUCCESS;
	int config_type_cnt = 0;
	uint64_t dev_cnt, gres_cnt, topo_cnt = 0;
	bool cpu_config_err = false, updated_config = false;
	gres_node_state_t *gres_data;
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
	bool has_file, has_type, rebuild_topo = false;
	uint32_t type_id;

	xassert(core_cnt);
	if (gres_ptr->gres_data == NULL)
		gres_ptr->gres_data = _build_gres_node_state();
	gres_data = (gres_node_state_t *) gres_ptr->gres_data;
	if (gres_data->node_feature)
		return rc;

	gres_cnt = _get_tot_gres_cnt(context_ptr->plugin_id, &topo_cnt,
				     &config_type_cnt);
	if ((gres_data->gres_cnt_config > gres_cnt) && !config_overrides) {
		if (reason_down && (*reason_down == NULL)) {
			xstrfmtcat(*reason_down,
				   "%s count reported lower than configured "
				   "(%"PRIu64" < %"PRIu64")",
				   context_ptr->gres_type,
				   gres_cnt, gres_data->gres_cnt_config);
		}
		rc = EINVAL;
	}
	if ((gres_cnt > gres_data->gres_cnt_config)) {
		debug("%s: %s: Ignoring excess count on node %s (%"
		      PRIu64" > %"PRIu64")",
		      __func__, context_ptr->gres_type, node_name, gres_cnt,
		      gres_data->gres_cnt_config);
		gres_cnt = gres_data->gres_cnt_config;
	}
	if (gres_data->gres_cnt_found != gres_cnt) {
		if (gres_data->gres_cnt_found != NO_VAL64) {
			info("%s: %s: Count changed on node %s (%"PRIu64" != %"PRIu64")",
			     __func__, context_ptr->gres_type, node_name,
			     gres_data->gres_cnt_found, gres_cnt);
		}
		if ((gres_data->gres_cnt_found != NO_VAL64) &&
		    (gres_data->gres_cnt_alloc != 0)) {
			if (reason_down && (*reason_down == NULL)) {
				xstrfmtcat(*reason_down,
					   "%s count changed and jobs are using them "
					   "(%"PRIu64" != %"PRIu64")",
					   context_ptr->gres_type,
					   gres_data->gres_cnt_found, gres_cnt);
			}
			rc = EINVAL;
		} else {
			gres_data->gres_cnt_found = gres_cnt;
			updated_config = true;
		}
	}
	if (!updated_config && gres_data->type_cnt) {
		/*
		 * This is needed to address the GRES specification in
		 * gres.conf having a Type option, while the GRES specification
		 * in slurm.conf does not.
		 */
		for (i = 0; i < gres_data->type_cnt; i++) {
			if (gres_data->type_cnt_avail[i])
				continue;
			updated_config = true;
			break;
		}
	}
	if (!updated_config)
		return rc;
	if ((gres_cnt > gres_data->gres_cnt_config) && config_overrides) {
		info("%s: %s: count on node %s inconsistent with slurmctld count (%"PRIu64" != %"PRIu64")",
		     __func__, context_ptr->gres_type, node_name,
		     gres_cnt, gres_data->gres_cnt_config);
		gres_cnt = gres_data->gres_cnt_config;	/* Ignore excess GRES */
	}
	if ((topo_cnt == 0) && (topo_cnt != gres_data->topo_cnt)) {
		/* Need to clear topology info */
		_gres_node_state_delete_topo(gres_data);

		gres_data->topo_cnt = topo_cnt;
	}

	has_file = context_ptr->config_flags & GRES_CONF_HAS_FILE;
	has_type = context_ptr->config_flags & GRES_CONF_HAS_TYPE;
	if (gres_id_shared(context_ptr->plugin_id))
		dev_cnt = topo_cnt;
	else
		dev_cnt = gres_cnt;
	if (has_file && (topo_cnt != gres_data->topo_cnt) && (dev_cnt == 0)) {
		/*
		 * Clear any vestigial GRES node state info.
		 */
		_gres_node_state_delete_topo(gres_data);

		xfree(gres_data->gres_bit_alloc);

		gres_data->topo_cnt = 0;
	} else if (has_file && (topo_cnt != gres_data->topo_cnt)) {
		/*
		 * Need to rebuild topology info.
		 * Resize the data structures here.
		 */
		rebuild_topo = true;
		gres_data->topo_gres_cnt_alloc =
			xrealloc(gres_data->topo_gres_cnt_alloc,
				 topo_cnt * sizeof(uint64_t));
		gres_data->topo_gres_cnt_avail =
			xrealloc(gres_data->topo_gres_cnt_avail,
				 topo_cnt * sizeof(uint64_t));
		for (i = 0; i < gres_data->topo_cnt; i++) {
			if (gres_data->topo_gres_bitmap) {
				FREE_NULL_BITMAP(gres_data->
						 topo_gres_bitmap[i]);
			}
			if (gres_data->topo_core_bitmap) {
				FREE_NULL_BITMAP(gres_data->
						 topo_core_bitmap[i]);
			}
			xfree(gres_data->topo_type_name[i]);
		}
		gres_data->topo_gres_bitmap =
			xrealloc(gres_data->topo_gres_bitmap,
				 topo_cnt * sizeof(bitstr_t *));
		gres_data->topo_core_bitmap =
			xrealloc(gres_data->topo_core_bitmap,
				 topo_cnt * sizeof(bitstr_t *));
		gres_data->topo_type_id = xrealloc(gres_data->topo_type_id,
						   topo_cnt * sizeof(uint32_t));
		gres_data->topo_type_name = xrealloc(gres_data->topo_type_name,
						     topo_cnt * sizeof(char *));
		if (gres_data->gres_bit_alloc)
			gres_data->gres_bit_alloc = bit_realloc(
				gres_data->gres_bit_alloc, dev_cnt);
		gres_data->topo_cnt = topo_cnt;
	} else if (gres_id_shared(context_ptr->plugin_id) &&
		   gres_data->topo_cnt) {
		/*
		 * Need to rebuild topology info to recover state after
		 * slurmctld restart with running jobs.
		 */
		rebuild_topo = true;
	}

	if (rebuild_topo) {
		iter = list_iterator_create(gres_conf_list);
		gres_inx = i = 0;
		while ((gres_slurmd_conf = (gres_slurmd_conf_t *)
			list_next(iter))) {
			if (gres_slurmd_conf->plugin_id !=
			    context_ptr->plugin_id)
				continue;
			if ((gres_data->gres_bit_alloc) &&
			    !gres_id_shared(context_ptr->plugin_id))
				gres_data->topo_gres_cnt_alloc[i] = 0;
			gres_data->topo_gres_cnt_avail[i] =
				gres_slurmd_conf->count;
			if (gres_slurmd_conf->cpus) {
				/* NOTE: gres_slurmd_conf->cpus is cores */
				bitstr_t *tmp_bitmap = bit_alloc(core_cnt);
				int ret = bit_unfmt(tmp_bitmap,
						    gres_slurmd_conf->cpus);
				if (ret != SLURM_SUCCESS) {
					error("%s: %s: invalid GRES core specification (%s) on node %s",
					      __func__, context_ptr->gres_type,
					      gres_slurmd_conf->cpus,
					      node_name);
					FREE_NULL_BITMAP(tmp_bitmap);
				} else
					gres_data->topo_core_bitmap[i] =
						tmp_bitmap;
				cpus_config = core_cnt;
			} else if (cpus_config && !cpu_config_err) {
				cpu_config_err = true;
				error("%s: %s: has CPUs configured for only some of the records on node %s",
				      __func__, context_ptr->gres_type,
				      node_name);
			}

			if (gres_slurmd_conf->links) {
				if (gres_data->links_cnt &&
				    (gres_data->link_len != gres_cnt)) {
					/* Size changed, need to rebuild */
					for (j = 0; j < gres_data->link_len;j++)
						xfree(gres_data->links_cnt[j]);
					xfree(gres_data->links_cnt);
				}
				if (!gres_data->links_cnt) {
					gres_data->link_len = gres_cnt;
					gres_data->links_cnt =
						xcalloc(gres_cnt,
							sizeof(int *));
					for (j = 0; j < gres_cnt; j++) {
						gres_data->links_cnt[j] =
							xcalloc(gres_cnt,
								sizeof(int));
					}
				}
			}
			if (gres_id_shared(gres_slurmd_conf->plugin_id)) {
				/* If running jobs recovered then already set */
				if (!gres_data->topo_gres_bitmap[i]) {
					gres_data->topo_gres_bitmap[i] =
						bit_alloc(dev_cnt);
					bit_set(gres_data->topo_gres_bitmap[i],
						gres_inx);
				}
				gres_inx++;
			} else if (dev_cnt == 0) {
				/*
				 * Slurmd found GRES, but slurmctld can't use
				 * them. Avoid creating zero-size bitmaps.
				 */
				has_file = false;
			} else {
				gres_data->topo_gres_bitmap[i] =
					bit_alloc(dev_cnt);
				for (j = 0; j < gres_slurmd_conf->count; j++) {
					if (gres_inx >= dev_cnt) {
						/* Ignore excess GRES on node */
						break;
					}
					bit_set(gres_data->topo_gres_bitmap[i],
						gres_inx);
					if (gres_data->gres_bit_alloc &&
					    bit_test(gres_data->gres_bit_alloc,
						     gres_inx)) {
						/* Set by recovered job */
						gres_data->topo_gres_cnt_alloc[i]++;
					}
					_links_str2array(
						gres_slurmd_conf->links,
						node_name, gres_data,
						gres_inx, gres_cnt);
					gres_inx++;
				}
			}
			gres_data->topo_type_id[i] =
				gres_build_id(gres_slurmd_conf->
					      type_name);
			gres_data->topo_type_name[i] =
				xstrdup(gres_slurmd_conf->type_name);
			i++;
			if (i >= gres_data->topo_cnt)
				break;
		}
		list_iterator_destroy(iter);
		if (cpu_config_err) {
			/*
			 * Some GRES of this type have "CPUs" configured. Set
			 * topo_core_bitmap for all others with all bits set.
			 */
			iter = list_iterator_create(gres_conf_list);
			while ((gres_slurmd_conf = (gres_slurmd_conf_t *)
				list_next(iter))) {
				if (gres_slurmd_conf->plugin_id !=
				    context_ptr->plugin_id)
					continue;
				for (j = 0; j < i; j++) {
					if (gres_data->topo_core_bitmap[j])
						continue;
					gres_data->topo_core_bitmap[j] =
						bit_alloc(core_cnt);
					bit_set_all(gres_data->
						    topo_core_bitmap[j]);
				}
			}
			list_iterator_destroy(iter);
		}
	} else if (!has_file && has_type) {
		/* Add GRES Type information as needed */
		iter = list_iterator_create(gres_conf_list);
		while ((gres_slurmd_conf = (gres_slurmd_conf_t *)
			list_next(iter))) {
			if (gres_slurmd_conf->plugin_id !=
			    context_ptr->plugin_id)
				continue;
			type_id = gres_build_id(
				gres_slurmd_conf->type_name);
			for (i = 0; i < gres_data->type_cnt; i++) {
				if (type_id == gres_data->type_id[i])
					break;
			}
			if (i < gres_data->type_cnt) {
				/* Update count as needed */
				gres_data->type_cnt_avail[i] =
					gres_slurmd_conf->count;
			} else {
				gres_add_type(gres_slurmd_conf->type_name,
					      gres_data,
					      gres_slurmd_conf->count);
			}

		}
		list_iterator_destroy(iter);
	}

	if ((orig_config == NULL) || (orig_config[0] == '\0'))
		gres_data->gres_cnt_config = 0;
	else if (gres_data->gres_cnt_config == NO_VAL64) {
		/* This should have been filled in by _node_config_init() */
		_get_gres_cnt(gres_data, orig_config,
			      context_ptr->gres_name,
			      context_ptr->gres_name_colon,
			      context_ptr->gres_name_colon_len);
	}

	gres_data->gres_cnt_avail = gres_data->gres_cnt_config;

	if (has_file) {
		uint64_t gres_bits;
		if (gres_id_shared(context_ptr->plugin_id)) {
			gres_bits = topo_cnt;
		} else {
			if (gres_data->gres_cnt_avail > MAX_GRES_BITMAP) {
				error("%s: %s has \"File\" plus very large \"Count\" "
				      "(%"PRIu64") for node %s, resetting value to %u",
				      __func__, context_ptr->gres_type,
				      gres_data->gres_cnt_avail, node_name,
				      MAX_GRES_BITMAP);
				gres_data->gres_cnt_avail = MAX_GRES_BITMAP;
				gres_data->gres_cnt_found = MAX_GRES_BITMAP;
			}
			gres_bits = gres_data->gres_cnt_avail;
		}

		_gres_bit_alloc_resize(gres_data, gres_bits);
	}

	if ((config_type_cnt > 1) &&
	    !_valid_gres_types(context_ptr->gres_type, gres_data, reason_down)){
		rc = EINVAL;
	} else if (!config_overrides &&
		   (gres_data->gres_cnt_found < gres_data->gres_cnt_config)) {
		if (reason_down && (*reason_down == NULL)) {
			xstrfmtcat(*reason_down,
				   "%s count too low (%"PRIu64" < %"PRIu64")",
				   context_ptr->gres_type,
				   gres_data->gres_cnt_found,
				   gres_data->gres_cnt_config);
		}
		rc = EINVAL;
	} else if (_valid_gres_type(context_ptr->gres_type, gres_data,
				    config_overrides, reason_down)) {
		rc = EINVAL;
	} else if (config_overrides && gres_data->topo_cnt &&
		   (gres_data->gres_cnt_found != gres_data->gres_cnt_config)) {
		error("%s on node %s configured for %"PRIu64" resources but "
		      "%"PRIu64" found, ignoring topology support",
		      context_ptr->gres_type, node_name,
		      gres_data->gres_cnt_config, gres_data->gres_cnt_found);
		if (gres_data->topo_core_bitmap) {
			for (i = 0; i < gres_data->topo_cnt; i++) {
				if (gres_data->topo_core_bitmap) {
					FREE_NULL_BITMAP(gres_data->
							 topo_core_bitmap[i]);
				}
				if (gres_data->topo_gres_bitmap) {
					FREE_NULL_BITMAP(gres_data->
							 topo_gres_bitmap[i]);
				}
				xfree(gres_data->topo_type_name[i]);
			}
			xfree(gres_data->topo_core_bitmap);
			xfree(gres_data->topo_gres_bitmap);
			xfree(gres_data->topo_gres_cnt_alloc);
			xfree(gres_data->topo_gres_cnt_avail);
			xfree(gres_data->topo_type_id);
			xfree(gres_data->topo_type_name);
		}
		gres_data->topo_cnt = 0;
	}

	return rc;
}

/*
 * Validate a node's configuration and put a gres record onto a list
 * Called immediately after gres_node_config_unpack().
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from merged slurm.conf/gres.conf
 * IN/OUT new_config - Updated gres info from slurm.conf
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN threads_per_core - Count of CPUs (threads) per core on this node
 * IN cores_per_sock - Count of cores per socket on this node
 * IN sock_cnt - Count of sockets on this node
 * IN config_overrides - true: Don't validate hardware, use slurm.conf
 *                             configuration
 *			 false: Validate hardware config, but use slurm.conf
 *                              config
 * OUT reason_down - set to an explanation of failure, if any, don't set if NULL
 */
extern int gres_node_config_validate(char *node_name,
				     char *orig_config,
				     char **new_config,
				     List *gres_list,
				     int threads_per_core,
				     int cores_per_sock, int sock_cnt,
				     bool config_overrides,
				     char **reason_down)
{
	int i, rc, rc2;
	gres_state_t *gres_ptr, *gres_gpu_ptr = NULL, *gres_mps_ptr = NULL;
	int core_cnt = sock_cnt * cores_per_sock;
	int cpu_cnt  = core_cnt * threads_per_core;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL))
		*gres_list = list_create(_gres_node_list_delete);
	for (i = 0; i < gres_context_cnt; i++) {
		/* Find or create gres_state entry on the list */
		gres_ptr = list_find_first(*gres_list, gres_find_id,
					   &gres_context[i].plugin_id);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = gres_context[i].plugin_id;
			gres_ptr->gres_name =
				xstrdup(gres_context[i].gres_name);
			gres_ptr->state_type = GRES_STATE_TYPE_NODE;
			list_append(*gres_list, gres_ptr);
		}
		rc2 = _node_config_validate(node_name, orig_config,
					    gres_ptr, cpu_cnt, core_cnt,
					    sock_cnt, config_overrides,
					    reason_down, &gres_context[i]);
		rc = MAX(rc, rc2);
		if (gres_ptr->plugin_id == gpu_plugin_id)
			gres_gpu_ptr = gres_ptr;
		else if (gres_ptr->plugin_id == mps_plugin_id)
			gres_mps_ptr = gres_ptr;
	}
	_sync_node_mps_to_gpu(gres_mps_ptr, gres_gpu_ptr);
	_build_node_gres_str(gres_list, new_config, cores_per_sock, sock_cnt);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/* Convert number to new value with suffix (e.g. 2096 -> 2K) */
static void _gres_scale_value(uint64_t gres_size, uint64_t *gres_scaled,
			      char **suffix)
{
	uint64_t tmp_gres_size = gres_size;
	int i;

	tmp_gres_size = gres_size;
	for (i = 0; i < 4; i++) {
		if ((tmp_gres_size != 0) && ((tmp_gres_size % 1024) == 0))
			tmp_gres_size /= 1024;
		else
			break;
	}

	*gres_scaled = tmp_gres_size;
	if (i == 0)
		*suffix = "";
	else if (i == 1)
		*suffix = "K";
	else if (i == 2)
		*suffix = "M";
	else if (i == 3)
		*suffix = "G";
	else
		*suffix = "T";
}

/*
 * Add a GRES from node_feature plugin
 * IN node_name - name of the node for which the gres information applies
 * IN gres_name - name of the GRES being added or updated from the plugin
 * IN gres_size - count of this GRES on this node
 * IN/OUT new_config - Updated GRES info from slurm.conf
 * IN/OUT gres_list - List of GRES records for this node to track usage
 */
extern void gres_node_feature(char *node_name,
			      char *gres_name, uint64_t gres_size,
			      char **new_config, List *gres_list)
{
	char *new_gres = NULL, *tok, *save_ptr = NULL, *sep = "", *suffix = "";
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_ptr;
	uint32_t plugin_id;
	uint64_t gres_scaled = 0;
	int gres_name_len;

	xassert(gres_name);
	gres_name_len = strlen(gres_name);
	plugin_id = gres_build_id(gres_name);
	if (*new_config) {
		tok = strtok_r(*new_config, ",", &save_ptr);
		while (tok) {
			if (!strncmp(tok, gres_name, gres_name_len) &&
			    ((tok[gres_name_len] == ':') ||
			     (tok[gres_name_len] == '\0'))) {
				/* Skip this record */
			} else {
				xstrfmtcat(new_gres, "%s%s", sep, tok);
				sep = ",";
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
	}
	_gres_scale_value(gres_size, &gres_scaled, &suffix);
	xstrfmtcat(new_gres, "%s%s:%"PRIu64"%s",
		   sep, gres_name, gres_scaled, suffix);
	xfree(*new_config);
	*new_config = new_gres;

	slurm_mutex_lock(&gres_context_lock);
	if (gres_context_cnt > 0) {
		if (*gres_list == NULL)
			*gres_list = list_create(_gres_node_list_delete);
		gres_ptr = list_find_first(*gres_list, gres_find_id,
					   &plugin_id);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = plugin_id;
			gres_ptr->gres_data = _build_gres_node_state();
			gres_ptr->gres_name = xstrdup(gres_name);
			gres_ptr->state_type = GRES_STATE_TYPE_NODE;
			list_append(*gres_list, gres_ptr);
		}
		gres_node_ptr = gres_ptr->gres_data;
		if (gres_size >= gres_node_ptr->gres_cnt_alloc) {
			gres_node_ptr->gres_cnt_avail = gres_size -
				gres_node_ptr->gres_cnt_alloc;
		} else {
			error("%s: Changed size count of GRES %s from %"PRIu64
			      " to %"PRIu64", resource over allocated",
			      __func__, gres_name,
			      gres_node_ptr->gres_cnt_avail, gres_size);
			gres_node_ptr->gres_cnt_avail = 0;
		}
		gres_node_ptr->gres_cnt_config = gres_size;
		gres_node_ptr->gres_cnt_found = gres_size;
		gres_node_ptr->node_feature = true;
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Check validity of a GRES change. Specifically if a GRES type has "Files"
 * configured then the only valid new counts are the current count or zero
 *
 * RET true of the requested change is valid
 */
static int _node_reconfig_test(char *node_name, char *new_gres,
			       gres_state_t *gres_ptr,
			       slurm_gres_context_t *context_ptr)
{
	gres_node_state_t *orig_gres_data, *new_gres_data;
	int rc = SLURM_SUCCESS;

	xassert(gres_ptr);
	if (!(context_ptr->config_flags & GRES_CONF_HAS_FILE))
		return SLURM_SUCCESS;

	orig_gres_data = gres_ptr->gres_data;
	new_gres_data = _build_gres_node_state();
	_get_gres_cnt(new_gres_data, new_gres,
		      context_ptr->gres_name,
		      context_ptr->gres_name_colon,
		      context_ptr->gres_name_colon_len);
	if ((new_gres_data->gres_cnt_config != 0) &&
	    (new_gres_data->gres_cnt_config !=
	     orig_gres_data->gres_cnt_config)) {
		error("Attempt to change gres/%s Count on node %s from %"
		      PRIu64" to %"PRIu64" invalid with File configuration",
		      context_ptr->gres_name, node_name,
		      orig_gres_data->gres_cnt_config,
		      new_gres_data->gres_cnt_config);
		rc = ESLURM_INVALID_GRES;
	}
	_gres_node_state_delete(new_gres_data);

	return rc;
}

static int _node_reconfig(char *node_name, char *new_gres, char **gres_str,
			  gres_state_t *gres_ptr, bool config_overrides,
			  slurm_gres_context_t *context_ptr,
			  bool *updated_gpu_cnt)
{
	int i;
	gres_node_state_t *gres_data;
	uint64_t gres_bits, orig_cnt;

	xassert(gres_ptr);
	xassert(updated_gpu_cnt);
	*updated_gpu_cnt = false;
	if (gres_ptr->gres_data == NULL)
		gres_ptr->gres_data = _build_gres_node_state();
	gres_data = gres_ptr->gres_data;
	orig_cnt = gres_data->gres_cnt_config;

	_get_gres_cnt(gres_data, new_gres,
		      context_ptr->gres_name,
		      context_ptr->gres_name_colon,
		      context_ptr->gres_name_colon_len);

	if (gres_data->gres_cnt_config == orig_cnt)
		return SLURM_SUCCESS;	/* No change in count */

	/* Update count */
	context_ptr->total_cnt -= orig_cnt;
	context_ptr->total_cnt += gres_data->gres_cnt_config;

	if (!gres_data->gres_cnt_config)
		gres_data->gres_cnt_avail = gres_data->gres_cnt_config;
	else if (gres_data->gres_cnt_found != NO_VAL64)
		gres_data->gres_cnt_avail = gres_data->gres_cnt_found;
	else if (gres_data->gres_cnt_avail == NO_VAL64)
		gres_data->gres_cnt_avail = 0;

	if (context_ptr->config_flags & GRES_CONF_HAS_FILE) {
		if (gres_id_shared(context_ptr->plugin_id))
			gres_bits = gres_data->topo_cnt;
		else
			gres_bits = gres_data->gres_cnt_avail;

		_gres_bit_alloc_resize(gres_data, gres_bits);
	} else if (gres_data->gres_bit_alloc &&
		   !gres_id_shared(context_ptr->plugin_id)) {
		/*
		 * If GRES count changed in configuration between reboots,
		 * update bitmap sizes as needed.
		 */
		gres_bits = gres_data->gres_cnt_avail;
		if (gres_bits != bit_size(gres_data->gres_bit_alloc)) {
			info("gres/%s count changed on node %s to %"PRIu64,
			     context_ptr->gres_name, node_name, gres_bits);
			if (gres_id_sharing(context_ptr->plugin_id))
				*updated_gpu_cnt = true;
			gres_data->gres_bit_alloc =
				bit_realloc(gres_data->gres_bit_alloc,
					    gres_bits);
			for (i = 0; i < gres_data->topo_cnt; i++) {
				if (gres_data->topo_gres_bitmap &&
				    gres_data->topo_gres_bitmap[i] &&
				    (gres_bits !=
				     bit_size(gres_data->topo_gres_bitmap[i]))){
					gres_data->topo_gres_bitmap[i] =
						bit_realloc(gres_data->
							    topo_gres_bitmap[i],
							    gres_bits);
				}
			}
		}
	}

	return SLURM_SUCCESS;
}

/* The GPU count on a node changed. Update MPS data structures to match */
static void _sync_node_mps_to_gpu(gres_state_t *mps_gres_ptr,
				  gres_state_t *gpu_gres_ptr)
{
	gres_node_state_t *gpu_gres_data, *mps_gres_data;
	uint64_t gpu_cnt, mps_alloc = 0, mps_rem;
	int i;

	if (!gpu_gres_ptr || !mps_gres_ptr)
		return;

	gpu_gres_data = gpu_gres_ptr->gres_data;
	mps_gres_data = mps_gres_ptr->gres_data;

	gpu_cnt = gpu_gres_data->gres_cnt_avail;
	if (mps_gres_data->gres_bit_alloc) {
		if (gpu_cnt == bit_size(mps_gres_data->gres_bit_alloc))
			return;		/* No change for gres/mps */
	}

	if (gpu_cnt == 0)
		return;			/* Still no GPUs */

	/* Free any excess gres/mps topo records */
	for (i = gpu_cnt; i < mps_gres_data->topo_cnt; i++) {
		if (mps_gres_data->topo_core_bitmap)
			FREE_NULL_BITMAP(mps_gres_data->topo_core_bitmap[i]);
		if (mps_gres_data->topo_gres_bitmap)
			FREE_NULL_BITMAP(mps_gres_data->topo_gres_bitmap[i]);
		xfree(mps_gres_data->topo_type_name[i]);
	}

	if (mps_gres_data->gres_cnt_avail == 0) {
		/* No gres/mps on this node */
		mps_gres_data->topo_cnt = 0;
		return;
	}

	if (!mps_gres_data->gres_bit_alloc) {
		mps_gres_data->gres_bit_alloc = bit_alloc(gpu_cnt);
	} else {
		mps_gres_data->gres_bit_alloc =
			bit_realloc(mps_gres_data->gres_bit_alloc,
				    gpu_cnt);
	}

	/* Add any additional required gres/mps topo records */
	if (mps_gres_data->topo_cnt) {
		mps_gres_data->topo_core_bitmap =
			xrealloc(mps_gres_data->topo_core_bitmap,
				 sizeof(bitstr_t *) * gpu_cnt);
		mps_gres_data->topo_gres_bitmap =
			xrealloc(mps_gres_data->topo_gres_bitmap,
				 sizeof(bitstr_t *) * gpu_cnt);
		mps_gres_data->topo_gres_cnt_alloc =
			xrealloc(mps_gres_data->topo_gres_cnt_alloc,
				 sizeof(uint64_t) * gpu_cnt);
		mps_gres_data->topo_gres_cnt_avail =
			xrealloc(mps_gres_data->topo_gres_cnt_avail,
				 sizeof(uint64_t) * gpu_cnt);
		mps_gres_data->topo_type_id =
			xrealloc(mps_gres_data->topo_type_id,
				 sizeof(uint32_t) * gpu_cnt);
		mps_gres_data->topo_type_name =
			xrealloc(mps_gres_data->topo_type_name,
				 sizeof(char *) * gpu_cnt);
	} else {
		mps_gres_data->topo_core_bitmap =
			xcalloc(gpu_cnt, sizeof(bitstr_t *));
		mps_gres_data->topo_gres_bitmap =
			xcalloc(gpu_cnt, sizeof(bitstr_t *));
		mps_gres_data->topo_gres_cnt_alloc =
			xcalloc(gpu_cnt, sizeof(uint64_t));
		mps_gres_data->topo_gres_cnt_avail =
			xcalloc(gpu_cnt, sizeof(uint64_t));
		mps_gres_data->topo_type_id =
			xcalloc(gpu_cnt, sizeof(uint32_t));
		mps_gres_data->topo_type_name =
			xcalloc(gpu_cnt, sizeof(char *));
	}

	/*
	 * Evenly distribute any remaining MPS counts.
	 * Counts get reset as needed when the node registers.
	 */
	for (i = 0; i < mps_gres_data->topo_cnt; i++)
		mps_alloc += mps_gres_data->topo_gres_cnt_avail[i];
	if (mps_alloc >= mps_gres_data->gres_cnt_avail)
		mps_rem = 0;
	else
		mps_rem = mps_gres_data->gres_cnt_avail - mps_alloc;
	for (i = mps_gres_data->topo_cnt; i < gpu_cnt; i++) {
		mps_gres_data->topo_gres_bitmap[i] = bit_alloc(gpu_cnt);
		bit_set(mps_gres_data->topo_gres_bitmap[i], i);
		mps_alloc = mps_rem / (gpu_cnt - i);
		mps_gres_data->topo_gres_cnt_avail[i] = mps_alloc;
		mps_rem -= mps_alloc;
	}
	mps_gres_data->topo_cnt = gpu_cnt;

	for (i = 0; i < mps_gres_data->topo_cnt; i++) {
		if (mps_gres_data->topo_gres_bitmap &&
		    mps_gres_data->topo_gres_bitmap[i] &&
		    (gpu_cnt != bit_size(mps_gres_data->topo_gres_bitmap[i]))) {
			mps_gres_data->topo_gres_bitmap[i] =
				bit_realloc(mps_gres_data->topo_gres_bitmap[i],
					    gpu_cnt);
		}
	}
}

/* Convert core bitmap into socket string, xfree return value */
static char *_core_bitmap2str(bitstr_t *core_map, int cores_per_sock,
			      int sock_per_node)
{
	char *sock_info = NULL, tmp[256];
	bitstr_t *sock_map;
	int c, s, core_offset, max_core;
	bool any_set = false;

	xassert(core_map);
	max_core = bit_size(core_map) - 1;
	sock_map = bit_alloc(sock_per_node);
	for (s = 0; s < sock_per_node; s++) {
		core_offset = s * cores_per_sock;
		for (c = 0; c < cores_per_sock; c++) {
			if (core_offset > max_core) {
				error("%s: bad core offset (%d >= %d)",
				      __func__, core_offset, max_core);
				break;
			}
			if (bit_test(core_map, core_offset++)) {
				bit_set(sock_map, s);
				any_set = true;
				break;
			}
		}
	}
	if (any_set) {
		bit_fmt(tmp, sizeof(tmp), sock_map);
		xstrfmtcat(sock_info, "(S:%s)", tmp);
	} else {
		/* We have a core bitmap with no bits set */
		sock_info = xstrdup("");
	}
	bit_free(sock_map);

	return sock_info;
}

/* Given a count, modify it as needed and return suffix (e.g. "M" for mega ) */
static char *_get_suffix(uint64_t *count)
{
	if (*count == 0)
		return "";
	if ((*count % ((uint64_t)1024 * 1024 * 1024 * 1024 * 1024)) == 0) {
		*count /= ((uint64_t)1024 * 1024 * 1024 * 1024 * 1024);
		return "P";
	} else if ((*count % ((uint64_t)1024 * 1024 * 1024 * 1024)) == 0) {
		*count /= ((uint64_t)1024 * 1024 * 1024 * 1024);
		return "T";
	} else if ((*count % ((uint64_t)1024 * 1024 * 1024)) == 0) {
		*count /= ((uint64_t)1024 * 1024 * 1024);
		return "G";
	} else if ((*count % (1024 * 1024)) == 0) {
		*count /= (1024 * 1024);
		return "M";
	} else if ((*count % 1024) == 0) {
		*count /= 1024;
		return "K";
	} else {
		return "";
	}
}

/* Build node's GRES string based upon data in that node's GRES list */
static void _build_node_gres_str(List *gres_list, char **gres_str,
				 int cores_per_sock, int sock_per_node)
{
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_state;
	bitstr_t *done_topo, *core_map;
	uint64_t gres_sum;
	char *sep = "", *suffix, *sock_info = NULL, *sock_str;
	int c, i, j;

	xassert(gres_str);
	xfree(*gres_str);
	for (c = 0; c < gres_context_cnt; c++) {
		/* Find gres_state entry on the list */
		gres_ptr = list_find_first(*gres_list, gres_find_id,
					   &gres_context[c].plugin_id);
		if (gres_ptr == NULL)
			continue;	/* Node has none of this GRES */

		gres_node_state = (gres_node_state_t *) gres_ptr->gres_data;
		if (gres_node_state->topo_cnt &&
		    gres_node_state->gres_cnt_avail) {
			done_topo = bit_alloc(gres_node_state->topo_cnt);
			for (i = 0; i < gres_node_state->topo_cnt; i++) {
				if (bit_test(done_topo, i))
					continue;
				bit_set(done_topo, i);
				gres_sum = gres_node_state->
					topo_gres_cnt_avail[i];
				if (gres_node_state->topo_core_bitmap[i]) {
					core_map = bit_copy(
						gres_node_state->
						topo_core_bitmap[i]);
				} else
					core_map = NULL;
				for (j = 0; j < gres_node_state->topo_cnt; j++){
					if (gres_node_state->topo_type_id[i] !=
					    gres_node_state->topo_type_id[j])
						continue;
					if (bit_test(done_topo, j))
						continue;
					bit_set(done_topo, j);
					gres_sum += gres_node_state->
						topo_gres_cnt_avail[j];
					if (core_map &&
					    gres_node_state->
					    topo_core_bitmap[j]) {
						bit_or(core_map,
						       gres_node_state->
						       topo_core_bitmap[j]);
					} else if (gres_node_state->
						   topo_core_bitmap[j]) {
						core_map = bit_copy(
							gres_node_state->
							topo_core_bitmap[j]);
					}
				}
				if (core_map) {
					sock_info = _core_bitmap2str(
						core_map,
						cores_per_sock,
						sock_per_node);
					bit_free(core_map);
					sock_str = sock_info;
				} else
					sock_str = "";
				suffix = _get_suffix(&gres_sum);
				if (gres_node_state->topo_type_name[i]) {
					xstrfmtcat(*gres_str,
						   "%s%s:%s:%"PRIu64"%s%s", sep,
						   gres_context[c].gres_name,
						   gres_node_state->
						   topo_type_name[i],
						   gres_sum, suffix, sock_str);
				} else {
					xstrfmtcat(*gres_str,
						   "%s%s:%"PRIu64"%s%s", sep,
						   gres_context[c].gres_name,
						   gres_sum, suffix, sock_str);
				}
				xfree(sock_info);
				sep = ",";
			}
			bit_free(done_topo);
		} else if (gres_node_state->type_cnt &&
			   gres_node_state->gres_cnt_avail) {
			for (i = 0; i < gres_node_state->type_cnt; i++) {
				gres_sum = gres_node_state->type_cnt_avail[i];
				suffix = _get_suffix(&gres_sum);
				xstrfmtcat(*gres_str, "%s%s:%s:%"PRIu64"%s",
					   sep, gres_context[c].gres_name,
					   gres_node_state->type_name[i],
					   gres_sum, suffix);
				sep = ",";
			}
		} else if (gres_node_state->gres_cnt_avail) {
			gres_sum = gres_node_state->gres_cnt_avail;
			suffix = _get_suffix(&gres_sum);
			xstrfmtcat(*gres_str, "%s%s:%"PRIu64"%s",
				   sep, gres_context[c].gres_name,
				   gres_sum, suffix);
			sep = ",";
		}
	}
}

/*
 * Note that a node's configuration has been modified (e.g. "scontol update ..")
 * IN node_name - name of the node for which the gres information applies
 * IN new_gres - Updated GRES information supplied from slurm.conf or scontrol
 * IN/OUT gres_str - Node's current GRES string, updated as needed
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN config_overrides - true: Don't validate hardware, use slurm.conf
 *                             configuration
 *			 false: Validate hardware config, but use slurm.conf
 *                              config
 * IN cores_per_sock - Number of cores per socket on this node
 * IN sock_per_node - Total count of sockets on this node (on any board)
 */
extern int gres_node_reconfig(char *node_name,
			      char *new_gres,
			      char **gres_str,
			      List *gres_list,
			      bool config_overrides,
			      int cores_per_sock,
			      int sock_per_node)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr = NULL, **gres_ptr_array;
	gres_state_t *gpu_gres_ptr = NULL, *mps_gres_ptr;

	rc = gres_init();
	slurm_mutex_lock(&gres_context_lock);
	gres_ptr_array = xcalloc(gres_context_cnt, sizeof(gres_state_t *));
	if ((gres_context_cnt > 0) && (*gres_list == NULL))
		*gres_list = list_create(_gres_node_list_delete);

	/* First validate all of the requested GRES changes */
	for (i = 0; (rc == SLURM_SUCCESS) && (i < gres_context_cnt); i++) {
		/* Find gres_state entry on the list */
		gres_ptr = list_find_first(*gres_list, gres_find_id,
					   &gres_context[i].plugin_id);
		if (gres_ptr == NULL)
			continue;
		gres_ptr_array[i] = gres_ptr;
		rc = _node_reconfig_test(node_name, new_gres, gres_ptr,
					 &gres_context[i]);
	}

	/* Now update the GRES counts */
	for (i = 0; (rc == SLURM_SUCCESS) && (i < gres_context_cnt); i++) {
		bool updated_gpu_cnt = false;
		if (gres_ptr_array[i] == NULL)
			continue;
		rc = _node_reconfig(node_name, new_gres, gres_str,
				    gres_ptr_array[i], config_overrides,
				    &gres_context[i], &updated_gpu_cnt);
		if (updated_gpu_cnt)
			gpu_gres_ptr = gres_ptr;
	}

	/* Now synchronize gres/gpu and gres/mps state */
	if (gpu_gres_ptr && have_mps) {
		/* Update gres/mps counts and bitmaps to match gres/gpu */
		gres_iter = list_iterator_create(*gres_list);
		while ((mps_gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_id_shared(mps_gres_ptr->plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		_sync_node_mps_to_gpu(mps_gres_ptr, gpu_gres_ptr);
	}

	/* Build new per-node gres_str */
	_build_node_gres_str(gres_list, gres_str, cores_per_sock,sock_per_node);
	slurm_mutex_unlock(&gres_context_lock);
	xfree(gres_ptr_array);

	return rc;
}

/*
 * Pack a node's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_node_config_validate()
 * IN/OUT buffer - location to write state to
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_node_state_pack(List gres_list, buf_t *buffer,
				char *node_name)
{
	int rc = SLURM_SUCCESS;
	uint32_t top_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t gres_bitmap_size, rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_ptr;

	if (gres_list == NULL) {
		pack16(rec_cnt, buffer);
		return rc;
	}

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		gres_node_ptr = (gres_node_state_t *) gres_ptr->gres_data;
		pack32(magic, buffer);
		pack32(gres_ptr->plugin_id, buffer);
		pack64(gres_node_ptr->gres_cnt_avail, buffer);
		/*
		 * Just note if gres_bit_alloc exists.
		 * Rebuild it based upon the state of recovered jobs
		 */
		if (gres_node_ptr->gres_bit_alloc)
			gres_bitmap_size = bit_size(gres_node_ptr->gres_bit_alloc);
		else
			gres_bitmap_size = 0;
		pack16(gres_bitmap_size, buffer);
		rec_cnt++;
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a node's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_node_state_pack()
 * IN/OUT buffer - location to read state from
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_node_state_unpack(List *gres_list, buf_t *buffer,
				  char *node_name,
				  uint16_t protocol_version)
{
	int i, rc;
	uint32_t magic = 0, plugin_id = 0;
	uint64_t gres_cnt_avail = 0;
	uint16_t gres_bitmap_size = 0, rec_cnt = 0;
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_ptr;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL))
		*gres_list = list_create(_gres_node_list_delete);

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;
			safe_unpack32(&plugin_id, buffer);
			safe_unpack64(&gres_cnt_avail, buffer);
			safe_unpack16(&gres_bitmap_size, buffer);
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			goto unpack_error;
		}
		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_context[i].plugin_id == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("%s: no plugin configured to unpack data type %u from node %s",
			      __func__, plugin_id, node_name);
			/*
			 * A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data.
			 */
			continue;
		}
		gres_node_ptr = _build_gres_node_state();
		gres_node_ptr->gres_cnt_avail = gres_cnt_avail;
		if (gres_bitmap_size) {
			gres_node_ptr->gres_bit_alloc =
				bit_alloc(gres_bitmap_size);
		}
		gres_ptr = xmalloc(sizeof(gres_state_t));
		gres_ptr->plugin_id = gres_context[i].plugin_id;
		gres_ptr->gres_data = gres_node_ptr;
		gres_ptr->gres_name = xstrdup(gres_context[i].gres_name);
		gres_ptr->state_type = GRES_STATE_TYPE_NODE;
		list_append(*gres_list, gres_ptr);
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("%s: unpack error from node %s", __func__, node_name);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

static void *_node_state_dup(void *gres_data)
{
	int i, j;
	gres_node_state_t *gres_ptr = (gres_node_state_t *) gres_data;
	gres_node_state_t *new_gres;

	if (gres_ptr == NULL)
		return NULL;

	new_gres = xmalloc(sizeof(gres_node_state_t));
	new_gres->gres_cnt_found  = gres_ptr->gres_cnt_found;
	new_gres->gres_cnt_config = gres_ptr->gres_cnt_config;
	new_gres->gres_cnt_avail  = gres_ptr->gres_cnt_avail;
	new_gres->gres_cnt_alloc  = gres_ptr->gres_cnt_alloc;
	new_gres->no_consume      = gres_ptr->no_consume;
	if (gres_ptr->gres_bit_alloc)
		new_gres->gres_bit_alloc = bit_copy(gres_ptr->gres_bit_alloc);

	if (gres_ptr->links_cnt && gres_ptr->link_len) {
		new_gres->links_cnt = xcalloc(gres_ptr->link_len,
					      sizeof(int *));
		j = sizeof(int) * gres_ptr->link_len;
		for (i = 0; i < gres_ptr->link_len; i++) {
			new_gres->links_cnt[i] = xmalloc(j);
			memcpy(new_gres->links_cnt[i],gres_ptr->links_cnt[i],j);
		}
		new_gres->link_len = gres_ptr->link_len;
	}

	if (gres_ptr->topo_cnt) {
		new_gres->topo_cnt         = gres_ptr->topo_cnt;
		new_gres->topo_core_bitmap = xcalloc(gres_ptr->topo_cnt,
						     sizeof(bitstr_t *));
		new_gres->topo_gres_bitmap = xcalloc(gres_ptr->topo_cnt,
						     sizeof(bitstr_t *));
		new_gres->topo_gres_cnt_alloc = xcalloc(gres_ptr->topo_cnt,
							sizeof(uint64_t));
		new_gres->topo_gres_cnt_avail = xcalloc(gres_ptr->topo_cnt,
							sizeof(uint64_t));
		new_gres->topo_type_id = xcalloc(gres_ptr->topo_cnt,
						 sizeof(uint32_t));
		new_gres->topo_type_name = xcalloc(gres_ptr->topo_cnt,
						   sizeof(char *));
		for (i = 0; i < gres_ptr->topo_cnt; i++) {
			if (gres_ptr->topo_core_bitmap[i]) {
				new_gres->topo_core_bitmap[i] =
					bit_copy(gres_ptr->topo_core_bitmap[i]);
			}
			new_gres->topo_gres_bitmap[i] =
				bit_copy(gres_ptr->topo_gres_bitmap[i]);
			new_gres->topo_gres_cnt_alloc[i] =
				gres_ptr->topo_gres_cnt_alloc[i];
			new_gres->topo_gres_cnt_avail[i] =
				gres_ptr->topo_gres_cnt_avail[i];
			new_gres->topo_type_id[i] = gres_ptr->topo_type_id[i];
			new_gres->topo_type_name[i] =
				xstrdup(gres_ptr->topo_type_name[i]);
		}
	}

	if (gres_ptr->type_cnt) {
		new_gres->type_cnt       = gres_ptr->type_cnt;
		new_gres->type_cnt_alloc = xcalloc(gres_ptr->type_cnt,
						   sizeof(uint64_t));
		new_gres->type_cnt_avail = xcalloc(gres_ptr->type_cnt,
						   sizeof(uint64_t));
		new_gres->type_id = xcalloc(gres_ptr->type_cnt,
					    sizeof(uint32_t));
		new_gres->type_name = xcalloc(gres_ptr->type_cnt,
					      sizeof(char *));
		for (i = 0; i < gres_ptr->type_cnt; i++) {
			new_gres->type_cnt_alloc[i] =
				gres_ptr->type_cnt_alloc[i];
			new_gres->type_cnt_avail[i] =
				gres_ptr->type_cnt_avail[i];
			new_gres->type_id[i] = gres_ptr->type_id[i];
			new_gres->type_name[i] =
				xstrdup(gres_ptr->type_name[i]);
		}
	}

	return new_gres;
}

/*
 * Duplicate a node gres status (used for will-run logic)
 * IN gres_list - node gres state information
 * RET a copy of gres_list or NULL on failure
 */
extern List gres_node_state_dup(List gres_list)
{
	int i;
	List new_list = NULL;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres;
	void *gres_data;

	if (gres_list == NULL)
		return new_list;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0)) {
		new_list = list_create(_gres_node_list_delete);
	}
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id != gres_context[i].plugin_id)
				continue;
			gres_data = _node_state_dup(gres_ptr->gres_data);
			if (gres_data) {
				new_gres = xmalloc(sizeof(gres_state_t));
				new_gres->plugin_id = gres_ptr->plugin_id;
				new_gres->gres_data = gres_data;
				new_gres->gres_name =
					xstrdup(gres_ptr->gres_name);
				gres_ptr->state_type = GRES_STATE_TYPE_NODE;
				list_append(new_list, new_gres);
			}
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup node record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_list;
}

static void _node_state_dealloc(gres_state_t *gres_ptr)
{
	int i;
	gres_node_state_t *gres_node_ptr;
	char *gres_name = NULL;

	gres_node_ptr = (gres_node_state_t *) gres_ptr->gres_data;
	gres_node_ptr->gres_cnt_alloc = 0;
	if (gres_node_ptr->gres_bit_alloc) {
		int i = bit_size(gres_node_ptr->gres_bit_alloc) - 1;
		if (i >= 0)
			bit_nclear(gres_node_ptr->gres_bit_alloc, 0, i);
	}

	if (gres_node_ptr->topo_cnt && !gres_node_ptr->topo_gres_cnt_alloc) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_ptr->plugin_id == gres_context[i].plugin_id) {
				gres_name = gres_context[i].gres_name;
				break;
			}
		}
		error("gres_node_state_dealloc_all: gres/%s topo_cnt!=0 "
		      "and topo_gres_cnt_alloc is NULL", gres_name);
	} else if (gres_node_ptr->topo_cnt) {
		for (i = 0; i < gres_node_ptr->topo_cnt; i++) {
			gres_node_ptr->topo_gres_cnt_alloc[i] = 0;
		}
	} else {
		/*
		 * This array can be set at startup if a job has been allocated
		 * specific GRES and the node has not registered with the
		 * details needed to track individual GRES (rather than only
		 * a GRES count).
		 */
		xfree(gres_node_ptr->topo_gres_cnt_alloc);
	}

	for (i = 0; i < gres_node_ptr->type_cnt; i++) {
		gres_node_ptr->type_cnt_alloc[i] = 0;
	}
}

/*
 * Deallocate all resources on this node previous allocated to any jobs.
 *	This function isused to synchronize state after slurmctld restarts or
 *	is reconfigured.
 * IN gres_list - node gres state information
 */
extern void gres_node_state_dealloc_all(List gres_list)
{
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (gres_list == NULL)
		return;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		_node_state_dealloc(gres_ptr);
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static char *_node_gres_used(void *gres_data, char *gres_name)
{
	gres_node_state_t *gres_node_ptr;
	char *sep = "";
	int i, j;

	xassert(gres_data);
	gres_node_ptr = (gres_node_state_t *) gres_data;

	if ((gres_node_ptr->topo_cnt != 0) &&
	    (gres_node_ptr->no_consume == false)) {
		bitstr_t *topo_printed = bit_alloc(gres_node_ptr->topo_cnt);
		xfree(gres_node_ptr->gres_used);    /* Free any cached value */
		for (i = 0; i < gres_node_ptr->topo_cnt; i++) {
			bitstr_t *topo_gres_bitmap = NULL;
			uint64_t gres_alloc_cnt = 0;
			char *gres_alloc_idx, tmp_str[64];
			if (bit_test(topo_printed, i))
				continue;
			bit_set(topo_printed, i);
			if (gres_node_ptr->topo_gres_bitmap[i]) {
				topo_gres_bitmap =
					bit_copy(gres_node_ptr->
						 topo_gres_bitmap[i]);
			}
			for (j = i + 1; j < gres_node_ptr->topo_cnt; j++) {
				if (bit_test(topo_printed, j))
					continue;
				if (gres_node_ptr->topo_type_id[i] !=
				    gres_node_ptr->topo_type_id[j])
					continue;
				bit_set(topo_printed, j);
				if (gres_node_ptr->topo_gres_bitmap[j]) {
					if (!topo_gres_bitmap) {
						topo_gres_bitmap =
							bit_copy(gres_node_ptr->
								 topo_gres_bitmap[j]);
					} else if (bit_size(topo_gres_bitmap) ==
						   bit_size(gres_node_ptr->
							    topo_gres_bitmap[j])){
						bit_or(topo_gres_bitmap,
						       gres_node_ptr->
						       topo_gres_bitmap[j]);
					}
				}
			}
			if (gres_node_ptr->gres_bit_alloc && topo_gres_bitmap &&
			    (bit_size(topo_gres_bitmap) ==
			     bit_size(gres_node_ptr->gres_bit_alloc))) {
				bit_and(topo_gres_bitmap,
					gres_node_ptr->gres_bit_alloc);
				gres_alloc_cnt = bit_set_count(topo_gres_bitmap);
			}
			if (gres_alloc_cnt > 0) {
				bit_fmt(tmp_str, sizeof(tmp_str),
					topo_gres_bitmap);
				gres_alloc_idx = tmp_str;
			} else {
				gres_alloc_idx = "N/A";
			}
			xstrfmtcat(gres_node_ptr->gres_used,
				   "%s%s:%s:%"PRIu64"(IDX:%s)", sep, gres_name,
				   gres_node_ptr->topo_type_name[i],
				   gres_alloc_cnt, gres_alloc_idx);
			sep = ",";
			FREE_NULL_BITMAP(topo_gres_bitmap);
		}
		FREE_NULL_BITMAP(topo_printed);
	} else if (gres_node_ptr->gres_used) {
		;	/* Used cached value */
	} else if (gres_node_ptr->type_cnt == 0) {
		if (gres_node_ptr->no_consume) {
			xstrfmtcat(gres_node_ptr->gres_used, "%s:0", gres_name);
		} else {
			xstrfmtcat(gres_node_ptr->gres_used, "%s:%"PRIu64,
				   gres_name, gres_node_ptr->gres_cnt_alloc);
		}
	} else {
		for (i = 0; i < gres_node_ptr->type_cnt; i++) {
			if (gres_node_ptr->no_consume) {
				xstrfmtcat(gres_node_ptr->gres_used,
					   "%s%s:%s:0", sep, gres_name,
					   gres_node_ptr->type_name[i]);
			} else {
				xstrfmtcat(gres_node_ptr->gres_used,
					   "%s%s:%s:%"PRIu64, sep, gres_name,
					   gres_node_ptr->type_name[i],
					   gres_node_ptr->type_cnt_alloc[i]);
			}
			sep = ",";
		}
	}

	return gres_node_ptr->gres_used;
}

static void _node_state_log(void *gres_data, char *node_name, char *gres_name)
{
	gres_node_state_t *gres_node_ptr;
	int i, j;
	char *buf = NULL, *sep, tmp_str[128];

	xassert(gres_data);
	gres_node_ptr = (gres_node_state_t *) gres_data;

	info("gres/%s: state for %s", gres_name, node_name);
	if (gres_node_ptr->gres_cnt_found == NO_VAL64) {
		snprintf(tmp_str, sizeof(tmp_str), "TBD");
	} else {
		snprintf(tmp_str, sizeof(tmp_str), "%"PRIu64,
			 gres_node_ptr->gres_cnt_found);
	}

	if (gres_node_ptr->no_consume) {
		info("  gres_cnt found:%s configured:%"PRIu64" "
		     "avail:%"PRIu64" no_consume",
		     tmp_str, gres_node_ptr->gres_cnt_config,
		     gres_node_ptr->gres_cnt_avail);
	} else {
		info("  gres_cnt found:%s configured:%"PRIu64" "
		     "avail:%"PRIu64" alloc:%"PRIu64"",
		     tmp_str, gres_node_ptr->gres_cnt_config,
		     gres_node_ptr->gres_cnt_avail,
		     gres_node_ptr->gres_cnt_alloc);
	}

	if (gres_node_ptr->gres_bit_alloc) {
		bit_fmt(tmp_str, sizeof(tmp_str),gres_node_ptr->gres_bit_alloc);
		info("  gres_bit_alloc:%s of %d",
		     tmp_str, (int) bit_size(gres_node_ptr->gres_bit_alloc));
	} else {
		info("  gres_bit_alloc:NULL");
	}

	info("  gres_used:%s", gres_node_ptr->gres_used);

	if (gres_node_ptr->links_cnt && gres_node_ptr->link_len) {
		for (i = 0; i < gres_node_ptr->link_len; i++) {
			sep = "";
			for (j = 0; j < gres_node_ptr->link_len; j++) {
				xstrfmtcat(buf, "%s%d", sep,
					   gres_node_ptr->links_cnt[i][j]);
				sep = ", ";
			}
			info("  links[%d]:%s", i, buf);
			xfree(buf);
		}
	}

	for (i = 0; i < gres_node_ptr->topo_cnt; i++) {
		info("  topo[%d]:%s(%u)", i, gres_node_ptr->topo_type_name[i],
		     gres_node_ptr->topo_type_id[i]);
		if (gres_node_ptr->topo_core_bitmap[i]) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_node_ptr->topo_core_bitmap[i]);
			info("   topo_core_bitmap[%d]:%s of %d", i, tmp_str,
			     (int)bit_size(gres_node_ptr->topo_core_bitmap[i]));
		} else
			info("   topo_core_bitmap[%d]:NULL", i);
		if (gres_node_ptr->topo_gres_bitmap[i]) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_node_ptr->topo_gres_bitmap[i]);
			info("   topo_gres_bitmap[%d]:%s of %d", i, tmp_str,
			     (int)bit_size(gres_node_ptr->topo_gres_bitmap[i]));
		} else
			info("   topo_gres_bitmap[%d]:NULL", i);
		info("   topo_gres_cnt_alloc[%d]:%"PRIu64"", i,
		     gres_node_ptr->topo_gres_cnt_alloc[i]);
		info("   topo_gres_cnt_avail[%d]:%"PRIu64"", i,
		     gres_node_ptr->topo_gres_cnt_avail[i]);
	}

	for (i = 0; i < gres_node_ptr->type_cnt; i++) {
		info("  type[%d]:%s(%u)", i, gres_node_ptr->type_name[i],
		     gres_node_ptr->type_id[i]);
		info("   type_cnt_alloc[%d]:%"PRIu64, i,
		     gres_node_ptr->type_cnt_alloc[i]);
		info("   type_cnt_avail[%d]:%"PRIu64, i,
		     gres_node_ptr->type_cnt_avail[i]);
	}
}

/*
 * Log a node's current gres state
 * IN gres_list - generated by gres_node_config_validate()
 * IN node_name - name of the node for which the gres information applies
 */
extern void gres_node_state_log(List gres_list, char *node_name)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_GRES) || !gres_list)
		return;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    gres_context[i].plugin_id)
				continue;
			_node_state_log(gres_ptr->gres_data, node_name,
					gres_context[i].gres_name);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Build a string indicating a node's drained GRES
 * IN gres_list - generated by gres_node_config_validate()
 * RET - string, must be xfreed by caller
 */
extern char *gres_get_node_drain(List gres_list)
{
	char *node_drain = xstrdup("N/A");

	return node_drain;
}

/*
 * Build a string indicating a node's used GRES
 * IN gres_list - generated by gres_node_config_validate()
 * RET - string, must be xfreed by caller
 */
extern char *gres_get_node_used(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	char *gres_used = NULL, *tmp;

	if (!gres_list)
		return gres_used;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    gres_context[i].plugin_id)
				continue;
			tmp = _node_gres_used(gres_ptr->gres_data,
					      gres_context[i].gres_name);
			if (!tmp)
				continue;
			if (gres_used)
				xstrcat(gres_used, ",");
			xstrcat(gres_used, tmp);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return gres_used;
}

/*
 * Give the total system count of a given GRES
 * Returns NO_VAL64 if name not found
 */
extern uint64_t gres_get_system_cnt(char *name)
{
	uint64_t count = NO_VAL64;
	int i;

	if (!name)
		return NO_VAL64;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (!xstrcmp(gres_context[i].gres_name, name)) {
			count = gres_context[i].total_cnt;
			break;
		}
	}
	slurm_mutex_unlock(&gres_context_lock);
	return count;
}


/*
 * Get the count of a node's GRES
 * IN gres_list - List of Gres records for this node to track usage
 * IN name - name of gres
 */
extern uint64_t gres_node_config_cnt(List gres_list, char *name)
{
	int i;
	gres_state_t *gres_ptr;
	gres_node_state_t *data_ptr;
	uint64_t count = 0;

	if (!gres_list || !name || !list_count(gres_list))
		return count;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (!xstrcmp(gres_context[i].gres_name, name)) {
			/* Find or create gres_state entry on the list */
			gres_ptr = list_find_first(gres_list, gres_find_id,
						   &gres_context[i].plugin_id);

			if (!gres_ptr || !gres_ptr->gres_data)
				break;
			data_ptr = (gres_node_state_t *)gres_ptr->gres_data;
			count = data_ptr->gres_cnt_config;
			break;
		} else if (!xstrncmp(name, gres_context[i].gres_name_colon,
				     gres_context[i].gres_name_colon_len)) {
			int type;
			uint32_t type_id;
			char *type_str = NULL;

			if (!(type_str = strchr(name, ':'))) {
				error("Invalid gres name '%s'", name);
				break;
			}
			type_str++;

			gres_ptr = list_find_first(gres_list, gres_find_id,
						   &gres_context[i].plugin_id);

			if (!gres_ptr || !gres_ptr->gres_data)
				break;
			data_ptr = (gres_node_state_t *)gres_ptr->gres_data;
			type_id = gres_build_id(type_str);
			for (type = 0; type < data_ptr->type_cnt; type++) {
				if (data_ptr->type_id[type] == type_id) {
					count = data_ptr->type_cnt_avail[type];
					break;
				}
			}
			break;
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return count;
}

static void _job_state_delete(void *gres_data)
{
	int i;
	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;

	if (gres_ptr == NULL)
		return;

	for (i = 0; i < gres_ptr->node_cnt; i++) {
		if (gres_ptr->gres_bit_alloc)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		if (gres_ptr->gres_bit_step_alloc)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_step_alloc[i]);
	}
	xfree(gres_ptr->gres_bit_alloc);
	xfree(gres_ptr->gres_cnt_node_alloc);
	xfree(gres_ptr->gres_bit_step_alloc);
	xfree(gres_ptr->gres_cnt_step_alloc);
	if (gres_ptr->gres_bit_select) {
		for (i = 0; i < gres_ptr->total_node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_select[i]);
		xfree(gres_ptr->gres_bit_select);
	}
	xfree(gres_ptr->gres_cnt_node_alloc);
	xfree(gres_ptr->gres_cnt_node_select);
	xfree(gres_ptr->gres_name);
	xfree(gres_ptr->type_name);
	xfree(gres_ptr);
}

extern void gres_job_list_delete(void *list_element)
{
	gres_state_t *gres_ptr;

	if (gres_init() != SLURM_SUCCESS)
		return;

	gres_ptr = (gres_state_t *) list_element;
	slurm_mutex_lock(&gres_context_lock);
	_job_state_delete(gres_ptr->gres_data);
	gres_ptr->gres_data = NULL;
	_gres_state_delete_members(gres_ptr);
	slurm_mutex_unlock(&gres_context_lock);
}

static int _clear_cpus_per_gres(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->cpus_per_gres = 0;
	return 0;
}
static int _clear_gres_per_job(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->gres_per_job = 0;
	return 0;
}
static int _clear_gres_per_node(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->gres_per_node = 0;
	return 0;
}
static int _clear_gres_per_socket(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->gres_per_socket = 0;
	return 0;
}
static int _clear_gres_per_task(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->gres_per_task = 0;
	return 0;
}
static int _clear_mem_per_gres(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->mem_per_gres = 0;
	return 0;
}
static int _clear_total_gres(void *x, void *arg)
{
	gres_state_t *gres_ptr = (gres_state_t *) x;
	gres_job_state_t *job_gres_data;
	job_gres_data = (gres_job_state_t *) gres_ptr->gres_data;
	job_gres_data->total_gres = 0;
	return 0;
}

/*
 * Ensure consistency of gres_per_* options
 * Modify task and node count as needed for consistentcy with GRES options
 * RET -1 on failure, 0 on success
 */
static int _test_gres_cnt(gres_job_state_t *job_gres_data,
			  uint32_t *num_tasks,
			  uint32_t *min_nodes, uint32_t *max_nodes,
			  uint16_t *ntasks_per_node,
			  uint16_t *ntasks_per_socket,
			  uint16_t *sockets_per_node,
			  uint16_t *cpus_per_task)
{
	int req_nodes, req_tasks, req_tasks_per_node, req_tasks_per_socket;
	int req_sockets, req_cpus_per_task;
	uint16_t cpus_per_gres;

	/* Ensure gres_per_job >= gres_per_node >= gres_per_socket */
	if (job_gres_data->gres_per_job &&
	    ((job_gres_data->gres_per_node &&
	      (job_gres_data->gres_per_node > job_gres_data->gres_per_job)) ||
	     (job_gres_data->gres_per_task &&
	      (job_gres_data->gres_per_task > job_gres_data->gres_per_job)) ||
	     (job_gres_data->gres_per_socket &&
	      (job_gres_data->gres_per_socket >
	       job_gres_data->gres_per_job)))) {
		error("Failed to ensure --%ss >= --gres=%s/--%ss-per-node >= --%ss-per-socket",
		      job_gres_data->gres_name,
		      job_gres_data->gres_name,
		      job_gres_data->gres_name,
		      job_gres_data->gres_name);
		return -1;
	}

	/* Ensure gres_per_job >= gres_per_task */
	if (job_gres_data->gres_per_node &&
	    ((job_gres_data->gres_per_task &&
	      (job_gres_data->gres_per_task > job_gres_data->gres_per_node)) ||
	     (job_gres_data->gres_per_socket &&
	      (job_gres_data->gres_per_socket >
	       job_gres_data->gres_per_node)))) {
		error("Failed to ensure --%ss >= --%ss-per-task",
		      job_gres_data->gres_name,
		      job_gres_data->gres_name);
		return -1;
	}

	/* gres_per_socket requires sockets-per-node count specification */
	if (job_gres_data->gres_per_socket) {
		if (*sockets_per_node == NO_VAL16) {
			error("--%ss-per-socket option requires --sockets-per-node specification",
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/* make sure --cpu-per-gres is not combined with --cpus-per-task */
	if (!running_in_slurmctld() && job_gres_data->cpus_per_gres &&
	    (*cpus_per_task != NO_VAL16)) {
		error("--cpus-per-%s is mutually exclusive with --cpus-per-task",
		      job_gres_data->gres_name);
		return -1;
	}


	/*
	 * Ensure gres_per_job is multiple of gres_per_node
	 * Ensure node count is consistent with GRES parameters
	 */
	if (job_gres_data->gres_per_job && job_gres_data->gres_per_node) {
		if (job_gres_data->gres_per_job % job_gres_data->gres_per_node){
			/* gres_per_job not multiple of gres_per_node */
			error("Failed to validate job spec, --%ss is not multiple of --gres=%s/--%ss-per-node",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
		req_nodes = job_gres_data->gres_per_job /
			job_gres_data->gres_per_node;
		if (((*min_nodes != NO_VAL) && (req_nodes < *min_nodes)) ||
		    (req_nodes > *max_nodes)) {
			error("Failed to validate job spec. Based on --%s and --gres=%s/--%ss-per-node required nodes (%u) doesn't fall between min_nodes (%u) and max_nodes (%u) boundaries.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      req_nodes,
			      *min_nodes,
			      *max_nodes);
			return -1;
		}
		*min_nodes = *max_nodes = req_nodes;
	}

	/*
	 * Ensure gres_per_node is multiple of gres_per_socket
	 * Ensure task count is consistent with GRES parameters
	 */
	if (job_gres_data->gres_per_node && job_gres_data->gres_per_socket) {
		if (job_gres_data->gres_per_node %
		    job_gres_data->gres_per_socket) {
			/* gres_per_node not multiple of gres_per_socket */
			error("Failed to validate job spec, --gres=%s/--%ss-per-node not multiple of --%ss-per-socket.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
		req_sockets = job_gres_data->gres_per_node /
			job_gres_data->gres_per_socket;
		if (*sockets_per_node == NO_VAL16)
			*sockets_per_node = req_sockets;
		else if (*sockets_per_node != req_sockets) {
			error("Failed to validate job spec. Based on --gres=%s/--%ss-per-node and --%ss-per-socket required number of sockets differ from --sockets-per-node.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/*
	 * Ensure ntasks_per_tres is multiple of num_tasks
	 */
	if (job_gres_data->ntasks_per_gres &&
	    (job_gres_data->ntasks_per_gres != NO_VAL16) &&
	    (*num_tasks != NO_VAL)) {
		int tmp = *num_tasks / job_gres_data->ntasks_per_gres;
		if ((tmp * job_gres_data->ntasks_per_gres) != *num_tasks) {
			error("Failed to validate job spec, -n/--ntasks has to be a multiple of --ntasks-per-%s.",
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/*
	 * Ensure gres_per_job is multiple of gres_per_task
	 * Ensure task count is consistent with GRES parameters
	 */
	if (job_gres_data->gres_per_task) {
		if(job_gres_data->gres_per_job) {
			if (job_gres_data->gres_per_job %
			    job_gres_data->gres_per_task) {
				/* gres_per_job not multiple of gres_per_task */
				error("Failed to validate job spec, --%ss not multiple of --%ss-per-task",
				      job_gres_data->gres_name,
				      job_gres_data->gres_name);
				return -1;
			}
			req_tasks = job_gres_data->gres_per_job /
				job_gres_data->gres_per_task;
			if (*num_tasks == NO_VAL)
				*num_tasks = req_tasks;
			else if (*num_tasks != req_tasks) {
				error("Failed to validate job spec. Based on --%ss and --%ss-per-task number of requested tasks differ from -n/--ntasks.",
				      job_gres_data->gres_name,
				      job_gres_data->gres_name);
				return -1;
			}
		} else if (*num_tasks != NO_VAL) {
			job_gres_data->gres_per_job = *num_tasks *
				job_gres_data->gres_per_task;
		} else {
			error("Failed to validate job spec. --%ss-per-task used without either --%ss or -n/--ntasks is not allowed.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/*
	 * Ensure gres_per_node is multiple of gres_per_task
	 * Ensure tasks_per_node is consistent with GRES parameters
	 */
	if (job_gres_data->gres_per_node && job_gres_data->gres_per_task) {
		if (job_gres_data->gres_per_node %
		    job_gres_data->gres_per_task) {
			/* gres_per_node not multiple of gres_per_task */
			error("Failed to validate job spec, --gres=%s/--%ss-per-node not multiple of --%ss-per-task.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
		req_tasks_per_node = job_gres_data->gres_per_node /
			job_gres_data->gres_per_task;
		if ((*ntasks_per_node == NO_VAL16) ||
		    (*ntasks_per_node == 0))
			*ntasks_per_node = req_tasks_per_node;
		else if (*ntasks_per_node != req_tasks_per_node) {
			error("Failed to validate job spec. Based on --gres=%s/--%ss-per-node and --%ss-per-task requested number of tasks per node differ from --ntasks-per-node.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/*
	 * Ensure gres_per_socket is multiple of gres_per_task
	 * Ensure ntasks_per_socket is consistent with GRES parameters
	 */
	if (job_gres_data->gres_per_socket && job_gres_data->gres_per_task) {
		if (job_gres_data->gres_per_socket %
		    job_gres_data->gres_per_task) {
			/* gres_per_socket not multiple of gres_per_task */
			error("Failed to validate job spec, --%ss-per-socket not multiple of --%ss-per-task.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
		req_tasks_per_socket = job_gres_data->gres_per_socket /
			job_gres_data->gres_per_task;
		if ((*ntasks_per_socket == NO_VAL16) ||
		    (*ntasks_per_socket == 0))
			*ntasks_per_socket = req_tasks_per_socket;
		else if (*ntasks_per_socket != req_tasks_per_socket) {
			error("Failed to validate job spec. Based on --%ss-per-socket and --%ss-per-task requested number of tasks per sockets differ from --ntasks-per-socket.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/* Ensure that cpus_per_gres * gres_per_task == cpus_per_task */
	if (job_gres_data->cpus_per_gres)
		cpus_per_gres = job_gres_data->cpus_per_gres;
	else
		cpus_per_gres = job_gres_data->def_cpus_per_gres;
	if (cpus_per_gres && job_gres_data->gres_per_task) {
		req_cpus_per_task = cpus_per_gres *job_gres_data->gres_per_task;
		if ((*cpus_per_task == NO_VAL16) ||
		    (*cpus_per_task == 0))
			*cpus_per_task = req_cpus_per_task;
		else if (*cpus_per_task != req_cpus_per_task) {
			error("Failed to validate job spec. Based on --cpus-per-%s and --%ss-per-task requested number of cpus differ from -c/--cpus-per-task.",
			      job_gres_data->gres_name,
			      job_gres_data->gres_name);
			return -1;
		}
	}

	/* Ensure tres_per_job >= node count */
	if (job_gres_data->gres_per_job) {
		if ((*min_nodes != NO_VAL) &&
		    (job_gres_data->gres_per_job < *min_nodes)) {
			error("Failed to validate job spec, --%ss < -N",
			      job_gres_data->gres_name);
			return -1;
		}
		if ((*max_nodes != NO_VAL) &&
		    (job_gres_data->gres_per_job < *max_nodes)) {
			*max_nodes = job_gres_data->gres_per_job;
		}
	}

	return 0;
}

/*
 * Translate a string, with optional suffix, into its equivalent numeric value
 * tok IN - the string to translate
 * value IN - numeric value
 * RET true if "tok" is a valid number
 */
static bool _is_valid_number(char *tok, unsigned long long int *value)
{
	unsigned long long int tmp_val;
	uint64_t mult;
	char *end_ptr = NULL;

	tmp_val = strtoull(tok, &end_ptr, 10);
	if (tmp_val == ULLONG_MAX)
		return false;
	if ((mult = suffix_mult(end_ptr)) == NO_VAL64)
		return false;
	tmp_val *= mult;
	*value = tmp_val;
	return true;
}

/*
 * Reentrant TRES specification parse logic
 * in_val IN - initial input string
 * type OUT -  must be xfreed by caller
 * cnt OUT - count of values
 * flags OUT - user flags (GRES_NO_CONSUME)
 * save_ptr IN/OUT - NULL on initial call, otherwise value from previous call
 * RET rc - error code
 */
static int _get_next_gres(char *in_val, char **type_ptr, int *context_inx_ptr,
			  uint64_t *cnt, uint16_t *flags, char **save_ptr)
{
	char *comma, *sep, *sep2, *name = NULL, *type = NULL;
	int i, rc = SLURM_SUCCESS;
	unsigned long long int value = 0;

	xassert(cnt);
	xassert(flags);
	xassert(save_ptr);
	*flags = 0;

	if (!in_val && (*save_ptr == NULL)) {
		return rc;
	}

	if (*save_ptr == NULL) {
		*save_ptr = in_val;
	}

next:	if (*save_ptr[0] == '\0') {	/* Empty input token */
		*save_ptr = NULL;
		goto fini;
	}

	if (!(sep = xstrstr(*save_ptr, "gres:"))) {
		debug2("%s is not a gres", *save_ptr);
		xfree(name);
		*save_ptr = NULL;
		goto fini;
	} else {
		sep += 5; /* strlen "gres:" */
		*save_ptr = sep;
	}

	name = xstrdup(*save_ptr);
	comma = strchr(name, ',');
	if (comma) {
		*save_ptr += (comma - name + 1);
		comma[0] = '\0';
	} else {
		*save_ptr += strlen(name);
	}

	if (name[0] == '\0') {
		/* Nothing but a comma */
		xfree(name);
		goto next;
	}

	sep = strchr(name, ':');
	if (sep) {
		sep[0] = '\0';
		sep++;
		sep2 = strchr(sep, ':');
		if (sep2) {
			sep2[0] = '\0';
			sep2++;
		}
	} else {
		sep2 = NULL;
	}

	if (sep2) {		/* Two colons */
		/* We have both type and count */
		if ((sep[0] == '\0') || (sep2[0] == '\0')) {
			/* Bad format (e.g. "gpu:tesla:" or "gpu::1") */
			rc = ESLURM_INVALID_GRES;
			goto fini;
		}
		type = xstrdup(sep);
		if (!_is_valid_number(sep2, &value)) {
			debug("%s: Invalid count value GRES %s:%s:%s", __func__,
			      name, type, sep2);
			rc = ESLURM_INVALID_GRES;
			goto fini;
		}
	} else if (sep) {	/* One colon */
		if (sep[0] == '\0') {
			/* Bad format (e.g. "gpu:") */
			rc = ESLURM_INVALID_GRES;
			goto fini;
		} else if (_is_valid_number(sep, &value)) {
			/* We have count, but no type */
			type = NULL;
		} else {
			/* We have type with implicit count of 1 */
			type = xstrdup(sep);
			value = 1;
		}
	} else {		/* No colon */
		/* We have no type and implicit count of 1 */
		type = NULL;
		value = 1;
	}
	if (value == 0) {
		xfree(name);
		xfree(type);
		goto next;
	}

	for (i = 0; i < gres_context_cnt; i++) {
		if (!xstrcmp(name, gres_context[i].gres_name) ||
		    !xstrncmp(name, gres_context[i].gres_name_colon,
			      gres_context[i].gres_name_colon_len))
			break;	/* GRES name match found */
	}
	if (i >= gres_context_cnt) {
		debug("%s: Failed to locate GRES %s", __func__, name);
		rc = ESLURM_INVALID_GRES;
		goto fini;
	}
	*context_inx_ptr = i;

fini:	if (rc != SLURM_SUCCESS) {
		*save_ptr = NULL;
		if (rc == ESLURM_INVALID_GRES) {
			info("%s: Invalid GRES job specification %s", __func__,
			     in_val);
		}
		xfree(type);
		*type_ptr = NULL;
	} else {
		*cnt = value;
		*type_ptr = type;
	}
	xfree(name);

	return rc;
}

/*
 * TRES specification parse logic
 * in_val IN - initial input string
 * cnt OUT - count of values
 * gres_list IN/OUT - where to search for (or add) new job TRES record
 * save_ptr IN/OUT - NULL on initial call, otherwise value from previous call
 * rc OUT - unchanged or an error code
 * RET gres - job record to set value in, found or created by this function
 */
static gres_job_state_t *_get_next_job_gres(char *in_val, uint64_t *cnt,
					    List gres_list, char **save_ptr,
					    int *rc)
{
	static char *prev_save_ptr = NULL;
	int context_inx = NO_VAL, my_rc = SLURM_SUCCESS;
	gres_job_state_t *job_gres_data = NULL;
	gres_state_t *gres_ptr;
	gres_key_t job_search_key;
	char *type = NULL, *name = NULL;
	uint16_t flags = 0;

	xassert(save_ptr);
	if (!in_val && (*save_ptr == NULL)) {
		return NULL;
	}

	if (*save_ptr == NULL) {
		prev_save_ptr = in_val;
	} else if (*save_ptr != prev_save_ptr) {
		error("%s: parsing error", __func__);
		my_rc = SLURM_ERROR;
		goto fini;
	}

	if (prev_save_ptr[0] == '\0') {	/* Empty input token */
		*save_ptr = NULL;
		return NULL;
	}

	if ((my_rc = _get_next_gres(in_val, &type, &context_inx,
				    cnt, &flags, &prev_save_ptr)) ||
	    (context_inx == NO_VAL)) {
		prev_save_ptr = NULL;
		goto fini;
	}

	/* Find the job GRES record */
	job_search_key.plugin_id = gres_context[context_inx].plugin_id;
	job_search_key.type_id = gres_build_id(type);
	gres_ptr = list_find_first(gres_list, gres_find_job_by_key,
				   &job_search_key);

	if (gres_ptr) {
		job_gres_data = gres_ptr->gres_data;
	} else {
		job_gres_data = xmalloc(sizeof(gres_job_state_t));
		job_gres_data->gres_name =
			xstrdup(gres_context[context_inx].gres_name);
		job_gres_data->type_id = gres_build_id(type);
		job_gres_data->type_name = type;
		type = NULL;	/* String moved above */
		gres_ptr = xmalloc(sizeof(gres_state_t));
		gres_ptr->plugin_id = gres_context[context_inx].plugin_id;
		gres_ptr->gres_data = job_gres_data;
		gres_ptr->gres_name =
			xstrdup(gres_context[context_inx].gres_name);
		gres_ptr->state_type = GRES_STATE_TYPE_JOB;
		list_append(gres_list, gres_ptr);
	}
	job_gres_data->flags = flags;

fini:	xfree(name);
	xfree(type);
	if (my_rc != SLURM_SUCCESS) {
		prev_save_ptr = NULL;
		if (my_rc == ESLURM_INVALID_GRES) {
			info("%s: Invalid GRES job specification %s", __func__,
			     in_val);
		}
		*rc = my_rc;
	}
	*save_ptr = prev_save_ptr;
	return job_gres_data;
}

/* Return true if job specification only includes cpus_per_gres or mem_per_gres
 * Return false if any other field set
 */
static bool _generic_job_state(gres_job_state_t *job_state)
{
	if (job_state->gres_per_job ||
	    job_state->gres_per_node ||
	    job_state->gres_per_socket ||
	    job_state->gres_per_task)
		return false;
	return true;
}

/*
 * Given a job's requested GRES configuration, validate it and build a GRES list
 * Note: This function can be used for a new request with gres_list==NULL or
 *	 used to update an existing job, in which case gres_list is a copy
 *	 of the job's original value (so we can clear fields as needed)
 * IN *tres* - job requested gres input string
 * IN/OUT num_tasks - requested task count, may be reset to provide
 *		      consistent gres_per_node/task values
 * IN/OUT min_nodes - requested minimum node count, may be reset to provide
 *		      consistent gres_per_node/task values
 * IN/OUT max_nodes - requested maximum node count, may be reset to provide
 *		      consistent gres_per_node/task values
 * IN/OUT ntasks_per_node - requested tasks_per_node count, may be reset to
 *		      provide consistent gres_per_node/task values
 * IN/OUT ntasks_per_socket - requested ntasks_per_socket count, may be reset to
 *		      provide consistent gres_per_node/task values
 * IN/OUT sockets_per_node - requested sockets_per_node count, may be reset to
 *		      provide consistent gres_per_socket/node values
 * IN/OUT cpus_per_task - requested cpus_per_task count, may be reset to
 *		      provide consistent gres_per_task/cpus_per_gres values
 * IN/OUT ntasks_per_tres - requested ntasks_per_tres count
 * OUT gres_list - List of GRES records for this job to track usage
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_job_state_validate(char *cpus_per_tres,
				   char *tres_freq,
				   char *tres_per_job,
				   char *tres_per_node,
				   char *tres_per_socket,
				   char *tres_per_task,
				   char *mem_per_tres,
				   uint32_t *num_tasks,
				   uint32_t *min_nodes,
				   uint32_t *max_nodes,
				   uint16_t *ntasks_per_node,
				   uint16_t *ntasks_per_socket,
				   uint16_t *sockets_per_node,
				   uint16_t *cpus_per_task,
				   uint16_t *ntasks_per_tres,
				   List *gres_list)
{
	typedef struct overlap_check {
		gres_job_state_t *without_model_state;
		uint32_t plugin_id;
		bool with_model;
		bool without_model;
	} overlap_check_t;
	overlap_check_t *over_list;
	int i, over_count = 0, rc = SLURM_SUCCESS, size;
	bool have_gres_gpu = false, have_gres_mps = false;
	bool overlap_merge = false;
	gres_state_t *gres_state;
	gres_job_state_t *job_gres_data;
	uint64_t cnt = 0;
	ListIterator iter;

	if (!cpus_per_tres && !tres_per_job && !tres_per_node &&
	    !tres_per_socket && !tres_per_task && !mem_per_tres &&
	    !ntasks_per_tres)
		return SLURM_SUCCESS;

	if ((tres_per_task || (*ntasks_per_tres != NO_VAL16)) &&
	    (*num_tasks == NO_VAL) && (*min_nodes != NO_VAL) &&
	    (*min_nodes == *max_nodes)) {
		/* Implicitly set task count */
		if (*ntasks_per_tres != NO_VAL16)
			*num_tasks = *min_nodes * *ntasks_per_tres;
		else if (*ntasks_per_node != NO_VAL16)
			*num_tasks = *min_nodes * *ntasks_per_node;
		else if (*cpus_per_task == NO_VAL16)
			*num_tasks = *min_nodes;
	}

	if ((rc = gres_init()) != SLURM_SUCCESS)
		return rc;

	if ((select_plugin_type != SELECT_TYPE_CONS_TRES) &&
	    (cpus_per_tres || tres_per_job || tres_per_socket ||
	     tres_per_task || mem_per_tres))
		return ESLURM_UNSUPPORTED_GRES;

	/*
	 * Clear fields as requested by job update (i.e. input value is "")
	 */
	if (*gres_list)
		(void) list_for_each(*gres_list, _clear_total_gres, NULL);
	if (*gres_list && cpus_per_tres && (cpus_per_tres[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_cpus_per_gres, NULL);
		cpus_per_tres = NULL;
	}
	if (*gres_list && tres_per_job && (tres_per_job[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_gres_per_job, NULL);
		tres_per_job = NULL;
	}
	if (*gres_list && tres_per_node && (tres_per_node[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_gres_per_node, NULL);
		tres_per_node = NULL;
	}
	if (*gres_list && tres_per_socket && (tres_per_socket[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_gres_per_socket, NULL);
		tres_per_socket = NULL;
	}
	if (*gres_list && tres_per_task && (tres_per_task[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_gres_per_task, NULL);
		tres_per_task = NULL;
	}
	if (*gres_list && mem_per_tres && (mem_per_tres[0] == '\0')) {
		(void) list_for_each(*gres_list, _clear_mem_per_gres, NULL);
		mem_per_tres = NULL;
	}

	/*
	 * Set new values as requested
	 */
	if (*gres_list == NULL)
		*gres_list = list_create(gres_job_list_delete);
	slurm_mutex_lock(&gres_context_lock);
	if (cpus_per_tres) {
		char *in_val = cpus_per_tres, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->cpus_per_gres = cnt;
			in_val = NULL;
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}
	if (tres_per_job) {
		char *in_val = tres_per_job, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->gres_per_job = cnt;
			in_val = NULL;
			job_gres_data->total_gres =
				MAX(job_gres_data->total_gres, cnt);
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}
	if (tres_per_node) {
		char *in_val = tres_per_node, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->gres_per_node = cnt;
			in_val = NULL;
			if (*min_nodes != NO_VAL)
				cnt *= *min_nodes;
			job_gres_data->total_gres =
				MAX(job_gres_data->total_gres, cnt);
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}
	if (tres_per_socket) {
		char *in_val = tres_per_socket, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->gres_per_socket = cnt;
			in_val = NULL;
			if ((*min_nodes != NO_VAL) &&
			    (*sockets_per_node != NO_VAL16)) {
				cnt *= (*min_nodes * *sockets_per_node);
			} else if ((*num_tasks != NO_VAL) &&
				   (*ntasks_per_socket != NO_VAL16)) {
				cnt *= ((*num_tasks + *ntasks_per_socket - 1) /
					*ntasks_per_socket);
			}
			job_gres_data->total_gres =
				MAX(job_gres_data->total_gres, cnt);
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}
	if (tres_per_task) {
		char *in_val = tres_per_task, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->gres_per_task = cnt;
			in_val = NULL;
			if (*num_tasks != NO_VAL)
				cnt *= *num_tasks;
			job_gres_data->total_gres =
				MAX(job_gres_data->total_gres, cnt);
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}
	if (mem_per_tres) {
		char *in_val = mem_per_tres, *save_ptr = NULL;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->mem_per_gres = cnt;
			in_val = NULL;
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
		}
	}

	/* *num_tasks and *ntasks_per_tres could be 0 on requeue */
	if (!ntasks_per_tres || !*ntasks_per_tres ||
	    (*ntasks_per_tres == NO_VAL16)) {
		/* do nothing */
	} else if (list_count(*gres_list) != 0) {
		/* Set num_tasks = gpus * ntasks/gpu */
		uint64_t gpus = _get_job_gres_list_cnt(*gres_list, "gpu", NULL);
		if (gpus != NO_VAL64)
			*num_tasks = gpus * *ntasks_per_tres;
		else
			error("%s: Can't set num_tasks = gpus * *ntasks_per_tres because there are no allocated GPUs",
			      __func__);
	} else if (*num_tasks && (*num_tasks != NO_VAL)) {
		/*
		 * If job_gres_list empty, and ntasks_per_tres is specified,
		 * then derive GPUs according to how many tasks there are.
		 * GPU GRES = [ntasks / (ntasks_per_tres)]
		 * For now, only generate type-less GPUs.
		 */
		uint32_t gpus = *num_tasks / *ntasks_per_tres;
		char *save_ptr = NULL, *gres = NULL, *in_val;
		xstrfmtcat(gres, "gres:gpu:%u", gpus);
		in_val = gres;
		while ((job_gres_data = _get_next_job_gres(in_val, &cnt,
							   *gres_list,
							   &save_ptr, &rc))) {
			job_gres_data->ntasks_per_gres = *ntasks_per_tres;
			/* Simulate a tres_per_job specification */
			job_gres_data->gres_per_job = cnt;
			job_gres_data->total_gres =
				MAX(job_gres_data->total_gres, cnt);
			in_val = NULL;
		}
		if (list_count(*gres_list) == 0)
			error("%s: Failed to add generated GRES %s (via ntasks_per_tres) to gres_list",
			      __func__, gres);
		xfree(gres);
	} else {
		error("%s: --ntasks-per-tres needs either a GRES GPU specification or a node/ntask specification",
		      __func__);
	}

	slurm_mutex_unlock(&gres_context_lock);

	if (rc != SLURM_SUCCESS)
		return rc;
	size = list_count(*gres_list);
	if (size == 0) {
		FREE_NULL_LIST(*gres_list);
		return rc;
	}

	/*
	 * Check for record overlap (e.g. "gpu:2,gpu:tesla:1")
	 * Ensure tres_per_job >= tres_per_node >= tres_per_socket
	 */
	over_list = xcalloc(size, sizeof(overlap_check_t));
	iter = list_iterator_create(*gres_list);
	while ((gres_state = (gres_state_t *) list_next(iter))) {
		job_gres_data = (gres_job_state_t *) gres_state->gres_data;
		if (_test_gres_cnt(job_gres_data, num_tasks, min_nodes,
				   max_nodes, ntasks_per_node,
				   ntasks_per_socket, sockets_per_node,
				   cpus_per_task) != 0) {
			rc = ESLURM_INVALID_GRES;
			break;
		}
		if (!have_gres_gpu && !xstrcmp(job_gres_data->gres_name, "gpu"))
			have_gres_gpu = true;
		if (!xstrcmp(job_gres_data->gres_name, "mps")) {
			have_gres_mps = true;
			/*
			 * gres/mps only supports a per-node count,
			 * set either explicitly or implicitly.
			 */
			if (job_gres_data->gres_per_job &&
			    (*max_nodes != 1)) {
				rc = ESLURM_INVALID_GRES;
				break;
			}
			if (job_gres_data->gres_per_socket &&
			    (*sockets_per_node != 1)) {
				rc = ESLURM_INVALID_GRES;
				break;
			}
			if (job_gres_data->gres_per_task && (*num_tasks != 1)) {
				rc = ESLURM_INVALID_GRES;
				break;
			}
		}
		if (have_gres_gpu && have_gres_mps) {
			rc = ESLURM_INVALID_GRES;
			break;
		}

		for (i = 0; i < over_count; i++) {
			if (over_list[i].plugin_id == gres_state->plugin_id)
				break;
		}
		if (i >= over_count) {
			over_list[over_count++].plugin_id =
				gres_state->plugin_id;
			if (job_gres_data->type_name) {
				over_list[i].with_model = true;
			} else {
				over_list[i].without_model = true;
				over_list[i].without_model_state =
					job_gres_data;
			}
		} else if (job_gres_data->type_name) {
			over_list[i].with_model = true;
			if (over_list[i].without_model)
				overlap_merge = true;
		} else {
			over_list[i].without_model = true;
			over_list[i].without_model_state = job_gres_data;
			if (over_list[i].with_model)
				overlap_merge = true;
		}
	}
	if (have_gres_mps && (rc == SLURM_SUCCESS) && tres_freq &&
	    strstr(tres_freq, "gpu")) {
		rc = ESLURM_INVALID_GRES;
	}

	if (overlap_merge) {	/* Merge generic data if possible */
		uint16_t cpus_per_gres;
		uint64_t mem_per_gres;
		for (i = 0; i < over_count; i++) {
			if (!over_list[i].with_model ||
			    !over_list[i].without_model_state)
				continue;
			if (!_generic_job_state(
				    over_list[i].without_model_state)) {
				rc = ESLURM_INVALID_GRES_TYPE;
				break;
			}
			/* Propagate generic parameters */
			cpus_per_gres =
				over_list[i].without_model_state->cpus_per_gres;
			mem_per_gres =
				over_list[i].without_model_state->mem_per_gres;
			list_iterator_reset(iter);
			while ((gres_state = (gres_state_t *)list_next(iter))) {
				job_gres_data = (gres_job_state_t *)
					gres_state->gres_data;
				if (over_list[i].plugin_id !=
				    gres_state->plugin_id)
					continue;
				if (job_gres_data ==
				    over_list[i].without_model_state) {
					list_remove(iter);
					continue;
				}
				if (job_gres_data->cpus_per_gres == 0) {
					job_gres_data->cpus_per_gres =
						cpus_per_gres;
				}
				if (job_gres_data->mem_per_gres == 0) {
					job_gres_data->mem_per_gres =
						mem_per_gres;
				}
			}
		}
	}
	list_iterator_destroy(iter);
	xfree(over_list);

	return rc;
}

/*
 * Determine if a job's specified GRES can be supported. This is designed to
 * prevent the running of a job using the GRES options only supported by the
 * select/cons_tres plugin when switching (on slurmctld restart) from the
 * cons_tres plugin to any other select plugin.
 *
 * IN gres_list - List of GRES records for this job to track usage
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_job_revalidate(List gres_list)
{
	gres_state_t *gres_state;
	gres_job_state_t *job_gres_data;
	ListIterator iter;
	int rc = SLURM_SUCCESS;

	if (!gres_list || (select_plugin_type == SELECT_TYPE_CONS_TRES))
		return SLURM_SUCCESS;

	iter = list_iterator_create(gres_list);
	while ((gres_state = (gres_state_t *) list_next(iter))) {
		job_gres_data = (gres_job_state_t *) gres_state->gres_data;
		if (job_gres_data->gres_per_job ||
		    job_gres_data->gres_per_socket ||
		    job_gres_data->gres_per_task) {
			rc = ESLURM_UNSUPPORTED_GRES;
			break;
		}
	}
	list_iterator_destroy(iter);

	return rc;
}

/*
 * Return TRUE if any of this job's GRES has a populated gres_bit_alloc element.
 * This indicates the allocated GRES has a File configuration parameter and is
 * tracking individual file assignments.
 */
static bool _job_has_gres_bits(List job_gres_list)
{
	ListIterator job_gres_iter;
	gres_state_t *gres_ptr;
	gres_job_state_t *job_gres_ptr;
	bool rc = false;
	int i;

	if (!job_gres_list)
		return false;

	job_gres_iter = list_iterator_create(job_gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		job_gres_ptr = gres_ptr->gres_data;
		if (!job_gres_ptr)
			continue;
		for (i = 0; i < job_gres_ptr->node_cnt; i++) {
			if (job_gres_ptr->gres_bit_alloc &&
			    job_gres_ptr->gres_bit_alloc[i]) {
				rc = true;
				break;
			}
		}
		if (rc)
			break;
	}
	list_iterator_destroy(job_gres_iter);

	return rc;
}

/*
 * Return count of configured GRES.
 * NOTE: For gres/mps return count of gres/gpu
 */
static int _get_node_gres_cnt(List node_gres_list, uint32_t plugin_id)
{
	ListIterator node_gres_iter;
	gres_node_state_t *gres_node_ptr;
	gres_state_t *gres_ptr;
	int gres_cnt = 0;

	if (!node_gres_list)
		return 0;

	if (plugin_id == mps_plugin_id)
		plugin_id = gpu_plugin_id;
	node_gres_iter = list_iterator_create(node_gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(node_gres_iter))) {
		if (gres_ptr->plugin_id != plugin_id)
			continue;
		gres_node_ptr = (gres_node_state_t *) gres_ptr->gres_data;
		gres_cnt = (int) gres_node_ptr->gres_cnt_config;
		break;
	}
	list_iterator_destroy(node_gres_iter);

	return gres_cnt;
}

/*
 * Return TRUE if the identified node in the job allocation can satisfy the
 * job's GRES specification without change in its bitmaps. In other words,
 * return FALSE if the job allocation identifies specific GRES devices and the
 * count of those devices on this node has changed.
 *
 * IN job_gres_list - List of GRES records for this job to track usage
 * IN node_inx - zero-origin index into this job's node allocation
 * IN node_gres_list - List of GRES records for this node
 */
static bool _validate_node_gres_cnt(uint32_t job_id, List job_gres_list,
				    int node_inx, List node_gres_list,
				    char *node_name)
{
	ListIterator job_gres_iter;
	gres_state_t *gres_ptr;
	gres_job_state_t *job_gres_ptr;
	bool rc = true;
	int job_gres_cnt, node_gres_cnt;

	if (!job_gres_list)
		return true;

	(void) gres_init();

	job_gres_iter = list_iterator_create(job_gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		job_gres_ptr = gres_ptr->gres_data;
		if (!job_gres_ptr || !job_gres_ptr->gres_bit_alloc)
			continue;
		if ((node_inx >= job_gres_ptr->node_cnt) ||
		    !job_gres_ptr->gres_bit_alloc[node_inx])
			continue;
		job_gres_cnt = bit_size(job_gres_ptr->gres_bit_alloc[node_inx]);
		node_gres_cnt = _get_node_gres_cnt(node_gres_list,
						   gres_ptr->plugin_id);
		if (job_gres_cnt != node_gres_cnt) {
			error("%s: Killing job %u: gres/%s count mismatch on node "
			      "%s (%d != %d)",
			      __func__, job_id, job_gres_ptr->gres_name,
			      node_name, job_gres_cnt, node_gres_cnt);
			rc = false;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);

	return rc;
}

/*
 * Determine if a job's specified GRES are currently valid. This is designed to
 * manage jobs allocated GRES which are either no longer supported or a GRES
 * configured with the "File" option in gres.conf where the count has changed,
 * in which case we don't know how to map the job's old GRES bitmap onto the
 * current GRES bitmaps.
 *
 * IN job_id - ID of job being validated (used for logging)
 * IN job_gres_list - List of GRES records for this job to track usage
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_job_revalidate2(uint32_t job_id, List job_gres_list,
				bitstr_t *node_bitmap)
{
	node_record_t *node_ptr;
	int rc = SLURM_SUCCESS;
	int i_first, i_last, i;
	int node_inx = -1;

	if (!job_gres_list || !node_bitmap ||
	    !_job_has_gres_bits(job_gres_list))
		return SLURM_SUCCESS;

	i_first = bit_ffs(node_bitmap);
	if (i_first >= 0)
		i_last = bit_fls(node_bitmap);
	else
		i_last = -2;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(node_bitmap, i))
			continue;
		node_ptr = node_record_table_ptr + i;
		node_inx++;
		if (!_validate_node_gres_cnt(job_id, job_gres_list, node_inx,
					     node_ptr->gres_list,
					     node_ptr->name)) {
			rc = ESLURM_INVALID_GRES;
			break;
		}
	}

	return rc;
}

/*
 * Find a sock_gres_t record in a list by matching the plugin_id and type_id
 *	from a gres_state_t job record
 * IN x - a sock_gres_t record to test
 * IN key - the gres_state_t record (from a job) we want to match
 * RET 1 on match, otherwise 0
 */
extern int gres_find_sock_by_job_state(void *x, void *key)
{
	sock_gres_t *sock_data = (sock_gres_t *) x;
	gres_state_t *job_gres_state = (gres_state_t *) key;
	gres_job_state_t *job_data;

	job_data = (gres_job_state_t *) job_gres_state->gres_data;
	if ((sock_data->plugin_id == job_gres_state->plugin_id) &&
	    (sock_data->type_id   == job_data->type_id))
		return 1;
	return 0;
}

/*
 * Create a (partial) copy of a job's gres state for job binding
 * IN gres_list - List of Gres records for this job to track usage
 * RET The copy or NULL on failure
 * NOTE: Only job details are copied, NOT the job step details
 */
extern List gres_job_state_dup(List gres_list)
{
	return gres_job_state_extract(gres_list, -1);
}

/* Copy gres_job_state_t record for ALL nodes */
static void *_job_state_dup(void *gres_data)
{

	int i;
	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;
	gres_job_state_t *new_gres_ptr;

	if (gres_ptr == NULL)
		return NULL;

	new_gres_ptr = xmalloc(sizeof(gres_job_state_t));
	new_gres_ptr->cpus_per_gres	= gres_ptr->cpus_per_gres;
	new_gres_ptr->gres_name		= xstrdup(gres_ptr->gres_name);
	new_gres_ptr->gres_per_job	= gres_ptr->gres_per_job;
	new_gres_ptr->gres_per_node	= gres_ptr->gres_per_node;
	new_gres_ptr->gres_per_socket	= gres_ptr->gres_per_socket;
	new_gres_ptr->gres_per_task	= gres_ptr->gres_per_task;
	new_gres_ptr->mem_per_gres	= gres_ptr->mem_per_gres;
	new_gres_ptr->node_cnt		= gres_ptr->node_cnt;
	new_gres_ptr->total_gres	= gres_ptr->total_gres;
	new_gres_ptr->type_id		= gres_ptr->type_id;
	new_gres_ptr->type_name		= xstrdup(gres_ptr->type_name);

	if (gres_ptr->gres_cnt_node_alloc) {
		i = sizeof(uint64_t) * gres_ptr->node_cnt;
		new_gres_ptr->gres_cnt_node_alloc = xmalloc(i);
		memcpy(new_gres_ptr->gres_cnt_node_alloc,
		       gres_ptr->gres_cnt_node_alloc, i);
	}
	if (gres_ptr->gres_bit_alloc) {
		new_gres_ptr->gres_bit_alloc = xcalloc(gres_ptr->node_cnt,
						       sizeof(bitstr_t *));
		for (i = 0; i < gres_ptr->node_cnt; i++) {
			if (gres_ptr->gres_bit_alloc[i] == NULL)
				continue;
			new_gres_ptr->gres_bit_alloc[i] =
				bit_copy(gres_ptr->gres_bit_alloc[i]);
		}
	}
	return new_gres_ptr;
}

/* Copy gres_job_state_t record for one specific node */
static void *_job_state_dup2(void *gres_data, int node_index)
{

	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;
	gres_job_state_t *new_gres_ptr;

	if (gres_ptr == NULL)
		return NULL;

	new_gres_ptr = xmalloc(sizeof(gres_job_state_t));
	new_gres_ptr->cpus_per_gres	= gres_ptr->cpus_per_gres;
	new_gres_ptr->gres_name		= xstrdup(gres_ptr->gres_name);
	new_gres_ptr->gres_per_job	= gres_ptr->gres_per_job;
	new_gres_ptr->gres_per_node	= gres_ptr->gres_per_node;
	new_gres_ptr->gres_per_socket	= gres_ptr->gres_per_socket;
	new_gres_ptr->gres_per_task	= gres_ptr->gres_per_task;
	new_gres_ptr->mem_per_gres	= gres_ptr->mem_per_gres;
	new_gres_ptr->node_cnt		= 1;
	new_gres_ptr->total_gres	= gres_ptr->total_gres;
	new_gres_ptr->type_id		= gres_ptr->type_id;
	new_gres_ptr->type_name		= xstrdup(gres_ptr->type_name);

	if (gres_ptr->gres_cnt_node_alloc) {
		new_gres_ptr->gres_cnt_node_alloc = xmalloc(sizeof(uint64_t));
		new_gres_ptr->gres_cnt_node_alloc[0] =
			gres_ptr->gres_cnt_node_alloc[node_index];
	}
	if (gres_ptr->gres_bit_alloc && gres_ptr->gres_bit_alloc[node_index]) {
		new_gres_ptr->gres_bit_alloc	= xmalloc(sizeof(bitstr_t *));
		new_gres_ptr->gres_bit_alloc[0] =
			bit_copy(gres_ptr->gres_bit_alloc[node_index]);
	}
	return new_gres_ptr;
}

/*
 * Create a (partial) copy of a job's gres state for a particular node index
 * IN gres_list - List of Gres records for this job to track usage
 * IN node_index - zero-origin index to the node
 * RET The copy or NULL on failure
 */
extern List gres_job_state_extract(List gres_list, int node_index)
{
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres_state;
	List new_gres_list = NULL;
	void *new_gres_data;

	if (gres_list == NULL)
		return new_gres_list;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		if (node_index == -1)
			new_gres_data = _job_state_dup(gres_ptr->gres_data);
		else {
			new_gres_data = _job_state_dup2(gres_ptr->gres_data,
							node_index);
		}
		if (new_gres_data == NULL)
			break;
		if (new_gres_list == NULL) {
			new_gres_list = list_create(gres_job_list_delete);
		}
		new_gres_state = xmalloc(sizeof(gres_state_t));
		new_gres_state->plugin_id = gres_ptr->plugin_id;
		new_gres_state->gres_data = new_gres_data;
		new_gres_state->gres_name = xstrdup(gres_ptr->gres_name);
		new_gres_state->state_type = GRES_STATE_TYPE_JOB;
		list_append(new_gres_list, new_gres_state);
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_gres_list;
}

/*
 * Pack a job's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_job_config_validate()
 * IN/OUT buffer - location to write state to
 * IN job_id - job's ID
 * IN details - if set then pack job step allocation details (only needed to
 *		save/restore job state, not needed in job credential for
 *		slurmd task binding)
 *
 * NOTE: A job's allocation to steps is not recorded here, but recovered with
 *	 the job step state information upon slurmctld restart.
 */
extern int gres_job_state_pack(List gres_list, buf_t *buffer,
			       uint32_t job_id, bool details,
			       uint16_t protocol_version)
{
	int i, rc = SLURM_SUCCESS;
	uint32_t top_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	gres_job_state_t *gres_job_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		gres_job_ptr = (gres_job_state_t *) gres_ptr->gres_data;

		if (protocol_version >= SLURM_20_11_PROTOCOL_VERSION) {
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			pack16(gres_job_ptr->cpus_per_gres, buffer);
			pack16(gres_job_ptr->flags, buffer);
			pack64(gres_job_ptr->gres_per_job, buffer);
			pack64(gres_job_ptr->gres_per_node, buffer);
			pack64(gres_job_ptr->gres_per_socket, buffer);
			pack64(gres_job_ptr->gres_per_task, buffer);
			pack64(gres_job_ptr->mem_per_gres, buffer);
			pack16(gres_job_ptr->ntasks_per_gres, buffer);
			pack64(gres_job_ptr->total_gres, buffer);
			packstr(gres_job_ptr->type_name, buffer);
			pack32(gres_job_ptr->node_cnt, buffer);

			if (gres_job_ptr->gres_cnt_node_alloc) {
				pack8((uint8_t) 1, buffer);
				pack64_array(gres_job_ptr->gres_cnt_node_alloc,
					     gres_job_ptr->node_cnt, buffer);
			} else {
				pack8((uint8_t) 0, buffer);
			}

			if (gres_job_ptr->gres_bit_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack_bit_str_hex(gres_job_ptr->
							 gres_bit_alloc[i],
							 buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (details && gres_job_ptr->gres_bit_step_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack_bit_str_hex(gres_job_ptr->
							 gres_bit_step_alloc[i],
							 buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (details && gres_job_ptr->gres_cnt_step_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack64(gres_job_ptr->
					       gres_cnt_step_alloc[i],
					       buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			rec_cnt++;
		} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			pack16(gres_job_ptr->cpus_per_gres, buffer);
			pack16(gres_job_ptr->flags, buffer);
			pack64(gres_job_ptr->gres_per_job, buffer);
			pack64(gres_job_ptr->gres_per_node, buffer);
			pack64(gres_job_ptr->gres_per_socket, buffer);
			pack64(gres_job_ptr->gres_per_task, buffer);
			pack64(gres_job_ptr->mem_per_gres, buffer);
			pack64(gres_job_ptr->total_gres, buffer);
			packstr(gres_job_ptr->type_name, buffer);
			pack32(gres_job_ptr->node_cnt, buffer);

			if (gres_job_ptr->gres_cnt_node_alloc) {
				pack8((uint8_t) 1, buffer);
				pack64_array(gres_job_ptr->gres_cnt_node_alloc,
					     gres_job_ptr->node_cnt, buffer);
			} else {
				pack8((uint8_t) 0, buffer);
			}

			if (gres_job_ptr->gres_bit_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack_bit_str_hex(gres_job_ptr->
							 gres_bit_alloc[i],
							 buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (details && gres_job_ptr->gres_bit_step_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack_bit_str_hex(gres_job_ptr->
							 gres_bit_step_alloc[i],
							 buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (details && gres_job_ptr->gres_cnt_step_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack64(gres_job_ptr->
					       gres_cnt_step_alloc[i],
					       buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			rec_cnt++;
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a job's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_job_state_pack()
 * IN/OUT buffer - location to read state from
 * IN job_id - job's ID
 */
extern int gres_job_state_unpack(List *gres_list, buf_t *buffer,
				 uint32_t job_id,
				 uint16_t protocol_version)
{
	int i = 0, rc;
	uint32_t magic = 0, plugin_id = 0, utmp32 = 0;
	uint16_t rec_cnt = 0;
	uint8_t  has_more = 0;
	gres_state_t *gres_ptr;
	gres_job_state_t *gres_job_ptr = NULL;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(gres_job_list_delete);
	}

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;

		if (protocol_version >= SLURM_20_11_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;
			safe_unpack32(&plugin_id, buffer);
			gres_job_ptr = xmalloc(sizeof(gres_job_state_t));
			safe_unpack16(&gres_job_ptr->cpus_per_gres, buffer);
			safe_unpack16(&gres_job_ptr->flags, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_job, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_node, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_socket, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_task, buffer);
			safe_unpack64(&gres_job_ptr->mem_per_gres, buffer);
			safe_unpack16(&gres_job_ptr->ntasks_per_gres, buffer);
			safe_unpack64(&gres_job_ptr->total_gres, buffer);
			safe_unpackstr_xmalloc(&gres_job_ptr->type_name,
					       &utmp32, buffer);
			gres_job_ptr->type_id =
				gres_build_id(gres_job_ptr->type_name);
			safe_unpack32(&gres_job_ptr->node_cnt, buffer);
			if (gres_job_ptr->node_cnt > NO_VAL)
				goto unpack_error;

			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_unpack64_array(
					&gres_job_ptr->gres_cnt_node_alloc,
					&utmp32, buffer);
			}

			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_bit_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(bitstr_t *));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_job_ptr->
							   gres_bit_alloc[i],
							   buffer);
				}
			}
			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_bit_step_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(bitstr_t *));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_job_ptr->
							   gres_bit_step_alloc[i],
							   buffer);
				}
			}
			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_cnt_step_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(uint64_t));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					safe_unpack64(&gres_job_ptr->
						      gres_cnt_step_alloc[i],
						      buffer);
				}
			}
		} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;
			safe_unpack32(&plugin_id, buffer);
			gres_job_ptr = xmalloc(sizeof(gres_job_state_t));
			safe_unpack16(&gres_job_ptr->cpus_per_gres, buffer);
			safe_unpack16(&gres_job_ptr->flags, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_job, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_node, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_socket, buffer);
			safe_unpack64(&gres_job_ptr->gres_per_task, buffer);
			safe_unpack64(&gres_job_ptr->mem_per_gres, buffer);
			gres_job_ptr->ntasks_per_gres = NO_VAL16;
			safe_unpack64(&gres_job_ptr->total_gres, buffer);
			safe_unpackstr_xmalloc(&gres_job_ptr->type_name,
					       &utmp32, buffer);
			gres_job_ptr->type_id =
				gres_build_id(gres_job_ptr->type_name);
			safe_unpack32(&gres_job_ptr->node_cnt, buffer);
			if (gres_job_ptr->node_cnt > NO_VAL)
				goto unpack_error;

			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_unpack64_array(
					&gres_job_ptr->gres_cnt_node_alloc,
					&utmp32, buffer);
			}

			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_bit_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(bitstr_t *));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_job_ptr->
							   gres_bit_alloc[i],
							   buffer);
				}
			}
			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_bit_step_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(bitstr_t *));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_job_ptr->
							   gres_bit_step_alloc[i],
							   buffer);
				}
			}
			safe_unpack8(&has_more, buffer);
			if (has_more) {
				safe_xcalloc(gres_job_ptr->gres_cnt_step_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(uint64_t));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					safe_unpack64(&gres_job_ptr->
						      gres_cnt_step_alloc[i],
						      buffer);
				}
			}
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			goto unpack_error;
		}

		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_context[i].plugin_id == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			/*
			 * A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data.
			 */
			error("%s: no plugin configured to unpack data type %u from job %u. This is likely due to a difference in the GresTypes configured in slurm.conf on different cluster nodes.",
			      __func__, plugin_id, job_id);
			_job_state_delete(gres_job_ptr);
			continue;
		}
		gres_job_ptr->gres_name = xstrdup(gres_context[i].gres_name);
		gres_ptr = xmalloc(sizeof(gres_state_t));
		gres_ptr->plugin_id = gres_context[i].plugin_id;
		gres_ptr->gres_data = gres_job_ptr;
		gres_ptr->gres_name = xstrdup(gres_context[i].gres_name);
		gres_ptr->state_type = GRES_STATE_TYPE_JOB;
		gres_job_ptr = NULL;	/* nothing left to free on error */
		list_append(*gres_list, gres_ptr);
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("%s: unpack error from job %u", __func__, job_id);
	if (gres_job_ptr)
		_job_state_delete(gres_job_ptr);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

/*
 * Pack a job's allocated gres information for use by prolog/epilog
 * IN gres_list - generated by gres_job_config_validate()
 * IN/OUT buffer - location to write state to
 */
extern int gres_job_alloc_pack(List gres_list, buf_t *buffer,
			       uint16_t protocol_version)
{
	int i, rc = SLURM_SUCCESS;
	uint32_t top_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_epilog_info_t *gres_job_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_job_ptr = (gres_epilog_info_t *) list_next(gres_iter))) {
		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			pack32(magic, buffer);
			pack32(gres_job_ptr->plugin_id, buffer);
			pack32(gres_job_ptr->node_cnt, buffer);
			if (gres_job_ptr->gres_cnt_node_alloc) {
				pack8((uint8_t) 1, buffer);
				pack64_array(gres_job_ptr->gres_cnt_node_alloc,
					     gres_job_ptr->node_cnt, buffer);
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (gres_job_ptr->gres_bit_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					pack_bit_str_hex(gres_job_ptr->
							 gres_bit_alloc[i],
							 buffer);
				}
			} else {
				pack8((uint8_t) 0, buffer);
			}
			rec_cnt++;
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

static void _epilog_list_del(void *x)
{
	gres_epilog_info_t *epilog_info = (gres_epilog_info_t *) x;
	int i;

	if (!epilog_info)
		return;

	if (epilog_info->gres_bit_alloc) {
		for (i = 0; i < epilog_info->node_cnt; i++)
			FREE_NULL_BITMAP(epilog_info->gres_bit_alloc[i]);
		xfree(epilog_info->gres_bit_alloc);
	}
	xfree(epilog_info->gres_cnt_node_alloc);
	xfree(epilog_info->node_list);
	xfree(epilog_info);
}

/*
 * Unpack a job's allocated gres information for use by prolog/epilog
 * OUT gres_list - restored state stored by gres_job_alloc_pack()
 * IN/OUT buffer - location to read state from
 */
extern int gres_job_alloc_unpack(List *gres_list, buf_t *buffer,
				 uint16_t protocol_version)
{
	int i = 0, rc;
	uint32_t magic = 0, utmp32 = 0;
	uint16_t rec_cnt = 0;
	uint8_t filled = 0;
	gres_epilog_info_t *gres_job_ptr = NULL;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_epilog_list_del);
	}

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;

		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;
			gres_job_ptr = xmalloc(sizeof(gres_epilog_info_t));
			safe_unpack32(&gres_job_ptr->plugin_id, buffer);
			safe_unpack32(&gres_job_ptr->node_cnt, buffer);
			if (gres_job_ptr->node_cnt > NO_VAL)
				goto unpack_error;
			safe_unpack8(&filled, buffer);
			if (filled) {
				safe_unpack64_array(
					&gres_job_ptr->gres_cnt_node_alloc,
					&utmp32, buffer);
			}
			safe_unpack8(&filled, buffer);
			if (filled) {
				safe_xcalloc(gres_job_ptr->gres_bit_alloc,
					     gres_job_ptr->node_cnt,
					     sizeof(bitstr_t *));
				for (i = 0; i < gres_job_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_job_ptr->
							   gres_bit_alloc[i],
							   buffer);
				}
			}
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			goto unpack_error;
		}

		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_context[i].plugin_id ==
			    gres_job_ptr->plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			/*
			 * A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data.
			 */
			error("%s: no plugin configured to unpack data type %u",
			      __func__, gres_job_ptr->plugin_id);
			_epilog_list_del(gres_job_ptr);
			continue;
		}
		list_append(*gres_list, gres_job_ptr);
		gres_job_ptr = NULL;
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("%s: unpack error", __func__);
	if (gres_job_ptr)
		_epilog_list_del(gres_job_ptr);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

/*
 * Build List of information needed to set job's Prolog or Epilog environment
 * variables
 *
 * IN job_gres_list - job's GRES allocation info
 * IN hostlist - list of nodes associated with the job
 * RET information about the job's GRES allocation needed by Prolog or Epilog
 */
extern List gres_g_epilog_build_env(List job_gres_list, char *node_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr = NULL;
	gres_epilog_info_t *epilog_info;
	List epilog_gres_list = NULL;

	if (!job_gres_list)
		return NULL;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(job_gres_list);
	while ((gres_ptr = list_next(gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_ptr->plugin_id == gres_context[i].plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("%s: gres not found in context.  This should never happen",
			      __func__);
			continue;
		}

		if (!gres_context[i].ops.epilog_build_env)
			continue;	/* No plugin to call */
		epilog_info = (*(gres_context[i].ops.epilog_build_env))
			(gres_ptr->gres_data);
		if (!epilog_info)
			continue;	/* No info to add for this plugin */
		if (!epilog_gres_list)
			epilog_gres_list = list_create(_epilog_list_del);
		epilog_info->plugin_id = gres_context[i].plugin_id;
		epilog_info->node_list = xstrdup(node_list);
		list_append(epilog_gres_list, epilog_info);
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return epilog_gres_list;
}

/*
 * Set environment variables as appropriate for a job's prolog or epilog based
 * GRES allocated to the job.
 *
 * IN/OUT epilog_env_ptr - environment variable array
 * IN epilog_gres_list - generated by TBD
 * IN node_inx - zero origin node index
 */
extern void gres_g_epilog_set_env(char ***epilog_env_ptr,
				  List epilog_gres_list, int node_inx)
{
	int i;
	ListIterator epilog_iter;
	gres_epilog_info_t *epilog_info;

	*epilog_env_ptr = NULL;
	if (!epilog_gres_list)
		return;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	epilog_iter = list_iterator_create(epilog_gres_list);
	while ((epilog_info = list_next(epilog_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (epilog_info->plugin_id == gres_context[i].plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("%s: GRES ID %u not found in context",
			      __func__, epilog_info->plugin_id);
			continue;
		}

		if (!gres_context[i].ops.epilog_set_env)
			continue;	/* No plugin to call */
		(*(gres_context[i].ops.epilog_set_env))
			(epilog_env_ptr, epilog_info, node_inx);
	}
	list_iterator_destroy(epilog_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * If core bitmap from slurmd differs in size from that in slurmctld,
 * then modify bitmap from slurmd so we can use bit_and, bit_or, etc.
 */
static bitstr_t *_core_bitmap_rebuild(bitstr_t *old_core_bitmap, int new_size)
{
	int i, j, old_size, ratio;
	bitstr_t *new_core_bitmap;

	new_core_bitmap = bit_alloc(new_size);
	old_size = bit_size(old_core_bitmap);
	if (old_size > new_size) {
		ratio = old_size / new_size;
		for (i = 0; i < new_size; i++) {
			for (j = 0; j < ratio; j++) {
				if (bit_test(old_core_bitmap, i*ratio+j)) {
					bit_set(new_core_bitmap, i);
					break;
				}
			}
		}
	} else {
		ratio = new_size / old_size;
		for (i = 0; i < old_size; i++) {
			if (!bit_test(old_core_bitmap, i))
				continue;
			for (j = 0; j < ratio; j++) {
				bit_set(new_core_bitmap, i*ratio+j);
			}
		}
	}

	return new_core_bitmap;
}

extern void gres_validate_node_cores(gres_node_state_t *node_gres_ptr,
				     int cores_ctld, char *node_name)
{
	int i, cores_slurmd;
	bitstr_t *new_core_bitmap;
	int log_mismatch = true;

	if (node_gres_ptr->topo_cnt == 0)
		return;

	if (node_gres_ptr->topo_core_bitmap == NULL) {
		error("Gres topo_core_bitmap is NULL on node %s", node_name);
		return;
	}


	for (i = 0; i < node_gres_ptr->topo_cnt; i++) {
		if (!node_gres_ptr->topo_core_bitmap[i])
			continue;
		cores_slurmd = bit_size(node_gres_ptr->topo_core_bitmap[i]);
		if (cores_slurmd == cores_ctld)
			continue;
		if (log_mismatch) {
			debug("Rebuilding node %s gres core bitmap (%d != %d)",
			      node_name, cores_slurmd, cores_ctld);
			log_mismatch = false;
		}
		new_core_bitmap = _core_bitmap_rebuild(
			node_gres_ptr->topo_core_bitmap[i],
			cores_ctld);
		FREE_NULL_BITMAP(node_gres_ptr->topo_core_bitmap[i]);
		node_gres_ptr->topo_core_bitmap[i] = new_core_bitmap;
	}
}

static uint32_t _job_test(void *job_gres_data, void *node_gres_data,
			  bool use_total_gres, bitstr_t *core_bitmap,
			  int core_start_bit, int core_end_bit, bool *topo_set,
			  uint32_t job_id, char *node_name, char *gres_name,
			  uint32_t plugin_id, bool disable_binding)
{
	int i, j, core_size, core_ctld, top_inx = -1;
	uint64_t gres_avail = 0, gres_max = 0, gres_total, gres_tmp;
	uint64_t min_gres_node = 0;
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_node_state_t *node_gres_ptr = (gres_node_state_t *) node_gres_data;
	uint32_t *cores_addnt = NULL; /* Additional cores avail from this GRES */
	uint32_t *cores_avail = NULL; /* cores initially avail from this GRES */
	uint32_t core_cnt = 0;
	bitstr_t *alloc_core_bitmap = NULL;
	bitstr_t *avail_core_bitmap = NULL;
	bool shared_gres = gres_id_shared(plugin_id);
	bool use_busy_dev = false;

	if (node_gres_ptr->no_consume)
		use_total_gres = true;

	if (!use_total_gres &&
	    gres_id_shared(plugin_id) &&
	    (node_gres_ptr->gres_cnt_alloc != 0)) {
		/* We must use the ONE already active GRES of this type */
		use_busy_dev = true;
	}

	/* Determine minimum GRES count needed on this node */
	if (job_gres_ptr->gres_per_job)
		min_gres_node = 1;
	min_gres_node = MAX(min_gres_node, job_gres_ptr->gres_per_node);
	min_gres_node = MAX(min_gres_node, job_gres_ptr->gres_per_socket);
	min_gres_node = MAX(min_gres_node, job_gres_ptr->gres_per_task);

	if (min_gres_node && node_gres_ptr->topo_cnt && *topo_set) {
		/*
		 * Need to determine how many GRES available for these
		 * specific cores
		 */
		if (core_bitmap) {
			core_ctld = core_end_bit - core_start_bit + 1;
			if (core_ctld < 1) {
				error("gres/%s: job %u cores on node %s < 1",
				      gres_name, job_id, node_name);
				return (uint32_t) 0;
			}
			gres_validate_node_cores(node_gres_ptr, core_ctld,
						 node_name);
		}
		for (i = 0; i < node_gres_ptr->topo_cnt; i++) {
			if (job_gres_ptr->type_name &&
			    (!node_gres_ptr->topo_type_name[i] ||
			     (node_gres_ptr->topo_type_id[i] !=
			      job_gres_ptr->type_id)))
				continue;
			if (use_busy_dev &&
			    (node_gres_ptr->topo_gres_cnt_alloc[i] == 0))
				continue;
			if (!node_gres_ptr->topo_core_bitmap[i]) {
				gres_avail += node_gres_ptr->
					topo_gres_cnt_avail[i];
				if (!use_total_gres) {
					gres_avail -= node_gres_ptr->
						topo_gres_cnt_alloc[i];
				}
				if (shared_gres)
					gres_max = MAX(gres_max, gres_avail);
				continue;
			}
			core_ctld = bit_size(node_gres_ptr->
					     topo_core_bitmap[i]);
			for (j = 0; j < core_ctld; j++) {
				if (core_bitmap &&
				    !bit_test(core_bitmap, core_start_bit + j))
					continue;
				if (!bit_test(node_gres_ptr->
					      topo_core_bitmap[i], j))
					continue; /* not avail for this gres */
				gres_avail += node_gres_ptr->
					topo_gres_cnt_avail[i];
				if (!use_total_gres) {
					gres_avail -= node_gres_ptr->
						topo_gres_cnt_alloc[i];
				}
				if (shared_gres)
					gres_max = MAX(gres_max, gres_avail);
				break;
			}
		}
		if (shared_gres)
			gres_avail = gres_max;
		if (min_gres_node > gres_avail)
			return (uint32_t) 0;	/* insufficient GRES avail */
		return NO_VAL;
	} else if (min_gres_node && node_gres_ptr->topo_cnt &&
		   !disable_binding) {
		/* Need to determine which specific cores can be used */
		gres_avail = node_gres_ptr->gres_cnt_avail;
		if (!use_total_gres)
			gres_avail -= node_gres_ptr->gres_cnt_alloc;
		if (min_gres_node > gres_avail)
			return (uint32_t) 0;	/* insufficient GRES avail */

		core_ctld = core_end_bit - core_start_bit + 1;
		if (core_bitmap) {
			if (core_ctld < 1) {
				error("gres/%s: job %u cores on node %s < 1",
				      gres_name, job_id, node_name);
				return (uint32_t) 0;
			}
			gres_validate_node_cores(node_gres_ptr, core_ctld,
						 node_name);
		} else {
			for (i = 0; i < node_gres_ptr->topo_cnt; i++) {
				if (!node_gres_ptr->topo_core_bitmap[i])
					continue;
				core_ctld = bit_size(node_gres_ptr->
						     topo_core_bitmap[i]);
				break;
			}
		}

		alloc_core_bitmap = bit_alloc(core_ctld);
		if (core_bitmap) {
			for (j = 0; j < core_ctld; j++) {
				if (bit_test(core_bitmap, core_start_bit + j))
					bit_set(alloc_core_bitmap, j);
			}
		} else {
			bit_nset(alloc_core_bitmap, 0, core_ctld - 1);
		}

		avail_core_bitmap = bit_copy(alloc_core_bitmap);
		cores_addnt = xcalloc(node_gres_ptr->topo_cnt,
				      sizeof(uint32_t));
		cores_avail = xcalloc(node_gres_ptr->topo_cnt,
				      sizeof(uint32_t));
		for (i = 0; i < node_gres_ptr->topo_cnt; i++) {
			if (node_gres_ptr->topo_gres_cnt_avail[i] == 0)
				continue;
			if (use_busy_dev &&
			    (node_gres_ptr->topo_gres_cnt_alloc[i] == 0))
				continue;
			if (!use_total_gres &&
			    (node_gres_ptr->topo_gres_cnt_alloc[i] >=
			     node_gres_ptr->topo_gres_cnt_avail[i]))
				continue;
			if (job_gres_ptr->type_name &&
			    (!node_gres_ptr->topo_type_name[i] ||
			     (node_gres_ptr->topo_type_id[i] !=
			      job_gres_ptr->type_id)))
				continue;
			if (!node_gres_ptr->topo_core_bitmap[i]) {
				cores_avail[i] = core_end_bit -
					core_start_bit + 1;
				continue;
			}
			core_size = bit_size(node_gres_ptr->topo_core_bitmap[i]);
			for (j = 0; j < core_size; j++) {
				if (core_bitmap &&
				    !bit_test(core_bitmap, core_start_bit + j))
					continue;
				if (bit_test(node_gres_ptr->
					     topo_core_bitmap[i], j)) {
					cores_avail[i]++;
				}
			}
		}

		/* Pick the topology entries with the most cores available */
		gres_avail = 0;
		gres_total = 0;
		while (gres_avail < min_gres_node) {
			top_inx = -1;
			for (j = 0; j < node_gres_ptr->topo_cnt; j++) {
				if ((gres_avail == 0) ||
				    (cores_avail[j] == 0) ||
				    !node_gres_ptr->topo_core_bitmap[j]) {
					cores_addnt[j] = cores_avail[j];
				} else {
					cores_addnt[j] = cores_avail[j] -
						bit_overlap(alloc_core_bitmap,
							    node_gres_ptr->
							    topo_core_bitmap[j]);
				}

				if (top_inx == -1) {
					if (cores_avail[j])
						top_inx = j;
				} else if (cores_addnt[j] > cores_addnt[top_inx])
					top_inx = j;
			}
			if ((top_inx < 0) || (cores_avail[top_inx] == 0)) {
				if (gres_total < min_gres_node)
					core_cnt = 0;
				break;
			}
			cores_avail[top_inx] = 0;	/* Flag as used */
			gres_tmp = node_gres_ptr->topo_gres_cnt_avail[top_inx];
			if (!use_total_gres &&
			    (gres_tmp >=
			     node_gres_ptr->topo_gres_cnt_alloc[top_inx])) {
				gres_tmp -= node_gres_ptr->
					topo_gres_cnt_alloc[top_inx];
			} else if (!use_total_gres) {
				gres_tmp = 0;
			}
			if (gres_tmp == 0) {
				error("gres/%s: topology allocation error on node %s",
				      gres_name, node_name);
				break;
			}
			/* update counts of allocated cores and GRES */
			if (shared_gres) {
				/*
				 * Process outside of loop after specific
				 * device selected
				 */
			} else if (!node_gres_ptr->topo_core_bitmap[top_inx]) {
				bit_nset(alloc_core_bitmap, 0, core_ctld - 1);
			} else if (gres_avail) {
				bit_or(alloc_core_bitmap,
				       node_gres_ptr->
				       topo_core_bitmap[top_inx]);
				if (core_bitmap)
					bit_and(alloc_core_bitmap,
						avail_core_bitmap);
			} else {
				bit_and(alloc_core_bitmap,
					node_gres_ptr->
					topo_core_bitmap[top_inx]);
			}
			if (shared_gres) {
				gres_total = MAX(gres_total, gres_tmp);
				gres_avail = gres_total;
			} else {
				/*
				 * Available GRES count is up to gres_tmp,
				 * but take 1 per loop to maximize available
				 * core count
				 */
				gres_avail += 1;
				gres_total += gres_tmp;
				core_cnt = bit_set_count(alloc_core_bitmap);
			}
		}
		if (shared_gres && (top_inx >= 0) &&
		    (gres_avail >= min_gres_node)) {
			if (!node_gres_ptr->topo_core_bitmap[top_inx]) {
				bit_nset(alloc_core_bitmap, 0, core_ctld - 1);
			} else {
				bit_or(alloc_core_bitmap,
				       node_gres_ptr->
				       topo_core_bitmap[top_inx]);
				if (core_bitmap)
					bit_and(alloc_core_bitmap,
						avail_core_bitmap);
			}
			core_cnt = bit_set_count(alloc_core_bitmap);
		}
		if (core_bitmap && (core_cnt > 0)) {
			*topo_set = true;
			for (i = 0; i < core_ctld; i++) {
				if (!bit_test(alloc_core_bitmap, i)) {
					bit_clear(core_bitmap,
						  core_start_bit + i);
				}
			}
		}
		FREE_NULL_BITMAP(alloc_core_bitmap);
		FREE_NULL_BITMAP(avail_core_bitmap);
		xfree(cores_addnt);
		xfree(cores_avail);
		return core_cnt;
	} else if (job_gres_ptr->type_name) {
		for (i = 0; i < node_gres_ptr->type_cnt; i++) {
			if (node_gres_ptr->type_name[i] &&
			    (node_gres_ptr->type_id[i] ==
			     job_gres_ptr->type_id))
				break;
		}
		if (i >= node_gres_ptr->type_cnt)
			return (uint32_t) 0;	/* no such type */
		gres_avail = node_gres_ptr->type_cnt_avail[i];
		if (!use_total_gres)
			gres_avail -= node_gres_ptr->type_cnt_alloc[i];
		gres_tmp = node_gres_ptr->gres_cnt_avail;
		if (!use_total_gres)
			gres_tmp -= node_gres_ptr->gres_cnt_alloc;
		gres_avail = MIN(gres_avail, gres_tmp);
		if (min_gres_node > gres_avail)
			return (uint32_t) 0;	/* insufficient GRES avail */
		return NO_VAL;
	} else {
		gres_avail = node_gres_ptr->gres_cnt_avail;
		if (!use_total_gres)
			gres_avail -= node_gres_ptr->gres_cnt_alloc;
		if (min_gres_node > gres_avail)
			return (uint32_t) 0;	/* insufficient GRES avail */
		return NO_VAL;
	}
}

/*
 * Determine how many cores on the node can be used by this job
 * IN job_gres_list  - job's gres_list built by gres_job_state_validate()
 * IN node_gres_list - node's gres_list built by gres_node_config_validate()
 * IN use_total_gres - if set then consider all gres resources as available,
 *		       and none are commited to running jobs
 * IN core_bitmap    - Identification of available cores (NULL if no restriction)
 * IN core_start_bit - index into core_bitmap for this node's first core
 * IN core_end_bit   - index into core_bitmap for this node's last core
 * IN job_id         - job's ID (for logging)
 * IN node_name      - name of the node (for logging)
 * IN disable binding- --gres-flags=disable-binding
 * RET: NO_VAL    - All cores on node are available
 *      otherwise - Count of available cores
 */
extern uint32_t gres_job_test(List job_gres_list, List node_gres_list,
			      bool use_total_gres, bitstr_t *core_bitmap,
			      int core_start_bit, int core_end_bit,
			      uint32_t job_id, char *node_name,
			      bool disable_binding)
{
	int i;
	uint32_t core_cnt, tmp_cnt;
	ListIterator job_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;
	bool topo_set = false;

	if (job_gres_list == NULL)
		return NO_VAL;
	if (node_gres_list == NULL)
		return 0;

	core_cnt = NO_VAL;
	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_ptr = list_find_first(node_gres_list, gres_find_id,
						&job_gres_ptr->plugin_id);
		if (node_gres_ptr == NULL) {
			/* node lack resources required by the job */
			core_cnt = 0;
			break;
		}

		for (i = 0; i < gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id !=
			    gres_context[i].plugin_id)
				continue;
			tmp_cnt = _job_test(job_gres_ptr->gres_data,
					    node_gres_ptr->gres_data,
					    use_total_gres, core_bitmap,
					    core_start_bit, core_end_bit,
					    &topo_set, job_id, node_name,
					    gres_context[i].gres_name,
					    gres_context[i].plugin_id,
					    disable_binding);
			if (tmp_cnt != NO_VAL) {
				if (core_cnt == NO_VAL)
					core_cnt = tmp_cnt;
				else
					core_cnt = MIN(tmp_cnt, core_cnt);
			}
			break;
		}
		if (core_cnt == 0)
			break;
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return core_cnt;
}

extern void gres_sock_delete(void *x)
{
	sock_gres_t *sock_gres = (sock_gres_t *) x;
	int s;

	if (sock_gres) {
		FREE_NULL_BITMAP(sock_gres->bits_any_sock);
		if (sock_gres->bits_by_sock) {
			for (s = 0; s < sock_gres->sock_cnt; s++)
				FREE_NULL_BITMAP(sock_gres->bits_by_sock[s]);
			xfree(sock_gres->bits_by_sock);
		}
		xfree(sock_gres->cnt_by_sock);
		xfree(sock_gres->gres_name);
		/* NOTE: sock_gres->job_specs is just a pointer, do not free */
		xfree(sock_gres->type_name);
		xfree(sock_gres);
	}
}

/*
 * Build a string containing the GRES details for a given node and socket
 * sock_gres_list IN - List of sock_gres_t entries
 * sock_inx IN - zero-origin socket for which information is to be returned
 *		 if value < 0, then report GRES unconstrained by core
 * RET string, must call xfree() to release memory
 */
extern char *gres_sock_str(List sock_gres_list, int sock_inx)
{
	ListIterator iter;
	sock_gres_t *sock_gres;
	char *gres_str = NULL, *sep = "";

	if (!sock_gres_list)
		return NULL;

	iter = list_iterator_create(sock_gres_list);
	while ((sock_gres = (sock_gres_t *) list_next(iter))) {
		if (sock_inx < 0) {
			if (sock_gres->cnt_any_sock) {
				if (sock_gres->type_name) {
					xstrfmtcat(gres_str, "%s%s:%s:%"PRIu64,
						   sep, sock_gres->gres_name,
						   sock_gres->type_name,
						   sock_gres->cnt_any_sock);
				} else {
					xstrfmtcat(gres_str, "%s%s:%"PRIu64,
						   sep, sock_gres->gres_name,
						   sock_gres->cnt_any_sock);
				}
				sep = " ";
			}
			continue;
		}
		if (!sock_gres->cnt_by_sock ||
		    (sock_gres->cnt_by_sock[sock_inx] == 0))
			continue;
		if (sock_gres->type_name) {
			xstrfmtcat(gres_str, "%s%s:%s:%"PRIu64, sep,
				   sock_gres->gres_name, sock_gres->type_name,
				   sock_gres->cnt_by_sock[sock_inx]);
		} else {
			xstrfmtcat(gres_str, "%s%s:%"PRIu64, sep,
				   sock_gres->gres_name,
				   sock_gres->cnt_by_sock[sock_inx]);
		}
		sep = " ";
	}
	list_iterator_destroy(iter);
	return gres_str;
}

/*
 * Determine how many GRES of a given type can be used by this job on a
 * given node and return a structure with the details. Note that multiple
 * GRES of a given type model can be distributed over multiple topo structures,
 * so we need to OR the core_bitmap over all of them.
 */
static sock_gres_t *_build_sock_gres_by_topo(
	gres_job_state_t *job_gres_ptr,
	gres_node_state_t *node_gres_ptr,
	bool use_total_gres, bitstr_t *core_bitmap,
	uint16_t sockets, uint16_t cores_per_sock,
	uint32_t job_id, char *node_name,
	bool enforce_binding, uint32_t s_p_n,
	bitstr_t **req_sock_map,
	uint32_t main_plugin_id, uint32_t alt_plugin_id,
	gres_node_state_t *alt_node_gres_ptr,
	uint32_t user_id, const uint32_t node_inx)
{
	int i, j, s, c;
	uint32_t tot_cores;
	sock_gres_t *sock_gres;
	int64_t add_gres;
	uint64_t avail_gres, min_gres = 1;
	bool match = false;
	bool use_busy_dev = false;

	if (node_gres_ptr->gres_cnt_avail == 0)
		return NULL;

	if (!use_total_gres &&
	    gres_id_shared(main_plugin_id) &&
	    (node_gres_ptr->gres_cnt_alloc != 0)) {
		/* We must use the ONE already active GRES of this type */
		use_busy_dev = true;
	}

	sock_gres = xmalloc(sizeof(sock_gres_t));
	sock_gres->sock_cnt = sockets;
	sock_gres->bits_by_sock = xcalloc(sockets, sizeof(bitstr_t *));
	sock_gres->cnt_by_sock = xcalloc(sockets, sizeof(uint64_t));
	for (i = 0; i < node_gres_ptr->topo_cnt; i++) {
		bool use_all_sockets = false;
		if (job_gres_ptr->type_name &&
		    (job_gres_ptr->type_id != node_gres_ptr->topo_type_id[i]))
			continue;	/* Wrong type_model */
		if (use_busy_dev &&
		    (node_gres_ptr->topo_gres_cnt_alloc[i] == 0))
			continue;
		if (!use_total_gres && !node_gres_ptr->no_consume &&
		    (node_gres_ptr->topo_gres_cnt_alloc[i] >=
		     node_gres_ptr->topo_gres_cnt_avail[i])) {
			continue;	/* No GRES remaining */
		}

		if (!use_total_gres && !node_gres_ptr->no_consume) {
			avail_gres = node_gres_ptr->topo_gres_cnt_avail[i] -
				node_gres_ptr->topo_gres_cnt_alloc[i];
		} else {
			avail_gres = node_gres_ptr->topo_gres_cnt_avail[i];
		}
		if (avail_gres == 0)
			continue;

		/*
		 * Job requested GPUs or MPS. Filter out resources already
		 * allocated to the other GRES type.
		 */
		if (alt_node_gres_ptr && alt_node_gres_ptr->gres_bit_alloc &&
		    node_gres_ptr->topo_gres_bitmap[i]) {
			c = bit_overlap(node_gres_ptr->topo_gres_bitmap[i],
					alt_node_gres_ptr->gres_bit_alloc);
			if ((alt_plugin_id == gpu_plugin_id) && (c > 0))
				continue;
			if ((alt_plugin_id == mps_plugin_id) && (c > 0)) {
				avail_gres -= c;
				if (avail_gres == 0)
					continue;
			}
		}

		/* gres/mps can only use one GPU per node */
		if ((main_plugin_id == mps_plugin_id) &&
		    (avail_gres > sock_gres->max_node_gres))
			sock_gres->max_node_gres = avail_gres;

		/*
		 * If some GRES is available on every socket,
		 * treat like no topo_core_bitmap is specified
		 */
		tot_cores = sockets * cores_per_sock;
		if (node_gres_ptr->topo_core_bitmap &&
		    node_gres_ptr->topo_core_bitmap[i]) {
			use_all_sockets = true;
			for (s = 0; s < sockets; s++) {
				bool use_this_socket = false;
				for (c = 0; c < cores_per_sock; c++) {
					j = (s * cores_per_sock) + c;
					if (bit_test(node_gres_ptr->
						     topo_core_bitmap[i], j)) {
						use_this_socket = true;
						break;
					}
				}
				if (!use_this_socket) {
					use_all_sockets = false;
					break;
				}
			}
		}

		if (!node_gres_ptr->topo_core_bitmap ||
		    !node_gres_ptr->topo_core_bitmap[i] ||
		    use_all_sockets) {
			/*
			 * Not constrained by core, but only specific
			 * GRES may be available (save their bitmap)
			 */
			sock_gres->cnt_any_sock += avail_gres;
			sock_gres->total_cnt += avail_gres;
			if (!sock_gres->bits_any_sock) {
				sock_gres->bits_any_sock =
					bit_copy(node_gres_ptr->
						 topo_gres_bitmap[i]);
			} else {
				bit_or(sock_gres->bits_any_sock,
				       node_gres_ptr->topo_gres_bitmap[i]);
			}
			match = true;
			continue;
		}

		/* Constrained by core */
		if (core_bitmap)
			tot_cores = MIN(tot_cores, bit_size(core_bitmap));
		if (node_gres_ptr->topo_core_bitmap[i]) {
			tot_cores = MIN(tot_cores,
					bit_size(node_gres_ptr->
						 topo_core_bitmap[i]));
		}
		for (s = 0; ((s < sockets) && avail_gres); s++) {
			if (enforce_binding && core_bitmap) {
				for (c = 0; c < cores_per_sock; c++) {
					j = (s * cores_per_sock) + c;
					if (bit_test(core_bitmap, j))
						break;
				}
				if (c >= cores_per_sock) {
					/* No available cores on this socket */
					continue;
				}
			}
			for (c = 0; c < cores_per_sock; c++) {
				j = (s * cores_per_sock) + c;
				if (j >= tot_cores)
					break;	/* Off end of core bitmap */
				if (node_gres_ptr->topo_core_bitmap[i] &&
				    !bit_test(node_gres_ptr->topo_core_bitmap[i],
					      j))
					continue;
				if (!node_gres_ptr->topo_gres_bitmap[i]) {
					error("%s: topo_gres_bitmap NULL on node %s",
					      __func__, node_name);
					continue;
				}
				if (!sock_gres->bits_by_sock[s]) {
					sock_gres->bits_by_sock[s] =
						bit_copy(node_gres_ptr->
							 topo_gres_bitmap[i]);
				} else {
					bit_or(sock_gres->bits_by_sock[s],
					       node_gres_ptr->topo_gres_bitmap[i]);
				}
				sock_gres->cnt_by_sock[s] += avail_gres;
				sock_gres->total_cnt += avail_gres;
				avail_gres = 0;
				match = true;
				break;
			}
		}
	}

	/* Process per-GRES limits */
	if (match && job_gres_ptr->gres_per_socket) {
		/*
		 * Clear core bitmap on sockets with insufficient GRES
		 * and disable excess GRES per socket
		 */
		for (s = 0; s < sockets; s++) {
			if (sock_gres->cnt_by_sock[s] <
			    job_gres_ptr->gres_per_socket) {
				/* Insufficient GRES, clear count */
				sock_gres->total_cnt -=
					sock_gres->cnt_by_sock[s];
				sock_gres->cnt_by_sock[s] = 0;
				if (enforce_binding && core_bitmap) {
					i = s * cores_per_sock;
					bit_nclear(core_bitmap, i,
						   i + cores_per_sock - 1);
				}
			} else if (sock_gres->cnt_by_sock[s] >
				   job_gres_ptr->gres_per_socket) {
				/* Excess GRES, reduce count */
				i = sock_gres->cnt_by_sock[s] -
					job_gres_ptr->gres_per_socket;
				sock_gres->cnt_by_sock[s] =
					job_gres_ptr->gres_per_socket;
				sock_gres->total_cnt -= i;
			}
		}
	}

	/*
	 * Satisfy sockets-per-node (s_p_n) limit by selecting the sockets with
	 * the most GRES. Sockets with low GRES counts have their core_bitmap
	 * cleared so that _allocate_sc() in cons_tres/job_test.c does not
	 * remove sockets needed to satisfy the job's GRES specification.
	 */
	if (match && enforce_binding && core_bitmap && (s_p_n < sockets)) {
		int avail_sock = 0;
		bool *avail_sock_flag = xcalloc(sockets, sizeof(bool));
		for (s = 0; s < sockets; s++) {
			if (sock_gres->cnt_by_sock[s] == 0)
				continue;
			for (c = 0; c < cores_per_sock; c++) {
				i = (s * cores_per_sock) + c;
				if (!bit_test(core_bitmap, i))
					continue;
				avail_sock++;
				avail_sock_flag[s] = true;
				break;
			}
		}
		while (avail_sock > s_p_n) {
			int low_gres_sock_inx = -1;
			for (s = 0; s < sockets; s++) {
				if (!avail_sock_flag[s])
					continue;
				if ((low_gres_sock_inx == -1) ||
				    (sock_gres->cnt_by_sock[s] <
				     sock_gres->cnt_by_sock[low_gres_sock_inx]))
					low_gres_sock_inx = s;
			}
			if (low_gres_sock_inx == -1)
				break;
			s = low_gres_sock_inx;
			i = s * cores_per_sock;
			bit_nclear(core_bitmap, i, i + cores_per_sock - 1);
			sock_gres->total_cnt -= sock_gres->cnt_by_sock[s];
			sock_gres->cnt_by_sock[s] = 0;
			avail_sock--;
			avail_sock_flag[s] = false;
		}
		xfree(avail_sock_flag);
	}

	if (match) {
		if (job_gres_ptr->gres_per_node)
			min_gres = job_gres_ptr->gres_per_node;
		if (job_gres_ptr->gres_per_task)
			min_gres = MAX(min_gres, job_gres_ptr->gres_per_task);
		if (sock_gres->total_cnt < min_gres)
			match = false;
	}


	/*
	 * If sockets-per-node (s_p_n) not specified then identify sockets
	 * which are required to satisfy gres_per_node or task specification
	 * so that allocated tasks can be distributed over multiple sockets
	 * if necessary.
	 */
	add_gres = min_gres - sock_gres->cnt_any_sock;
	if (match && core_bitmap && (s_p_n == NO_VAL) && (add_gres > 0) &&
	    job_gres_ptr->gres_per_node) {
		int avail_sock = 0, best_sock_inx = -1;
		bool *avail_sock_flag = xcalloc(sockets, sizeof(bool));
		for (s = 0; s < sockets; s++) {
			if (sock_gres->cnt_by_sock[s] == 0)
				continue;
			for (c = 0; c < cores_per_sock; c++) {
				i = (s * cores_per_sock) + c;
				if (!bit_test(core_bitmap, i))
					continue;
				avail_sock++;
				avail_sock_flag[s] = true;
				if ((best_sock_inx == -1) ||
				    (sock_gres->cnt_by_sock[s] >
				     sock_gres->cnt_by_sock[best_sock_inx])) {
					best_sock_inx = s;
				}
				break;
			}
		}
		while ((best_sock_inx != -1) && (add_gres > 0)) {
			if (*req_sock_map == NULL)
				*req_sock_map = bit_alloc(sockets);
			bit_set(*req_sock_map, best_sock_inx);
			add_gres -= sock_gres->cnt_by_sock[best_sock_inx];
			avail_sock_flag[best_sock_inx] = false;
			if (add_gres <= 0)
				break;
			/* Find next best socket */
			best_sock_inx = -1;
			for (s = 0; s < sockets; s++) {
				if ((sock_gres->cnt_by_sock[s] == 0) ||
				    !avail_sock_flag[s])
					continue;
				if ((best_sock_inx == -1) ||
				    (sock_gres->cnt_by_sock[s] >
				     sock_gres->cnt_by_sock[best_sock_inx])) {
					best_sock_inx = s;
				}
			}
		}
		xfree(avail_sock_flag);
	}

	if (match) {
		sock_gres->type_id = job_gres_ptr->type_id;
		sock_gres->type_name = xstrdup(job_gres_ptr->type_name);
	} else {
		gres_sock_delete(sock_gres);
		sock_gres = NULL;
	}
	return sock_gres;
}

/*
 * Determine how many GRES of a given type can be used by this job on a
 * given node and return a structure with the details. Note that multiple
 * GRES of a given type model can be configured, so pick the right one.
 */
static sock_gres_t *_build_sock_gres_by_type(gres_job_state_t *job_gres_ptr,
					     gres_node_state_t *node_gres_ptr,
					     bool use_total_gres, bitstr_t *core_bitmap,
					     uint16_t sockets, uint16_t cores_per_sock,
					     uint32_t job_id, char *node_name)
{
	int i;
	sock_gres_t *sock_gres;
	uint64_t avail_gres, min_gres = 1, gres_tmp;
	bool match = false;

	if (job_gres_ptr->gres_per_node)
		min_gres = job_gres_ptr-> gres_per_node;
	if (job_gres_ptr->gres_per_socket)
		min_gres = MAX(min_gres, job_gres_ptr->gres_per_socket);
	if (job_gres_ptr->gres_per_task)
		min_gres = MAX(min_gres, job_gres_ptr->gres_per_task);
	sock_gres = xmalloc(sizeof(sock_gres_t));
	for (i = 0; i < node_gres_ptr->type_cnt; i++) {
		if (job_gres_ptr->type_name &&
		    (job_gres_ptr->type_id != node_gres_ptr->type_id[i]))
			continue;	/* Wrong type_model */
		if (!use_total_gres &&
		    (node_gres_ptr->type_cnt_alloc[i] >=
		     node_gres_ptr->type_cnt_avail[i])) {
			continue;	/* No GRES remaining */
		} else if (!use_total_gres) {
			avail_gres = node_gres_ptr->type_cnt_avail[i] -
				node_gres_ptr->type_cnt_alloc[i];
		} else {
			avail_gres = node_gres_ptr->type_cnt_avail[i];
		}
		gres_tmp = node_gres_ptr->gres_cnt_avail;
		if (!use_total_gres)
			gres_tmp -= node_gres_ptr->gres_cnt_alloc;
		avail_gres = MIN(avail_gres, gres_tmp);
		if (avail_gres < min_gres)
			continue;	/* Insufficient GRES remaining */
		sock_gres->cnt_any_sock += avail_gres;
		sock_gres->total_cnt += avail_gres;
		match = true;
	}
	if (match) {
		sock_gres->type_id = job_gres_ptr->type_id;
		sock_gres->type_name = xstrdup(job_gres_ptr->type_name);
	} else
		xfree(sock_gres);

	return sock_gres;
}

/*
 * Determine how many GRES of a given type can be used by this job on a
 * given node and return a structure with the details. No GRES type.
 */
static sock_gres_t *_build_sock_gres_basic(gres_job_state_t *job_gres_ptr,
					   gres_node_state_t *node_gres_ptr,
					   bool use_total_gres, bitstr_t *core_bitmap,
					   uint16_t sockets, uint16_t cores_per_sock,
					   uint32_t job_id, char *node_name)
{
	sock_gres_t *sock_gres;
	uint64_t avail_gres, min_gres = 1;

	if (job_gres_ptr->type_name)
		return NULL;
	if (!use_total_gres &&
	    (node_gres_ptr->gres_cnt_alloc >= node_gres_ptr->gres_cnt_avail))
		return NULL;	/* No GRES remaining */

	if (job_gres_ptr->gres_per_node)
		min_gres = job_gres_ptr-> gres_per_node;
	if (job_gres_ptr->gres_per_socket)
		min_gres = MAX(min_gres, job_gres_ptr->gres_per_socket);
	if (job_gres_ptr->gres_per_task)
		min_gres = MAX(min_gres, job_gres_ptr->gres_per_task);
	if (!use_total_gres) {
		avail_gres = node_gres_ptr->gres_cnt_avail -
			node_gres_ptr->gres_cnt_alloc;
	} else
		avail_gres = node_gres_ptr->gres_cnt_avail;
	if (avail_gres < min_gres)
		return NULL;	/* Insufficient GRES remaining */

	sock_gres = xmalloc(sizeof(sock_gres_t));
	sock_gres->cnt_any_sock += avail_gres;
	sock_gres->total_cnt += avail_gres;

	return sock_gres;
}

static void _sock_gres_log(List sock_gres_list, char *node_name)
{
	sock_gres_t *sock_gres;
	ListIterator iter;
	int i, len = -1;
	char tmp[32] = "";

	if (!sock_gres_list)
		return;

	info("Sock_gres state for %s", node_name);
	iter = list_iterator_create(sock_gres_list);
	while ((sock_gres = (sock_gres_t *) list_next(iter))) {
		info("Gres:%s Type:%s TotalCnt:%"PRIu64" MaxNodeGres:%"PRIu64,
		     sock_gres->gres_name, sock_gres->type_name,
		     sock_gres->total_cnt, sock_gres->max_node_gres);
		if (sock_gres->bits_any_sock) {
			bit_fmt(tmp, sizeof(tmp), sock_gres->bits_any_sock);
			len = bit_size(sock_gres->bits_any_sock);
		}
		info("  Sock[ANY]Cnt:%"PRIu64" Bits:%s of %d",
		     sock_gres->cnt_any_sock, tmp, len);

		for (i = 0; i < sock_gres->sock_cnt; i++) {
			if (sock_gres->cnt_by_sock[i] == 0)
				continue;
			tmp[0] = '\0';
			len = -1;
			if (sock_gres->bits_by_sock &&
			    sock_gres->bits_by_sock[i]) {
				bit_fmt(tmp, sizeof(tmp),
					sock_gres->bits_by_sock[i]);
				len = bit_size(sock_gres->bits_by_sock[i]);
			}
			info("  Sock[%d]Cnt:%"PRIu64" Bits:%s of %d", i,
			     sock_gres->cnt_by_sock[i], tmp, len);
		}
	}
	list_iterator_destroy(iter);
}

/*
 * Determine how many cores on each socket of a node can be used by this job
 * IN job_gres_list   - job's gres_list built by gres_job_state_validate()
 * IN node_gres_list  - node's gres_list built by gres_node_config_validate()
 * IN use_total_gres  - if set then consider all gres resources as available,
 *			and none are commited to running jobs
 * IN/OUT core_bitmap - Identification of available cores on this node
 * IN sockets         - Count of sockets on the node
 * IN cores_per_sock  - Count of cores per socket on this node
 * IN job_id          - job's ID (for logging)
 * IN node_name       - name of the node (for logging)
 * IN enforce_binding - if true then only use GRES with direct access to cores
 * IN s_p_n           - Expected sockets_per_node (NO_VAL if not limited)
 * OUT req_sock_map   - bitmap of specific requires sockets
 * IN user_id         - job's user ID
 * IN node_inx        - index of node to be evaluated
 * RET: List of sock_gres_t entries identifying what resources are available on
 *	each socket. Returns NULL if none available. Call FREE_NULL_LIST() to
 *	release memory.
 */
extern List gres_job_test2(List job_gres_list, List node_gres_list,
			   bool use_total_gres, bitstr_t *core_bitmap,
			   uint16_t sockets, uint16_t cores_per_sock,
			   uint32_t job_id, char *node_name,
			   bool enforce_binding, uint32_t s_p_n,
			   bitstr_t **req_sock_map, uint32_t user_id,
			   const uint32_t node_inx)
{
	List sock_gres_list = NULL;
	ListIterator job_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;
	gres_job_state_t  *job_data_ptr;
	gres_node_state_t *node_data_ptr;
	uint32_t local_s_p_n;

	if (!job_gres_list || (list_count(job_gres_list) == 0))
		return sock_gres_list;
	if (!node_gres_list)	/* Node lacks GRES to match */
		return sock_gres_list;
	(void) gres_init();

	sock_gres_list = list_create(gres_sock_delete);
	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		sock_gres_t *sock_gres = NULL;
		node_gres_ptr = list_find_first(node_gres_list, gres_find_id,
						&job_gres_ptr->plugin_id);
		if (node_gres_ptr == NULL) {
			/* node lack GRES of type required by the job */
			FREE_NULL_LIST(sock_gres_list);
			break;
		}
		job_data_ptr = (gres_job_state_t *) job_gres_ptr->gres_data;
		node_data_ptr = (gres_node_state_t *) node_gres_ptr->gres_data;

		if (job_data_ptr->gres_per_job &&
		    !job_data_ptr->gres_per_socket)
			local_s_p_n = s_p_n;	/* Maximize GRES per node */
		else
			local_s_p_n = NO_VAL;	/* No need to optimize socket */
		if (core_bitmap && (bit_ffs(core_bitmap) == -1)) {
			sock_gres = NULL;	/* No cores available */
		} else if (node_data_ptr->topo_cnt) {
			uint32_t alt_plugin_id = 0;
			gres_node_state_t *alt_node_data_ptr = NULL;
			if (!use_total_gres && have_gpu && have_mps) {
				if (job_gres_ptr->plugin_id == gpu_plugin_id)
					alt_plugin_id = mps_plugin_id;
				if (job_gres_ptr->plugin_id == mps_plugin_id)
					alt_plugin_id = gpu_plugin_id;
			}
			if (alt_plugin_id) {
				node_gres_ptr = list_find_first(node_gres_list,
								gres_find_id,
								&alt_plugin_id);
			}
			if (alt_plugin_id && node_gres_ptr) {
				alt_node_data_ptr = (gres_node_state_t *)
					node_gres_ptr->gres_data;
			} else {
				/* GRES of interest not on this node */
				alt_plugin_id = 0;
			}
			sock_gres = _build_sock_gres_by_topo(
				job_data_ptr,
				node_data_ptr, use_total_gres,
				core_bitmap, sockets, cores_per_sock,
				job_id, node_name, enforce_binding,
				local_s_p_n, req_sock_map,
				job_gres_ptr->plugin_id,
				alt_plugin_id, alt_node_data_ptr,
				user_id, node_inx);
		} else if (node_data_ptr->type_cnt) {
			sock_gres = _build_sock_gres_by_type(
				job_data_ptr,
				node_data_ptr, use_total_gres,
				core_bitmap, sockets, cores_per_sock,
				job_id, node_name);
		} else {
			sock_gres = _build_sock_gres_basic(
				job_data_ptr,
				node_data_ptr, use_total_gres,
				core_bitmap, sockets, cores_per_sock,
				job_id, node_name);
		}
		if (!sock_gres) {
			/* node lack available resources required by the job */
			bit_clear_all(core_bitmap);
			FREE_NULL_LIST(sock_gres_list);
			break;
		}
		sock_gres->job_specs  = job_data_ptr;
		sock_gres->gres_name  = xstrdup(job_data_ptr->gres_name);
		sock_gres->node_specs = node_data_ptr;
		sock_gres->plugin_id  = job_gres_ptr->plugin_id;
		list_append(sock_gres_list, sock_gres);
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	if (slurm_conf.debug_flags & DEBUG_FLAG_GRES)
		_sock_gres_log(sock_gres_list, node_name);

	return sock_gres_list;
}

static void _accumulate_job_set_env_info(gres_state_t *gres_ptr,
					 int node_inx,
					 bitstr_t **gres_bit_alloc,
					 int *gres_cnt)
{
	gres_job_state_t *gres_job_ptr =
		(gres_job_state_t *) gres_ptr->gres_data;
	if ((node_inx >= 0) && (node_inx < gres_job_ptr->node_cnt) &&
	    gres_job_ptr->gres_bit_alloc &&
	    gres_job_ptr->gres_bit_alloc[node_inx]) {
		if (!*gres_bit_alloc) {
			*gres_bit_alloc = bit_alloc(bit_size(
				gres_job_ptr->gres_bit_alloc[node_inx]));
		}
		bit_or(*gres_bit_alloc, gres_job_ptr->gres_bit_alloc[node_inx]);
	}
	gres_cnt += gres_job_ptr->gres_cnt_node_alloc[node_inx];

}

/*
 * Set environment variables as required for a batch job
 * IN/OUT job_env_ptr - environment variable array
 * IN gres_list - generated by gres_job_alloc()
 * IN node_inx - zero origin node index
 */
extern void gres_g_job_set_env(char ***job_env_ptr, List job_gres_list,
			       int node_inx)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr = NULL;
	int gres_cnt = 0;
	bitstr_t *gres_bit_alloc = NULL;
	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].ops.job_set_env == NULL)
			continue;	/* No plugin to call */
		if (job_gres_list) {
			gres_iter = list_iterator_create(job_gres_list);
			while ((gres_ptr = (gres_state_t *)
				list_next(gres_iter))) {
				if (gres_ptr->plugin_id !=
				    gres_context[i].plugin_id)
					continue;
				_accumulate_job_set_env_info(gres_ptr, node_inx,
							     &gres_bit_alloc,
							     &gres_cnt);
			}
			list_iterator_destroy(gres_iter);
		}
		(*(gres_context[i].ops.job_set_env))(job_env_ptr,
						     gres_bit_alloc, gres_cnt,
						     GRES_INTERNAL_FLAG_NONE);
		gres_cnt = 0;
		FREE_NULL_BITMAP(gres_bit_alloc);
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Translate GRES flag to string.
 * NOT reentrant
 */
static char *_gres_flags_str(uint16_t flags)
{
	if (flags & GRES_NO_CONSUME)
		return "no_consume";
	return "";
}

static void _job_state_log(void *gres_data, uint32_t job_id, uint32_t plugin_id)
{
	gres_job_state_t *gres_ptr;
	char *sparse_msg = "", tmp_str[128];
	int i;

	xassert(gres_data);
	gres_ptr = (gres_job_state_t *) gres_data;
	info("gres_job_state gres:%s(%u) type:%s(%u) job:%u flags:%s",
	     gres_ptr->gres_name, plugin_id, gres_ptr->type_name,
	     gres_ptr->type_id, job_id, _gres_flags_str(gres_ptr->flags));
	if (gres_ptr->cpus_per_gres)
		info("  cpus_per_gres:%u", gres_ptr->cpus_per_gres);
	else if (gres_ptr->def_cpus_per_gres)
		info("  def_cpus_per_gres:%u", gres_ptr->def_cpus_per_gres);
	if (gres_ptr->gres_per_job)
		info("  gres_per_job:%"PRIu64, gres_ptr->gres_per_job);
	if (gres_ptr->gres_per_node) {
		info("  gres_per_node:%"PRIu64" node_cnt:%u",
		     gres_ptr->gres_per_node, gres_ptr->node_cnt);
	}
	if (gres_ptr->gres_per_socket)
		info("  gres_per_socket:%"PRIu64, gres_ptr->gres_per_socket);
	if (gres_ptr->gres_per_task)
		info("  gres_per_task:%"PRIu64, gres_ptr->gres_per_task);
	if (gres_ptr->mem_per_gres)
		info("  mem_per_gres:%"PRIu64, gres_ptr->mem_per_gres);
	else if (gres_ptr->def_mem_per_gres)
		info("  def_mem_per_gres:%"PRIu64, gres_ptr->def_mem_per_gres);
	if (gres_ptr->ntasks_per_gres)
		info("  ntasks_per_gres:%u", gres_ptr->ntasks_per_gres);

	/*
	 * These arrays are only used for resource selection and may include
	 * data for many nodes not used in the resources eventually allocated
	 * to this job.
	 */
	if (gres_ptr->total_node_cnt) {
		sparse_msg = " (sparsely populated for resource selection)";
		info("  total_node_cnt:%u%s", gres_ptr->total_node_cnt,
		     sparse_msg);
	}
	for (i = 0; i < gres_ptr->total_node_cnt; i++) {
		if (gres_ptr->gres_cnt_node_select &&
		    gres_ptr->gres_cnt_node_select[i]) {
			info("  gres_cnt_node_select[%d]:%"PRIu64,
			     i, gres_ptr->gres_cnt_node_select[i]);
		}
		if (gres_ptr->gres_bit_select &&
		    gres_ptr->gres_bit_select[i]) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_select[i]);
			info("  gres_bit_select[%d]:%s of %d", i, tmp_str,
			     (int) bit_size(gres_ptr->gres_bit_select[i]));
		}
	}

	if (gres_ptr->total_gres)
		info("  total_gres:%"PRIu64, gres_ptr->total_gres);
	if (gres_ptr->node_cnt)
		info("  node_cnt:%"PRIu32, gres_ptr->node_cnt);
	for (i = 0; i < gres_ptr->node_cnt; i++) {
		if (gres_ptr->gres_cnt_node_alloc &&
		    gres_ptr->gres_cnt_node_alloc[i]) {
			info("  gres_cnt_node_alloc[%d]:%"PRIu64,
			     i, gres_ptr->gres_cnt_node_alloc[i]);
		} else if (gres_ptr->gres_cnt_node_alloc)
			info("  gres_cnt_node_alloc[%d]:NULL", i);

		if (gres_ptr->gres_bit_alloc && gres_ptr->gres_bit_alloc[i]) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_alloc[i]);
			info("  gres_bit_alloc[%d]:%s of %d", i, tmp_str,
			     (int) bit_size(gres_ptr->gres_bit_alloc[i]));
		} else if (gres_ptr->gres_bit_alloc)
			info("  gres_bit_alloc[%d]:NULL", i);

		if (gres_ptr->gres_bit_step_alloc &&
		    gres_ptr->gres_bit_step_alloc[i]) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_step_alloc[i]);
			info("  gres_bit_step_alloc[%d]:%s of %d", i, tmp_str,
			     (int) bit_size(gres_ptr->gres_bit_step_alloc[i]));
		} else if (gres_ptr->gres_bit_step_alloc)
			info("  gres_bit_step_alloc[%d]:NULL", i);

		if (gres_ptr->gres_cnt_step_alloc) {
			info("  gres_cnt_step_alloc[%d]:%"PRIu64"", i,
			     gres_ptr->gres_cnt_step_alloc[i]);
		}
	}
}

/*
 * Extract from the job/step gres_list the count of GRES of the specified name
 * and (optionally) type. If no type is specified, then the count will include
 * all GRES of that name, regardless of type.
 *
 * IN gres_list  - job/step record's gres_list.
 * IN gres_name - the name of the GRES to query.
 * IN gres_type - (optional) the type of the GRES to query.
 * IN is_job - True if the GRES list is for the job, false if for the step.
 * RET The number of GRES in the job/step gres_list or NO_VAL64 if not found.
 */
static uint64_t _get_gres_list_cnt(List gres_list, char *gres_name,
				   char *gres_type, bool is_job)
{
	uint64_t gres_val = NO_VAL64;
	uint32_t plugin_id;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	bool filter_type;

	if ((gres_list == NULL) || (list_count(gres_list) == 0))
		return gres_val;

	plugin_id = gres_build_id(gres_name);

	if (gres_type && (gres_type[0] != '\0'))
		filter_type = true;
	else
		filter_type = false;

	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = list_next(gres_iter))) {
		uint64_t total_gres;
		void *type_name;

		if (gres_ptr->plugin_id != plugin_id)
			continue;

		if (is_job) {
			gres_job_state_t *gres =
				(gres_job_state_t *)gres_ptr->gres_data;
			type_name = gres->type_name;
			total_gres = gres->total_gres;
		} else {
			gres_step_state_t *gres =
				(gres_step_state_t *)gres_ptr->gres_data;
			type_name = gres->type_name;
			total_gres = gres->total_gres;
		}

		/* If we are filtering on GRES type, ignore other types */
		if (filter_type &&
		    xstrcasecmp(gres_type, type_name))
			continue;

		if ((total_gres == NO_VAL64) || (total_gres == 0))
			continue;

		if (gres_val == NO_VAL64)
			gres_val = total_gres;
		else
			gres_val += total_gres;
	}
	list_iterator_destroy(gres_iter);

	return gres_val;
}

static uint64_t _get_job_gres_list_cnt(List gres_list, char *gres_name,
				       char *gres_type)
{
	return _get_gres_list_cnt(gres_list, gres_name, gres_type, true);
}

static uint64_t _get_step_gres_list_cnt(List gres_list, char *gres_name,
					char *gres_type)
{
	return _get_gres_list_cnt(gres_list, gres_name, gres_type, false);
}

/*
 * Log a job's current gres state
 * IN gres_list - generated by gres_job_state_validate()
 * IN job_id - job's ID
 */
extern void gres_job_state_log(List gres_list, uint32_t job_id)
{
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_GRES) || !gres_list)
		return;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		_job_state_log(gres_ptr->gres_data, job_id,
			       gres_ptr->plugin_id);
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static int _find_device(void *x, void *key)
{
	gres_device_t *device_x = (gres_device_t *)x;
	gres_device_t *device_key = (gres_device_t *)key;

	if (!xstrcmp(device_x->path, device_key->path))
		return 1;

	return 0;
}

extern List gres_g_get_devices(List gres_list, bool is_job)
{
	int j;
	ListIterator gres_itr, dev_itr;
	gres_state_t *gres_ptr;
	bitstr_t **local_bit_alloc = NULL;
	uint32_t node_cnt;
	gres_device_t *gres_device;
	List gres_devices;
	List device_list = NULL;

	(void) gres_init();

	/*
	 * Create a unique device list of all possible GRES device files.
	 * Initialize each device to deny.
	 */
	for (j = 0; j < gres_context_cnt; j++) {
		if (!gres_context[j].ops.get_devices)
			continue;
		gres_devices = (*(gres_context[j].ops.get_devices))();
		if (!gres_devices || !list_count(gres_devices))
			continue;
		dev_itr = list_iterator_create(gres_devices);
		while ((gres_device = list_next(dev_itr))) {
			if (!device_list)
				device_list = list_create(NULL);
			gres_device->alloc = 0;
			/*
			 * Keep the list unique by not adding duplicates (in the
			 * case of MPS and GPU)
			 */
			if (!list_find_first(device_list, _find_device,
					     gres_device))
				list_append(device_list, gres_device);
		}
		list_iterator_destroy(dev_itr);
	}

	if (!gres_list)
		return device_list;

	slurm_mutex_lock(&gres_context_lock);
	gres_itr = list_iterator_create(gres_list);
	while ((gres_ptr = list_next(gres_itr))) {
		for (j = 0; j < gres_context_cnt; j++) {
			if (gres_ptr->plugin_id == gres_context[j].plugin_id)
				break;
		}

		if (j >= gres_context_cnt) {
			error("We were unable to find the gres in the context!!!  This should never happen");
			continue;
		}

		if (!gres_ptr->gres_data)
			continue;

		if (is_job) {
			gres_job_state_t *gres_data_ptr =
				(gres_job_state_t *)gres_ptr->gres_data;
			local_bit_alloc = gres_data_ptr->gres_bit_alloc;
			node_cnt = gres_data_ptr->node_cnt;
		} else {
			gres_step_state_t *gres_data_ptr =
				(gres_step_state_t *)gres_ptr->gres_data;
			local_bit_alloc = gres_data_ptr->gres_bit_alloc;
			node_cnt = gres_data_ptr->node_cnt;
		}

		if ((node_cnt != 1) ||
		    !local_bit_alloc ||
		    !local_bit_alloc[0] ||
		    !gres_context[j].ops.get_devices)
			continue;

		gres_devices = (*(gres_context[j].ops.get_devices))();
		if (!gres_devices) {
			error("We should had got gres_devices, but for some reason none were set in the plugin.");
			continue;
		}

		dev_itr = list_iterator_create(gres_devices);
		while ((gres_device = list_next(dev_itr))) {
			if (bit_test(local_bit_alloc[0], gres_device->index)) {
				gres_device_t *gres_device2;
				/*
				 * search for the device among the unique
				 * devices list (since two plugins could have
				 * device records that point to the same file,
				 * like with GPU and MPS)
				 */
				gres_device2 = list_find_first(device_list,
							       _find_device,
							       gres_device);
				/*
				 * Set both, in case they point to different
				 * records
				 */
				gres_device->alloc = 1;
				if (gres_device2)
					gres_device2->alloc = 1;
			}
		}
		list_iterator_destroy(dev_itr);
	}
	list_iterator_destroy(gres_itr);
	slurm_mutex_unlock(&gres_context_lock);

	return device_list;
}

static void _step_state_delete(void *gres_data)
{
	int i;
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;

	if (gres_ptr == NULL)
		return;

	FREE_NULL_BITMAP(gres_ptr->node_in_use);
	if (gres_ptr->gres_bit_alloc) {
		for (i = 0; i < gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		xfree(gres_ptr->gres_bit_alloc);
	}
	xfree(gres_ptr->gres_cnt_node_alloc);
	xfree(gres_ptr->type_name);
	xfree(gres_ptr);
}

extern void gres_step_list_delete(void *list_element)
{
	gres_state_t *gres_ptr = (gres_state_t *) list_element;

	_step_state_delete(gres_ptr->gres_data);
	gres_ptr->gres_data = NULL;
	_gres_state_delete_members(gres_ptr);
}


static int _step_get_gres_cnt(void *x, void *arg)
{
	gres_state_t *job_gres_ptr = (gres_state_t *)x;
	foreach_gres_cnt_t *foreach_gres_cnt = (foreach_gres_cnt_t *)arg;
	gres_job_state_t *gres_job_state;
	gres_key_t *job_search_key = foreach_gres_cnt->job_search_key;
	bool ignore_alloc = foreach_gres_cnt->ignore_alloc;
	slurm_step_id_t *step_id = foreach_gres_cnt->step_id;
	int node_offset = job_search_key->node_offset;

	/* This isn't the gres we are looking for */
	if (!gres_find_job_by_key_with_cnt(job_gres_ptr, job_search_key))
		return 0;

	/* This is the first time we have found a matching GRES. */
	if (foreach_gres_cnt->gres_cnt == INFINITE64)
		foreach_gres_cnt->gres_cnt = 0;

	gres_job_state = job_gres_ptr->gres_data;
	if ((node_offset >= gres_job_state->node_cnt) &&
	    (gres_job_state->node_cnt != 0)) { /* GRES is type no_consume */
		error("gres/%s: %s %ps node offset invalid (%d >= %u)",
		      gres_job_state->gres_name, __func__, step_id,
		      node_offset, gres_job_state->node_cnt);
		foreach_gres_cnt->gres_cnt = 0;
		return -1;
	}
	if (!gres_id_shared(job_search_key->plugin_id) &&
	    gres_job_state->gres_bit_alloc &&
	    gres_job_state->gres_bit_alloc[node_offset]) {
		foreach_gres_cnt->gres_cnt += bit_set_count(
			gres_job_state->gres_bit_alloc[node_offset]);
		if (!ignore_alloc &&
		    gres_job_state->gres_bit_step_alloc &&
		    gres_job_state->gres_bit_step_alloc[node_offset]) {
			foreach_gres_cnt->gres_cnt -=
				bit_set_count(gres_job_state->
					      gres_bit_step_alloc[node_offset]);
		}
	} else if (gres_job_state->gres_cnt_node_alloc &&
		   gres_job_state->gres_cnt_step_alloc) {
		foreach_gres_cnt->gres_cnt +=
			gres_job_state->gres_cnt_node_alloc[node_offset];
		if (!ignore_alloc) {
			foreach_gres_cnt->gres_cnt -= gres_job_state->
				gres_cnt_step_alloc[node_offset];
		}
	} else {
		debug3("gres/%s:%s: %s %ps gres_bit_alloc and gres_cnt_node_alloc are NULL",
		       gres_job_state->gres_name, gres_job_state->type_name,
		       __func__, step_id);
		foreach_gres_cnt->gres_cnt = NO_VAL64;
		return -1;
	}
	return 0;
}

static uint64_t _step_test(void *step_gres_data, bool first_step_node,
			   uint16_t cpus_per_task, int max_rem_nodes,
			   bool ignore_alloc, uint64_t gres_cnt)
{
	gres_step_state_t *step_gres_ptr = (gres_step_state_t *) step_gres_data;
	uint64_t core_cnt, min_gres = 1, task_cnt;

	xassert(step_gres_ptr);

	if (!gres_cnt)
		return 0;

	if (first_step_node) {
		if (ignore_alloc)
			step_gres_ptr->gross_gres = 0;
		else
			step_gres_ptr->total_gres = 0;
	}
	if (step_gres_ptr->gres_per_node)
		min_gres = step_gres_ptr-> gres_per_node;
	if (step_gres_ptr->gres_per_socket)
		min_gres = MAX(min_gres, step_gres_ptr->gres_per_socket);
	if (step_gres_ptr->gres_per_task)
		min_gres = MAX(min_gres, step_gres_ptr->gres_per_task);
	if (step_gres_ptr->gres_per_step &&
	    (step_gres_ptr->gres_per_step > step_gres_ptr->total_gres) &&
	    (max_rem_nodes == 1)) {
		uint64_t gres_per_step = step_gres_ptr->gres_per_step;
		if (ignore_alloc)
			gres_per_step -= step_gres_ptr->gross_gres;
		else
			gres_per_step -= step_gres_ptr->total_gres;
		min_gres = MAX(min_gres, gres_per_step);
	}

	if (gres_cnt != NO_VAL64) {
		if (min_gres > gres_cnt) {
			core_cnt = 0;
		} else if (step_gres_ptr->gres_per_task) {
			task_cnt = (gres_cnt + step_gres_ptr->gres_per_task - 1)
				/ step_gres_ptr->gres_per_task;
			core_cnt = task_cnt * cpus_per_task;
		} else
			core_cnt = NO_VAL64;
	} else {
		gres_cnt = 0;
		core_cnt = NO_VAL64;
	}

	if (core_cnt != 0) {
		if (ignore_alloc)
			step_gres_ptr->gross_gres += gres_cnt;
		else
			step_gres_ptr->total_gres += gres_cnt;
	}

	return core_cnt;
}

/*
 * TRES specification parse logic
 * in_val IN - initial input string
 * cnt OUT - count of values
 * gres_list IN/OUT - where to search for (or add) new step TRES record
 * save_ptr IN/OUT - NULL on initial call, otherwise value from previous call
 * rc OUT - unchanged or an error code
 * RET gres - step record to set value in, found or created by this function
 */
static gres_step_state_t *_get_next_step_gres(char *in_val, uint64_t *cnt,
					      List gres_list, char **save_ptr,
					      int *rc)
{
	static char *prev_save_ptr = NULL;
	int context_inx = NO_VAL, my_rc = SLURM_SUCCESS;
	gres_step_state_t *step_gres_data = NULL;
	gres_state_t *gres_ptr;
	gres_key_t step_search_key;
	char *type = NULL, *name = NULL;
	uint16_t flags = 0;

	xassert(save_ptr);
	if (!in_val && (*save_ptr == NULL)) {
		return NULL;
	}

	if (*save_ptr == NULL) {
		prev_save_ptr = in_val;
	} else if (*save_ptr != prev_save_ptr) {
		error("%s: parsing error", __func__);
		my_rc = SLURM_ERROR;
		goto fini;
	}

	if (prev_save_ptr[0] == '\0') {	/* Empty input token */
		*save_ptr = NULL;
		return NULL;
	}

	if ((my_rc = _get_next_gres(in_val, &type, &context_inx,
				    cnt, &flags, &prev_save_ptr)) ||
	    (context_inx == NO_VAL)) {
		prev_save_ptr = NULL;
		goto fini;
	}

	/* Find the step GRES record */
	step_search_key.plugin_id = gres_context[context_inx].plugin_id;
	step_search_key.type_id = gres_build_id(type);
	gres_ptr = list_find_first(gres_list, gres_find_step_by_key,
				   &step_search_key);

	if (gres_ptr) {
		step_gres_data = gres_ptr->gres_data;
	} else {
		step_gres_data = xmalloc(sizeof(gres_step_state_t));
		step_gres_data->type_id = gres_build_id(type);
		step_gres_data->type_name = type;
		type = NULL;	/* String moved above */
		gres_ptr = xmalloc(sizeof(gres_state_t));
		gres_ptr->plugin_id = gres_context[context_inx].plugin_id;
		gres_ptr->gres_data = step_gres_data;
		gres_ptr->gres_name =
			xstrdup(gres_context[context_inx].gres_name);
		gres_ptr->state_type = GRES_STATE_TYPE_STEP;
		list_append(gres_list, gres_ptr);
	}
	step_gres_data->flags = flags;

fini:	xfree(name);
	xfree(type);
	if (my_rc != SLURM_SUCCESS) {
		prev_save_ptr = NULL;
		if (my_rc == ESLURM_INVALID_GRES)
			info("Invalid GRES job specification %s", in_val);
		*rc = my_rc;
	}
	*save_ptr = prev_save_ptr;
	return step_gres_data;
}

/* Test that the step does not request more GRES than the job contains */
static void _validate_step_counts(List step_gres_list, List job_gres_list,
				  int *rc)
{
	ListIterator iter;
	gres_state_t *job_gres_ptr, *step_gres_ptr;
	gres_job_state_t *job_gres_data;
	gres_step_state_t *step_gres_data;
	gres_key_t job_search_key;
	uint16_t cpus_per_gres;
	uint64_t mem_per_gres;

	if (!step_gres_list || (list_count(step_gres_list) == 0))
		return;
	if (!job_gres_list  || (list_count(job_gres_list)  == 0)) {
		*rc = ESLURM_INVALID_GRES;
		return;
	}

	iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(iter))) {
		step_gres_data = (gres_step_state_t *) step_gres_ptr->gres_data;
		job_search_key.plugin_id = step_gres_ptr->plugin_id;
		if (step_gres_data->type_id == 0)
			job_search_key.type_id = NO_VAL;
		else
			job_search_key.type_id = step_gres_data->type_id;
		job_gres_ptr = list_find_first(job_gres_list,
					       gres_find_job_by_key,
					       &job_search_key);
		if (!job_gres_ptr || !job_gres_ptr->gres_data) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		job_gres_data = (gres_job_state_t *) job_gres_ptr->gres_data;
		if (job_gres_data->cpus_per_gres)
			cpus_per_gres = job_gres_data->cpus_per_gres;
		else
			cpus_per_gres = job_gres_data->def_cpus_per_gres;
		if (cpus_per_gres && step_gres_data->cpus_per_gres &&
		    (cpus_per_gres < step_gres_data->cpus_per_gres)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		if (job_gres_data->gres_per_job &&
		    step_gres_data->gres_per_step &&
		    (job_gres_data->gres_per_job <
		     step_gres_data->gres_per_step)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		if (job_gres_data->gres_per_node &&
		    step_gres_data->gres_per_node &&
		    (job_gres_data->gres_per_node <
		     step_gres_data->gres_per_node)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		if (job_gres_data->gres_per_socket &&
		    step_gres_data->gres_per_socket &&
		    (job_gres_data->gres_per_socket <
		     step_gres_data->gres_per_socket)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		if (job_gres_data->gres_per_task &&
		    step_gres_data->gres_per_task &&
		    (job_gres_data->gres_per_task <
		     step_gres_data->gres_per_task)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}
		if (job_gres_data->mem_per_gres)
			mem_per_gres = job_gres_data->mem_per_gres;
		else
			mem_per_gres = job_gres_data->def_mem_per_gres;
		if (mem_per_gres && step_gres_data->mem_per_gres &&
		    (mem_per_gres < step_gres_data->mem_per_gres)) {
			*rc = ESLURM_INVALID_GRES;
			break;
		}

	}
	list_iterator_destroy(iter);
}


static int _handle_ntasks_per_tres_step(List new_step_list,
					 uint16_t ntasks_per_tres,
					 uint32_t *num_tasks,
					 uint32_t *cpu_count)
{
	gres_step_state_t *step_gres_data;
	uint64_t cnt = 0;
	int rc = SLURM_SUCCESS;

	uint64_t tmp = _get_step_gres_list_cnt(new_step_list, "gpu", NULL);
	if ((tmp == NO_VAL64) && (*num_tasks != NO_VAL)) {
		/*
		 * Generate GPUs from ntasks_per_tres when not specified
		 * and ntasks is specified
		 */
		uint32_t gpus = *num_tasks / ntasks_per_tres;
		/* For now, do type-less GPUs */
		char *save_ptr = NULL, *gres = NULL, *in_val;
		xstrfmtcat(gres, "gres:gpu:%u", gpus);
		in_val = gres;
		if (*num_tasks != ntasks_per_tres * gpus) {
			log_flag(GRES, "%s: -n/--ntasks %u is not a multiple of --ntasks-per-gpu=%u",
				 __func__, *num_tasks, ntasks_per_tres);
			return ESLURM_INVALID_GRES;
		}
		while ((step_gres_data =
			_get_next_step_gres(in_val, &cnt,
					    new_step_list,
					    &save_ptr, &rc))) {
			/* Simulate a tres_per_job specification */
			step_gres_data->gres_per_step = cnt;
			step_gres_data->total_gres =
				MAX(step_gres_data->total_gres, cnt);
			in_val = NULL;
		}
		xfree(gres);
		xassert(list_count(new_step_list) != 0);
	} else if (tmp != NO_VAL64) {
		tmp = tmp * ntasks_per_tres;
		if (*num_tasks < tmp) {
			*num_tasks = tmp;
		}
		if (*cpu_count < tmp) {
			*cpu_count = tmp;
		}
	} else {
		error("%s: ntasks_per_tres was specified, but there was either no task count or no GPU specification to go along with it, or both were already specified.",
		      __func__);
		rc = SLURM_ERROR;
	}

	return rc;
}

/*
 * Given a step's requested gres configuration, validate it and build gres list
 * IN *tres* - step's requested gres input string
 * OUT step_gres_list - List of Gres records for this step to track usage
 * IN job_gres_list - List of Gres records for this job
 * IN job_id, step_id - ID of the step being allocated.
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_step_state_validate(char *cpus_per_tres,
				    char *tres_per_step,
				    char *tres_per_node,
				    char *tres_per_socket,
				    char *tres_per_task,
				    char *mem_per_tres,
				    uint16_t ntasks_per_tres,
				    List *step_gres_list,
				    List job_gres_list, uint32_t job_id,
				    uint32_t step_id,
				    uint32_t *num_tasks,
				    uint32_t *cpu_count)
{
	int rc;
	gres_step_state_t *step_gres_data;
	List new_step_list;
	uint64_t cnt = 0;

	*step_gres_list = NULL;
	if ((rc = gres_init()) != SLURM_SUCCESS)
		return rc;

	slurm_mutex_lock(&gres_context_lock);
	new_step_list = list_create(gres_step_list_delete);
	if (cpus_per_tres) {
		char *in_val = cpus_per_tres, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->cpus_per_gres = cnt;
			in_val = NULL;
		}
	}
	if (tres_per_step) {
		char *in_val = tres_per_step, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->gres_per_step = cnt;
			in_val = NULL;
			step_gres_data->total_gres =
				MAX(step_gres_data->total_gres, cnt);
		}
	}
	if (tres_per_node) {
		char *in_val = tres_per_node, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->gres_per_node = cnt;
			in_val = NULL;
			/* Step only has 1 node, always */
			step_gres_data->total_gres =
				MAX(step_gres_data->total_gres, cnt);
		}
	}
	if (tres_per_socket) {
		char *in_val = tres_per_socket, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->gres_per_socket = cnt;
			in_val = NULL;
			// TODO: What is sockets_per_node and ntasks_per_socket?
			// if (*sockets_per_node != NO_VAL16) {
			//	cnt *= *sockets_per_node;
			// } else if ((*num_tasks != NO_VAL) &&
			//	   (*ntasks_per_socket != NO_VAL16)) {
			//	cnt *= ((*num_tasks + *ntasks_per_socket - 1) /
			//		*ntasks_per_socket);
			// }
			// step_gres_data->total_gres =
			//	MAX(step_gres_data->total_gres, cnt);
		}
	}
	if (tres_per_task) {
		char *in_val = tres_per_task, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->gres_per_task = cnt;
			in_val = NULL;
			if (*num_tasks != NO_VAL)
				cnt *= *num_tasks;
			step_gres_data->total_gres =
				MAX(step_gres_data->total_gres, cnt);
		}
	}
	if (mem_per_tres) {
		char *in_val = mem_per_tres, *save_ptr = NULL;
		while ((step_gres_data = _get_next_step_gres(in_val, &cnt,
							     new_step_list,
							     &save_ptr, &rc))) {
			step_gres_data->mem_per_gres = cnt;
			in_val = NULL;
		}
	}

	if ((ntasks_per_tres != NO_VAL16) && num_tasks && cpu_count) {
		rc = _handle_ntasks_per_tres_step(new_step_list,
						  ntasks_per_tres,
						  num_tasks,
						  cpu_count);
	}

	if (list_count(new_step_list) == 0) {
		FREE_NULL_LIST(new_step_list);
	} else {
		if (rc == SLURM_SUCCESS)
			_validate_step_counts(new_step_list, job_gres_list,
					      &rc);
		if (rc == SLURM_SUCCESS)
			*step_gres_list = new_step_list;
		else
			FREE_NULL_LIST(new_step_list);
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

static void *_step_state_dup(void *gres_data)
{

	int i;
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;
	gres_step_state_t *new_gres_ptr;

	xassert(gres_ptr);
	new_gres_ptr = xmalloc(sizeof(gres_step_state_t));
	new_gres_ptr->cpus_per_gres	= gres_ptr->cpus_per_gres;
	new_gres_ptr->gres_per_step	= gres_ptr->gres_per_step;
	new_gres_ptr->gres_per_node	= gres_ptr->gres_per_node;
	new_gres_ptr->gres_per_socket	= gres_ptr->gres_per_socket;
	new_gres_ptr->gres_per_task	= gres_ptr->gres_per_task;
	new_gres_ptr->mem_per_gres	= gres_ptr->mem_per_gres;
	new_gres_ptr->node_cnt		= gres_ptr->node_cnt;
	new_gres_ptr->total_gres	= gres_ptr->total_gres;

	if (gres_ptr->node_in_use)
		new_gres_ptr->node_in_use = bit_copy(gres_ptr->node_in_use);

	if (gres_ptr->gres_cnt_node_alloc) {
		i = sizeof(uint64_t) * gres_ptr->node_cnt;
		new_gres_ptr->gres_cnt_node_alloc = xmalloc(i);
		memcpy(new_gres_ptr->gres_cnt_node_alloc,
		       gres_ptr->gres_cnt_node_alloc, i);
	}
	if (gres_ptr->gres_bit_alloc) {
		new_gres_ptr->gres_bit_alloc = xcalloc(gres_ptr->node_cnt,
						       sizeof(bitstr_t *));
		for (i = 0; i < gres_ptr->node_cnt; i++) {
			if (gres_ptr->gres_bit_alloc[i] == NULL)
				continue;
			new_gres_ptr->gres_bit_alloc[i] =
				bit_copy(gres_ptr->gres_bit_alloc[i]);
		}
	}
	return new_gres_ptr;
}

static void *_step_state_dup2(void *gres_data, int node_index)
{

	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;
	gres_step_state_t *new_gres_ptr;

	xassert(gres_ptr);
	new_gres_ptr = xmalloc(sizeof(gres_step_state_t));
	new_gres_ptr->cpus_per_gres	= gres_ptr->cpus_per_gres;
	new_gres_ptr->gres_per_step	= gres_ptr->gres_per_step;
	new_gres_ptr->gres_per_node	= gres_ptr->gres_per_node;
	new_gres_ptr->gres_per_socket	= gres_ptr->gres_per_socket;
	new_gres_ptr->gres_per_task	= gres_ptr->gres_per_task;
	new_gres_ptr->mem_per_gres	= gres_ptr->mem_per_gres;
	new_gres_ptr->node_cnt		= 1;
	new_gres_ptr->total_gres	= gres_ptr->total_gres;

	if (gres_ptr->node_in_use)
		new_gres_ptr->node_in_use = bit_copy(gres_ptr->node_in_use);

	if (gres_ptr->gres_cnt_node_alloc) {
		new_gres_ptr->gres_cnt_node_alloc = xmalloc(sizeof(uint64_t));
		new_gres_ptr->gres_cnt_node_alloc[0] =
			gres_ptr->gres_cnt_node_alloc[node_index];
	}

	if ((node_index < gres_ptr->node_cnt) && gres_ptr->gres_bit_alloc &&
	    gres_ptr->gres_bit_alloc[node_index]) {
		new_gres_ptr->gres_bit_alloc = xmalloc(sizeof(bitstr_t *));
		new_gres_ptr->gres_bit_alloc[0] =
			bit_copy(gres_ptr->gres_bit_alloc[node_index]);
	}
	return new_gres_ptr;
}

/*
 * Create a copy of a step's gres state
 * IN gres_list - List of Gres records for this step to track usage
 * RET The copy or NULL on failure
 */
List gres_step_state_dup(List gres_list)
{
	return gres_step_state_extract(gres_list, -1);
}

/*
 * Create a copy of a step's gres state for a particular node index
 * IN gres_list - List of Gres records for this step to track usage
 * IN node_index - zero-origin index to the node
 * RET The copy or NULL on failure
 */
List gres_step_state_extract(List gres_list, int node_index)
{
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres_state;
	List new_gres_list = NULL;
	void *new_gres_data;

	if (gres_list == NULL)
		return new_gres_list;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		if (node_index == -1)
			new_gres_data = _step_state_dup(gres_ptr->gres_data);
		else {
			new_gres_data = _step_state_dup2(gres_ptr->gres_data,
							 node_index);
		}
		if (new_gres_list == NULL) {
			new_gres_list = list_create(gres_step_list_delete);
		}
		new_gres_state = xmalloc(sizeof(gres_state_t));
		new_gres_state->plugin_id = gres_ptr->plugin_id;
		new_gres_state->gres_data = new_gres_data;
		new_gres_state->gres_name = xstrdup(gres_ptr->gres_name);
		new_gres_state->state_type = GRES_STATE_TYPE_STEP;
		list_append(new_gres_list, new_gres_state);
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_gres_list;
}

/*
 * Pack a step's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_ctld_step_alloc()
 * IN/OUT buffer - location to write state to
 * IN step_id - job and step ID for logging
 */
extern int gres_step_state_pack(List gres_list, buf_t *buffer,
				slurm_step_id_t *step_id,
				uint16_t protocol_version)
{
	int i, rc = SLURM_SUCCESS;
	uint32_t top_offset, tail_offset, magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	gres_step_state_t *gres_step_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		gres_step_ptr = (gres_step_state_t *) gres_ptr->gres_data;

		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			pack16(gres_step_ptr->cpus_per_gres, buffer);
			pack16(gres_step_ptr->flags, buffer);
			pack64(gres_step_ptr->gres_per_step, buffer);
			pack64(gres_step_ptr->gres_per_node, buffer);
			pack64(gres_step_ptr->gres_per_socket, buffer);
			pack64(gres_step_ptr->gres_per_task, buffer);
			pack64(gres_step_ptr->mem_per_gres, buffer);
			pack64(gres_step_ptr->total_gres, buffer);
			pack32(gres_step_ptr->node_cnt, buffer);
			pack_bit_str_hex(gres_step_ptr->node_in_use, buffer);
			if (gres_step_ptr->gres_cnt_node_alloc) {
				pack8((uint8_t) 1, buffer);
				pack64_array(gres_step_ptr->gres_cnt_node_alloc,
					     gres_step_ptr->node_cnt, buffer);
			} else {
				pack8((uint8_t) 0, buffer);
			}
			if (gres_step_ptr->gres_bit_alloc) {
				pack8((uint8_t) 1, buffer);
				for (i = 0; i < gres_step_ptr->node_cnt; i++)
					pack_bit_str_hex(gres_step_ptr->
							 gres_bit_alloc[i],
							 buffer);
			} else {
				pack8((uint8_t) 0, buffer);
			}
			rec_cnt++;
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a step's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_step_state_pack()
 * IN/OUT buffer - location to read state from
 * IN step_id - job and step ID for logging
 */
extern int gres_step_state_unpack(List *gres_list, buf_t *buffer,
				  slurm_step_id_t *step_id,
				  uint16_t protocol_version)
{
	int i, rc;
	uint32_t magic = 0, plugin_id = 0, uint32_tmp = 0;
	uint16_t rec_cnt = 0;
	uint8_t data_flag = 0;
	gres_state_t *gres_ptr;
	gres_step_state_t *gres_step_ptr = NULL;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(gres_step_list_delete);
	}

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
			safe_unpack32(&magic, buffer);
			if (magic != GRES_MAGIC)
				goto unpack_error;
			safe_unpack32(&plugin_id, buffer);
			gres_step_ptr = xmalloc(sizeof(gres_step_state_t));
			safe_unpack16(&gres_step_ptr->cpus_per_gres, buffer);
			safe_unpack16(&gres_step_ptr->flags, buffer);
			safe_unpack64(&gres_step_ptr->gres_per_step, buffer);
			safe_unpack64(&gres_step_ptr->gres_per_node, buffer);
			safe_unpack64(&gres_step_ptr->gres_per_socket, buffer);
			safe_unpack64(&gres_step_ptr->gres_per_task, buffer);
			safe_unpack64(&gres_step_ptr->mem_per_gres, buffer);
			safe_unpack64(&gres_step_ptr->total_gres, buffer);
			safe_unpack32(&gres_step_ptr->node_cnt, buffer);
			if (gres_step_ptr->node_cnt > NO_VAL)
				goto unpack_error;
			unpack_bit_str_hex(&gres_step_ptr->node_in_use, buffer);
			safe_unpack8(&data_flag, buffer);
			if (data_flag) {
				safe_unpack64_array(
					&gres_step_ptr->gres_cnt_node_alloc,
					&uint32_tmp, buffer);
			}
			safe_unpack8(&data_flag, buffer);
			if (data_flag) {
				gres_step_ptr->gres_bit_alloc =
					xcalloc(gres_step_ptr->node_cnt,
						sizeof(bitstr_t *));
				for (i = 0; i < gres_step_ptr->node_cnt; i++) {
					unpack_bit_str_hex(&gres_step_ptr->
							   gres_bit_alloc[i],
							   buffer);
				}
			}
		} else {
			error("%s: protocol_version %hu not supported",
			      __func__, protocol_version);
			goto unpack_error;
		}

		for (i = 0; i < gres_context_cnt; i++) {
			if (gres_context[i].plugin_id == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			/*
			 * A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data.
			 */
			info("%s: no plugin configured to unpack data type %u from %ps",
			     __func__, plugin_id, step_id);
			_step_state_delete(gres_step_ptr);
			gres_step_ptr = NULL;
			continue;
		}
		gres_ptr = xmalloc(sizeof(gres_state_t));
		gres_ptr->plugin_id = gres_context[i].plugin_id;
		gres_ptr->gres_data = gres_step_ptr;
		gres_ptr->gres_name = xstrdup(gres_context[i].gres_name);
		gres_ptr->state_type = GRES_STATE_TYPE_STEP;
		gres_step_ptr = NULL;
		list_append(*gres_list, gres_ptr);
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("%s: unpack error from %ps", __func__, step_id);
	if (gres_step_ptr)
		_step_state_delete(gres_step_ptr);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

/* Return the count of GRES of a specific name on this machine
 * IN step_gres_list - generated by gres_ctld_step_alloc()
 * IN gres_name - name of the GRES to match
 * RET count of GRES of this specific name available to the job or NO_VAL64
 */
extern uint64_t gres_step_count(List step_gres_list, char *gres_name)
{
	uint64_t gres_cnt = NO_VAL64;
	gres_state_t *gres_ptr = NULL;
	gres_step_state_t *gres_step_ptr = NULL;
	ListIterator gres_iter;
	int i;

	if (!step_gres_list)
		return gres_cnt;

	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (xstrcmp(gres_context[i].gres_name, gres_name))
			continue;
		gres_iter = list_iterator_create(step_gres_list);
		while ((gres_ptr = (gres_state_t *)list_next(gres_iter))) {
			if (gres_ptr->plugin_id != gres_context[i].plugin_id)
				continue;
			gres_step_ptr = (gres_step_state_t*)gres_ptr->gres_data;
			/* gres_cnt_node_alloc has one element in slurmstepd */
			if (gres_cnt == NO_VAL64)
				gres_cnt =
					gres_step_ptr->gres_cnt_node_alloc[0];
			else
				gres_cnt +=
					gres_step_ptr->gres_cnt_node_alloc[0];
		}
		list_iterator_destroy(gres_iter);
		break;
	}
	slurm_mutex_unlock(&gres_context_lock);

	return gres_cnt;
}

/*
 * Given a GRES context index, return a bitmap representing those GRES
 * which are available from the CPUs current allocated to this process.
 * This function only works with task/cgroup and constrained devices or
 * if the job step has access to the entire node's resources.
 */
static bitstr_t * _get_usable_gres(int context_inx)
{
#if defined(__APPLE__)
	return NULL;
#else
#ifdef __NetBSD__
	// On NetBSD, cpuset_t is an opaque data type
	cpuset_t *mask = cpuset_create();
#else
	cpu_set_t mask;
#endif
	bitstr_t *usable_gres = NULL;
	int i, i_last, rc;
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
	int gres_inx = 0;

	if (!gres_conf_list) {
		error("gres_conf_list is null!");
		return NULL;
	}

	CPU_ZERO(&mask);
#ifdef __FreeBSD__
	rc = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
				sizeof(mask), &mask);
#else
	rc = sched_getaffinity(0, sizeof(mask), &mask);
#endif
	if (rc) {
		error("sched_getaffinity error: %m");
		return usable_gres;
	}

	usable_gres = bit_alloc(MAX_GRES_BITMAP);
	iter = list_iterator_create(gres_conf_list);
	while ((gres_slurmd_conf = (gres_slurmd_conf_t *) list_next(iter))) {
		if (gres_slurmd_conf->plugin_id !=
		    gres_context[context_inx].plugin_id)
			continue;
		if ((gres_inx + gres_slurmd_conf->count) >= MAX_GRES_BITMAP) {
			error("GRES %s bitmap overflow ((%d + %"PRIu64") >= %d)",
			      gres_slurmd_conf->name, gres_inx,
			      gres_slurmd_conf->count, MAX_GRES_BITMAP);
			continue;
		}
		if (!gres_slurmd_conf->cpus_bitmap) {
			bit_nset(usable_gres, gres_inx,
				 gres_inx + gres_slurmd_conf->count - 1);
		} else {
			i_last = bit_fls(gres_slurmd_conf->cpus_bitmap);
			for (i = 0; i <= i_last; i++) {
				if (!bit_test(gres_slurmd_conf->cpus_bitmap, i))
					continue;
				if (!CPU_ISSET(i, &mask))
					continue;
				bit_nset(usable_gres, gres_inx,
					 gres_inx + gres_slurmd_conf->count -1);
				break;
			}
		}
		gres_inx += gres_slurmd_conf->count;
	}
	list_iterator_destroy(iter);

#ifdef __NetBSD__
	cpuset_destroy(mask);
#endif

	return usable_gres;
#endif
}

/*
 * If ntasks_per_gres is > 0, modify usable_gres so that this task can only use
 * one GPU. This will make it so only one GPU can be bound to this task later
 * on. Use local_proc_id (task rank) and ntasks_per_gres to determine which GPU
 * to bind to. Assign out tasks to GPUs in a block-like distribution.
 * TODO: This logic needs improvement when tasks and GPUs span sockets.
 *
 * IN/OUT - usable_gres
 * IN - ntasks_per_gres
 * IN - local_proc_id
 */
static void _filter_usable_gres(bitstr_t *usable_gres, int ntasks_per_gres,
				int local_proc_id)
{
	int gpu_count, n, idx;
	char *str;
	if (ntasks_per_gres <= 0)
		return;

	/* # of GPUs this task has an affinity to */
	gpu_count = bit_set_count(usable_gres);

	str = bit_fmt_hexmask_trim(usable_gres);
	log_flag(GRES, "%s: local_proc_id = %d; usable_gres (ALL): %s",
		 __func__, local_proc_id, str);
	xfree(str);

	/* No need to filter if no usable_gres or already only 1 to use */
	if ((gpu_count == 0) || (gpu_count == 1)) {
		log_flag(GRES, "%s: (task %d) No need to filter since usable_gres count is 0 or 1",
			 __func__, local_proc_id);
		return;
	}

	/* Map task rank to one of the GPUs (block distribution) */
	n = (local_proc_id / ntasks_per_gres) % gpu_count;
	/* Find the nth set bit in usable_gres */
	idx = bit_get_bit_num(usable_gres, n);

	log_flag(GRES, "%s: local_proc_id = %d; n = %d; ntasks_per_gres = %d; idx = %d",
		 __func__, local_proc_id, n, ntasks_per_gres, idx);

	if (idx == -1) {
		error("%s: (task %d) usable_gres did not have >= %d set GPUs, so can't do a single bind on set GPU #%d. Defaulting back to the original usable_gres.",
		      __func__, local_proc_id, n + 1, n);
		return;
	}

	/* Return a bitmap with this as the only usable GRES */
	bit_clear_all(usable_gres);
	bit_set(usable_gres, idx);
	str = bit_fmt_hexmask_trim(usable_gres);
	log_flag(GRES, "%s: local_proc_id = %d; usable_gres (single filter): %s",
		 __func__, local_proc_id, str);
	xfree(str);
}

/*
 * Configure the GRES hardware allocated to the current step while privileged
 *
 * IN step_gres_list - Step's GRES specification
 * IN node_id        - relative position of this node in step
 * IN settings       - string containing configuration settings for the hardware
 */
extern void gres_g_step_hardware_init(List step_gres_list,
				      uint32_t node_id, char *settings)
{
	int i;
	ListIterator iter;
	gres_state_t *gres_ptr;
	gres_step_state_t *gres_step_ptr;
	bitstr_t *devices;

	if (!step_gres_list)
		return;

	(void) gres_init();
	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (gres_context[i].ops.step_hardware_init == NULL)
			continue;

		iter = list_iterator_create(step_gres_list);
		while ((gres_ptr = list_next(iter))) {
			if (gres_ptr->plugin_id == gres_context[i].plugin_id)
				break;
		}
		list_iterator_destroy(iter);
		if (!gres_ptr || !gres_ptr->gres_data)
			continue;
		gres_step_ptr = (gres_step_state_t *) gres_ptr->gres_data;
		if ((gres_step_ptr->node_cnt != 1) ||
		    !gres_step_ptr->gres_bit_alloc ||
		    !gres_step_ptr->gres_bit_alloc[0])
			continue;

		devices = gres_step_ptr->gres_bit_alloc[0];
		if (settings)
			debug2("settings: %s", settings);
		if (devices) {
			char *dev_str = bit_fmt_full(devices);
			info("devices: %s", dev_str);
			xfree(dev_str);
		}
		(*(gres_context[i].ops.step_hardware_init))(devices, settings);
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Optionally undo GRES hardware configuration while privileged
 */
extern void gres_g_step_hardware_fini(void)
{
	int i;
	(void) gres_init();
	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		if (gres_context[i].ops.step_hardware_fini == NULL) {
			continue;
		}
		(*(gres_context[i].ops.step_hardware_fini)) ();
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Given a set GRES maps and the local process ID, return the bitmap of
 * GRES that should be available to this task.
 */
static bitstr_t *_get_gres_map(char *map_gres, int local_proc_id)
{
	bitstr_t *usable_gres = NULL;
	char *tmp, *tok, *save_ptr = NULL, *mult;
	int task_offset = 0, task_mult;
	int map_value;

	if (!map_gres || !map_gres[0])
		return NULL;

	while (usable_gres == NULL) {
		tmp = xstrdup(map_gres);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if ((mult = strchr(tok, '*'))) {
				mult[0] = '\0';
				task_mult = atoi(mult + 1);
			} else
				task_mult = 1;
			if (task_mult == 0) {
				error("Repetition count of 0 not allowed in --gpu-bind=map_gpu, using 1 instead");
				task_mult = 1;
			}
			if ((local_proc_id >= task_offset) &&
			    (local_proc_id <= (task_offset + task_mult - 1))) {
				map_value = strtol(tok, NULL, 0);
				if ((map_value < 0) ||
				    (map_value >= MAX_GRES_BITMAP)) {
					error("Invalid --gpu-bind=map_gpu value specified.");
					xfree(tmp);
					goto end;	/* Bad value */
				}
				usable_gres = bit_alloc(MAX_GRES_BITMAP);
				bit_set(usable_gres, map_value);
				break;	/* All done */
			} else {
				task_offset += task_mult;
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
	}
end:

	return usable_gres;
}

/*
 * Given a set GRES masks and the local process ID, return the bitmap of
 * GRES that should be available to this task.
 */
static bitstr_t * _get_gres_mask(char *mask_gres, int local_proc_id)
{
	bitstr_t *usable_gres = NULL;
	char *tmp, *tok, *save_ptr = NULL, *mult;
	int i, task_offset = 0, task_mult;
	uint64_t mask_value;

	if (!mask_gres || !mask_gres[0])
		return NULL;

	while (usable_gres == NULL) {
		tmp = xstrdup(mask_gres);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if ((mult = strchr(tok, '*')))
				task_mult = atoi(mult + 1);
			else
				task_mult = 1;
			if (task_mult == 0) {
				error("Repetition count of 0 not allowed in --gpu-bind=mask_gpu, using 1 instead");
				task_mult = 1;
			}
			if ((local_proc_id >= task_offset) &&
			    (local_proc_id <= (task_offset + task_mult - 1))) {
				mask_value = strtol(tok, NULL, 0);
				if ((mask_value <= 0) ||
				    (mask_value >= 0xffffffff)) {
					error("Invalid --gpu-bind=mask_gpu value specified.");
					xfree(tmp);
					goto end;	/* Bad value */
				}
				usable_gres = bit_alloc(MAX_GRES_BITMAP);
				for (i = 0; i < 64; i++) {
					if ((mask_value >> i) & 0x1)
						bit_set(usable_gres, i);
				}
				break;	/* All done */
			} else {
				task_offset += task_mult;
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
	}
end:

	return usable_gres;
}

static void _accumulate_step_set_env_info(gres_state_t *gres_ptr,
					 bitstr_t **gres_bit_alloc,
					 int *gres_cnt)
{
	gres_step_state_t *gres_step_ptr =
		(gres_step_state_t *)gres_ptr->gres_data;

	if ((gres_step_ptr->node_cnt == 1) &&
	    gres_step_ptr->gres_bit_alloc &&
	    gres_step_ptr->gres_bit_alloc[0]) {
		if (!*gres_bit_alloc) {
			*gres_bit_alloc = bit_alloc(bit_size(
				gres_step_ptr->gres_bit_alloc[0]));
		}
		bit_or(*gres_bit_alloc, gres_step_ptr->gres_bit_alloc[0]);
	}
	if (gres_step_ptr->gres_cnt_node_alloc)
		*gres_cnt += gres_step_ptr->gres_cnt_node_alloc[0];
}

/*
 * Set environment as required for all tasks of a job step
 * IN/OUT job_env_ptr - environment variable array
 * IN step_gres_list - generated by gres_ctld_step_alloc()
 */
extern void gres_g_step_set_env(char ***job_env_ptr, List step_gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr = NULL;
	int gres_cnt = 0;
	bitstr_t *gres_bit_alloc = NULL;

	(void) gres_init();
	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		slurm_gres_context_t gres_ctx = gres_context[i];
		if (!gres_ctx.ops.step_set_env)
			continue;	/* No plugin to call */
		if (!step_gres_list)
			continue;
		gres_iter = list_iterator_create(step_gres_list);
		while ((gres_ptr = (gres_state_t *)list_next(gres_iter))) {
			if (gres_ptr->plugin_id != gres_ctx.plugin_id)
				continue;
			_accumulate_step_set_env_info(
				gres_ptr, &gres_bit_alloc, &gres_cnt);
		}
		list_iterator_destroy(gres_iter);
		(*(gres_ctx.ops.step_set_env))(job_env_ptr, gres_bit_alloc,
					       gres_cnt,
					       GRES_INTERNAL_FLAG_NONE);
		gres_cnt = 0;
		FREE_NULL_BITMAP(gres_bit_alloc);
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Change the task's inherited environment (from the step, and set by
 * gres_g_step_set_env()). Use this to implement GPU task binding.
 *
 * IN/OUT job_env_ptr - environment variable array
 * IN step_gres_list - generated by gres_ctld_step_alloc()
 * IN accel_bind_type - GRES binding options (old format, a bitmap)
 * IN tres_bind - TRES binding directives (new format, a string)
 * IN local_proc_id - task rank, local to this compute node only
 */
extern void gres_g_task_set_env(char ***job_env_ptr, List step_gres_list,
				uint16_t accel_bind_type, char *tres_bind,
				int local_proc_id)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr = NULL;
	bool bind_gpu = accel_bind_type & ACCEL_BIND_CLOSEST_GPU;
	bool bind_nic = accel_bind_type & ACCEL_BIND_CLOSEST_NIC;
	char *sep, *map_gpu = NULL, *mask_gpu = NULL;
	bitstr_t *usable_gres = NULL;
	gres_internal_flags_t gres_internal_flags = GRES_INTERNAL_FLAG_NONE;
	int tasks_per_gres = 0;
	int gres_cnt = 0;
	bitstr_t *gres_bit_alloc = NULL;

	if (!bind_gpu && tres_bind && (sep = strstr(tres_bind, "gpu:"))) {
		sep += 4;
		if (!strncasecmp(sep, "verbose,", 8)) {
			gres_internal_flags |= GRES_INTERNAL_FLAG_VERBOSE;
			sep += 8;
		}
		if (!strncasecmp(sep, "single:", 7)) {
			sep += 7;
			tasks_per_gres = strtol(sep, NULL, 0);
			if ((tasks_per_gres <= 0) ||
			    (tasks_per_gres == LONG_MAX)) {
				error("%s: single:%s does not specify a valid number. Defaulting to 1.",
				      __func__, sep);
				tasks_per_gres = 1;
			}
			bind_gpu = true;
		} else if (!strncasecmp(sep, "closest", 7))
			bind_gpu = true;
		else if (!strncasecmp(sep, "map_gpu:", 8))
			map_gpu = sep + 8;
		else if (!strncasecmp(sep, "mask_gpu:", 9))
			mask_gpu = sep + 9;
	}

	(void) gres_init();
	slurm_mutex_lock(&gres_context_lock);
	for (i = 0; i < gres_context_cnt; i++) {
		slurm_gres_context_t gres_ctx = gres_context[i];
		if (!gres_ctx.ops.task_set_env)
			continue;	/* No plugin to call */
		if (!step_gres_list)
			continue;
		if (bind_gpu || bind_nic || map_gpu || mask_gpu) {
			/* Set the GRES that this task can use (usable_gres) */
			if (!xstrcmp(gres_ctx.gres_name, "gpu")) {
				if (map_gpu) {
					usable_gres = _get_gres_map(
						map_gpu, local_proc_id);
				} else if (mask_gpu) {
					usable_gres = _get_gres_mask(
						mask_gpu, local_proc_id);
				} else if (bind_gpu) {
					usable_gres = _get_usable_gres(i);
					_filter_usable_gres(usable_gres,
							    tasks_per_gres,
							    local_proc_id);
				}
				else
					continue;
			} else if (!xstrcmp(gres_ctx.gres_name,
					    "nic")) {
				if (bind_nic)
					usable_gres = _get_usable_gres(i);
				else
					continue;
			} else {
				continue;
			}
		}
		gres_iter = list_iterator_create(step_gres_list);
		while ((gres_ptr = (gres_state_t *)list_next(gres_iter))) {
			if (gres_ptr->plugin_id != gres_ctx.plugin_id)
				continue;
			_accumulate_step_set_env_info(
				gres_ptr, &gres_bit_alloc, &gres_cnt);
		}
		list_iterator_destroy(gres_iter);
		(*(gres_ctx.ops.task_set_env))(job_env_ptr, gres_bit_alloc,
					       gres_cnt, usable_gres,
					       gres_internal_flags);
		gres_cnt = 0;
		FREE_NULL_BITMAP(gres_bit_alloc);
		FREE_NULL_BITMAP(usable_gres);
	}
	slurm_mutex_unlock(&gres_context_lock);
	FREE_NULL_BITMAP(usable_gres);
}

static void _step_state_log(void *gres_data, slurm_step_id_t *step_id,
			    char *gres_name)
{
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;
	char tmp_str[128];
	int i;

	xassert(gres_ptr);
	info("gres:%s type:%s(%u) %ps flags:%s state", gres_name,
	     gres_ptr->type_name, gres_ptr->type_id, step_id,
	     _gres_flags_str(gres_ptr->flags));
	if (gres_ptr->cpus_per_gres)
		info("  cpus_per_gres:%u", gres_ptr->cpus_per_gres);
	if (gres_ptr->gres_per_step)
		info("  gres_per_step:%"PRIu64, gres_ptr->gres_per_step);
	if (gres_ptr->gres_per_node) {
		info("  gres_per_node:%"PRIu64" node_cnt:%u",
		     gres_ptr->gres_per_node, gres_ptr->node_cnt);
	}
	if (gres_ptr->gres_per_socket)
		info("  gres_per_socket:%"PRIu64, gres_ptr->gres_per_socket);
	if (gres_ptr->gres_per_task)
		info("  gres_per_task:%"PRIu64, gres_ptr->gres_per_task);
	if (gres_ptr->mem_per_gres)
		info("  mem_per_gres:%"PRIu64, gres_ptr->mem_per_gres);

	if (gres_ptr->node_in_use == NULL)
		info("  node_in_use:NULL");
	else if (gres_ptr->gres_bit_alloc == NULL)
		info("  gres_bit_alloc:NULL");
	else {
		for (i = 0; i < gres_ptr->node_cnt; i++) {
			if (!bit_test(gres_ptr->node_in_use, i))
				continue;
			if (gres_ptr->gres_bit_alloc[i]) {
				bit_fmt(tmp_str, sizeof(tmp_str),
					gres_ptr->gres_bit_alloc[i]);
				info("  gres_bit_alloc[%d]:%s of %d", i,
				     tmp_str,
				     (int)bit_size(gres_ptr->gres_bit_alloc[i]));
			} else
				info("  gres_bit_alloc[%d]:NULL", i);
		}
	}
}

/*
 * Log a step's current gres state
 * IN gres_list - generated by gres_ctld_step_alloc()
 * IN job_id - job's ID
 * IN step_id - step's ID
 */
extern void gres_step_state_log(List gres_list, uint32_t job_id,
				uint32_t step_id)
{
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	slurm_step_id_t tmp_step_id;

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_GRES) || !gres_list)
		return;

	(void) gres_init();

	tmp_step_id.job_id = job_id;
	tmp_step_id.step_het_comp = NO_VAL;
	tmp_step_id.step_id = step_id;

	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		_step_state_log(gres_ptr->gres_data, &tmp_step_id,
				gres_ptr->gres_name);
	}
	list_iterator_destroy(gres_iter);
}

/*
 * Determine how many cores of a job's allocation can be allocated to a step
 *	on a specific node
 * IN job_gres_list - a running job's allocated gres info
 * IN/OUT step_gres_list - a pending job step's gres requirements
 * IN node_offset - index into the job's node allocation
 * IN first_step_node - true if this is node zero of the step (do initialization)
 * IN cpus_per_task - number of CPUs required per task
 * IN max_rem_nodes - maximum nodes remaining for step (including this one)
 * IN ignore_alloc - if set ignore resources already allocated to running steps
 * IN job_id, step_id - ID of the step being allocated.
 * RET Count of available cores on this node (sort of):
 *     NO_VAL64 if no limit or 0 if node is not usable
 */
extern uint64_t gres_step_test(List step_gres_list, List job_gres_list,
			       int node_offset, bool first_step_node,
			       uint16_t cpus_per_task, int max_rem_nodes,
			       bool ignore_alloc,
			       uint32_t job_id, uint32_t step_id)
{
	uint64_t core_cnt, tmp_cnt;
	ListIterator step_gres_iter;
	gres_state_t *step_gres_ptr;
	gres_step_state_t *step_data_ptr = NULL;
	slurm_step_id_t tmp_step_id;
	foreach_gres_cnt_t foreach_gres_cnt;

	if (step_gres_list == NULL)
		return NO_VAL64;
	if (job_gres_list == NULL)
		return 0;

	if (cpus_per_task == 0)
		cpus_per_task = 1;
	core_cnt = NO_VAL64;
	(void) gres_init();

	tmp_step_id.job_id = job_id;
	tmp_step_id.step_het_comp = NO_VAL;
	tmp_step_id.step_id = step_id;

	memset(&foreach_gres_cnt, 0, sizeof(foreach_gres_cnt));
	foreach_gres_cnt.ignore_alloc = ignore_alloc;
	foreach_gres_cnt.step_id = &tmp_step_id;

	slurm_mutex_lock(&gres_context_lock);
	step_gres_iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(step_gres_iter))) {
		gres_key_t job_search_key;

		step_data_ptr = (gres_step_state_t *)step_gres_ptr->gres_data;
		job_search_key.plugin_id = step_gres_ptr->plugin_id;
		if (step_data_ptr->type_name)
			job_search_key.type_id = step_data_ptr->type_id;
		else
			job_search_key.type_id = NO_VAL;

		job_search_key.node_offset = node_offset;

		foreach_gres_cnt.job_search_key = &job_search_key;
		foreach_gres_cnt.gres_cnt = INFINITE64;

		(void)list_for_each(job_gres_list, _step_get_gres_cnt,
				    &foreach_gres_cnt);

		if (foreach_gres_cnt.gres_cnt == INFINITE64) {
			/* job lack resources required by the step */
			core_cnt = 0;
			break;
		}
		tmp_cnt = _step_test(step_data_ptr, first_step_node,
				     cpus_per_task, max_rem_nodes,
				     ignore_alloc, foreach_gres_cnt.gres_cnt);
		if ((tmp_cnt != NO_VAL64) && (tmp_cnt < core_cnt))
			core_cnt = tmp_cnt;

		if (core_cnt == 0)
			break;
	}
	list_iterator_destroy(step_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return core_cnt;
}

/*
 * Return TRUE if this plugin ID consumes GRES count > 1 for a single device
 * file (e.g. MPS)
 */
extern bool gres_id_shared(uint32_t plugin_id)
{
	if (plugin_id == mps_plugin_id)
		return true;
	return false;
}
/*
 * Return TRUE if this plugin ID shares resources with another GRES that
 * consumes subsets of its resources (e.g. GPU)
 */
extern bool gres_id_sharing(uint32_t plugin_id)
{
	if (plugin_id == gpu_plugin_id)
		return true;
	return false;
}

/*
 * Determine total count GRES of a given type are allocated to a job across
 * all nodes
 * IN job_gres_list - job's gres_list built by gres_job_state_validate()
 * IN gres_name - name of a GRES type
 * RET count of this GRES allocated to this job
 */
extern uint64_t gres_get_value_by_type(List job_gres_list, char *gres_name)
{
	int i;
	uint32_t plugin_id;
	uint64_t gres_cnt = 0;
	ListIterator job_gres_iter;
	gres_state_t *job_gres_ptr;
	gres_job_state_t *job_gres_data;

	if (job_gres_list == NULL)
		return NO_VAL64;

	gres_cnt = NO_VAL64;
	(void) gres_init();
	plugin_id = gres_build_id(gres_name);

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != plugin_id)
				continue;
			job_gres_data = (gres_job_state_t *)
				job_gres_ptr->gres_data;
			gres_cnt = job_gres_data->gres_per_node;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return gres_cnt;
}

/*
 * Fill in an array of GRES type ids contained within the given node gres_list
 *		and an array of corresponding counts of those GRES types.
 * IN gres_list - a List of GRES types found on a node.
 * IN arrlen - Length of the arrays (the number of elements in the gres_list).
 * IN gres_count_ids, gres_count_vals - the GRES type ID's and values found
 *		in the gres_list.
 * IN val_type - Type of value desired, see GRES_VAL_TYPE_*
 * RET SLURM_SUCCESS or error code
 */
extern int gres_node_count(List gres_list, int arr_len,
			   uint32_t *gres_count_ids,
			   uint64_t *gres_count_vals,
			   int val_type)
{
	ListIterator  node_gres_iter;
	gres_state_t* node_gres_ptr;
	void*         node_gres_data;
	uint64_t      val;
	int           rc, ix = 0;

	rc = gres_init();
	if ((rc == SLURM_SUCCESS) && (arr_len <= 0))
		rc = EINVAL;
	if (rc != SLURM_SUCCESS)
		return rc;

	slurm_mutex_lock(&gres_context_lock);

	node_gres_iter = list_iterator_create(gres_list);
	while ((node_gres_ptr = (gres_state_t*) list_next(node_gres_iter))) {
		gres_node_state_t *node_gres_state_ptr;
		val = 0;
		node_gres_data = node_gres_ptr->gres_data;
		node_gres_state_ptr = (gres_node_state_t *) node_gres_data;
		xassert(node_gres_state_ptr);

		switch (val_type) {
		case (GRES_VAL_TYPE_FOUND):
			val = node_gres_state_ptr->gres_cnt_found;
			break;
		case (GRES_VAL_TYPE_CONFIG):
			val = node_gres_state_ptr->gres_cnt_config;
			break;
		case (GRES_VAL_TYPE_AVAIL):
			val = node_gres_state_ptr->gres_cnt_avail;
			break;
		case (GRES_VAL_TYPE_ALLOC):
			val = node_gres_state_ptr->gres_cnt_alloc;
		}

		gres_count_ids[ix]  = node_gres_ptr->plugin_id;
		gres_count_vals[ix] = val;
		if (++ix >= arr_len)
			break;
	}
	list_iterator_destroy(node_gres_iter);

	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/* Send GRES information to slurmstepd on the specified file descriptor */
extern void gres_g_send_stepd(int fd, slurm_msg_t *msg)
{
	int len;

	/* Setup the gres_device list and other plugin-specific data */
	(void) gres_init();

	slurm_mutex_lock(&gres_context_lock);
	xassert(gres_context_buf);

	len = get_buf_offset(gres_context_buf);
	safe_write(fd, &len, sizeof(len));
	safe_write(fd, get_buf_data(gres_context_buf), len);

	slurm_mutex_unlock(&gres_context_lock);

	if (msg->msg_type != REQUEST_BATCH_JOB_LAUNCH) {
		launch_tasks_request_msg_t *job =
			(launch_tasks_request_msg_t *)msg->data;
		/* Send the merged slurm.conf/gres.conf and autodetect data */
		if (job->accel_bind_type || job->tres_bind || job->tres_freq) {
			len = get_buf_offset(gres_conf_buf);
			safe_write(fd, &len, sizeof(len));
			safe_write(fd, get_buf_data(gres_conf_buf), len);
		}
	}

	return;
rwfail:
	error("%s: failed", __func__);
	slurm_mutex_unlock(&gres_context_lock);

	return;
}

/* Receive GRES information from slurmd on the specified file descriptor */
extern void gres_g_recv_stepd(int fd, slurm_msg_t *msg)
{
	int len, rc;
	buf_t *buffer = NULL;

	slurm_mutex_lock(&gres_context_lock);

	safe_read(fd, &len, sizeof(int));

	buffer = init_buf(len);
	safe_read(fd, buffer->head, len);

	rc = _unpack_context_buf(buffer);

	if (rc == SLURM_ERROR)
		goto rwfail;

	FREE_NULL_BUFFER(buffer);
	if (msg->msg_type != REQUEST_BATCH_JOB_LAUNCH) {
		launch_tasks_request_msg_t *job =
			(launch_tasks_request_msg_t *)msg->data;
		/* Recv the merged slurm.conf/gres.conf and autodetect data */
		if (job->accel_bind_type || job->tres_bind || job->tres_freq) {
			safe_read(fd, &len, sizeof(int));

			buffer = init_buf(len);
			safe_read(fd, buffer->head, len);

			rc = _unpack_gres_conf(buffer);

			if (rc == SLURM_ERROR)
				goto rwfail;

			FREE_NULL_BUFFER(buffer);
		}
	}

	slurm_mutex_unlock(&gres_context_lock);

	/* Set debug flags and init_run only */
	(void) gres_init();

	return;
rwfail:
	FREE_NULL_BUFFER(buffer);
	error("%s: failed", __func__);
	slurm_mutex_unlock(&gres_context_lock);

	/* Set debug flags and init_run only */
	(void) gres_init();

	return;
}

/* Get generic GRES data types here. Call the plugin for others */
static int _get_job_info(int gres_inx, gres_job_state_t *job_gres_data,
			 uint32_t node_inx, enum gres_job_data_type data_type,
			 void *data)
{
	uint64_t *u64_data = (uint64_t *) data;
	bitstr_t **bit_data = (bitstr_t **) data;
	int rc = SLURM_SUCCESS;

	if (!job_gres_data || !data)
		return EINVAL;
	if (node_inx >= job_gres_data->node_cnt)
		return ESLURM_INVALID_NODE_COUNT;
	if (data_type == GRES_JOB_DATA_COUNT) {
		*u64_data = job_gres_data->gres_per_node;
	} else if (data_type == GRES_JOB_DATA_BITMAP) {
		if (job_gres_data->gres_bit_alloc)
			*bit_data = job_gres_data->gres_bit_alloc[node_inx];
		else
			*bit_data = NULL;
	} else {
		/* Support here for plugin-specific data types */
		rc = (*(gres_context[gres_inx].ops.job_info))
			(job_gres_data, node_inx, data_type, data);
	}

	return rc;
}

/*
 * get data from a job's GRES data structure
 * IN job_gres_list  - job's GRES data structure
 * IN gres_name - name of a GRES type
 * IN node_inx - zero-origin index of the node within the job's allocation
 *	for which data is desired
 * IN data_type - type of data to get from the job's data
 * OUT data - pointer to the data from job's GRES data structure
 *            DO NOT FREE: This is a pointer into the job's data structure
 * RET - SLURM_SUCCESS or error code
 */
extern int gres_get_job_info(List job_gres_list, char *gres_name,
			     uint32_t node_inx,
			     enum gres_job_data_type data_type, void *data)
{
	int i, rc = ESLURM_INVALID_GRES;
	uint32_t plugin_id;
	ListIterator job_gres_iter;
	gres_state_t *job_gres_ptr;
	gres_job_state_t *job_gres_data;

	if (data == NULL)
		return EINVAL;
	if (job_gres_list == NULL)	/* No GRES allocated */
		return ESLURM_INVALID_GRES;

	(void) gres_init();
	plugin_id = gres_build_id(gres_name);

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != plugin_id)
				continue;
			job_gres_data = (gres_job_state_t *)
				job_gres_ptr->gres_data;
			rc = _get_job_info(i, job_gres_data, node_inx,
					   data_type, data);
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/* Get generic GRES data types here. Call the plugin for others */
static int _get_step_info(int gres_inx, gres_step_state_t *step_gres_data,
			  uint32_t node_inx, enum gres_step_data_type data_type,
			  void *data)
{
	uint64_t *u64_data = (uint64_t *) data;
	bitstr_t **bit_data = (bitstr_t **) data;
	int rc = SLURM_SUCCESS;

	if (!step_gres_data || !data)
		return EINVAL;
	if (node_inx >= step_gres_data->node_cnt)
		return ESLURM_INVALID_NODE_COUNT;
	if (data_type == GRES_STEP_DATA_COUNT) {
		*u64_data = step_gres_data->gres_per_node;
	} else if (data_type == GRES_STEP_DATA_BITMAP) {
		if (step_gres_data->gres_bit_alloc)
			*bit_data = step_gres_data->gres_bit_alloc[node_inx];
		else
			*bit_data = NULL;
	} else {
		/* Support here for plugin-specific data types */
		rc = (*(gres_context[gres_inx].ops.step_info))
			(step_gres_data, node_inx, data_type, data);
	}

	return rc;
}

/*
 * get data from a step's GRES data structure
 * IN step_gres_list  - step's GRES data structure
 * IN gres_name - name of a GRES type
 * IN node_inx - zero-origin index of the node within the job's allocation
 *	for which data is desired. Note this can differ from the step's
 *	node allocation index.
 * IN data_type - type of data to get from the step's data
 * OUT data - pointer to the data from step's GRES data structure
 *            DO NOT FREE: This is a pointer into the step's data structure
 * RET - SLURM_SUCCESS or error code
 */
extern int gres_get_step_info(List step_gres_list, char *gres_name,
			      uint32_t node_inx,
			      enum gres_step_data_type data_type, void *data)
{
	int i, rc = ESLURM_INVALID_GRES;
	uint32_t plugin_id;
	ListIterator step_gres_iter;
	gres_state_t *step_gres_ptr;
	gres_step_state_t *step_gres_data;

	if (data == NULL)
		return EINVAL;
	if (step_gres_list == NULL)	/* No GRES allocated */
		return ESLURM_INVALID_GRES;

	(void) gres_init();
	plugin_id = gres_build_id(gres_name);

	slurm_mutex_lock(&gres_context_lock);
	step_gres_iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(step_gres_iter))) {
		for (i = 0; i < gres_context_cnt; i++) {
			if (step_gres_ptr->plugin_id != plugin_id)
				continue;
			step_gres_data = (gres_step_state_t *)
				step_gres_ptr->gres_data;
			rc = _get_step_info(i, step_gres_data, node_inx,
					    data_type, data);
			break;
		}
	}
	list_iterator_destroy(step_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

extern uint32_t gres_get_autodetect_flags(void)
{
	return autodetect_flags;
}

extern void gres_clear_tres_cnt(uint64_t *tres_cnt, bool locked)
{
	static bool first_run = 1;
	static slurmdb_tres_rec_t tres_rec;
	int tres_pos;
	assoc_mgr_lock_t locks = { .tres = READ_LOCK };

	/* we only need to init this once */
	if (first_run) {
		first_run = 0;
		memset(&tres_rec, 0, sizeof(slurmdb_tres_rec_t));
		tres_rec.type = "gres";
	}

	/* must be locked first before gres_contrex_lock!!! */
	if (!locked)
		assoc_mgr_lock(&locks);

	slurm_mutex_lock(&gres_context_lock);
	/* Initialize all GRES counters to zero. Increment them later. */
	for (int i = 0; i < gres_context_cnt; i++) {
		tres_rec.name =	gres_context[i].gres_name;
		if (tres_rec.name &&
		    ((tres_pos = assoc_mgr_find_tres_pos(
			      &tres_rec, true)) !=-1))
			tres_cnt[tres_pos] = 0;
	}
	slurm_mutex_unlock(&gres_context_lock);

	/* must be locked first before gres_contrex_lock!!! */
	if (!locked)
		assoc_mgr_unlock(&locks);
}

extern char *gres_device_major(char *dev_path)
{
	int loc_major, loc_minor;
	char *ret_major = NULL;
	struct stat fs;

	if (stat(dev_path, &fs) < 0) {
		error("%s: stat(%s): %m", __func__, dev_path);
		return NULL;
	}
	loc_major = (int)major(fs.st_rdev);
	loc_minor = (int)minor(fs.st_rdev);
	debug3("%s : %s major %d, minor %d",
	       __func__, dev_path, loc_major, loc_minor);
	if (S_ISBLK(fs.st_mode)) {
		xstrfmtcat(ret_major, "b %d:", loc_major);
		//info("device is block ");
	}
	if (S_ISCHR(fs.st_mode)) {
		xstrfmtcat(ret_major, "c %d:", loc_major);
		//info("device is character ");
	}
	xstrfmtcat(ret_major, "%d rwm", loc_minor);

	return ret_major;
}

/* Free memory for gres_device_t record */
extern void destroy_gres_device(void *gres_device_ptr)
{
	gres_device_t *gres_device = (gres_device_t *) gres_device_ptr;

	if (!gres_device)
		return;
	xfree(gres_device->path);
	xfree(gres_device->major);
	xfree(gres_device);
}

/* Destroy a gres_slurmd_conf_t record, free it's memory */
extern void destroy_gres_slurmd_conf(void *x)
{
	gres_slurmd_conf_t *p = (gres_slurmd_conf_t *) x;

	xassert(p);
	xfree(p->cpus);
	FREE_NULL_BITMAP(p->cpus_bitmap);
	xfree(p->file);		/* Only used by slurmd */
	xfree(p->links);
	xfree(p->name);
	xfree(p->type_name);
	xfree(p);
}


/*
 * Convert GRES config_flags to a string. The pointer returned references local
 * storage in this function, which is not re-entrant.
 */
extern char *gres_flags2str(uint8_t config_flags)
{
	static char flag_str[128];
	char *sep = "";

	flag_str[0] = '\0';
	if (config_flags & GRES_CONF_COUNT_ONLY) {
		strcat(flag_str, sep);
		strcat(flag_str, "CountOnly");
		sep = ",";
	}

	if (config_flags & GRES_CONF_HAS_FILE) {
		strcat(flag_str, sep);
		strcat(flag_str, "HAS_FILE");
		sep = ",";
	}

	if (config_flags & GRES_CONF_LOADED) {
		strcat(flag_str, sep);
		strcat(flag_str, "LOADED");
		sep = ",";
	}

	if (config_flags & GRES_CONF_HAS_TYPE) {
		strcat(flag_str, sep);
		strcat(flag_str, "HAS_TYPE");
		sep = ",";
	}

	return flag_str;
}

/*
 * Creates a gres_slurmd_conf_t record to add to a list of gres_slurmd_conf_t
 * records
 */
extern void add_gres_to_list(List gres_list, char *name, uint64_t device_cnt,
			     int cpu_cnt, char *cpu_aff_abs_range,
			     bitstr_t *cpu_aff_mac_bitstr, char *device_file,
			     char *type, char *links)
{
	gres_slurmd_conf_t *gpu_record;
	bool use_empty_first_record = false;
	ListIterator itr = list_iterator_create(gres_list);

	/*
	 * If the first record already exists and has a count of 0 then
	 * overwrite it.
	 * This is a placeholder record created in _merge_config()
	 */
	gpu_record = list_next(itr);
	if (gpu_record && (gpu_record->count == 0))
		use_empty_first_record = true;
	else
		gpu_record = xmalloc(sizeof(gres_slurmd_conf_t));
	gpu_record->cpu_cnt = cpu_cnt;
	if (cpu_aff_mac_bitstr)
		gpu_record->cpus_bitmap = bit_copy(cpu_aff_mac_bitstr);
	if (device_file)
		gpu_record->config_flags |= GRES_CONF_HAS_FILE;
	if (type)
		gpu_record->config_flags |= GRES_CONF_HAS_TYPE;
	gpu_record->cpus = xstrdup(cpu_aff_abs_range);
	gpu_record->type_name = xstrdup(type);
	gpu_record->name = xstrdup(name);
	gpu_record->file = xstrdup(device_file);
	gpu_record->links = xstrdup(links);
	gpu_record->count = device_cnt;
	gpu_record->plugin_id = gres_build_id(name);
	if (!use_empty_first_record)
		list_append(gres_list, gpu_record);
	list_iterator_destroy(itr);
}

extern char *gres_prepend_tres_type(const char *gres_str)
{
	char *output = NULL;

	if (gres_str) {
		output = xstrdup_printf("gres:%s", gres_str);
		xstrsubstituteall(output, ",", ",gres:");
	}
	return output;
}
