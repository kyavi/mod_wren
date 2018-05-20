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
