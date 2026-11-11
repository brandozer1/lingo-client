// update.c - the update lingo: keeps an installed app up to date from the releases Onsay signs, for a floor built into an
// app like Onsay for Windows (LINGOS in ../../core/build.ps1), never a part of the floor itself. An install is a folder,
// root, holding version.txt, which names the version that runs, and a folder for each version, app-<version>, its
// launcher starting the one version.txt names. A newer version is put together beside the one running, each file checked
// against the release Onsay signed, and goes in when the app says, by writing version.txt, so an update never touches a
// file in use and one that fails halfway changes nothing.
//
// 1. readied(r, root)     the newest release put whole in root, and older versions let go
// 2. check(r, a)          update.check(root) -> {version} once a newer version is ready, else nothing
//    apply(r, a)          update.apply(root, version) -> {version}: that version runs from the next start
// 3. update_calls()       its calls, added as the floor starts
#include <stdio.h>
#include <string.h>
#include "lingo.h"

// The files of a release that are this computer's own: those for every computer, and those built for its kind.
#ifdef _WIN64
#define ARCH "x64"
#elif defined(_WIN32)
#define ARCH "x86"
#elif defined(__aarch64__)
#define ARCH "arm64"
#else
#define ARCH "x64"
#endif

////////// 1. READIED //////////

// A version like 1.2.3, and a path inside a folder, like lingos/agent.lingo, with no way out of it.
static int version_ok(const char *v) { return *v && strlen(v) < 24 && strspn(v, "0123456789.") == strlen(v); }

static int path_ok(const char *p) {
  return *p && *p != '/' && strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/") == strlen(p) &&
      !strstr(p, "..") && !strstr(p, "//");
}

// Whether the file at path is these bytes, by its sha256 as hex.
static int same(const char *path, const char *hex) {
  buf b = { 0 };
  unsigned char sum[32];
  char mine[65];
  int ok = pl_read(path, &b);
  sha256(b.s ? b.s : "", b.n, sum);
  for (int i = 0; i < 32; i++) snprintf(mine + i * 2, 3, "%02x", sum[i]);
  buf_free(&b);
  return ok && !strcmp(mine, hex);
}

// A folder and everything in it, gone, as far as it can be.
static void removed(const char *path);
static void each_removed(const char *name, int folder, double size, double changed, void *arg) {
  arena *a = arena_new();
  const char *inside = arena_fmt(a, "%s/%s", (const char *)arg, name);
  (void)size, (void)changed;
  if (folder)
    removed(inside);
  else
    pl_delete(inside);
  arena_free(a);
}
static void removed(const char *path) {
  pl_list(path, each_removed, (void *)path);
  pl_delete(path);
}

// Older versions, every app- folder that is neither the one running nor the one coming.
typedef struct {
  const char *root, *now, *next;
} versions;
static void each_older(const char *name, int folder, double size, double changed, void *arg) {
  versions *v = arg;
  (void)size, (void)changed;
  if (!folder || strncmp(name, "app-", 4) || !strcmp(name + 4, v->now) || !strcmp(name + 4, v->next)) return;
  arena *a = arena_new();
  removed(arena_fmt(a, "%s/%s", v->root, name));
  arena_free(a);
}

/* 1. The newest release, {release: {version, files: [{path, name, sha256, size, arch}]}, signed}, from Onsay, checked by
      its key: put whole in root/app-<version>, each file kept when it is there already, copied from the version running
      when it is the same, or downloaded as name from Onsay's releases, and checked by its sha256. Its version, or NULL
      when root runs it already. Fails with why. */
static const char *readied(run *r, const char *root) {
  arena *a = r->a;
  buf error = { 0 };
  buf now = { 0 };
  val *got = pl_http(a, "GET", arena_fmt(a, "%s/updates/release", lingo_onsay()), v_rec(a), NULL, 0, 30, &error);
  const char *text = text_or(got, "text", "");
  val *said = json_parse(a, text, strlen(text), NULL);
  val *release = rec_get(said, "release");
  val *files = rec_get(release, "files");
  const char *version = text_or(release, "version", "");
  if (!got) fail(r, "phrase", "Onsay's releases couldn't be reached: %s", error.s ? arena_fmt(a, "%s", error.s) : "");
  buf_free(&error);
  if (!release || !lingo_vouched(release, rec_get(said, "signed")) || !version_ok(version) || !files || files->k != V_LIST)
    fail(r, "phrase", "The newest release isn't one Onsay signed");
  pl_read(arena_fmt(a, "%s/version.txt", root), &now);
  const char *running = arena_fmt(a, "%.*s", (int)strcspn(now.s ? now.s : "", "\r\n \t"), now.s ? now.s : "");
  buf_free(&now);
  if (!strcmp(running, version)) return NULL;
  const char *into = arena_fmt(a, "%s/app-%s", root, version);
  for (size_t i = 0; i < files->l.n; i++) {
    val *f = files->l.v[i];
    const char *path = text_or(f, "path", "");
    const char *hex = text_or(f, "sha256", "");
    const char *arch = text_or(f, "arch", "");
    const char *to = arena_fmt(a, "%s/%s", into, path);
    const char *was = arena_fmt(a, "%s/app-%s/%s", root, running, path);
    if (!path_ok(path)) fail(r, "phrase", "The release names a file outside its folder: %s", path);
    if ((*arch && strcmp(arch, ARCH)) || same(to, hex)) continue;
    const char *slash = strrchr(to, '/');
    pl_folder(arena_fmt(a, "%.*s", (int)(slash - to), to));
    buf b = { 0 };
    if (*running && same(was, hex) && pl_read(was, &b)) {
      pl_write(to, b.s ? b.s : "", b.n);
      buf_free(&b);
      continue;
    }
    const char *url = arena_fmt(a, "%s/updates/%s", lingo_onsay(), text_or(f, "name", ""));
    double began = pl_now();
    val *file = pl_http(a, "GET", url, v_rec(a), NULL, 0, 300, &error);
    lingo_called("web", NULL, NULL, "GET", url, NULL, NULL, 0, file, file ? NULL : error.s, 0, (pl_now() - began) * 1000);
    val *bytes = rec_get(file, "bytes") ? rec_get(file, "bytes") : rec_get(file, "text");
    val *status = rec_get(file, "status");
    if (!bytes || !status || status->n != 200 || !pl_write(to, bytes->t.s, bytes->t.len) || !same(to, hex)) {
      pl_delete(to);
      fail(r, "phrase", "%s of version %s didn't download whole", path, version);
    }
    buf_free(&error);
  }
  versions v = { root, running, version };
  pl_list(root, each_older, &v);
  return version;
}

////////// 2. CHECK AND APPLY //////////

// 2.
static val *check(run *r, val **a) {
  const char *version = readied(r, text_of(r, a[0])->t.s);
  if (!version) return NOTHING;
  val *out = v_rec(r->a);
  rec_set(r->a, out, "version", v_cstr(r->a, version));
  return out;
}

// 2. Only the newest release goes in, put together and checked again first.
static val *apply(run *r, val **a) {
  const char *root = text_of(r, a[0])->t.s;
  const char *wanted = text_of(r, a[1])->t.s;
  const char *version = readied(r, root);
  val *out = v_rec(r->a);
  if (version && strcmp(version, wanted)) fail(r, "phrase", "%s isn't the newest version any more: %s is", wanted, version);
  if (version && !pl_write(arena_fmt(r->a, "%s/version.txt", root), version, strlen(version)))
    fail(r, "phrase", "version.txt couldn't be written in %s", root);
  rec_set(r->a, out, "version", v_cstr(r->a, wanted));
  return out;
}

////////// 3. UPDATE_CALLS //////////

// 3.
void update_calls(void) {
  phrase_add("update", "net, files.write",
      "check(root: path) -> {version: text}: the newest release of the app installed in root, put together beside the one "
      "running and checked against what Onsay signed, once it is newer, else nothing",
      check);
  phrase_add("update", "files.write",
      "apply(root: path, version: text) -> {version: text}: that version, once check has it ready, runs from the app's next "
      "start",
      apply);
}
