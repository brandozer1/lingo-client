// request.ls - Web requests for a program, like fetch: GET, POST, PUT, PATCH and DELETE to a web address or an API,
// with headers, a query, a JSON or text body, a timeout, and retries when a service is busy or down. Gives the status,
// the headers, the text and the JSON read from it.
//
// 1. send(method, address, options=nothing)        -> response: options {headers, query, json, text, timeout, retries}
// 2. get(address, options=nothing)                 -> response
// 3. post(address, body=nothing, options=nothing)  -> response: a record or list body as JSON, anything else as text
// 4. put(address, body=nothing, options=nothing)   -> response
// 5. patch(address, body=nothing, options=nothing) -> response
// 6. delete(address, options=nothing)              -> response
// response = {status, ok, headers, text, json}: ok for 200 to 299, json the text read as JSON when the answer says it is

////////// 1-6. REQUESTS //////////

// 1. A web request with any method to an address, and its response: {status, ok, headers, text, json}, ok for 200 to
// 299, json the text read as JSON when the answer says it is. Every answer comes back, a 404 too, so check ok. method
// is GET, POST, PUT, PATCH, DELETE or any other. options are headers (a record), query (a record), json or text (the
// body), timeout (seconds, 30 when left out) and retries (how many more tries after a 429, a 5xx or a network that
// failed, none when left out, each after the answer's Retry-After or twice the last wait, from half a second).
fn send(method, address, options=nothing) {
  o = options ?? {}
  headers = o.headers ?? {}
  body = nothing
  if o.json != nothing {
    body = std.to_json(o.json)
    if not has(headers, "content-type") {headers["Content-Type"] = "application/json"}
  } else if o.text != nothing {body = std.text(o.text)}
  if o.query != nothing {address = std.text(address) + ("?" in address ? "&" : "?") + url.to_query(o.query)}
  retries = o.retries ?? 0
  wait = 0.5
  for turn in 0..retries {
    r = nothing
    why = nothing
    try {r = http.send(method, address, headers, body, o.timeout ?? 30)} catch e {why = e}
    if r != nothing and r.status != 429 and r.status < 500 or turn == retries {
      if r == nothing {fail why}
      return answer(r)
    }
    after = r == nothing ? nothing : std.number(r.headers["retry-after"] ?? "")
    compute.wait(after ?? wait)
    wait *= 2
  }
}

// 2. A GET request to a web address or an API, and its response: {status, ok, headers, text, json}. options as send
// takes them: headers, query, timeout and retries.
fn get(address, options=nothing) {return send("GET", address, options)}

// 3. A POST request to a web address or an API, a record or list body sent as JSON and anything else as text, and its
// response. options as send takes them.
fn post(address, body=nothing, options=nothing) {return send("POST", address, holding(options, body))}

// 4. A PUT request to a web address or an API, a record or list body sent as JSON and anything else as text, and its
// response. options as send takes them.
fn put(address, body=nothing, options=nothing) {return send("PUT", address, holding(options, body))}

// 5. A PATCH request to a web address or an API, a record or list body sent as JSON and anything else as text, and its
// response. options as send takes them.
fn patch(address, body=nothing, options=nothing) {return send("PATCH", address, holding(options, body))}

// 6. A DELETE request to a web address or an API, and its response. options as send takes them.
fn delete(address, options=nothing) {return send("DELETE", address, options)}

////////// HELPERS //////////

// What http.send gave, as a response. For request's own use.
fn answer(r) {
  json = "json" in (r.headers["content-type"] ?? "") ? json.parse(r.text) : nothing
  return {status: r.status, ok: r.status >= 200 and r.status < 300, headers: r.headers, text: r.text, json: json}
}

// The options with the body in them: as json for a record or a list, as text for anything else. For request's own use.
fn holding(options, body) {
  o = options ?? {}
  if body == nothing {return o}
  k = json.kind(body)
  if k == "record" or k == "list" {o.json = body} else {o.text = body}
  return o
}

// Whether the headers have that one, whatever its case. For request's own use.
fn has(headers, name) {
  for k in headers {if text.lower(k) == name {return true}}
  return false
}
