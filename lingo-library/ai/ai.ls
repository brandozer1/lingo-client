"The person's AI, for every lingo: Onsay's own AI, there the moment the computer is signed in, or their own key for Claude, ChatGPT, Gemini, Grok, OpenRouter, or any service that answers the way OpenAI does, like Ollama on their own computer. It keeps the one key and sends each call to the service it is for, so agents and other lingos never handle keys or speak to a service themselves."
// 1. prompt(prompt, system=nothing, model=nothing) -> text: the AI's one answer, no tools and nothing kept
// 2. chat(messages, tools=nothing, model=nothing) -> {role, content, tool_calls?, thinking?}: the AI's next message for
//                                  a conversation, OpenAI's way, for a lingo running its own loop with tools of its own,
//                                  like an agent
//
// Messages and tools are always OpenAI's: {role: "system" | "user", content}, {role: "assistant", content, tool_calls:
// [{id, type: "function", function: {name, arguments}}]} and {role: "tool", tool_call_id, content}, and tools as
// [{type: "function", function: {name, description, parameters}}]. A message's content may be a list of parts, {type:
// "text", text} and {type: "image_url", image_url: {url: "data:image/jpeg;base64,..."}}, a picture for a model that
// takes one. Claude is spoken to in its own shape, made from these and back again as each message goes.
//
// Its settings, a key for each service, url, service and model, are said in openapi.json beside it and kept on this
// computer: set them in the Onsay app's settings, or with the command line's set. With no key it talks to Onsay's AI,
// free for 30 days, through use(...) of its address, which Onsay answers for the account this computer is signed in on
// and meters. One key of your own is enough to use that instead.

////////// 1. ASKING //////////

fn prompt(prompt, system=nothing, model=nothing) "One answer from an AI model to a prompt, as plain text: to answer, write, summarize, translate, classify or pull facts out of words, for a lingo or program that needs an AI model once. Not for a chat with the person, which an agent is for. system is what the model is told it is and how to answer, like answer with one word. It uses no tools and keeps nothing. model is a model id of the service it uses, the one in its settings when left out." {
  told = system == nothing ? [] : [{role: "system", content: system}]
  return chat(told + [{role: "user", content: prompt}], nothing, model).content
}

fn chat(messages, tools=nothing, model=nothing) "An AI model for a lingo's own agent: the model's next message for a conversation of chat messages in OpenAI's format, which may call the tools it is given. Use it as the AI model of a lingo that runs its own loop and its own tools, like an agent: send the conversation, run the tools it calls, add what they gave, and send it again until it answers with no tool calls. messages are OpenAI's chat messages, a system message first when there is one, and a message's content may hold a picture as an image_url part. tools are OpenAI's function tools, or nothing for none. It gives \{role: \"assistant\", content, tool_calls}, tool_calls only when it calls them, and thinking, which goes back unchanged on that message for Claude. model is a model id of the service it uses, the one in its settings when left out." {
  system = nothing
  said = []
  for m in messages ?? [] {
    if m.role == "system" {
      system = (system == nothing ? "" : system + "\n\n") + std.text(m.content)
    } else {
      said += [m]
    }
  }
  return _reply(_service(model), system, said, tools ?? [])
}

////////// HELPERS //////////

fn _services() "The services it can talk to with the person's own key, in the order it picks one when none is set: how it speaks to each, where it answers, the setting its key is in, its default model and where a key is made." {
  return {
    claude: {kind: "claude", name: "Claude", setting: "claude_key", url: "https://api.anthropic.com/v1", model: "claude-opus-5-5", keys: "console.anthropic.com"},
    chatgpt: {kind: "openai", name: "ChatGPT", setting: "openai_key", url: "https://api.openai.com/v1", model: "gpt-5-mini", keys: "platform.openai.com"},
    gemini: {kind: "openai", name: "Gemini", setting: "gemini_key", url: "https://generativelanguage.googleapis.com/v1beta/openai", model: "gemini-2.5-flash", keys: "aistudio.google.com"},
    grok: {kind: "openai", name: "Grok", setting: "grok_key", url: "https://api.x.ai/v1", model: "grok-4", keys: "console.x.ai"},
    openrouter: {kind: "openai", name: "OpenRouter", setting: "openrouter_key", url: "https://openrouter.ai/api/v1", model: "openai/gpt-5-mini", keys: "openrouter.ai/keys"}
  }
}

fn _service(model) "Which AI it talks to, from its settings: \{kind, name, key, url, model}, kind claude, openai or onsay, the way it is spoken to. The service set wins, else the first with a key, else Onsay's AI. url, when set, is where ChatGPT, or Claude with service claude, is reached instead, like Ollama's." {
  keys = {
    claude: _given(setting("claude_key")),
    chatgpt: _given(setting("openai_key")),
    gemini: _given(setting("gemini_key")),
    grok: _given(setting("grok_key")),
    openrouter: _given(setting("openrouter_key"))
  }
  url = _given(setting("url"))
  pick = _given(setting("service"))
  if pick == nothing and url != nothing {pick = "chatgpt"}
  for name in std.keys(_services()) {
    if pick == nothing and keys[name] != nothing {pick = name}
  }
  pick = pick ?? "onsay"
  model = model ?? _given(setting("model"))
  if pick == "onsay" {return {kind: "onsay", name: "Onsay's AI", key: nothing, url: "https://api.onsay.dev/ai/chat", model: model}}
  s = _services()[pick]
  if s == nothing {fail "service is onsay, claude, chatgpt, gemini, grok or openrouter: set it in ai's settings, in the Onsay app or with the command line's set"}
  reached = (pick == "claude" or pick == "chatgpt") ? url : nothing
  if keys[pick] == nothing and reached == nothing {fail "{s.name} needs your API key: set {s.setting} in ai's settings, in the Onsay app. Get one at {s.keys}"}
  ending = s.kind == "claude" ? "/messages" : "/chat/completions"
  return {kind: s.kind, name: reached ?? s.name, key: keys[pick], url: (reached ?? s.url) + ending, model: model ?? s.model}
}

fn _given(x) "A setting, or nothing when it is left empty." {
  return x == "" ? nothing : x
}

fn _reply(ai, system, said, tools) "The model's next message, as OpenAI writes it: \{role: \"assistant\", content, tool_calls}, tool_calls only when it uses tools. Onsay's AI is asked with use(...) of its address, which Onsay answers for this computer's account in the same shape. Claude reads the conversation in its own shape: a tool's result is the person's turn, turns of the same side are one, a picture is an image block, and its own thinking goes back to it unchanged while the turn runs, kept as thinking on its message." {
  told = system == nothing ? [] : [{role: "system", content: system}]
  if ai.kind == "onsay" {
    body = {messages: told + said}
    if std.length(tools) > 0 {body.tools = tools}
    if ai.model != nothing {body.model = ai.model}
    try {a = use(ai.url, body)} catch e {
      if "connected" in std.text(e) {fail "Sign in to Onsay to use its AI, or add your own key in ai's settings"}
      fail "{e}"
    }
    return _heard_openai(a)
  }
  headers = {}
  headers["Content-Type"] = "application/json"
  if ai.kind == "claude" {
    headers["x-api-key"] = ai.key
    headers["anthropic-version"] = "2023-06-01"
    headers["anthropic-beta"] = "server-side-fallback-2026-07-01"
    body = {model: ai.model, max_tokens: 16000, messages: _claude_turns(said), fallbacks: "default"}
    if system != nothing {body.system = system}
    if std.length(tools) > 0 {body.tools = _claude_tools(tools)}
    a = _heard(ai, http.send("POST", ai.url, headers, std.to_json(body), 120))
    if a.stop_reason == "refusal" {return {role: "assistant", content: "Claude declined to help with that one."}}
    out = {role: "assistant", content: ""}
    thinking = []
    calls = []
    for b in a.content {
      if b.type == "text" {out.content += b.text}
      if b.type == "thinking" or b.type == "redacted_thinking" {thinking += [b]}
      if b.type == "tool_use" {calls += [{id: b.id, type: "function", function: {name: b.name, arguments: std.to_json(b.input ?? {})}}]}
    }
    if std.length(thinking) > 0 {out.thinking = thinking}
    if std.length(calls) > 0 {out.tool_calls = calls}
    return out
  }
  if ai.key != nothing {headers["Authorization"] = "Bearer {ai.key}"}
  body = {model: ai.model, messages: told + said}
  if std.length(tools) > 0 {body.tools = tools}
  return _heard_openai(_heard(ai, http.send("POST", ai.url, headers, std.to_json(body), 120)))
}

fn _claude_turns(said) "The conversation in Claude's shape: each message its blocks, a tool's result the person's turn, and turns of the same side joined into one." {
  turns = []
  for m in said {
    blocks = m.thinking ?? []
    if m.role == "tool" {
      blocks += [{type: "tool_result", tool_use_id: m.tool_call_id, content: std.text(m.content)}]
    } else {
      blocks += _claude_blocks(m.content)
    }
    for c in m.tool_calls ?? [] {
      input = {}
      try {input = std.from_json(c.function.arguments) ?? {}} catch e {input = {}}
      blocks += [{type: "tool_use", id: c.id, name: c.function.name, input: input}]
    }
    role = m.role == "assistant" ? "assistant" : "user"
    n = std.length(turns)
    if n > 0 and turns[n - 1].role == role {
      last = turns[n - 1]
      last.content += blocks
      turns[n - 1] = last
    } else if std.length(blocks) > 0 {
      turns += [{role: role, content: blocks}]
    }
  }
  return turns
}

fn _claude_blocks(content) "A message's content as Claude's blocks: its words as text, and a picture given as a data address as an image." {
  if content == nothing or content == "" {return []}
  if content == std.text(content) {return [{type: "text", text: content}]}
  out = []
  for part in content {
    if part.type == "text" {out += [{type: "text", text: std.text(part.text)}]}
    if part.type == "image_url" {
      address = std.text(part.image_url.url)
      pieces = text.split(address, ";base64,")
      kind = std.slice(pieces[0], 5)
      out += [{type: "image", source: {type: "base64", media_type: kind, data: pieces[1]}}]
    }
  }
  return out
}

fn _claude_tools(tools) "Tools as OpenAI writes them, in the shape Claude reads." {
  out = []
  for t in tools {
    out += [{name: t.function.name, description: t.function.description ?? "", input_schema: t.function.parameters ?? {type: "object", properties: {}}}]
  }
  return out
}

fn _heard_openai(a) "An answer in the shape OpenAI gives, as the next message." {
  m = a.choices[0].message
  out = {role: "assistant", content: m.content ?? ""}
  calls = []
  for c in m.tool_calls ?? [] {calls += [{id: c.id, type: "function", function: {name: c.function.name, arguments: c.function.arguments ?? "\{}"}}]}
  if std.length(calls) > 0 {out.tool_calls = calls}
  return out
}

fn _heard(ai, r) "A web answer from the AI, read as JSON, or what went wrong in plain words." {
  if r.status == 401 or r.status == 403 {fail "{ai.name} turned the key down: check it in ai's settings, in the Onsay app or with the command line's set"}
  if r.status < 200 or r.status > 299 {fail "{ai.name} answered {r.status}: {r.text}"}
  return std.from_json(r.text)
}
