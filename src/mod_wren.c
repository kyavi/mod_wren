#include <apr_pools.h>
#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>

#include <stdlib.h>
#include <string.h>

#include "wren.h"

/**
 * A WrenState contains a VM and everything relevant to the current request
 * it's serving.
 */
typedef struct {
	request_rec *request_rec;
	bool lock;
	WrenVM *vm;
} WrenState;

/* TODO: Add mutex locks for enabling multiple states. */
#define NUM_WREN_STATES 1
WrenState *wren_states;

#define ERROR_START \
	"<div style='display: inline-block; width: 100%%; " \
		"background-color: #E0E0E0;'>"

#define ERROR_END \
	"</div>"

/**
 * Sets the output of Wren's print functions.
 *
 * All generated HTML comes through here.
 */
static void wren_write(WrenVM *vm, const char *str)
{
	WrenState *wren_state = wrenGetUserData(vm);
	ap_rprintf(wren_state->request_rec, "%s", str);
}

static void wren_err(WrenVM *vm, WrenErrorType type, const char *module,
		int line, const char *message)
{
	WrenState *wren_state = wrenGetUserData(vm);

	/*
	 * Wren prints out the script name, which doesn't help very much.
	 * There might be subtleties I'm missing right now though.
	 */
	if(message[0] == '(' && strcmp(message, "(script)") == 0)
		return;

	ap_rprintf(wren_state->request_rec,
			ERROR_START
			"<p><b>Line %d: </b>" /* line number */
			"%s</p>" /* error message */
			ERROR_END,
			line, message
		);
}

static void wren_fn_default(WrenVM *vm)
{
	/*
	 * Default function to bind, because we prefer logging a failed bind with
	 * Apache instead of crashing.
	 */
}

/**
 * wrenSetSlotNewList(vm, list_slot)
 *
 * wrenSetSlotDouble(vm, value_slot, value)
 * wrenInsertInList(vm, list_slot, index, value_slot)
 *
 * Index of -1 to push back.
 */
static void wren_fn_get_test_list(WrenVM *vm)
{
	wrenSetSlotNewList(vm, 0);

	wrenSetSlotDouble(vm, 1, 1.0);
	wrenInsertInList(vm, 0, -1, 1);
	wrenSetSlotDouble(vm, 2, 2.0);
	wrenInsertInList(vm, 0, -1, 2);
	wrenSetSlotDouble(vm, 3, 3.0);
	wrenInsertInList(vm, 0, -1, 3);
}

WrenForeignMethodFn wren_bind_foreign_methods(WrenVM *vm, const char *module,
		const char *class_name, bool is_static, const char *signature)
{
	if(strcmp(module, "main") != 0) {
		ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
				"Tried to bind foreign method in module '%s'", module);
		return wren_fn_default;
	}

	if(strcmp(class_name, "Web") == 0) {
		if(is_static == true) {
			if(strcmp(signature, "getTestList()") == 0)
				return wren_fn_get_test_list;
		}
	}

	ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
			"Failed to find foreign method '%s.%s'", class_name, signature);

	return wren_fn_default;
}

static void module_init(apr_pool_t *pool, server_rec *s)
{
	ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
			"Initialising mod_wren");

	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.writeFn = wren_write;
	config.errorFn = wren_err;
	config.bindForeignMethodFn = wren_bind_foreign_methods;

	wren_states = calloc(NUM_WREN_STATES, sizeof(WrenState));

	for(size_t i = 0; i < NUM_WREN_STATES; ++i) {
		wren_states[i].vm = wrenNewVM(&config);
		wrenSetUserData(wren_states[i].vm, &wren_states[i]);

		/*
		 * We only declare foreign methods once whilst creating the interpeter
		 * because they persist, and adding them again each time inside the
		 * handler's wrenInterpret causes a fatal error.
		 */
		wrenInterpret(wren_states[i].vm,
				"class Web {\n"
				"	foreign static getTestList()\n"
				"}\n"
			);
	}
}

/**
 * Returns the first available WrenState, or spins until one is available.
 */
static WrenState* wren_acquire_state(request_rec *r)
{
	WrenState *out;

	for(size_t i = 0; i < NUM_WREN_STATES; ++i) {
		if(wren_states[i].lock == true)
			continue;

		wren_states[i].lock = true;
		out = &wren_states[i];
		out->request_rec = r;
		return out;
	}

	apr_sleep(125000);
	return wren_acquire_state(r);
}

/**
 * Release a WrenState to be reused.
 */
static void wren_release_state(WrenState *wren_state)
{
	wren_state->request_rec = NULL;
	wren_state->lock = false;
}

static char* wren_load(WrenState *wren_state)
{
	FILE *file = fopen(wren_state->request_rec->filename, "r");
	char *buf;
	size_t file_len;

	if(file == NULL)
		return NULL;

	fseek(file, 0, SEEK_END);
	file_len = ftell(file) ?: 1;
	fseek(file, 0, SEEK_SET);

	buf = calloc(file_len + 1, 1);
	file_len = fread(buf, 1, file_len, file);
	buf[file_len + 1] = '\0';
	fclose(file);

	return buf;
}

static int wren_handler(request_rec *r)
{
	WrenState *wren_state = wren_acquire_state(r);
	char *wren_code;

	if((wren_code = wren_load(wren_state)) == NULL) {
		wren_release_state(wren_state);
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	ap_set_content_type(r, "text/html");
	wrenInterpret(wren_state->vm, wren_code);

	wren_release_state(wren_state);
	free(wren_code);

	return OK;
}

static void register_hooks(apr_pool_t *pool)
{
	ap_hook_child_init(module_init, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_handler(wren_handler, NULL, NULL, APR_HOOK_LAST);
}

module AP_MODULE_DECLARE_DATA wren_module = {
	STANDARD20_MODULE_STUFF,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	register_hooks
};
