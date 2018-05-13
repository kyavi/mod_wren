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

static void module_init(apr_pool_t *pool, server_rec *s)
{
	ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
			"Initialising mod_wren");

	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.writeFn = wren_write;
	config.errorFn = wren_err;

	wren_states = calloc(NUM_WREN_STATES, sizeof(WrenState));

	for(size_t i = 0; i < NUM_WREN_STATES; ++i) {
		wren_states[i].vm = wrenNewVM(&config);
		wrenSetUserData(wren_states[i].vm, &wren_states[i]);
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

static int wren_handler(request_rec *r)
{
	WrenState *wren_state = wren_acquire_state(r);

	ap_set_content_type(r, "text/html");
	wrenInterpret(wren_state->vm, "System.print(\"Hello, world!\")");

	wren_release_state(wren_state);

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
