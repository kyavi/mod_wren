# mod_wren classes

mod_wren adds two new Wren classes included with every page:

* **Web**, containing static functions to interact with the server.
* **WebDB**, to create and query a database connection with mod_dbd

## Web

### static getCookie(key: String)

Retrieve the value of a browser cookie. Returns Null if the cookie is not set.

```javascript
var visited = Web.getCookie("testCookie") || "not found"
System.write("<div>Test cookie: %(visited)</div>")
```

### static getEnv()

Retrieve Apache headers and environment variables as a Map of key/value strings.

```javascript
var env = Web.getEnv()

for (x in env.keys) {
	System.write("<div><b>%(x):</b> %(env[x])</div>")
}
```

### static parseGet()

Returns any GET parameters as a Map of key/value pairs, or an empty table if
there's nothing to parse.

The value of each key/value pair will be a string unless multiple parameters
use the same key, in which case it will be a list of strings.

```javascript
/* Page request /?testing=true&dup=true&dup=true */

var env = Web.getEnv()

if (env["Request-Method"] == "GET") {
	var getParams = Web.parseGet()
	System.write(getParams["testing"].type) /* String */
	System.write(getParams["dup"].type)     /* List */
	System.write(getParams["notsent"].type) /* Null*/
}
```

### static parsePost()

Returns any POST parameters as a Map of key/value strings, or an empty table if
there's nothing to parse. Only returns values on the first call.

The value of each key/value pair will be a string unless multiple parameters
use the same key, in which case it will be a list of strings.

```javascript
var env = Web.getEnv()

if (env["Request-Method"] == "POST") {
	var postParams = Web.parsePost()
	var username = postParams["username"]

	System.write("<div>Username: %(username)</div>")

	var postParamsAgain = Web.parsePost() /* Second call is always {} */
}
```

### static setContentType(type: String)

Sets the content type for the current document. Pages return as ``text/html``
by default.

```javascript
Web.setContentType('text/javascript')
System.write("{}")
```

### static setCookie(key: String, value: String, expiration: Num, path: String)

If the browser allows cookies, set a cookie key/value. Expiration time is in
seconds, or can be set to 0 for a session cookie. The cookie path can be used
to set the cookie for specific subdirectories. If unsure, set to '/'.

```javascript
Web.setCookie("sessionCookie", "value", 0, "/")
Web.setCookie("tempCookie", "value", 180, "/") /* Set for three minutes. */
```

### static setHeader(key: String, value: String)

Set a page header with the given key/value pair. 

```javascript
Web.setHeader("Location", "https://github.com/azufluup/mod_wren")
```

### static setReturnCode(code: Num)

Sets an
[HTTP status code](https://en.wikipedia.org/wiki/List_of_HTTP_status_codes)
as the page handler return value, throwing a server error. Set to 0 (OK) by
default, which returns the page normally. To set a return status without
throwing a server error, use ``Web.setStatusCode(key)``.

```javascript
Web.setReturnCode(404) /* Generate the server's 404 page. */
```

### static setStatusCode(key: Num)

Sets an
[HTTP status code](https://en.wikipedia.org/wiki/List_of_HTTP_status_codes),
indicating the page state after it has finished running, set to 200 (OK) by
default. To generate a server response, use ``Web.setReturnCode(key)``.

```javascript
Web.setStatusCode(404) /* Forbidden: maybe the user needs an account. */
```


## WebDB

### WebDB.open(db: String) constructor

Open a database through mod_dbd. The database type is set in your Apache
configuration.

```javascript
var db = WebDB.open("host=localhost,user=root")
```

### WebDB.close()

Close an open database connection. This will happen automatically when the
variable leaves scope.

```javascript
var db = WebDB.open("host=localhost,user=root")
db.close()
```

### WebDB.run(command: String)

Run a database command. Returns true on success, otherwise false. Sets
WebDB.error on failure.

```javascript
var db = WebDB.open("host=localhost,user=root")
db.run("use TestDatabase;")
```

### WebDB.query(query: String)

Queries a database and returns a list containing lists of database rows with
columns indexed to match the query, or Null if the query fails. Sets
WebDB.error on failure.

```javascript
var db = WebDB.open("host=localhost,user=root")
db.run("use PeopleDatabase;")

var table = db.query("select Name,Age,FavouriteFood from PersonTable;") || {}

for (row in table) {
	var name = row[0]
	var age  = row[1]
	var food = row[2]

	System.write("<div>%(name) is %(age) years old, and likes %(food)</div>")
}
```

### WebDB.escape(str: String)

Escape a given string to be used in a query for the mod_dbd database type.

```javascript
var db = WebDB.open("host=localhost,user=root")
db.run("use TestDatabase;")

var query_string = db.escape("This might be 'unsafe'")
db.query("select * from TestTable where Something='%(query_string)';")
```

### WebDB.error getter

Getter for the error message string of the last reported failure, or Null if
there have been no errors.

```javascript
var WebDB.open("host=localhost,user=root")
db.run("use NonexistentTable")
System.write("%(db.error || "Everything is fine")")
```

### WebDB.clearError()

Used to set the WebDB.error getter to Null.

It's **much** more preferable to check your errors as you go along and avoid
this.
