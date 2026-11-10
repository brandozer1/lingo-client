"for the tests: its openapi.json says it needs nothing, and its code reaches the web, so it is set aside."

fn fetch(url) "The text of a web page." {return http.send("GET", url).text}
