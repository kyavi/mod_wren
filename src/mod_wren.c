#include <apr_dbd.h>
#include <apr_hash.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>

#include <apache2/mod_dbd.h>

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

/* TODO: make database inclusion a compile-time option. */
typedef struct {
	apr_dbd_t *handle;
	const apr_dbd_driver_t *driver;
	bool alive;
	const char *error;
	apr_pool_t *pool;
} DatabaseConn;

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

/* The amount by which the page output buffer capacity increases on resize. */
#define PARSE_BUFFER_GROWTH_RATE 1.5

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

/**
 * Opens a connection to a database through mod_dbd with 'params', with a new
 * memory pool.
 *
 * On error, sets the error string returned by WebDB.error
 */
static void db_open(WrenState *wren_state, DatabaseConn *db,
		const char *params)
{
	ap_dbd_t *dbd;
	apr_pool_t *pool;
	const char *error;
	request_rec *r = wren_state->request_rec;

	if(params == NULL) {
		db->error = "No parameters provided for database connection";
		return;
	}

	/*
	 * Acquire a connection to dbd lasting for lifetime of the current request.
	 *
	 * TODO: try moving this out to WrenState and reusing the connection for
	 * multiple connections in the same thread.
	 */
	if((dbd = ap_dbd_acquire(r)) == NULL) {
		db->error = "Failed to acquire a database connection!";
		return;
	}

	/* Each database connection uses its own resource pool. */
	if(apr_pool_create(&pool, r->pool) != APR_SUCCESS) {
		db->error = "Failed to create new database pool";
		return;
	}

	db->alive = true;
	db->handle = dbd->handle;
	db->driver = dbd->driver;
	db->pool = pool;

	if(apr_dbd_open_ex(
			db->driver, pool, params, &db->handle, &error) != APR_SUCCESS)
	{
		apr_pool_clear(pool);
		apr_pool_destroy(pool);
		memset(db, 0x0, sizeof(DatabaseConn));

		db->error = error;
		return;
	}
}

/**
 * Close a database connection if it's currently alive.
 */
static void db_close(DatabaseConn *db)
{
	if(db->alive == false)
		return;

	if(apr_dbd_close(db->driver, db->handle) != APR_SUCCESS)
		db->error = "Failed to close database connection"; /* ¯\_(ツ)_/¯ */

	db->driver = 0;
	db->handle = 0;
	apr_pool_clear(db->pool); /* Destroys sub-pools that mod_dbd might make. */
	apr_pool_destroy(db->pool);
	db->alive = false;
}

/**
 * WebDB foreign class allocate.
 *
 * Allocates the memory for a DatabaseConn in a WebDB instance.
 */
void wren_foreign_dbd_allocate(WrenVM *vm)
{
	DatabaseConn *db = (DatabaseConn*)
		wrenSetSlotNewForeign(vm, 0, 0, sizeof(DatabaseConn));

	memset(db, 0x0, sizeof(DatabaseConn));
}

/**
 * WebDB foreign class finalize
 *
 * If a database connection for the WebDB instance has been left open, close it.
 */
void wren_foreign_dbd_finalize(void *data)
{
	DatabaseConn *db = (DatabaseConn*)data;

	if(db->alive == true)
		db_close(db);
}

/**
 * WebDB.open()
 *
 * Constructor for the WebDB object, opening a database with a user-provided
 * parameter string.
 */
static void wren_foreign_webdb_open(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);
	const char *db_params;

	db_params = wrenGetSlotType(vm, 1) == WREN_TYPE_STRING ?
		wrenGetSlotString(vm, 1) : NULL;

	db_open(wren_state, db, db_params);
}

/**
 * WebDB.close()
 *
 * Close the database connection related to a WebDB instance.
 */
static void wren_foreign_webdb_close(WrenVM *vm)
{
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);

	db_close(db);
}

/**
 * WebDB.run()
 *
 * Runs a provided statement.
 *
 * Returns true if the statement executes successfully, otherwise false.
 */
static void wren_foreign_webdb_run(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);
	const char *run;
	int rows = 0;
	int result;

	if(wrenGetSlotType(vm, 1) != WREN_TYPE_STRING) {
		db->error = "Type error in db.run(): must provide a string.";
		wrenSetSlotBool(vm, 0, false);
		return;
	}

	run = wrenGetSlotString(vm, 1);
	result = apr_dbd_query(db->driver, db->handle, &rows, run);

	if(result != APR_SUCCESS)
		db->error = apr_dbd_error(db->driver, db->handle, result);

	wrenSetSlotBool(vm, 0, result == APR_SUCCESS);
}

/**
 * WebDB.query()
 *
 * Runs a database query and returns a list of results, containing a list for
 * each table row returned. For example:
 *
 *   db.query("select Name,Age,FavouriteFood from PersonTable;")
 *
 * If the PersonTable contains two rows, the return value would be a list
 * containing two lists:
 *
 *   [
 *     [ "Eloise", "18", "Spinach" ]
 *     [ "Miranda", "25", "Chocolate" ]
 *   ]
 */
static void wren_foreign_webdb_query(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);

	if(wrenGetSlotType(vm, 1) != WREN_TYPE_STRING) {
		db->error = "Type error in db.query(): must provide a string.";
		wrenSetSlotNull(vm, 0);
		return;
	}

	apr_dbd_results_t *results = NULL;
	apr_dbd_row_t *apr_row = NULL;
	const char *query = wrenGetSlotString(vm, 1);
	int select_result;
	int cols;
	int rows;
	int slot = 0;

	/*
	 * We request the database results synchronously (allowing random access to
	 * any row) so that we can get the numbers of rows with apr_dbd_num_tuples.
	 * This is slower than getting them asynchronously, but we need the number
	 * of rows so that we can request enough slots from Wren.
	 */
	if((select_result = apr_dbd_select(db->driver, db->pool, db->handle,
				&results, query, -1) != APR_SUCCESS) || results == NULL)
	{
		db->error = apr_dbd_error(db->driver, db->handle, select_result);
		wrenSetSlotNull(vm, 0);
		return;
	}

	rows = apr_dbd_num_tuples(db->driver, results);
	cols = apr_dbd_num_cols(db->driver, results);

	/*
	 * Reserve the number of table, elements, plus a list to hold each row,
	 * plus the big list we're returning in slot 0.
	 */
	wrenEnsureSlots(vm, rows * cols + rows + 1);
	wrenSetSlotNewList(vm, slot++);

	/*
	 * For each row, create a new list and insert the value of each column.
	 */
	for(int row = 1;
			apr_dbd_get_row(db->driver, r->pool, results, &apr_row, row) != -1;
			++row)
	{
		int list_slot = slot++;

		wrenSetSlotNewList(vm, list_slot);

		for(int col = 0; col < cols; ++col) {
			const char *entry = apr_dbd_get_entry(db->driver, apr_row, col);
			int entry_slot = slot++;

			if(entry != NULL)
				wrenSetSlotString(vm, entry_slot, entry);
			else
				wrenSetSlotNull(vm, entry_slot);

			wrenInsertInList(vm, list_slot, -1, entry_slot);
		}

		wrenInsertInList(vm, 0, -1, list_slot);
	}
}

/**
 * WebDB.error
 *
 * Getter, returns the last string generated by a WebDB function, or null if
 * there have been no errors.
 */
static void wren_foreign_webdb_error(WrenVM *vm)
{
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);

	if(db->error != NULL)
		wrenSetSlotString(vm, 0, db->error);
	else
		wrenSetSlotNull(vm, 0);
}

/**
 * WebDB.clearError()
 *
 * Sets the current error string to null. Hopefully you check for errors after,
 * ~everything~ you do, but this is here just in case.
 */
static void wren_foreign_webdb_clearError(WrenVM *vm)
{
	DatabaseConn *db = (DatabaseConn*)wrenGetSlotForeign(vm, 0);

	db->error = NULL;
}

/**
 * Inserts a provided array of headers into a Wren map at the specified 'slot'.
 *
 * Makes two assumptions:
 *   - There's a map to be written to at slot 0.
 *   - There are enough slots to write to.
 *
 * 'slots' gets incremented by the number of slots written to.
 */
static void wren_headers_to_map(WrenState *wren_state,
		const apr_array_header_t *headers, int *slot)
{
	WrenVM *vm = wren_state->vm;
	const apr_table_entry_t *e = (apr_table_entry_t*) headers->elts;

	for(size_t i = 0; i < headers->nelts; ++i) {
		wrenSetSlotString(vm, *slot, e[i].key);
		wrenSetSlotString(vm, *slot + 1, e[i].val);
		wrenInsertInMap(vm, 0, *slot, *slot + 1);

		*slot += 2;
	}
}

/**
 * Return the headers from the current request_rec as a map of key/value
 * pairs.
 */
static void wren_fn_getEnv(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	const apr_array_header_t *req_headers =
		apr_table_elts(wren_state->request_rec->headers_in);
	const apr_array_header_t *subprocess_env =
		apr_table_elts(wren_state->request_rec->subprocess_env);
	int slot = 0;

	/* Enough slots for a key value pair per headers, plus the map. */
	wrenEnsureSlots(vm, (req_headers->nelts + subprocess_env->nelts) * 2 + 1);
	wrenSetSlotNewMap(vm, slot++);

	wren_headers_to_map(wren_state, req_headers, &slot);
	wren_headers_to_map(wren_state, subprocess_env, &slot);
}

/**
 * Parse URL parameters and add them to a Wren map of key/value pairs, starting
 * at slot 0.
 *
 * TODO: Have multiple inputs with the same name be read as a table (currently
 * the last input overrides all of the prior ones).
 */
static void wren_parse_url_params(WrenState *wren_state, char *args)
{
	WrenVM *vm = wren_state->vm;
	request_rec *r = wren_state->request_rec;

	/* We'll increment the number of slots we expect as we parse arguments. */
	int num_slots = 1;
	int slot = 0;

	wrenSetSlotNewMap(vm, slot++);

	/* Count up the number of key value pairs. */
	{
		/* Assigned arguments come through as key1=val1&key2=val2.
		 * If an ampersand is to the right of an equals sign, it's
		 * an empty argument we don't have to worry about.
		 */
		char *ptr = args;
		while((ptr = strchr(ptr, '=')) != NULL) {
			++ptr;

			if(*ptr != '&' && *ptr != '\0')
				num_slots += 2;
		}
	}

	wrenEnsureSlots(vm, num_slots);

	/*
	 * Loop through each argument, separate the key/value pair, un-URIfy them,
	 * and push them to the Wren map.
	 */
	char *ptr = args;
	char *key, *val;
	while(*ptr && (val = ap_getword(r->pool, (const char**)&ptr, '&'))) {
		key = ap_getword(r->pool, (const char**)&val, '=');

		if(*val == '\0')
			continue;

		for(size_t i = 0; i < strlen(val); ++i) {
			if(val[i] == '+')
				val[i] = ' ';
		}

		ap_unescape_url((char*)key);
		ap_unescape_url((char*)val);

		wrenSetSlotString(vm, slot, key);
		wrenSetSlotString(vm, slot + 1, val);
		wrenInsertInMap(vm, 0, slot, slot + 1);

		slot += 2;
	}

}

/**
 * Read GET parameters and return them as a Wren map of key/value pairs.
 */
static void wren_fn_parseGet(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	char *args = wren_state->request_rec->args;

	if(args == NULL) {
		wrenSetSlotNull(vm, 0);
		return;
	}

	wren_parse_url_params(wren_state, args);
}

/**
 * Read POST parameters and return them as a Wren map of key/value pairs.
 */
static void wren_fn_parsePost(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;

	/* Check that we're okay to read our arguments. */
	if(ap_setup_client_block(r, REQUEST_CHUNKED_ERROR) != OK ||
			ap_should_client_block(r) == false)
	{
		wrenSetSlotNull(vm, 0);
		return;
	}

	/*
	 * We read through the POST arguments one HUGE_STRING_LEN at a time,
	 * appending the results to args_buf to end up with the complete list
	 * of POST arguments.
	 */
	char *args_buf;
	{
		char read_buf[HUGE_STRING_LEN];
		apr_off_t len = r->remaining;
		apr_off_t read_pos = 0;
		apr_off_t read_len;
		apr_off_t read_size;

		args_buf = apr_pcalloc(r->pool, len + 1);

		while((read_len = ap_get_client_block(r, read_buf, sizeof(read_buf))) > 0) {
			/* We should never exceed the read buffer length, but just in case. */
			read_size = read_pos + read_len > len ? len - read_pos : read_len;

			memcpy(args_buf + read_pos, read_buf, read_size);
			read_pos += read_size;
		}
	}

	wren_parse_url_params(wren_state, args_buf);
}

/**
 * Retrieve the cookie value for a provided key.
 *
 * Slot 1: Cookie name
 */
static void wren_fn_getCookie(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;

	const char *cookie_name = wrenGetSlotString(vm, 1);
	const char *data = apr_table_get(r->headers_in, "cookie");
	const char *pair;
	const char *key;
	const char *val;

	if(cookie_name == NULL || data == NULL) {
		wrenSetSlotNull(vm, 0);
		return;
	}

	/* Search through all the cookies set until we find the one we want. */
	while(*data && (pair = ap_getword(r->pool, &data, ';'))) {
		val = ap_getword(r->pool, (const char**)&pair, ';');
		key = ap_getword(r->pool, (const char**)&val, '=');

		/* Convert encoded to the expected readable form. */
		while(*key && (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n'))
			key++;

		if(strcmp(key, cookie_name) == 0) {
			wrenSetSlotString(vm, 0, val);
			return;
		}
	}
}

/**
 * Set a cookie.
 *
 * Slot 1: Cookie name (string)
 * Slot 2: Cookie value (string)
 * Slot 3: Expiration time in seconds (double)
 * Slot 4: Cookie path (string)
 */
static void wren_fn_setCookie(WrenVM *vm)
{
	WrenState *wren_state = wrenGetUserData(vm);
	request_rec *r = wren_state->request_rec;

	const char *cookie;
	const char *name  = wrenGetSlotString(vm, 1);
	const char *value = wrenGetSlotString(vm, 2);
	int expires       = wrenGetSlotDouble(vm, 3);
	const char *path  = wrenGetSlotString(vm, 4);

	if(expires > 0) {
		cookie = apr_psprintf(r->pool, "%s=%s; Max-Age=%u;Path=%s;",
				name, value, expires, path);
	}
	else {
		cookie = apr_psprintf(r->pool, "%s=%s; Path=%s;",
				name, value, path);
	}

	apr_table_set(r->headers_out, "Set-Cookie", cookie);
}

/**
 * Maps foreign method signatures to functions. We receive a signature of a
 * function inside a class, inside a module, and see if we have something that
 * maps up.
 *
 * We currently only bind things inside the "main" module.
 */
static WrenForeignMethodFn wren_bind_foreign_method(WrenVM *vm,
		const char *module, const char *class_name, bool is_static,
		const char *signature)
{
	if(strcmp(module, "main") == 0) {
		if(strcmp(class_name, "Web") == 0) {
			if(is_static == true) {
				if(strcmp(signature, "getCookie(_)") == 0)
					return wren_fn_getCookie;
				if(strcmp(signature, "setCookie(_,_,_,_)") == 0)
					return wren_fn_setCookie;

				if(strcmp(signature, "wrapped_getEnv()") == 0)
					return wren_fn_getEnv;
				if(strcmp(signature, "wrapped_parseGet()") == 0)
					return wren_fn_parseGet;
				if(strcmp(signature, "wrapped_parsePost()") == 0)
					return wren_fn_parsePost;

			}
		}

		if(strcmp(class_name, "WebDB") == 0) {
			if(is_static == false) {
				if(strcmp(signature, "init open(_)") == 0)
					return wren_foreign_webdb_open;
				if(strcmp(signature, "close()") == 0)
					return wren_foreign_webdb_close;
				if(strcmp(signature, "run(_)") == 0)
					return wren_foreign_webdb_run;
				if(strcmp(signature, "error") == 0)
					return wren_foreign_webdb_error;
				if(strcmp(signature, "clearError()") == 0)
					return wren_foreign_webdb_clearError;
				if(strcmp(signature, "wrapped_query(_)") == 0)
					return wren_foreign_webdb_query;
			}
		}
	}

	ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
			"Failed to find foreign method '%s.%s'", class_name, signature);

	return NULL;
}

/**
 * Maps foreign classes to their related allocate and finalize functions.
 *
 * Allocate runs whenever a new instance of the object is created, before the
 * constructor.
 *
 * Finalize doesn't run when the object leaves scope. It gets called whenever
 * the garbage collection gets to it.
 */
static WrenForeignClassMethods wren_bind_foreign_class(WrenVM *vm,
		const char *module, const char *class_name)
{
	WrenForeignClassMethods ret;
	ret.allocate = NULL;
	ret.finalize = NULL;

	if(strcmp(module, "main") == 0) {
		if(strcmp(class_name, "WebDB") == 0) {
			ret.allocate = wren_foreign_dbd_allocate;
			ret.finalize = wren_foreign_dbd_finalize;
		}
	}

	return ret;
}

static void module_init(apr_pool_t *pool, server_rec *s)
{
	ap_log_error("mod_wren.c", __LINE__, 1, APLOG_NOTICE, -1, NULL,
			"Initialising mod_wren");

	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.writeFn = wren_write;
	config.errorFn = wren_err;
	config.bindForeignMethodFn = wren_bind_foreign_method;
	config.bindForeignClassFn  = wren_bind_foreign_class;

	wren_states = calloc(NUM_WREN_STATES, sizeof(WrenState));

	for(size_t i = 0; i < NUM_WREN_STATES; ++i) {
		wren_states[i].vm = wrenNewVM(&config);
		wrenSetUserData(wren_states[i].vm, &wren_states[i]);

		/*
		 * Declare foreign methods as the first thing the VM runs so that
		 * they're available for all page loads.
		 *
		 * TODO: Wren has a bug with the foreign method API where methods where
		 * for methods that return a list, the list comes out as a Num type
		 * until something else has been run. To work around this we have
		 * foreign methods returning lists act as a wrapper for the actual
		 * functionality, plus a print before the return.
		 */
		wrenInterpret(wren_states[i].vm,
				"class Web {\n"
				"	foreign static getCookie(a)\n"
				"	foreign static setCookie(a,b,c,d)\n"
				"	foreign static wrapped_getEnv()\n"
				"	foreign static wrapped_parseGet()\n"
				"	foreign static wrapped_parsePost()\n"

				"	static getEnv() {\n"
				"		var ret = Web.wrapped_getEnv()\n"
				"		System.write(\"\")\n"
				"		return ret\n"
				"	}\n"
				"	static parseGet() {\n"
				"		var ret = Web.wrapped_parseGet()\n"
				"		System.write(\"\")\n"
				"		return ret\n"
				"	}\n"
				"	static parsePost() {\n"
				"		var ret = Web.wrapped_parsePost()\n"
				"		System.write(\"\")\n"
				"		return ret\n"
				"	}\n"
				"}\n"
				"\n"

				"foreign class WebDB {\n"
				"	foreign construct open(a)\n"
				"	foreign close()\n"
				"	foreign run(a)\n"
				"	foreign error\n"
				"	foreign clearError()\n"

				"	foreign wrapped_query(a)\n"
				"	query(q) {\n"
				"		var ret = this.wrapped_query(q)\n"
				"		System.write(\"\")\n"
				"		return ret\n"
				"	}\n"
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
	/*
	 * Forces cleanup of all foreign classes, which means all our hanging
	 * database connections will get closed.
	 */
	wrenCollectGarbage(wren_state->vm);

	wren_state->request_rec = NULL;
	wren_state->lock = false;
}

/**
 * Resizes the given wren_buf to be larger than the current capacity.
 *
 * Hopefully PARSE_BUFFER_GROWTH_RATE is sensible enough that the number of
 * times this happens is kept to a minimum.
 */
static void parse_grow_buffer(char **wren_buf, size_t *wren_index,
		size_t *capacity)
{
	size_t new_capacity = *capacity * PARSE_BUFFER_GROWTH_RATE + 1;
	char *new_buf = calloc(new_capacity, 1);

	strncpy(new_buf, *wren_buf, *wren_index);

	free(*wren_buf);
	*wren_buf = new_buf;
	*capacity = new_capacity;
}

/**
 * Write the given write_str to the wren_buf, resizing if necessary and pushing
 * wren_index along.
 */
static void parse_write(char **wren_buf, size_t *wren_index, size_t *capacity,
		const char *write_str)
{
	size_t len = strlen(write_str);

	while(*wren_index + len >= *capacity)
		parse_grow_buffer(wren_buf, wren_index, capacity);

	strncpy(*wren_buf + *wren_index, write_str, len);
	*wren_index += len;
}

/**
 * Copy write_len from buf to wren_buf, resizing wren_buf if necessary and
 * pushing wren_index and buf_index along.
 */
static void parse_write_from_buf(char **wren_buf, size_t *wren_index,
		size_t *capacity, const char *buf, size_t *buf_index, size_t write_len)
{
	while(*wren_index + write_len >= *capacity)
		parse_grow_buffer(wren_buf, wren_index, capacity);

	strncpy(*wren_buf + *wren_index, buf + *buf_index, write_len);
	*buf_index += write_len;
	*wren_index += write_len;
}

/**
 * Write an HTML block inside the file_buf, starting from file_index,
 * to the output wren_buf, starting at wren_index.
 *
 * file_index will get progressed by html_len, the amount of raw HTML
 * within the file. wren_index will get progressed a bit more, html_len plus
 * extra characters for function calls and escaping.
 */
static void parse_write_html(char **wren_buf, size_t *wren_index,
		size_t *wren_capacity, const char *file_buf, size_t *file_index,
		size_t html_len)
{
	if(html_len == 0 || (html_len == 1 && *(file_buf + *file_index) == '\n'))
		return;

	/* Start write */
	parse_write(wren_buf, wren_index, wren_capacity, "\nSystem.write(\"");

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

		parse_write_from_buf(wren_buf, wren_index, wren_capacity, file_buf,
				file_index, segment_len);

		parse_write(wren_buf, wren_index, wren_capacity, "\\");
		html_len -= segment_len;

		next_quote   = strchr(file_buf + *file_index + 1, '"')  ?: (char*)INTPTR_MAX;
		next_percent = strchr(file_buf + *file_index + 1, '%')  ?: (char*)INTPTR_MAX;
		next_slash   = strchr(file_buf + *file_index + 1, '\\') ?: (char*)INTPTR_MAX;
	} while(html_len > 0);

	parse_write_from_buf(wren_buf, wren_index, wren_capacity, file_buf,
			file_index, html_len);

	/* Close up the System.write. */
	parse_write(wren_buf, wren_index, wren_capacity, "\")\n");
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
 *
 * Returns OK with wren_code allocated on success, otherwise a failing HTTP
 * code with no allocation.
 */
static int wren_parse(WrenState *wren_state, char **wren_code, bool raw)
{
	/* Open up a file and write it to a buffer we can work from. */
	FILE *file = fopen(wren_state->request_rec->canonical_filename, "r");
	char *file_buf, *out_buf;
	size_t file_len, read_len;

	if(file == NULL)
		return errno == ENOENT ? HTTP_NOT_FOUND : HTTP_INTERNAL_SERVER_ERROR;

	fseek(file, 0, SEEK_END);
	file_len = ftell(file) ?: 1;
	fseek(file, 0, SEEK_SET);

	if(file_len == 0) {
		*wren_code = strdup("");
		return OK;
	}

	/*
	 * We're making this +4 larger than needed so we can wrap it in curly braces
	 * if we return it raw, plus one for NUL.
	 */
	file_buf = calloc(file_len + 5, 1);
	read_len = fread(file_buf + 2, 1, file_len, file);
	fclose(file);

	if(read_len != file_len) {
		free(file_buf);
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	/*
	 * We want to accept the whole file as Wren without parsing, so we wrap it
	 * in its own scope and send it on its way.
	 */
	if(raw == true) {
		file_buf[0] = '{';
		file_buf[1] = '\n';
		file_buf[file_len + 1] = '\n';
		file_buf[file_len + 2] = '}';

		*wren_code = file_buf;
		return OK;
	}

	/*
	 * Allocate another buffer for the output Wren code. This needs to be
	 * bigger than the input to account for the function calls and escaping
	 * we add.
	 */
	size_t file_index = 2;
	size_t out_index = 0;
	size_t out_capacity = MIN(128, file_len * PARSE_BUFFER_GROWTH_RATE);

	out_buf = calloc(out_capacity, 1);
	parse_write(&out_buf, &out_index, &out_capacity, "{\n");

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
			parse_write_html(&out_buf, &out_index, &out_capacity, file_buf,
					&file_index, html_len);
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
			parse_write_html(&out_buf, &out_index, &out_capacity, file_buf,
					&file_index, html_len);
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
			parse_write(&out_buf, &out_index, &out_capacity,
					"\nSystem.write(\"%(");
		}

		/*
		 * Write out whatever we found in our Wren tags and push the file read
		 * index along to the end of the closing tag.
		 */
		size_t write_len = closing_tag_pos - (file_buf + file_index);
		parse_write_from_buf(&out_buf, &out_index, &out_capacity, file_buf,
				&file_index, write_len);

		if(expr == true)
			parse_write(&out_buf, &out_index, &out_capacity, ")\")\n");

		file_index += closing_tag_len;
	}

	parse_write(&out_buf, &out_index, &out_capacity, "\n}");

	free(file_buf);
	*wren_code = out_buf;
	return OK;
}

/**
 * Main Wren handler that gets hooked when we call a Wren file, and converts
 * the file to something that can be understood by the WrenVM and runs it.
 */
static int wren_handler(request_rec *r)
{
	WrenState *wren_state;
	char *wren_code = NULL;
	int ret = OK;

	/*
	 * If the file extension is .wren, we'll use the raw file as Wren code.
	 * Otherwise we'll parse it as mixed HTML/Wren.
	 */
	bool raw_wren = ({
		const char *file_extension = strrchr(r->canonical_filename, '.');
		strcmp(file_extension ?: "", ".wren") == 0;
	});

	/* Make sure the request is for us. */
	if(strcmp(r->handler ?: "", "wren") != 0)
		return DECLINED;

	/* TODO: allow other methods (most critically, POST). */
	if(r->method_number != M_GET &&
			r->method_number != M_POST)
		return HTTP_METHOD_NOT_ALLOWED;

	wren_state = wren_acquire_state(r);

	if((ret = wren_parse(wren_state, &wren_code, raw_wren)) != OK) {
		wren_release_state(wren_state);
		return ret;
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
