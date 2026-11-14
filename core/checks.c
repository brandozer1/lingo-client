// checks.c - who is calling and whether they may: callers by HTTP Message Signatures (RFC 9421) or a badge, grants and the
// policy, this computer's own key, and values as Onsay signs them. Shared by every lingo client built here, included whole
// by floor.c and lingo.c after language.c. Each gives it onsay_name(out), this computer's name, lingo_home(), its home,
// keyed(a, keyid), a signer's key {key, handle, mine}, and ONSAY_KEY. See ../../onsay/docs/lingo-function.md, Badges and
// grants.
//
// 1. named(s)                                 -> whether s is a lingo's name
// 2. canonical(b, x), key_bytes, signed_by    -> a value as Onsay signs it, a key's bytes, and a signature checked
// 3. names, covers, opens, let_in             -> whether a grant names a caller and opens an address, and the one check
// 4. badge_of(a, text), badge_make            -> a badge checked, and one made by this computer's key
// 5. signer(a, headers, ...), sign_request   -> who signed a request, and one this computer sends signed
// 6. key_of(name), key_sign(name), key_forget -> this computer's own keys, kept sealed in home/keys
// 7. header_names(a, h)                       -> headers as a record by lower case name

// Each lingo client uses the part it needs, so one that goes unused in one of them is no mistake.
#pragma clang diagnostic ignored "-Wunused-function"

static val *keyed(arena *a, const char *keyid);
static int covers(const char *address, const char *here, const char *path, int listing);

////////// CHECKS //////////

// A lingo's name: lower case letters, digits, _ and -, neither of those two first or last, and never lingo, the
// language's own name.
static int named(const char *s) {
  size_t n = strlen(s);
  return n && n <= 64 && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == n && !strchr("_-", s[0]) &&
      !strchr("_-", s[n - 1]) && strcmp(s, "lingo");
}


// value as Onsay signs it: JSON with every record's keys sorted and no spaces.
static void canonical(buf *b, val *x) {
  if (x->k == V_LIST) {
    buf_chr(b, '[');
    for (size_t i = 0; i < x->l.n; i++) {
      if (i) buf_chr(b, ',');
      canonical(b, x->l.v[i]);
    }
    buf_chr(b, ']');
  } else if (x->k == V_REC) {
    size_t order[256];
    size_t n = x->r.n < 256 ? x->r.n : 256;
    for (size_t i = 0; i < n; i++) {
      size_t j = i;
      for (; j > 0 && strcmp(x->r.key[order[j - 1]], x->r.key[i]) > 0; j--) order[j] = order[j - 1];
      order[j] = i;
    }
    buf_chr(b, '{');
    for (size_t i = 0; i < n; i++) {
      if (i) buf_chr(b, ',');
      buf_json_str(b, x->r.key[order[i]], strlen(x->r.key[order[i]]));
      buf_chr(b, ':');
      canonical(b, x->r.v[order[i]]);
    }
    buf_chr(b, '}');
  } else
    buf_json(b, x);
}


// A key, ed25519:<base64>, as its 32 bytes. 0 when it isn't one.
static int key_bytes(const char *text, unsigned char key[32]) {
  buf raw = { 0 };
  int ok = text && !strncmp(text, "ed25519:", 8) && unbase64(text + 8, strlen(text + 8), &raw) && raw.n == 32;
  if (ok) memcpy(key, raw.s, 32);
  buf_free(&raw);
  return ok;
}


// Whether signed, <anything>:<base64>, is a signature over value as Onsay signs it, by key, Onsay's when NULL.
static int signed_by(val *value, val *signed_, const char *key) {
  unsigned char k[32];
  const char *s = signed_ && signed_->k == V_TEXT ? signed_->t.s : "";
  const char *colon = strchr(s, ':');
  buf text = { 0 };
  buf raw = { 0 };
  canonical(&text, value);
  int ok = colon && key_bytes(key ? key : ONSAY_KEY, k) && unbase64(colon + 1, strlen(colon + 1), &raw) &&
      raw.n == 64 && ed25519_check(k, text.s, text.n, (unsigned char *)raw.s);
  buf_free(&text);
  buf_free(&raw);
  return ok;
}


/* A request's caller, by whatever proof it came with. name is how the log names them: this computer, me, onsay, a
   signer's keyid like laptop.brandon, or ip 203.0.113.7 for anyone unproven. owner when it is this computer's own: the
   command line, the account's own call, or a computer of its handle. badge is the badge it came with, checked, its
   grants in order, and begun when the owner signed its first. base and signature are what its signer signed and the
   signature, kept so Onsay can check a charge was for a call they made. */
typedef struct {
  const char *name;
  int owner, begun;
  val *badge;
  const char *base, *signature;
} caller;


// Whether to, a grant's, names who: public, the very name, a handle, every one of its computers, like brandon for
// laptop.brandon, or a route on this computer, every lingo running under it, like / for each of them.
static int names(const char *to, const char *who) {
  const char *dot = strchr(who, '.');
  if (*to == '/') return *who == '/' && covers(to, "", who, 0);
  return !strcmp(to, "public") || !strcmp(to, who) || (dot && !strcmp(to, dot + 1));
}


/* Whether address, a grant's, covers path on this computer, here its name like desk.brandon, empty before signing in,
   or when listing touches it, so a lingo shows when a grant reaches any of its route. One starting with / is on this
   computer; any other starts with a computer or a handle. * is any one part, and an address covers itself and
   everything under it. */
static int covers(const char *address, const char *here, const char *path, int listing) {
  const char *p = address;
  const char *q = path;
  if (*p != '/') {
    size_t n = strcspn(p, "/");
    const char *dot = strchr(here, '.');
    int ours = (n == 1 && *p == '*') || (strlen(here) == n && !strncmp(p, here, n)) ||
        (dot && strlen(dot + 1) == n && !strncmp(p, dot + 1, n));
    if (!ours) return 0;
    p += n;
  }
  for (;;) {
    while (*p == '/') p++;
    while (*q == '/') q++;
    size_t a = strcspn(p, "/");
    size_t b = strcspn(q, "/");
    if (!a) return 1;
    if (!b) return listing;
    if (!(a == 1 && *p == '*') && (a != b || strncmp(p, q, a))) return 0;
    p += a;
    q += b;
  }
}


// Whether grant g lets path in: a version this computer knows, 1, when it says one, in date, one of its allow covering
// it and none of its except.
static int opens(val *g, const char *here, const char *path, int listing) {
  val *allow = rec_get(g, "allow");
  val *except = rec_get(g, "except");
  val *until = rec_get(g, "until");
  val *v = rec_get(g, "v");
  int in = 0;
  if (v && !(v->k == V_NUM && v->n == 1)) return 0;
  if (until && until->k == V_NUM && until->n < pl_now()) return 0;
  for (size_t i = 0; allow && allow->k == V_LIST && !in && i < allow->l.n; i++)
    in = allow->l.v[i]->k == V_TEXT && covers(allow->l.v[i]->t.s, here, path, listing);
  for (size_t i = 0; except && except->k == V_LIST && in && i < except->l.n; i++)
    in = !(except->l.v[i]->k == V_TEXT && covers(except->l.v[i]->t.s, here, path, 0));
  return in;
}


/* Whether the caller may reach path, or any of a lingo's route when listing, by the policy p: the owner always; anyone
   else by a grant in the policy naming them, or by the badge they came with, when its first grant is the owner's own or
   handed on by someone a policy grant names and lets pass it on, every grant opens path, and its last is for the
   caller. A caller with a badge is judged by the badge alone, the owner too, so a run handed on from one computer to
   the next never borrows the authority of a computer on the way. The control routes, /.lingo/, take only the owner or
   a badge it began. why says what decided. */
static int let_in(arena *a, val *p, const caller *c, const char *path, int listing, const char **why) {
  val *grants = v_list(a);
  int control = !strncmp(path, "/.lingo/", 8);
  buf name = { 0 };
  onsay_name(&name);
  const char *here = arena_fmt(a, "%s", name.s ? name.s : "");
  buf_free(&name);
  // This computer's own grants, then the account's, which never speak for the lingos here: only policy.json does.
  for (int k = 0; k < 2; k++) {
    val *some = rec_get(p, k ? "grants" : "own");
    for (size_t i = 0; some && some->k == V_LIST && i < some->l.n; i++)
      if (!k || *text_or(some->l.v[i], "to", "") != '/') list_push(a, grants, some->l.v[i]);
  }
  if (c->owner && !c->badge) {
    *why = "the owner";
    return 1;
  }
  for (size_t i = 0; !control && !c->badge && grants && grants->k == V_LIST && i < grants->l.n; i++) {
    val *g = grants->l.v[i];
    if (names(text_or(g, "to", ""), c->name) && opens(g, here, path, listing)) {
      *why = arena_fmt(a, "grant: %s", text_or(g, "to", ""));
      return 1;
    }
  }
  if (c->badge) {
    val *first = c->badge->l.v[0];
    val *last = c->badge->l.v[c->badge->l.n - 1];
    int in = c->begun;
    for (size_t i = 0; !in && !control && grants && grants->k == V_LIST && i < grants->l.n; i++) {
      val *g = grants->l.v[i];
      in = truthy(rec_get(g, "pass") ? rec_get(g, "pass") : NOTHING) && names(text_or(g, "to", ""), text_or(first, "by", "")) &&
          opens(g, here, path, listing);
    }
    in = in && names(text_or(last, "to", ""), c->name);
    for (size_t i = 0; in && i < c->badge->l.n; i++) in = opens(c->badge->l.v[i], here, path, listing);
    if (in) {
      *why = arena_fmt(a, "badge: %s", text_or(first, "by", ""));
      return 1;
    }
  }
  *why = "no grant lets them in";
  return 0;
}


/* 4. A badge checked: the base64 of a JSON list of grants, each handing on the one before. Every grant is version 1,
      v, the one this computer knows, so a newer one fails closed. Every signature checks out against its signer's key,
      this computer's own, by its name or its key, or the one on its record, over the sig before it and the grant as
      Onsay signs, and each grant but the last lets pass on to the next's signer.
      Its grants, begun set when the owner signed the first, or NULL with why. */
static val *badge_of(arena *a, const char *text, int *begun, const char **why) {
  buf raw = { 0 };
  buf name = { 0 };
  buf self = { 0 };
  unsigned char mine[32];
  val *b = unbase64(text, strlen(text), &raw) ? json_parse(a, raw.s ? raw.s : "", raw.n, NULL) : NULL;
  const char *before = "";
  int keyed_here = key_of("device", 0, mine);
  buf_free(&raw);
  onsay_name(&name);
  if (keyed_here) {
    buf_str(&self, "ed25519:");
    base64(&self, mine, 32);
  }
  *why = NULL;
  if (!b || b->k != V_LIST || !b->l.n) *why = "Its badge isn't one: the base64 of a list of grants";
  for (size_t i = 0; !*why && i < b->l.n; i++) {
    val *g = b->l.v[i];
    val *bare = v_rec(a);
    const char *by = text_or(g, "by", "");
    const char *sig = text_or(g, "sig", "");
    val *v = rec_get(g, "v");
    int own = keyed_here && ((name.n && !strcmp(by, name.s)) || !strcmp(by, self.s));
    val *who = own || g->k != V_REC ? NULL : keyed(a, by);
    const char *their = text_or(who, "key", "");
    buf key = { 0 };
    buf said = { 0 };
    buf message = { 0 };
    int known = own || (!strncmp(their, "ed25519:", 8) && unbase64(their + 8, strlen(their + 8), &key) && key.n == 32);
    for (size_t k = 0; g->k == V_REC && k < g->r.n; k++)
      if (strcmp(g->r.key[k], "sig")) rec_set(a, bare, g->r.key[k], g->r.v[k]);
    buf_str(&message, before);
    canonical(&message, bare);
    if (g->k != V_REC || !*text_or(g, "to", ""))
      *why = "Each grant in its badge says who it is for, in to";
    else if (!v)
      *why = arena_fmt(a, "Its badge's grant by %s says no version, v, so it can't be read", by);
    else if (!(v->k == V_NUM && v->n == 1))
      *why = arena_fmt(a, "Its badge's grant by %s is a version this computer doesn't know", by);
    else if (!known)
      *why = arena_fmt(a, "There's no key known for %s, who signed its badge", by);
    else if (!unbase64(sig, strlen(sig), &said) || said.n != 64 ||
        !ed25519_check(own ? mine : (unsigned char *)key.s, message.s, message.n, (unsigned char *)said.s))
      *why = arena_fmt(a, "Its badge's grant by %s doesn't check out", by);
    else if (i + 1 < b->l.n && (!truthy(rec_get(g, "pass") ? rec_get(g, "pass") : NOTHING) ||
                                   !names(text_or(g, "to", ""), text_or(b->l.v[i + 1], "by", ""))))
      *why = arena_fmt(a, "Its badge's grant by %s isn't handed on to whoever signed the next", by);
    if (!i) *begun = own || truthy(rec_get(who, "mine") ? rec_get(who, "mine") : NOTHING);
    before = sig;
    buf_free(&key);
    buf_free(&said);
    buf_free(&message);
  }
  buf_free(&name);
  buf_free(&self);
  return *why ? NULL : b;
}


/* 8. A badge made here, signed by this computer's key: one grant {v, by, to, allow, except, pass, until, data, sig},
      version 1, by this computer's name, or its key itself, ed25519:<base64>, before it has one, for public, everything
      and a day unless g says, stapled on to g's badge when it has one. As base64, to send, or NULL with why. */
static val *badge_make(arena *a, val *g, const char **why) {
  val *list = v_list(a);
  val *grant = v_rec(a);
  val *given = rec_get(g, "badge");
  val *everything = v_list(a);
  buf raw = { 0 };
  buf name = { 0 };
  buf message = { 0 };
  buf out = { 0 };
  unsigned char key[32];
  unsigned char sig[64];
  if (given && given->k == V_TEXT) {
    val *old = unbase64(given->t.s, given->t.len, &raw) ? json_parse(a, raw.s ? raw.s : "", raw.n, NULL) : NULL;
    buf_free(&raw);
    if (!old || old->k != V_LIST || !old->l.n) {
      *why = "Its badge isn't one: the base64 of a list of grants";
      return NULL;
    }
    list = old;
  }
  if (!key_of("device", 1, key)) {
    *why = "This computer couldn't make or use its key";
    return NULL;
  }
  onsay_name(&name);
  if (!name.n) {
    buf_str(&name, "ed25519:");
    base64(&name, key, 32);
  }
  list_push(a, everything, v_cstr(a, "/"));
  rec_set(a, grant, "v", v_num(a, 1));
  rec_set(a, grant, "by", v_text(a, name.s, name.n));
  rec_set(a, grant, "to", v_cstr(a, "public"));
  rec_set(a, grant, "allow", everything);
  rec_set(a, grant, "until", v_num(a, floor(pl_now()) + 86400));
  buf_free(&name);
  for (size_t i = 0; g->k == V_REC && i < g->r.n; i++)
    if (strstr(" to allow except pass until data ", arena_fmt(a, " %s ", g->r.key[i]))) rec_set(a, grant, g->r.key[i], g->r.v[i]);
  if (rec_get(grant, "to")->k != V_TEXT || rec_get(grant, "allow")->k != V_LIST) {
    *why = "A badge is for someone, to, like alice or public, and allows a list of addresses, like [\"/notes\"]";
    return NULL;
  }
  buf_str(&message, list->l.n ? text_or(list->l.v[list->l.n - 1], "sig", "") : "");
  canonical(&message, grant);
  list_push(a, list, grant);
  int made = key_sign("device", message.s, message.n, sig);
  buf_free(&message);
  if (!made) {
    *why = "This computer couldn't make or use its key";
    return NULL;
  }
  base64(&out, sig, 64);
  rec_set(a, grant, "sig", v_text(a, out.s, out.n));
  out.n = 0;
  buf_json(&message, list);
  base64(&out, (unsigned char *)message.s, message.n);
  val *badge = v_text(a, out.s, out.n);
  buf_free(&message);
  buf_free(&out);
  return badge;
}


static struct {
  char *seen;
  double at;
} * nonces;   // each signature's keyid and nonce, kept 5 minutes, so none is used twice
static size_t nonce_count;
static pl_gate *nonce_gate;   // keeps the nonces, which calls coming in on their own threads check at once; made by the
                              // lingo client as it starts


/* 4. Who signed a request, by HTTP Message Signatures (RFC 9421) with Ed25519. Its Signature-Input covers @method,
   @target-uri and content-digest at least, and onsay-badge when it carries one, and says when it was made, its nonce
   and the signer's keyid, like laptop.brandon, whose key the running native lingo that looks keys up gives (key in
   lingo_hooks). It must check out,
   be made within 5 minutes, its nonce new in that time, and its Content-Digest the body's sha-256. Its @target-uri is
   x-forwarded-proto (else http), :// x-forwarded-host (else host), and x-forwarded-uri (else the path), so one passed on
   by a proxy is checked as it was sent. Fills in c, or gives why not. */
static const char *signer(arena *a, val *h, const char *method, const char *path, val *body, caller *c) {
  const char *input = text_or(h, "signature-input", "");
  const char *eq = strchr(input, '=');
  const char *close = eq ? strchr(eq, ')') : NULL;
  const char *p;
  const char *keyid = NULL;
  const char *nonce = NULL;
  const char *uri = text_or(h, "x-forwarded-uri", path);
  const char *host = text_or(h, "x-forwarded-host", text_or(h, "host", ""));
  const char *why = NULL;
  const char *missing = NULL;
  buf base = { 0 };
  buf raw = { 0 };
  buf digest = { 0 };
  buf key = { 0 };
  unsigned char sum[32];
  int covers = 0;
  int badge_covered = 0;
  double created = 0;
  double now = pl_now();
  if (!eq || eq[1] != '(' || !close)
    return "Its Signature-Input isn't one, like on=(\"@method\" \"@target-uri\" \"content-digest\");created=...;nonce=\"...\";keyid=\"...\"";
  for (p = eq + 2; p < close; p++) {
    if (*p != '"') continue;
    size_t n = strcspn(p + 1, "\"");
    const char *id = v_text(a, p + 1, n)->t.s;
    const char *value = NULL;
    if (!strcmp(id, "@method"))
      value = method;
    else if (!strcmp(id, "@target-uri"))
      value = arena_fmt(a, "%s://%s%s", text_or(h, "x-forwarded-proto", "http"), host, uri);
    else if (!strcmp(id, "@authority"))
      value = host;
    else if (!strcmp(id, "@path"))
      value = v_text(a, uri, strcspn(uri, "?"))->t.s;
    else if (!strcmp(id, "@query"))
      value = strchr(uri, '?') ? strchr(uri, '?') : "?";
    else if (id[0] != '@')
      value = text_or(h, id, NULL);
    if (!value) missing = id;
    covers += !strcmp(id, "@method") + !strcmp(id, "@target-uri") + !strcmp(id, "content-digest");
    badge_covered |= !strcmp(id, "onsay-badge");
    buf_fmt(&base, "\"%s\": %s\n", id, value ? value : "");
    p += n + 1;
  }
  buf_fmt(&base, "\"@signature-params\": %s", eq + 1);
  for (p = close + 1; *p == ';'; p += 1 + strcspn(p + 1, ";")) {
    const char *value = p + 1 + strcspn(p + 1, "=;");
    size_t n = *value == '=' ? strcspn(++value, ";") : 0;
    const char *text = n >= 2 && value[0] == '"' ? v_text(a, value + 1, n - 2)->t.s : v_text(a, value, n)->t.s;
    if (!strncmp(p + 1, "created=", 8))
      created = atof(text);
    else if (!strncmp(p + 1, "nonce=", 6))
      nonce = text;
    else if (!strncmp(p + 1, "keyid=", 6))
      keyid = text;
  }
  const char *label = arena_fmt(a, "%.*s=:", (int)(eq - input), input);
  const char *sig = strstr(text_or(h, "signature", ""), label);
  const char *said = strstr(text_or(h, "content-digest", ""), "sha-256=:");
  val *who = !missing && keyid ? keyed(a, keyid) : NULL;
  const char *their = text_or(who, "key", "");
  sha256(body->t.s, body->t.len, sum);
  base64(&digest, sum, 32);
  if (missing)
    why = arena_fmt(a, "Its signature covers %s, which it hasn't", missing);
  else if (covers < 3)
    why = "Its signature must cover @method, @target-uri and content-digest";
  else if (rec_get(h, "onsay-badge") && !badge_covered)
    why = "Its signature must cover its Onsay-Badge, so the badge can't be lifted onto another request";
  else if (!keyid || !nonce || !created)
    why = "Its Signature-Input needs created, nonce and keyid";
  else if (created < now - 300 || created > now + 300)
    why = "Its signature wasn't made within 5 minutes";
  else if (!said || strncmp(said + 9, digest.s, digest.n) || said[9 + digest.n] != ':')
    why = "Its Content-Digest isn't its body's sha-256";
  else if (!who)
    why = arena_fmt(a, "There's no key known for %s", keyid);
  else if (!sig || !unbase64(sig + strlen(label), strcspn(sig + strlen(label), ":"), &raw) || raw.n != 64 ||
      strncmp(their, "ed25519:", 8) || !unbase64(their + 8, strlen(their + 8), &key) || key.n != 32 ||
      !ed25519_check((unsigned char *)key.s, base.s, base.n, (unsigned char *)raw.s))
    why = arena_fmt(a, "Its signature doesn't check out for %s", keyid);
  else {
    const char *seen = arena_fmt(a, "%s %s", keyid, nonce);
    size_t core_kept = 0;
    pl_enter(nonce_gate);
    for (size_t i = 0; i < nonce_count; i++) {
      if (nonces[i].at < now - 300) {
        free(nonces[i].seen);
        continue;
      }
      if (!strcmp(nonces[i].seen, seen)) why = "Its nonce was used already";
      nonces[core_kept++] = nonces[i];
    }
    nonce_count = core_kept;
    if (!why) {
      nonces = realloc(nonces, (nonce_count + 1) * sizeof *nonces);
      nonces[nonce_count].seen = strdup(seen);
      nonces[nonce_count++].at = now;
      c->name = keyid;
      c->owner = truthy(rec_get(who, "mine") ? rec_get(who, "mine") : NOTHING);
      c->base = v_text(a, base.s, base.n)->t.s;
      c->signature = v_text(a, sig + strlen(label), strcspn(sig + strlen(label), ":"))->t.s;
    }
    pl_leave(nonce_gate);
  }
  buf_free(&base);
  buf_free(&raw);
  buf_free(&digest);
  buf_free(&key);
  return why;
}


// 9. The seed of this device's key called name, in home/keys, made and sealed first when missing and make is set.
static int seed_of(const char *name, int make, unsigned char seed[32]) {
  buf path = { 0 };
  buf got = { 0 };
  buf_fmt(&path, "%s/keys", lingo_home());
  int ok = named(name);
  if (ok) {
    if (make) pl_folder(path.s);
    buf_fmt(&path, "/%s", name);
    ok = pl_unseal(path.s, &got) && got.n == 32;
  }
  if (ok)
    memcpy(seed, got.s, 32);
  else if (named(name) && make) {
    pl_random(seed, 32);
    ok = pl_seal(path.s, seed, 32);
  }
  if (got.s) memset(got.s, 0, got.n);
  buf_free(&path);
  buf_free(&got);
  return ok;
}


int key_of(const char *name, int make, unsigned char key[32]) {
  unsigned char seed[32];
  if (!seed_of(name, make, seed)) return 0;
  ed25519_key(seed, key);
  memset(seed, 0, sizeof seed);
  return 1;
}


int key_sign(const char *name, const void *msg, size_t n, unsigned char sig[64]) {
  unsigned char seed[32];
  if (!seed_of(name, 0, seed)) return 0;
  ed25519_sign(seed, msg, n, sig);
  memset(seed, 0, sizeof seed);
  return 1;
}


void key_forget(const char *name) {
  buf path = { 0 };
  buf_fmt(&path, "%s/keys/%s", lingo_home(), name);
  if (named(name)) pl_delete(path.s);
  buf_free(&path);
}

// Headers as a record by lower case name, each value text.
static val *header_names(arena *a, val *h) {
  val *out = v_rec(a);
  for (size_t i = 0; h && h->k == V_REC && i < h->r.n; i++) {
    val *v = h->r.v[i]->k == V_TEXT ? h->r.v[i] : NULL;
    buf b = { 0 };
    if (!v) buf_text(&b, h->r.v[i]);
    const char *text = b.s ? b.s : "";
    if (v) text = v->t.s;
    header_add(a, out, h->r.key[i], strlen(h->r.key[i]), text, v ? v->t.len : b.n);
    buf_free(&b);
  }
  return out;
}


/* 5. A request this computer sends, signed by its own key (RFC 9421), the other side of signer: Content-Digest, and a
      Signature-Input over @method, @target-uri and content-digest, and the Onsay-Badge already in headers when there is
      one, made now with a new nonce, its keyid this computer's name, or its key itself, ed25519:<base64>, before it has
      one. Added to headers; 0 when it has no key. */
static int sign_request(arena *a, val *headers, const char *method, const char *url, const char *body, size_t n) {
  unsigned char sum[32];
  unsigned char key[32];
  unsigned char sig[64];
  unsigned char nonce[16];
  buf keyid = { 0 };
  buf digest = { 0 };
  buf once = { 0 };
  buf params = { 0 };
  buf base = { 0 };
  buf said = { 0 };
  if (!key_of("device", 1, key)) return 0;
  onsay_name(&keyid);
  if (!keyid.n) {
    buf_str(&keyid, "ed25519:");
    base64(&keyid, key, 32);
  }
  sha256(body ? body : "", n, sum);
  buf_str(&digest, "sha-256=:");
  base64(&digest, sum, 32);
  buf_chr(&digest, ':');
  pl_random(nonce, sizeof nonce);
  base64(&once, nonce, sizeof nonce);
  const char *badge = text_or(headers, "Onsay-Badge", NULL);
  buf_fmt(&params, "(\"@method\" \"@target-uri\" \"content-digest\"%s);created=%.0f;nonce=\"%s\";keyid=\"%s\"",
      badge ? " \"onsay-badge\"" : "", floor(pl_now()), once.s, keyid.s);
  buf_fmt(&base, "\"@method\": %s\n\"@target-uri\": %s\n\"content-digest\": %s\n", method, url, digest.s);
  if (badge) buf_fmt(&base, "\"onsay-badge\": %s\n", badge);
  buf_fmt(&base, "\"@signature-params\": %s", params.s);
  int ok = key_sign("device", base.s, base.n, sig);
  if (ok) {
    buf_str(&said, "sig=:");
    base64(&said, sig, 64);
    buf_chr(&said, ':');
    rec_set(a, headers, "Content-Digest", v_text(a, digest.s, digest.n));
    rec_set(a, headers, "Signature-Input", v_cstr(a, arena_fmt(a, "sig=%s", params.s)));
    rec_set(a, headers, "Signature", v_text(a, said.s, said.n));
  }
  buf_free(&keyid);
  buf_free(&digest);
  buf_free(&once);
  buf_free(&params);
  buf_free(&base);
  buf_free(&said);
  return ok;
}
