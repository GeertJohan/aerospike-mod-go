/*
 * Copyright 2008-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <setjmp.h>         // needed for gracefully handling lua panics

// #include <fault.h>

#include <pthread.h>
#include <dlfcn.h>

#include <citrusleaf/cf_queue.h>
#include <citrusleaf/cf_rchash.h>

#include <citrusleaf/alloc.h>

#include <aerospike/as_aerospike.h>
#include <aerospike/as_log_macros.h>
#include <aerospike/as_types.h>

#include <aerospike/mod_go.h>
#include <aerospike/mod_go_config.h>

#include "internal.h"

/******************************************************************************
 * MACROS
 ******************************************************************************/

#define GO_PARAM_COUNT_THRESHOLD 20 // warn if a function call exceeds this

#define MOD_GO_CONFIG_USRPATH "/opt/aerospike/usr/udf/go"

/******************************************************************************
 * TYPES
 ******************************************************************************/

struct context_s;
typedef struct context_s context;

struct context_s {
	mod_go_config      config;
	pthread_rwlock_t *  lock;
};

/******************************************************************************
 * VARIABLES
 ******************************************************************************/

static const pthread_rwlock_t mod_go_lock;

/**
 * Go Module Specific Data
 * This will populate the module.source field
 */
static context mod_go_source = {
	.config = {
		.user_path      = MOD_GO_CONFIG_USRPATH
	},
	.lock = NULL
};


/******************************************************************************
 * STATIC FUNCTIONS
 ******************************************************************************/

static int update(as_module *, as_module_event *);
static int apply_record(as_module *, as_udf_context *, const char *, const char *, as_rec *, as_list *, as_result *);
static int apply_stream(as_module *, as_udf_context *, const char *, const char *, as_stream *, as_list *, as_stream *, as_result *);


/******************************************************************************
 * FUNCTIONS
 ******************************************************************************/

/**
 * Module Configurator.
 * This configures and reconfigures the module. This can be called an
 * arbitrary number of times during the lifetime of the server.
 *
 * @param m the module being configured.
 * @return 0 = success, 1 = source is NULL, 2 = event.data is invalid, 3 = unable to create lock, 4 = unabled to create cache
 * @sychronization: Caller should have a write lock
 */
static int update(as_module * m, as_module_event * e) {
	
	context * ctx = (context *) (m ? m->source : NULL);

	if ( ctx == NULL ) return 1;

	switch ( e->type ) {
		case AS_MODULE_EVENT_CONFIGURE: {
			as_log_trace("configuring go"); // TODO: looks like confiugration isn't done?!?!?
			mod_go_config * config     = (mod_go_config *) e->data.config;

			if ( ctx->lock == NULL ) {
				ctx->lock = &mod_go_lock;
#ifdef __linux__
				pthread_rwlockattr_t rwattr;
				if (0 != pthread_rwlockattr_init(&rwattr)) {
					return 3;
				}
				if (0 != pthread_rwlockattr_setkind_np(&rwattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP)) {
					return 3;
				}
				if (0 != pthread_rwlock_init(ctx->lock, &rwattr)) {
					return 3;
				}
#else
				pthread_rwlockattr_t rwattr;
				if (0 != pthread_rwlockattr_init(&rwattr)) {
					return 3;
				}

				if (0 != pthread_rwlock_init(ctx->lock, &rwattr)) {
					return 3;
				}
#endif
			}

			// Attempt to open the directory.
			// If it opens, then set the ctx value.
			// Otherwise, we alert the user of the error when a UDF is called. (for now)
			if ( config->user_path[0] != '\0' ) {
				DIR * dir = opendir(config->user_path);
				if ( dir == 0 ) {
					ctx->config.user_path[0] = '\0';
					strncpy(ctx->config.user_path+1, config->user_path, 255);
				} else {
					strncpy(ctx->config.user_path, config->user_path, 256);
					closedir(dir);
				}
				dir = NULL;
			}

			break;
		}
		case AS_MODULE_EVENT_FILE_SCAN: {
			if ( ctx->config.user_path[0] == '\0' ) return 2;
			break;
		}
		case AS_MODULE_EVENT_FILE_ADD: {
			if ( e->data.filename == NULL ) return 2;
			break;
		}
		case AS_MODULE_EVENT_FILE_REMOVE: {
			if ( e->data.filename == NULL ) return 2;
			break;
		}
		case AS_MODULE_EVENT_CLEAR_CACHE: {
			break;
		}
	}

	return 0;
}

/**
 * Validates a UDF module
 */
static int validate(as_module * m, as_aerospike * as, const char * filename, const char * content, uint32_t size, as_module_error * err) {

	int rc = 0;

	err->scope = 0;
	err->code = 0;
	err->message[0] = '\0';
	err->file[0] = '\0';
	err->line = 0;
	err->func[0] = '\0';

	context * ctx = (context *) m->source;

	// create filepath for validation so
	as_log_trace("building filepath for file %s", filename);
	char    filepath[1024]  = {0};
	{
		char *  p               = filepath;
		char *  prefix          = "validate.";
		int     prefix_len      = strlen(prefix);
		char *  user_path       = ctx->config.user_path;
		size_t  user_path_len   = strlen(user_path);
		int     filename_len    = strlen(filename);

		memcpy(p, user_path, sizeof(char) * strlen(user_path));
		p += user_path_len;

		memcpy(p, "/", 1);
		p += 1;

		memcpy(p, prefix, prefix_len);
		p += prefix_len;

		memcpy(p, filename, filename_len);
		p += filename_len;

		p[0] = '\0';
	}

	// write validation so to disk
	as_log_trace("writing .so for validation: %s", filepath);
	FILE * file = fopen(filepath, "w");
	if (file == NULL) {
		as_log_debug("could not open udf put to %s: %d", filepath, errno);
		return -1;
	}
	int r = fwrite(content, sizeof(char), size, file);
	if (r <= 0) {
		fclose(file);
		as_log_debug("could not write file %s %d", filepath, r);
		return -1;
	}

	fclose(file);
	file = NULL;

	// open .so and check that `as_mod_go_v0_apply_record`, `as_mod_go_v0_apply_steam` and `as_mod_go_v0_info` are available.
	// call `ad_mod_go_v0_info` and send the info to logs.
	// error when functions are unavailable.

	// validate so
	// char *string_to_pass = NULL;
	// if (asprintf(&string_to_pass, "This is a test.") < 0) {
	// 	return -1;
	// }

	char file_name[80];
	void* plugin;
	int vmajor;
	int vminor;
	bool plugin_supports_dlclose = false;
	
	char* dlresult;
	plugin = dlopen(filepath, RTLD_NOW);
	if (!plugin) {
		 as_log_debug("cannot load go module %s: %s", filename, dlerror());
		err->code = 10;
		goto Cleanup;
	}
	as_log_trace("UDF module %s loaded", filename);
	
	// TODO: place these outside this func
	typedef int (*aerospike_udf_go_get_api_version_major_f) ();
	typedef int (*aerospike_udf_go_get_api_version_minor_f) ();
	typedef char* (*aerospike_udf_go_get_property_f) (char*);
	typedef int (*aerospike_udf_go_setup_f) ();
	typedef int (*aerospike_udf_go_apply_record_f) (char*);
	typedef int (*aerospike_udf_go_apply_stream_f) (char*);
	
	
	aerospike_udf_go_get_api_version_minor_f aerospike_udf_go_get_api_version_minor;
	aerospike_udf_go_get_api_version_major_f aerospike_udf_go_get_api_version_major;
	aerospike_udf_go_get_property_f aerospike_udf_go_get_property;
	aerospike_udf_go_setup_f aerospike_udf_go_setup;
	aerospike_udf_go_apply_record_f aerospike_udf_go_apply_record;
	aerospike_udf_go_apply_stream_f aerospike_udf_go_apply_stream;

	// locate api version functions
	aerospike_udf_go_get_api_version_major = dlsym(plugin, "aerospike_udf_go_get_api_version_major");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_get_api_version_major in %s: %s", filename, dlresult);
		err->code = 20;
		goto Cleanup;
	}
	
	aerospike_udf_go_get_api_version_minor = dlsym(plugin, "aerospike_udf_go_get_api_version_minor");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_get_api_version_minor in %s: %s", filename, dlresult);
		err->code = 21;
		goto Cleanup;
	}
	
	// get and check version
	vmajor = aerospike_udf_go_get_api_version_major();
	if (vmajor != 1) {
		as_log_debug("Incompatible API version major %d in %s", vmajor, filename);
		err->code = 22;
		goto Cleanup;
	}
	vminor = aerospike_udf_go_get_api_version_minor();
	as_log_debug("UDF module %s has conn version %d.%d", filename, vmajor, vminor);
	
	// load v1 functions
	aerospike_udf_go_get_property = dlsym(plugin, "aerospike_udf_go_get_property");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_get_property in %s: %s", filename, dlresult);
		err->code = 30;
		goto Cleanup;
	}
	aerospike_udf_go_setup = dlsym(plugin, "aerospike_udf_go_setup");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_setup in plugin: %s", dlresult);
		err->code = 31;
		goto Cleanup;
	}
	aerospike_udf_go_apply_record = dlsym(plugin, "aerospike_udf_go_apply_record");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_apply_record in plugin: %s", dlresult);
		err->code = 32;
		goto Cleanup;
	}
	aerospike_udf_go_apply_stream = dlsym(plugin, "aerospike_udf_go_apply_stream");
	dlresult = dlerror();
	if (dlresult) {
		as_log_debug("Cannot find aerospike_udf_go_apply_stream in plugin: %s", dlresult);
		err->code = 33;
		goto Cleanup;
	}
	
	// check wether dlclose is supported
	char* supportsDlclose = aerospike_udf_go_get_property("dlclose-supported");
	if (strcmp(supportsDlclose, "true") == 0) {
		plugin_supports_dlclose = true;
		as_log_debug("UDF module %s supports dlclose", filename);
	} else {
		as_log_debug("UDF module %s does not support dlclose", filename);
	}
	free(supportsDlclose);
	
	// get connection package name and version
	char* connName = aerospike_udf_go_get_property("conn-name");
	char* connVersion = aerospike_udf_go_get_property("conn-version");
	as_log_debug("UDF module %s uses conn %s at version %s", filename, connName, connVersion);
	free(connVersion);
	free(connName);
	
	// get go version that was used to compile the UDF module
	char* goVersion = aerospike_udf_go_get_property("go-version");
	as_log_debug("UDF module %s was compiled with go %s", filename, goVersion);
	free(goVersion);
	
	// call setup function
	int res = aerospike_udf_go_setup();
	if (res) {
		as_log_debug("error running aerospike_udf_go_setup for udf-module %s, result: %d", filename, res);
		err->code = 41;
		goto Cleanup;
	}
	as_log_debug("UDF module %s setup completed", filename);
	
	// TODO: remove from validate
	int applyResHelloWorld = aerospike_udf_go_apply_record("HelloWorld");
	if (applyResHelloWorld) {
		as_log_debug("error applying UDF '%s'.%s to record, result: %d", filename, "HelloWorld", applyResHelloWorld);
		err->code = 254;
		goto Cleanup;
	}
	
	// TODO: remove from validate
	int applyResFoobar = aerospike_udf_go_apply_record("Foobar");
	if (applyResFoobar) {
		as_log_debug("error applying UDF '%s'.%s to record, result: %d", filename, "Foobar", applyResFoobar);
		err->code = 254;
		goto Cleanup;
	}
	
Cleanup:
	if (dlresult != NULL) {
		free(dlresult);
	}
	if (plugin_supports_dlclose && plugin != NULL) {
		dlclose(plugin);
	}

	// remove validation .so from disk
	unlink(filepath);

	if ( err->code == 0 ) {
		as_log_trace("Go Validation Pass for '%s'", filename);
	} else {
		as_log_debug("Go Validation Fail for '%s': (%d) %s", filename, err->code, err->message);
	}

	return err->code;
}

// static int apply(lua_State * l, as_udf_context *udf_ctx, int err, int argc, as_result * res, bool is_stream) {
// 	//
// 	// as_log_trace("apply");
// 	//
// 	// as_log_trace("rc = %d", rc);
// 	//
// 	// if ( rc == 0 ) { // indicates lua-execution success
// 	// 	if ( (is_stream == false) && (res != NULL) ) { // if is_stream is true, no need to set result as success
// 	// 		as_val * rv = mod_lua_retval(l);
// 	// 		as_result_setsuccess(res, rv);
// 	// 	}
// 	// }
// 	// else {
// 	// 	if ( res != NULL ) {
// 	// 		as_val * rv = mod_lua_retval(l);
// 	// 		as_result_setfailure(res, rv);
// 	// 	}
// 	// }
// 	//
// 	// if ( is_stream || (res == NULL) ) { //if is_stream is true then whether res is null or not rc should be returned
// 	// 	return rc;
// 	// } else {
// 	// 	return 0;
// 	// }
// }


/**
 * Applies a record and arguments to the function specified by a fully-qualified name.
 *
 * TODO: Remove redundancies between apply_record() and apply_stream()
 *
 * @param m module from which the fqn will be resolved.
 * @param udf_ctx udf execution context
 * @param function fully-qualified name of the function to invoke.
 * @param r record to apply to the function.
 * @param args list of arguments for the function represented as vals
 * @param res pointer to an as_result that will be populated with the result.
 * @return 0 on success, otherwise 1
 */
static int apply_record(as_module * m, as_udf_context * udf_ctx, const char * filename, const char * function, as_rec * r, as_list * args, as_result * res) {

	int         rc      = 0;
	context *   ctx     = (context *) m->source;    // mod-go context
	int         argc    = 0;                        // Number of arguments pushed onto the stack
	int         err     = 0;                        // Error handler
	as_aerospike *as    = udf_ctx->as;              // aerospike object

	as_log_trace("apply_record: BEGIN");

	// // lease a state
	// as_log_trace("apply_record: poll state");
	// rc = poll_state(ctx, &citem);
	//
	// if ( rc != 0 ) {
	// 	as_log_trace("apply_record: Unable to poll a state");
	// 	return rc;
	// }
	//
	// //++ call apply_record
	//
	// // return the state
	// pthread_rwlock_rdlock(ctx->lock);
	// as_log_trace("apply_record: offer state");
	// offer_state(ctx, &citem);
	// pthread_rwlock_unlock(ctx->lock);

	as_log_trace("apply_record: END");
	return rc;
}



/**
 * Applies function to a stream and set of arguments.
 *
 * Proxies to `m->hooks->apply_stream(m, ...)`
 *
 * TODO: Remove redundancies between apply_record() and apply_stream()
 *
 * @param m module from which the fqn will be resolved.
 * @param udf_ctx udf execution context
 * @param function fully-qualified name of the function to invoke.
 * @param istream stream to apply to the function.
 * @param args list of arguments for the function represented as vals
 * @param ostream output stream which will be populated by applying the function.
 * @param res pointer to an as_result that will be populated with the result.
 * @return 0 on success, otherwise 1
 */
static int apply_stream(as_module * m, as_udf_context *udf_ctx, const char * filename, const char * function, as_stream * istream, as_list * args, as_stream * ostream, as_result * res) {

	int         rc      = 0;
	context *   ctx     = (context *) m->source;    // mod-go context
	int         argc    = 0;                        // Number of arguments pushed onto the stack
	int         err     = 0;                        // Error handler
	as_aerospike *as    = udf_ctx->as;              // aerospike object

	as_log_trace("apply_stream: BEGIN");

	// // lease a state
	// as_log_trace("apply_stream: poll state");
	// rc = poll_state(ctx, &citem);
	//
	// if ( rc != 0 ) {
	// 	as_log_trace("apply_stream: Unable to poll a state");
	// 	return rc;
	// }
	//
	// //++ call  `as_mod_go_v0_apply_stream`
	//
	// // release the context
	// pthread_rwlock_rdlock(ctx->lock);
	// as_log_trace("apply_stream: lose the context");
	// offer_state(ctx, &citem);
	// pthread_rwlock_unlock(ctx->lock);

	as_log_trace("apply_stream: END");
	return rc;
}


/**
 * Module Hooks
 */
static const as_module_hooks mod_go_hooks = {
	.destroy        = NULL,
	.update         = update,
	.validate		= validate,
	.apply_record   = apply_record,
	.apply_stream   = apply_stream
};

/**
 * Module
 */
as_module mod_go = {
	.source         = &mod_go_source,
	.hooks          = &mod_go_hooks,
	.lock           = &mod_go_lock
};
