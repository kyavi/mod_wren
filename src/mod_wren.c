#include <apr_pools.h>
#include <apr_tables.h>
#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>

#include <stdlib.h>
#include <string.h>

#include "wren.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

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

#define TAG_BLOCK_OPEN "<?wren"
#define TAG_BLOCK_OPEN_LEN strlen(TAG_BLOCK_OPEN)
#define TAG_BLOCK_CLOSE "?>"
#define TAG_BLOCK_CLOSE_LEN strlen(TAG_BLOCK_CLOSE)

#define TAG_EXPR_OPEN "<%="
#define TAG_EXPR_OPEN_LEN strlen(TAG_EXPR_OPEN)
#define TAG_EXPR_CLOSE "%>"
#define TAG_EXPR_CLOSE_LEN strlen(TAG_EXPR_CLOSE)

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
 * Return the headers from the current request_rec as a map of key/value
 * pairs.
 *
 * Bug: must call something like 'System.write("")' or 'var v={}' for the map
 * to actually be registered properly on the other side. Not sure why. I think
 * it's a Wren problem rather than something over here.
 */
static void wren_fn_getEnv(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	const apr_array_header_t *headers =
		apr_table_elts(wren_state->request_rec->headers_in);
	const apr_table_entry_t *e = (apr_table_entry_t*) headers->elts;

	wrenEnsureSlots(vm, headers->nelts * 2 + 1);
	wrenSetSlotNewMap(vm, 0);

	for(size_t i = 0; i < headers->nelts; ++i) {
		int key_slot = i * 2 + 1;

		wrenSetSlotString(vm, key_slot, e[i].key);
		wrenSetSlotString(vm, key_slot + 1, e[i].val);
		wrenInsertInMap(vm, 0, key_slot, key_slot + 1);
	}
}

/**
 * Maps foreign method signatures to functions. We receive a signature of a
 * function inside a class, inside a module, and see if we have something that
 * maps up.
 *
 * We currently only bind things inside the "main" module.
 */
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
			if(strcmp(signature, "getEnv()") == 0)
				return wren_fn_getEnv;
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
				"	foreign static getEnv()\n"
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

/**
 * Write an HTML block inside the file_buf, starting from file_index,
 * to the output wren_buf, starting at wren_index.
 *
 * file_index will get progressed by html_len, the amount of raw HTML
 * within the file. wren_index will get progressed a bit more, html_len plus
 * extra characters for function calls and escaping.
 */
static void wren_parse_insert_html(char *file_buf, char *wren_buf,
		size_t *file_index, size_t *wren_index, size_t html_len)
{
	if(html_len == 0 || (html_len == 1 && *(file_buf + *file_index) == '\n'))
		return;

	/* Start write */
	strncpy(wren_buf + *wren_index, "\nSystem.write(\"", 15);
	*wren_index += 15;

	/*
	 * Write the actual HTML segment to the buffer, escaping forbidden
	 * characters as we go.
	 *
	 * This isn't the most elegantly written thing in the world.
	 */
	char *next_quote   = strchr(file_buf + *file_index, '"')  ?: (char*)INTPTR_MAX;
	char *next_percent = strchr(file_buf + *file_index, '%')  ?: (char*)INTPTR_MAX;
	char *next_slash   = strchr(file_buf + *file_index, '\\') ?: (char*)INTPTR_MAX;

	do {
		char *next_char = MIN(next_quote, MIN(next_percent, next_slash));

		if(next_char > file_buf + (*file_index) + html_len)
			break;

		size_t segment_len = next_char - (file_buf + *file_index);

		strncpy(wren_buf + *wren_index, (file_buf + *file_index), segment_len);
		*file_index += segment_len;
		*wren_index += segment_len + 1;
		wren_buf[*wren_index - 1] = '\\';
		html_len -= segment_len;

		next_quote   = strchr(file_buf + *file_index + 1, '"')  ?: (char*)INTPTR_MAX;
		next_percent = strchr(file_buf + *file_index + 1, '%')  ?: (char*)INTPTR_MAX;
		next_slash   = strchr(file_buf + *file_index + 1, '\\') ?: (char*)INTPTR_MAX;
	} while(html_len > 0);

	strncpy(wren_buf + *wren_index, (file_buf + *file_index), html_len);
	*file_index += html_len;
	*wren_index += html_len;

	/* Close up the write. */
	strncpy(wren_buf + *wren_index, "\")\n", 3);
	*wren_index += 3;
}

/**
 * Parse a Wren page, looking for Wren code blocks, Wren expressions, and
 * regular HTML.
 *
 * Wren blocks (<?wren ... ?>) get inserted straight into the output buffer.
 *
 * Wren expressions (<%= ... %>) get wrapped in an expression call
 * (System.write("%(...)")).
 *
 * The rest is regular HTML, which gets its special characters escaped before
 * being placed in a System.write("...") call.
 */
static char* wren_parse(WrenState *wren_state)
{
	/* Open up a file and write it to a buffer we can work from. */
	FILE *file = fopen(wren_state->request_rec->filename, "r");
	char *file_buf, *out_buf;
	size_t file_len, read_len;

	if(file == NULL)
		return NULL;

	fseek(file, 0, SEEK_END);
	file_len = ftell(file) ?: 1;
	fseek(file, 0, SEEK_SET);

	file_buf = calloc(file_len + 1, 1);
	read_len = fread(file_buf, 1, file_len, file);
	fclose(file);

	if(read_len != file_len) {
		free(file_buf);
		return NULL;
	}

	/*
	 * Allocate another buffer for the output Wren code. This needs to be
	 * bigger than the input to account for the function calls and escaping
	 * we add.
	 *
	 * TODO: There needs to be some method of dynamically resizing this, since
	 * it's currently possible to exceed it.
	 */
	size_t file_index = 0;
	size_t out_index = 0;

	/* Whilst this is a work in progress - easily request the raw file. */
	if(strcmp(wren_state->request_rec->args ?: "", "raw") == 0)
		return file_buf;

	out_buf = calloc(file_len * 15, 1);
	out_buf[out_index++] = '{';

	/*
	 * Go through the file looking for Wren tags and converting HTML blocks to
	 * Wren System.write statements.
	 */
	while(file_index < file_len) {
		char *next_block_open = strstr(
				file_buf + file_index, TAG_BLOCK_OPEN) ?: (char*)INTPTR_MAX;
		char *next_expr_open = strstr(
				file_buf + file_index, TAG_EXPR_OPEN) ?: (char*)INTPTR_MAX;

		/*
		 * There are no more Wren segments to parse. Add the rest to the output
		 * buffer as HTML.
		 */
		if(next_block_open == (char*)INTPTR_MAX &&
				next_expr_open == (char*)INTPTR_MAX)
		{
			size_t html_len = file_len - file_index;
			wren_parse_insert_html(file_buf, out_buf,
					&file_index, &out_index, html_len);
			break;
		}

		/*
		 * Otherwise, we've hit a Wren segment.
		 *
		 * This could be a Wren code block (<?wren ... ?>) or an expression
		 * (<%= ... %>), so we check for which tag we've hit. If we're in
		 * an expression, we wrap it in a System.write("%(...))".
		 */
		bool expr = next_block_open > next_expr_open ? true : false;
		char *next             = expr ? next_expr_open     : next_block_open;
		char *closing_tag      = expr ? TAG_EXPR_CLOSE     : TAG_BLOCK_CLOSE;
		size_t opening_tag_len = expr ? TAG_EXPR_OPEN_LEN  : TAG_BLOCK_OPEN_LEN;
		size_t closing_tag_len = expr ? TAG_EXPR_CLOSE_LEN : TAG_BLOCK_CLOSE_LEN;

		if(file_len - file_index > 0) {
			size_t html_len = next - (file_buf + file_index);
			wren_parse_insert_html(file_buf, out_buf,
					&file_index, &out_index, html_len);
		}

		file_index += opening_tag_len;
		char *closing_tag_pos = strstr(file_buf + file_index, closing_tag);

		if(closing_tag_pos == NULL) {
			/*
			 * Mismatched opening/closing tag. This should probably be handled,
			 * but for now, just let Wren fail.
			 */
			break;
		}

		if(expr == true) {
			strncpy(out_buf + out_index, "\nSystem.write(\"%(", 17);
			out_index += 17;
		}

		/*
		 * Write out whatever we found in our Wren tags and push the file read
		 * index along to the end of the closing tag.
		 */
		strncpy(out_buf + out_index, file_buf + file_index,
				closing_tag_pos - (file_buf + file_index));
		out_index += closing_tag_pos - (file_buf + file_index);
		file_index += closing_tag_pos - (file_buf + file_index) + closing_tag_len;

		if(expr == true) {
			strncpy(out_buf + out_index, ")\")\n", 4);
			out_index += 4;
		}
	}

	out_buf[out_index++] = '}';

	free(file_buf);
	return out_buf;
}

/**
 * Main Wren handler that gets hooked when we call a Wren file, and converts
 * the file to something that can be understood by the WrenVM and runs it.
 */
static int wren_handler(request_rec *r)
{
	WrenState *wren_state;
	char *wren_code;

	/* Make sure the request is for us. */
	if(strcmp(r->handler ?: "", "wren") != 0)
		return DECLINED;

	/* TODO: allow other methods (most critically, POST). */
	if(r->method_number != M_GET)
		return HTTP_METHOD_NOT_ALLOWED;

	wren_state = wren_acquire_state(r);

	if((wren_code = wren_parse(wren_state)) == NULL) {
		wren_release_state(wren_state);
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	/* Pre-release debug: ?raw will print the generated Wren code. */
	if(strcmp(r->args ?: "", "compiled") == 0 ||
			strcmp(r->args ?: "", "raw") == 0)
	{
		ap_set_content_type(r, "text/plain");
		ap_rprintf(r, "%s", wren_code);
	}
	else {
		ap_set_content_type(r, "text/html");
		wrenInterpret(wren_state->vm, wren_code);
	}

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
