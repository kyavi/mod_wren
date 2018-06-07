# mod_wren

mod_wren is an Apache server module which embeds the scripting language
[Wren](https://wren.io) ([GitHub](https://github.com/munificent/wren)). The
module supports all of Wren's usual features, along with a small set of
functions for standard server-side programming such as parsing GET/POST
requests, getting and setting cookies, and database interaction.

## Writing Wren pages

mod_wren allows Wren code and expressions to be interleaved with HTML on pages
with the extension **.wrp**:

```xml
<!DOCTYPE html><?wren

import "random" for Random

var random = Random.new()
?>
<html>
	<body>
		<h1>Random numbers</h1>
		<div>Your random number: <%= random.float() %></div>
	</body>
</html>
```

Files with the standard **.wren** extension can run plain Wren code:

```javascript
/**
 * Read a number from the GET parameter 'count' and return a JSON array of
 * that many random floats.
 */
import "random" for Random

Web.setContentType("text/json")
Web.setStatusCode(200) /* 200 is the default. */

var numbers = []
var random = Random.new()
var getParams = Web.parseGet() || {}
var count = Num.fromString(getParams["count"] || "") || 0

for(x in 0 ... count) numbers.add("\"%(x)\":%(random.float())")
System.write("{" + numbers.join(",") + "}")
```

## Building

mod_wren is currently only supported on Linux, tested on Debian 9 (Stretch),
and depends on having
[mod_dbd](https://httpd.apache.org/docs/2.4/mod/mod_dbd.html) enabled. To
build and install:

```bash
git clone https://github.com/azufluup/mod_wren
cd mod_wren
make
make install # Requires privileges in /etc/apache2
```

Building requires an internet connection to clone the
[Wren repo](https://github.com/munificent/wren). 

## Running

Add the following lines to your Apache configuration (e.g.
``/etc/apache2/apache2.conf``) to tell mod_wren to handle **.wren** and
**.wrp** files:

```apache
LoadModule wren_module modules/mod_wren.so

<FilesMatch "\..*(wren|wrp)$">
	SetHandler wren
</Files>
```

If you want ``index.wrp`` files to be considered as potential resources for
directory indexes, you can also add:

```apache
DirectoryIndex index.wrp index.html
```

## Error reporting

Any errors in your Wren program will display as on the page, indicating the
problematic module and line number. This can be disabled in your Apache config
with the directive ``ModWrenErrors 0``.

Wren makes [meaningful use of newlines](http://wren.io/syntax.html#newlines) to
close statements. mod_wren follows suit to keep line numbers reported in error
messages synchronised with source code. For this to work, Wren opening tags
should be followed by a newline, and closing tags placed on their own line:

```xml
<h1>Correct</h1><?wren
	System.write("These tags are opening and closing the Wren block correctly.")
?>
<p>That's how it's done!</p>
```

## Classes and Modules

mod_wren supplies
[new classes](https://github.com/kyavi/mod_wren/docs/classes.md) for
interacting with the server.

As with regular Wren, you can import other modules into your current document.

``import "modules/something"`` will try to load ``modules/something.wren``
relative to the directory of your current document.

``import "/modules/something"`` will try to load from your web server root
(e.g. ``/var/www/html/modules/something``).
