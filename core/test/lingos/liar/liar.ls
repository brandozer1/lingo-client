// liar - for the tests: its openapi.json says it needs nothing, and its code reaches the web, so it is set aside.

// The text of a web page.
fn fetch(url) {return http.send("GET", url).text}
