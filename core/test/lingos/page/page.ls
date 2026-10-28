// page - for the tests: a lingo that answers web requests at its route, /hello, with serve(request), shared with anyone.

// Every request to /hello, answered by its path: text, JSON, a status and headers of its own, or a failure.
fn serve(request) {
  if request.path == "/text" {return "hi " + (request.query.name ?? "there")}
  if request.path == "/json" {return {got: request.json, method: request.method, type: request.headers["content-type"]}}
  if request.path == "/status" {return {status: 201, headers: {"X-Made": "page"}, body: "made"}}
  if request.path == "/fail" {fail "it broke"}
  return {status: 404, body: {error: "no page at " + request.path}}
}
