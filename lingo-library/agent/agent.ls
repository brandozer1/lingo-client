// agent.ls - The library's AI, on your own key: Claude, ChatGPT, OpenRouter, or any service that answers the way
// OpenAI does, like Ollama on your own computer. It talks with the person and uses any lingo it needs on the way, or
// gives a program one answer.
//
// 1. ask(prompt, chat=nothing, model=nothing) -> text: its reply, after at most 8 turns. chat is the turns before the
//                                  prompt, each {from: you or onsay, text}, and model any model id of the service it uses
// 2. answer(prompt, model=nothing)            -> text: one model's answer to one prompt, for a program
// The model ask talks to has two tools: lingo(task, values), which finds a lingo that does a task and runs it, and
// use(address, values), anything named by its address. Each tool's answer, or what went wrong, goes back to the model
// until it replies.
//
// Its settings, claude_key, openai_key, openrouter_key, url, service and model, are said in openapi.json beside it and
// kept on this computer: set them in the Onsay app's Lingos settings, or with the command line's set. One key is enough.

////////// 1-2. ASKING //////////

// 1. The AI agent answers what the person just said, reading the conversation so far. Use it for anything said to Onsay
// in a chat: greetings, small talk, questions, questions about the conversation like what did I just say, and tasks
// that need other lingos. Not for a program that only needs one answer from an AI model. prompt is what the person just
// said, in their own words. chat is the conversation before it, oldest first: a list of {from, text}, from being you or
// onsay. Give it whenever there is one. model is a model id of the service it uses, the one in its settings when left out.
fn ask(prompt, chat=nothing, model=nothing) {
  ai = service(model)
  said = []
  for t in chat ?? [] {said += [{role: t.from == "you" ? "user" : "assistant", content: t.text}]}
  said += [{role: "user", content: prompt}]
  turns = 8
  for turn in 1..turns {
    m = reply(ai, said, true)
    if std.length(m.calls) == 0 {return m.text}
    said += [m.said]
    results = []
    for c in m.calls {results += [{id: c.id, text: used(c.name, c.args)}]}
    said += answered(ai, results)
  }
  return "I took {turns} turns and still hadn't finished, so I stopped. Try asking for less at once."
}

// 2. One AI model's answer to one prompt, as text, for a program that needs a model to answer, write, summarize or
// translate. It sees only this prompt: no chat, no memory and no lingos. Not for chatting with the person: ask does
// that. prompt is the whole question or task, with everything the model needs to know. model is a model id of the
// service it uses, the one in its settings when left out.
fn answer(prompt, model=nothing) {
  return reply(service(model), [{role: "user", content: prompt}], false).text
}

////////// HELPERS //////////

// Which AI it talks to, from its settings: {kind, name, key, url, model}, kind claude or openai, the way it is spoken
// to. For the agent's own use.
fn service(model) {
  claude = given(setting("claude_key"))
  openai = given(setting("openai_key"))
  router = given(setting("openrouter_key"))
  url = given(setting("url"))
  pick = given(setting("service"))
  if pick == nothing {
    if claude != nothing {
      pick = "claude"
    } else if openai != nothing or url != nothing {
      pick = "chatgpt"
    } else if router != nothing {
      pick = "openrouter"
    }
  }
  model = model ?? given(setting("model"))
  if pick == "claude" {
    if claude == nothing {fail "Claude needs your API key: set claude_key in agent's settings, in the Onsay app or with the command line's set. Get one at console.anthropic.com"}
    return {kind: "claude", name: "Claude", key: claude, url: "https://api.anthropic.com/v1/messages", model: model ?? "claude-opus-5-5"}
  }
  if pick == "chatgpt" {
    if openai == nothing and url == nothing {fail "ChatGPT needs your API key: set openai_key in agent's settings, in the Onsay app or with the command line's set. Get one at platform.openai.com"}
    return {kind: "openai", name: url == nothing ? "ChatGPT" : url, key: openai, url: (url ?? "https://api.openai.com/v1") + "/chat/completions", model: model ?? "gpt-5-mini"}
  }
  if pick == "openrouter" {
    if router == nothing {fail "OpenRouter needs your key: set openrouter_key in agent's settings, in the Onsay app or with the command line's set. Get one at openrouter.ai/keys"}
    return {kind: "openai", name: "OpenRouter", key: router, url: "https://openrouter.ai/api/v1/chat/completions", model: model ?? "openai/gpt-5-mini"}
  }
  fail "I need a key for an AI to answer: set claude_key, openai_key or openrouter_key in agent's settings, in the Onsay app or with the command line's set"
}

// A setting, or nothing when it is left empty. For the agent's own use.
fn given(x) {
  return x == "" ? nothing : x
}

// The model's next message: {text, calls, said}, calls each {id, name, args} and said the message to keep in the
// conversation, as that service writes it. With tools, the agent's guide and tools go with it. For the agent's own use.
fn reply(ai, said, tools) {
  headers = {}
  headers["Content-Type"] = "application/json"
  if ai.kind == "claude" {
    headers["x-api-key"] = ai.key
    headers["anthropic-version"] = "2023-06-01"
    headers["anthropic-beta"] = "server-side-fallback-2026-07-01"
    body = {model: ai.model, max_tokens: 16000, messages: said, fallbacks: "default"}
    if tools {
      body.system = guide()
      body.tools = described("claude")
    }
    a = heard(ai, http.send("POST", ai.url, headers, std.to_json(body), 120))
    if a.stop_reason == "refusal" {return {text: "Claude declined to help with that one.", calls: [], said: nothing}}
    text = ""
    calls = []
    for b in a.content {
      if b.type == "text" {text += b.text}
      if b.type == "tool_use" {calls += [{id: b.id, name: b.name, args: b.input ?? {}}]}
    }
    return {text: text, calls: calls, said: {role: "assistant", content: a.content}}
  }
  if ai.key != nothing {headers["Authorization"] = "Bearer {ai.key}"}
  body = {model: ai.model, messages: said}
  if tools {
    body.messages = [{role: "system", content: guide()}] + said
    body.tools = described("openai")
  }
  m = heard(ai, http.send("POST", ai.url, headers, std.to_json(body), 120)).choices[0].message
  calls = []
  for c in m.tool_calls ?? [] {
    args = {}
    try {args = std.from_json(c.function.arguments) ?? {}} catch e {args = {}}
    calls += [{id: c.id, name: c.function.name, args: args}]
  }
  return {text: m.content ?? "", calls: calls, said: {role: "assistant", content: m.content, tool_calls: m.tool_calls}}
}

// A web answer from the AI, read as JSON, or what went wrong in plain words. For the agent's own use.
fn heard(ai, r) {
  if r.status == 401 or r.status == 403 {fail "{ai.name} turned the key down: check it in agent's settings, in the Onsay app or with the command line's set"}
  if r.status < 200 or r.status > 299 {fail "{ai.name} answered {r.status}: {r.text}"}
  return std.from_json(r.text)
}

// What the tools gave, as the messages that service reads next. For the agent's own use.
fn answered(ai, results) {
  if ai.kind == "claude" {
    blocks = []
    for x in results {blocks += [{type: "tool_result", tool_use_id: x.id, content: x.text}]}
    return [{role: "user", content: blocks}]
  }
  out = []
  for x in results {out += [{role: "tool", tool_call_id: x.id, content: x.text}]}
  return out
}

// What the agent's model is told it is, and what its tools are for. For the agent's own use.
fn guide() {
  return "You are Onsay's AI agent, and the person talks to you in Onsay's chat. You run as a lingo, an app that lets " +
    "Onsay work with one of their computers. Answer plainly and truthfully. The messages before this one are your " +
    "conversation with the person so far: use them, and quote them exactly when asked what was said. You have two tools. " +
    "lingo(task, values) finds a lingo that does the task, runs it and gives back only its answer. Use it for anything you " +
    "can't know or do yourself, like the time now, the weather, a web page, or something on the person's computers, " +
    "instead of saying you can't: say the task in plain words with every detail it needs, and give any values it needs by " +
    "name in values, like city for the weather. use(address, values) reaches what an address names and gives back its " +
    "answer: a lingo's function, like laptop.brandon/lingos/text.lingo/slug with the value text, or a web address, when " +
    "the person or an answer names one. A tool that fails says why: try another way, or tell the person. When you have " +
    "what you need, reply."
}

// The agent's tools, in the format kind reads: claude or openai. The first value each takes is the one it needs. For the
// agent's own use.
fn described(kind) {
  all = [
    {name: "lingo", does: "Finds a lingo that does a task, runs it, and gives back its answer. For anything you can't know or do yourself", takes: {
      task: {type: "string", description: "What to do, in plain words, with every detail it needs"},
      values: {type: "object", description: "The values it needs, by name, like city for the weather, or none"},
    }},
    {name: "use", does: "Reaches what an address names and gives back its answer: a lingo's function, like laptop.brandon/lingos/text.lingo/slug, or a web address", takes: {
      address: {type: "string", description: "What to reach: a computer, then the lingo's file and its function, like laptop.brandon/lingos/text.lingo/slug, or a web address"},
      values: {type: "object", description: "Its values by name, like text for text.lingo/slug, or none"},
    }},
  ]
  out = []
  for t in all {
    schema = {type: "object", properties: t.takes, required: [std.keys(t.takes)[0]]}
    if kind == "claude" {
      out += [{name: t.name, description: t.does, input_schema: schema}]
    } else {
      out += [{type: "function", function: {name: t.name, description: t.does, parameters: schema}}]
    }
  }
  return out
}

// What a tool the model asked for gave, as text, or what went wrong, in plain words. It never reaches itself: its lingo
// tool never picks it, since a lingo(...) never picks the function asking, and its use tool refuses an address of it.
fn used(name, a) {
  try {
    if name == "lingo" {
      got = lingo(a.task, a.values)
    } else if name == "use" and "/agent.lingo" in std.text(a.address) {
      return "That is this agent, which would only ask itself again: answer, or use another lingo"
    } else if name == "use" {
      got = use(a.address, a.values)
    } else {
      return "There's no tool called {name}: there are lingo and use"
    }
    return std.text(got ?? "It gave nothing back")
  } catch e {
    return "That didn't work: {e}"
  }
}
