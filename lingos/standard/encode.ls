// encode.ls - Encodes text for a program and decodes it again: base64 and its URL-safe form, parts of web addresses
// as %XX, HTML entities, and hex. It works on UTF-8 bytes, so the letters of every language come through whole.
//
// 1. base64(text, url_safe=false)  -> text: url_safe writes - and _ for + and /, with no = at the end
// 2. from_base64(text)             -> text: either alphabet, with = at the end or not, spaces and new lines skipped
// 3. url(text)                     -> text: every byte but A to Z, a to z, 0 to 9 and - _ . ~ written %XX
// 4. from_url(text)                -> text: each %XX back, a + left as it is
// 5. html(text)                    -> text: & < > " and ' written as entities
// 6. from_html(text)               -> text: &amp; &lt; &gt; &quot; &apos; &nbsp; and numbered ones like &#233; back
// 7. hex(text)                     -> text: two lower case digits for each byte
// 8. from_hex(text)                -> text: and back, spaces skipped

////////// 1-2. BASE64 //////////

// 1. Text as base64. url_safe writes - and _ instead of + and /, with no = at the end, for web addresses and tokens.
fn base64(text, url_safe=false) {
  a = letters(url_safe)
  b = std.bytes(text)
  n = std.length(b)
  out = []
  i = 0
  while i < n {
    x = b[i] * 65536 + (b[i + 1] ?? 0) * 256 + (b[i + 2] ?? 0)
    out += [a[math.round(x / 262144, 0, "down")], a[math.round(x / 4096, 0, "down") % 64]]
    out += [i + 1 < n ? a[math.round(x / 64, 0, "down") % 64] : url_safe ? "" : "=", i + 2 < n ? a[x % 64] : url_safe ? "" : "="]
    i += 3
  }
  return text.join(out)
}

// 2. Base64 back to text, in either alphabet, with or without = at the end. Fails when it isn't base64.
fn from_base64(text) {
  at = {"-": 62, "_": 63}
  for i, c in letters(false) {at[c] = i}
  bits = 0
  held = 0
  out = []
  for c in std.text(text) {
    v = at[c]
    if v == nothing {
      if c == "=" or text.trim(c) == "" {continue}
      fail "that isn't base64: it has {c} in it"
    }
    bits = bits * 64 + v
    held += 6
    if held >= 8 {
      held -= 8
      p = 1
      for k in 1..held {p *= 2}
      out += [math.round(bits / p, 0, "down")]
      bits = bits % p
    }
  }
  return std.from_bytes(out)
}

////////// 3-4. WEB ADDRESSES //////////

// 3. Text as one part of a web address, like a search word: every byte but letters, digits and - _ . ~ written %XX.
fn url(text) {
  out = []
  for c in std.text(text) {
    if c in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~" {out += [c]}
    else {for b in std.bytes(c) {out += ["%" + text.upper(two(b))]}}
  }
  return text.join(out)
}

// 4. A part of a web address back to text, each %XX as its byte. A + stays a +: a query's + for a space is
// url.from_query's to read.
fn from_url(text) {
  b = std.bytes(text)
  out = []
  i = 0
  while i < std.length(b) {
    hi = b[i] == 37 ? digit(b[i + 1] ?? 0) : nothing
    lo = hi == nothing ? nothing : digit(b[i + 2] ?? 0)
    if lo == nothing {
      out += [b[i]]
      i += 1
    } else {
      out += [hi * 16 + lo]
      i += 3
    }
  }
  return std.from_bytes(out)
}

////////// 5-6. HTML //////////

// 5. Text made safe to put inside HTML: & < > " and ' as entities.
fn html(text) {
  t = std.text(text)
  for p in [["&", "&amp;"], ["<", "&lt;"], [">", "&gt;"], ["\"", "&quot;"], ["'", "&#39;"]] {t = text.replace(t, p[0], p[1])}
  return t
}

// 6. HTML entities back to text: &amp; &lt; &gt; &quot; &apos; &nbsp; and numbered ones like &#233; or &#xE9;. An
// entity it doesn't know stays as it is.
fn from_html(text) {
  names = {amp: "&", lt: "<", gt: ">", quot: "\"", apos: "'", nbsp: std.from_bytes([194, 160])}
  parts = text.split(std.text(text), "&")
  out = [parts[0]]
  for part in std.slice(parts, 1) {
    end = text.split(part, ";")
    v = std.length(end) > 1 and std.length(end[0]) <= 10 ? names[end[0]] ?? numbered(end[0]) : nothing
    out += [v == nothing ? "&" + part : v + std.slice(part, std.length(end[0]) + 1)]
  }
  return text.join(out)
}

////////// 7-8. HEX //////////

// 7. Text as hex, two lower case digits for each of its UTF-8 bytes.
fn hex(text) {
  out = []
  for b in std.bytes(text) {out += [two(b)]}
  return text.join(out)
}

// 8. Hex back to text. Spaces are skipped, and it fails when it isn't hex.
fn from_hex(text) {
  b = std.bytes(text.replace(std.text(text), " ", ""))
  if std.length(b) % 2 == 1 {fail "hex has two digits for each byte"}
  out = []
  for i in 0..std.length(b) / 2 - 1 {
    hi = digit(b[i * 2])
    lo = digit(b[i * 2 + 1])
    if hi == nothing or lo == nothing {fail "that isn't hex: its digits are 0 to 9 and a to f"}
    out += [hi * 16 + lo]
  }
  return std.from_bytes(out)
}

////////// HELPERS //////////

// base64's 64 letters, the URL-safe ones when url_safe. For encode's own use.
fn letters(url_safe) {return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" + (url_safe ? "-_" : "+/")}

// A hex digit's value from its byte, or nothing when it isn't one. For encode's own use.
fn digit(b) {return b >= 48 and b <= 57 ? b - 48 : b >= 97 and b <= 102 ? b - 87 : b >= 65 and b <= 70 ? b - 55 : nothing}

// A byte as two hex digits. For encode's own use.
fn two(b) {
  d = "0123456789abcdef"
  return d[math.round(b / 16, 0, "down")] + d[b % 16]
}

// &#233; or &#xE9; as its character, from the name between & and ;, or nothing. For encode's own use.
fn numbered(name) {
  if std.slice(name, 0, 1) != "#" {return nothing}
  base = text.lower(std.slice(name, 1, 2)) == "x" ? 16 : 10
  n = 0
  for c in std.slice(name, base == 16 ? 2 : 1) {
    d = digit(std.bytes(c)[0])
    if d == nothing or d >= base {return nothing}
    n = n * base + d
  }
  return n > 0 and n < 1114112 ? std.from_bytes(utf8(n)) : nothing
}

// A character's number as its UTF-8 bytes, like 233 as [195, 169]. For encode's own use.
fn utf8(n) {
  if n < 128 {return [n]}
  if n < 2048 {return [192 + math.round(n / 64, 0, "down"), 128 + n % 64]}
  if n < 65536 {return [224 + math.round(n / 4096, 0, "down"), 128 + math.round(n / 64, 0, "down") % 64, 128 + n % 64]}
  return [240 + math.round(n / 262144, 0, "down"), 128 + math.round(n / 4096, 0, "down") % 64, 128 + math.round(n / 64, 0, "down") % 64, 128 + n % 64]
}
