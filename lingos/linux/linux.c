// linux.c - the base calls on Linux, built into the lingo client for Linux (build.ps1). The same names as on every computer
// (../onsay/calls_every.h and calls_device.h): files, commands and chat work here, and a call this build can't do yet, like
// the screen's, says so when it is called.
//
// 1. helpers       values, paths
// 2. files         read, write, add_to, edit, exists, list, find, search, copy, move, delete, make_folder
// 3. commands      run, start, output, running, stop
// 4. chat          ask, say, step
// 5. base_calls()  every call joined to its function, called as the lingo client starts (onsay_lingo)
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "platform.h"

#define WHOLE 4096           // the longest path
#define MOST_OUT (4u << 20)  // the most of a command's output kept, in bytes
#define RUN_SECONDS 600      // the longest commands.run waits

////////// 1. HELPERS //////////

static const char *str(run *r, val *x) {
  if (x->k == V_TEXT) return x->t.s;
  buf b = { 0 };
  buf_text(&b, x);
  val *t = v_text(r->a, b.s ? b.s : "", b.n);
  buf_free(&b);
  return t->t.s;
}

static void put(run *r, val *rec, const char *key, val *v) { rec_set(r->a, rec, key, v); }
static void put_num(run *r, val *rec, const char *key, double n) { rec_set(r->a, rec, key, v_num(r->a, n)); }

static const char *home_folder(void) {
  const char *home = getenv("HOME");
  return home && *home ? home : "/";
}

// A path as the person means it: ~ for home, downloads, desktop or documents for those folders, otherwise from home,
// with . and .. worked out, and checked with the core for need, when given.
static val *path_of(run *r, val *p, const char *need) {
  const char *given = str(r, p), *folder = NULL;
  size_t first = strcspn(given, "/");
  if (first == 1 && given[0] == '~') folder = "";
  else if (first == 9 && !strncasecmp(given, "downloads", 9)) folder = "/Downloads";
  else if (first == 7 && !strncasecmp(given, "desktop", 7)) folder = "/Desktop";
  else if (first == 9 && !strncasecmp(given, "documents", 9)) folder = "/Documents";

  buf joined = { 0 };
  if (folder) buf_fmt(&joined, "%s%s%s", home_folder(), folder, given + first);
  else if (given[0] == '/') buf_str(&joined, given);
  else buf_fmt(&joined, "%s/%s", home_folder(), given);

  // Each part in turn: . is dropped, and .. takes the last one back.
  char *text = joined.s ? joined.s : "/";
  buf whole = { 0 };
  for (char *part = strtok(text, "/"); part; part = strtok(NULL, "/")) {
    if (!strcmp(part, ".")) continue;
    if (!strcmp(part, "..")) {
      while (whole.n && whole.s[--whole.n] != '/');
      if (whole.s) whole.s[whole.n] = 0;
      continue;
    }
    buf_chr(&whole, '/');
    buf_str(&whole, part);
  }
  val *out = whole.n ? v_text(r->a, whole.s, whole.n) : v_cstr(r->a, "/");
  buf_free(&joined), buf_free(&whole);
  if (need) may(r, need, out->t.s);
  return out;
}

static void why_not(run *r, const char *what, const char *path) {
  const char *why = errno == ENOENT ? "it doesn't exist" : errno == EACCES || errno == EPERM ? "this computer denied access" : strerror(errno);
  fail(r, "phrase", "can't %s %s: %s", what, path, why);
}

// The folders above a path, made.
static void parent_folder(run *r, const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash && slash > path) pl_folder(v_text(r->a, path, (size_t)(slash - path))->t.s);
}

////////// 2. FILES //////////

static val *fs_read(run *r, val **a) {
  val *p = path_of(r, a[0], "files.read");
  buf b = { 0 };
  errno = 0;
  if (!pl_read(p->t.s, &b)) why_not(r, "read", p->t.s);
  size_t skip = b.n >= 3 && !memcmp(b.s, "\xEF\xBB\xBF", 3) ? 3 : 0;   // a byte order mark isn't part of the text
  val *out = v_text(r->a, b.n ? b.s + skip : "", b.n - skip);
  buf_free(&b);
  return out;
}

static val *fs_write(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  buf b = { 0 };
  if (a[1]->k == V_TEXT) buf_add(&b, a[1]->t.s, a[1]->t.len);
  else buf_json(&b, a[1]);
  parent_folder(r, p->t.s);
  errno = 0;
  int ok = pl_write(p->t.s, b.s ? b.s : "", b.n);
  buf_free(&b);
  if (!ok) why_not(r, "write", p->t.s);
  return NOTHING;
}

static val *fs_append(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  const char *text = str(r, a[1]);
  parent_folder(r, p->t.s);
  errno = 0;
  if (!pl_append(p->t.s, text, strlen(text))) why_not(r, "add to", p->t.s);
  return NOTHING;
}

static val *fs_exists(run *r, val **a) {
  struct stat s;
  return VBOOL(stat(path_of(r, a[0], "files.read")->t.s, &s) == 0);
}

// * is any run of characters and ? any one, ignoring case, the same as on Windows.
static int glob(const char *p, const char *s) {
  if (!*p) return !*s;
  if (*p == '*') return glob(p + 1, s) || (*s && glob(p, s + 1));
  return *s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && glob(p + 1, s + 1);
}

static val *file_rec(run *r, const char *name, const char *full, const struct stat *s) {
  char when[32];
  struct tm t;
  gmtime_r(&s->st_mtime, &t);
  strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S.000Z", &t);
  val *f = v_rec(r->a);
  put(r, f, "name", v_cstr(r->a, name));
  put(r, f, "path", v_cstr(r->a, full));
  put_num(r, f, "size", (double)s->st_size);
  put(r, f, "folder", VBOOL(S_ISDIR(s->st_mode)));
  put(r, f, "changed", v_cstr(r->a, when));
  return f;
}

// Entries in folder whose name fits pattern, recursive with deep, up to 10,000. A link to a folder isn't followed.
static void look_in(run *r, const char *folder, const char *pattern, int deep, val *out) {
  DIR *d = opendir(folder);
  if (!d) return;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (out->l.n >= 10000 || *r->stop) break;
    char full[WHOLE];
    struct stat s;
    snprintf(full, sizeof full, "%s/%s", !strcmp(folder, "/") ? "" : folder, e->d_name);
    if (lstat(full, &s)) continue;
    if (glob(pattern, e->d_name)) list_push(r->a, out, file_rec(r, e->d_name, full, &s));
    if (deep && S_ISDIR(s.st_mode)) look_in(r, full, pattern, deep, out);
  }
  closedir(d);
}

static val *listing(run *r, val **a, int deep) {
  val *p = path_of(r, a[0], "files.read"), *out = v_list(r->a);
  struct stat s;
  if (stat(p->t.s, &s) || !S_ISDIR(s.st_mode)) fail(r, "phrase", "there is no folder %s", p->t.s);
  look_in(r, p->t.s, a[1]->k == V_NOTHING ? "*" : str(r, a[1]), deep, out);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  return out;
}

static val *fs_list(run *r, val **a) { return listing(r, a, 0); }
static val *fs_find(run *r, val **a) { return listing(r, a, 1); }

/* files.search: every line holding the words, in any case, in the text files of a folder and the folders inside it,
   leaving out .git and node_modules, files over SEARCH_MOST bytes and files that aren't text. */
#define SEARCH_MOST 5000000
#define SEARCH_LINE 300
static val *fs_search(run *r, val **a) {
  const char *words = str(r, a[1]);
  size_t n = strlen(words);
  if (!n) fail(r, "phrase", "files.search needs the words to look for");
  double most = a[3]->k == V_NUM ? a[3]->n : 100;
  val *where[2] = { a[0], a[2] };
  val *found = listing(r, where, 1), *out = v_list(r->a);
  for (size_t i = 0; i < found->l.n && out->l.n < most && !*r->stop; i++) {
    val *f = found->l.v[i];
    const char *path = text_or(f, "path", "");
    if (truthy(rec_get(f, "folder")) || rec_get(f, "size")->n > SEARCH_MOST) continue;
    if (strstr(path, "/.git/") || strstr(path, "/node_modules/")) continue;
    buf b = { 0 };
    if (!pl_read(path, &b) || !b.n || memchr(b.s, 0, b.n < 8000 ? b.n : 8000)) { buf_free(&b); continue; }
    int number = 0;
    for (char *line = b.s, *end = b.s + b.n; line < end && out->l.n < most;) {
      char *stop = memchr(line, '\n', (size_t)(end - line));
      size_t len = stop ? (size_t)(stop - line) : (size_t)(end - line);
      number++;
      int has = 0;
      for (size_t k = 0; k + n <= len && !has; k++) has = !strncasecmp(line + k, words, n);
      if (has) {
        val *hit = v_rec(r->a);
        put(r, hit, "path", v_cstr(r->a, path));
        put_num(r, hit, "line", number);
        size_t shown = len && line[len - 1] == '\r' ? len - 1 : len;
        put(r, hit, "text", v_text(r->a, line, shown > SEARCH_LINE ? SEARCH_LINE : shown));
        list_push(r->a, out, hit);
      }
      line += len + 1;
    }
    buf_free(&b);
  }
  return out;
}

// How many times what is in the text.
static int times_in(const char *text, const char *what) {
  int count = 0;
  for (const char *at = strstr(text, what); at; at = strstr(at + strlen(what), what)) count++;
  return count;
}

/* files.edit: exact words in a file replaced by others. Words that are there more than once are refused unless all is
   set, so the wrong place is never changed. */
static val *fs_edit(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  const char *find = str(r, a[1]), *with = str(r, a[2]);
  if (!*find) fail(r, "phrase", "files.edit needs the words to replace");
  buf b = { 0 };
  errno = 0;
  if (!pl_read(p->t.s, &b)) why_not(r, "read", p->t.s);
  const char *text = v_text(r->a, b.s ? b.s : "", b.n)->t.s;
  buf_free(&b);
  int count = times_in(text, find);
  if (!count) fail(r, "phrase", "those words aren't in %s", p->t.s);
  if (count > 1 && !truthy(a[3])) fail(r, "phrase", "those words are in %s %d times: give more of what is around them, or all as true", p->t.s, count);
  buf out = { 0 };
  const char *from = text;
  for (const char *at = strstr(from, find); at; at = strstr(from, find)) {
    buf_add(&out, from, (size_t)(at - from));
    buf_str(&out, with);
    from = at + strlen(find);
  }
  buf_str(&out, from);
  errno = 0;
  int ok = pl_write(p->t.s, out.s ? out.s : "", out.n);
  buf_free(&out);
  if (!ok) why_not(r, "write", p->t.s);
  val *rec = v_rec(r->a);
  put_num(r, rec, "replaced", count);
  return rec;
}

// Into to if it is a folder, otherwise as to, with the folders above it made. Only a file can be copied, as on Windows.
static void copy_or_move(run *r, val **a, int move) {
  val *from = path_of(r, a[0], move ? "files.write" : "files.read"), *to = path_of(r, a[1], "files.write");
  struct stat s;
  if (stat(from->t.s, &s)) fail(r, "phrase", "there is nothing at %s", from->t.s);
  const char *target = to->t.s;
  struct stat there;
  if (!stat(target, &there) && S_ISDIR(there.st_mode)) {
    const char *name = strrchr(from->t.s, '/');
    buf joined = { 0 };
    buf_fmt(&joined, "%s%s", target, name ? name : "/");
    target = v_text(r->a, joined.s, joined.n)->t.s;
    buf_free(&joined);
  }
  parent_folder(r, target);
  if (move && !rename(from->t.s, target)) return;
  buf b = { 0 };
  errno = 0;
  if (!pl_read(from->t.s, &b)) why_not(r, move ? "move" : "copy", from->t.s);
  int ok = pl_write(target, b.s ? b.s : "", b.n);
  buf_free(&b);
  if (!ok) why_not(r, move ? "move" : "copy", from->t.s);
  chmod(target, s.st_mode);
  if (move && unlink(from->t.s)) why_not(r, "move", from->t.s);
}

static val *fs_copy(run *r, val **a) { copy_or_move(r, a, 0); return NOTHING; }
static val *fs_move(run *r, val **a) { copy_or_move(r, a, 1); return NOTHING; }

// To the trash in the person's home, so they can always get it back, the same promise as the Recycle Bin.
static val *fs_delete(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  struct stat s;
  if (lstat(p->t.s, &s)) fail(r, "phrase", "there is nothing at %s", p->t.s);
  buf trash = { 0 }, into = { 0 };
  buf_fmt(&trash, "%s/.local/share/Trash/files", home_folder());
  pl_folder(trash.s);
  const char *name = strrchr(p->t.s, '/');
  buf_fmt(&into, "%s/%s.%ld", trash.s, name ? name + 1 : "item", (long)pl_now());
  errno = 0;
  int ok = !rename(p->t.s, into.s);
  buf_free(&trash), buf_free(&into);
  if (!ok) why_not(r, "move to the trash", p->t.s);
  return NOTHING;
}

static val *fs_folder(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  if (!pl_folder(p->t.s)) fail(r, "phrase", "can't make the folder %s", p->t.s);
  return NOTHING;
}

////////// 3. COMMANDS //////////

// The output of a finished or stopped command, without its last new lines.
static val *trimmed(run *r, buf *out) {
  while (out->n && (out->s[out->n - 1] == '\n' || out->s[out->n - 1] == '\r' || out->s[out->n - 1] == ' ')) out->s[--out->n] = 0;
  val *text = v_text(r->a, out->s ? out->s : "", out->n);
  buf_free(out);
  return text;
}

// The command in the shell, in a process group of its own so a stop ends all of it, its output read until it ends.
static val *shell_run(run *r, val **a) {
  const char *command = str(r, a[0]);
  int pipes[2];
  if (pipe(pipes)) fail(r, "phrase", "the shell wouldn't start");
  pid_t child = fork();
  if (child < 0) { close(pipes[0]); close(pipes[1]); fail(r, "phrase", "the shell wouldn't start"); }
  if (!child) {
    setpgid(0, 0);
    dup2(pipes[1], 1);
    dup2(pipes[1], 2);
    close(pipes[0]);
    close(pipes[1]);
    int none = open("/dev/null", O_RDONLY);
    if (none >= 0) dup2(none, 0);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(pipes[1]);

  buf out = { 0 };
  double until = pl_now() + RUN_SECONDS;
  int open_ = 1, status = 0, ended = 0;
  char chunk[8192];
  while (open_ || !ended) {
    if (*r->stop || pl_now() > until) {
      int stopped = *r->stop;
      kill(-child, SIGKILL);
      waitpid(child, &status, 0);
      close(pipes[0]);
      buf_free(&out);
      if (stopped) fail(r, "stopped", "Stopped by the person");
      fail(r, "phrase", "it was still running after %d seconds, so it was stopped", RUN_SECONDS);
    }
    struct pollfd watch = { .fd = pipes[0], .events = POLLIN };
    if (open_ && poll(&watch, 1, 50) > 0) {
      ssize_t got = read(pipes[0], chunk, sizeof chunk);
      if (got > 0 && out.n < MOST_OUT) buf_add(&out, chunk, (size_t)got);
      if (got <= 0) open_ = 0;
    } else if (!open_) pl_sleep(0.05);
    if (!ended && waitpid(child, &status, WNOHANG) == child) ended = 1;
    if (ended && open_ && poll(&watch, 1, 0) == 0) open_ = 0;   // what it left running keeps the pipe, but it is done
  }
  close(pipes[0]);
  val *rec = v_rec(r->a);
  put_num(r, rec, "code", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
  put(r, rec, "out", trimmed(r, &out));
  return rec;
}

// Where a started command keeps what commands.output reads: <temp>/onsay-<id>.<what>.
static const char *kept_at(run *r, const char *id, const char *what) {
  const char *temp = getenv("TMPDIR");
  buf b = { 0 };
  buf_fmt(&b, "%s/onsay-%s.%s", temp && *temp ? temp : "/tmp", id, what);
  const char *out = v_text(r->a, b.s, b.n)->t.s;
  buf_free(&b);
  return out;
}

// In the background, on after this call returns. Two forks, so it never waits on this program.
static val *shell_start(run *r, val **a) {
  const char *command = str(r, a[0]);
  unsigned long long random_id = 0;
  pl_random(&random_id, sizeof random_id);
  buf id = { 0 };
  buf_fmt(&id, "%llu", random_id);
  val *given = v_text(r->a, id.s, id.n);
  buf_free(&id);
  const char *log = kept_at(r, given->t.s, "log"), *code = kept_at(r, given->t.s, "code"), *pid = kept_at(r, given->t.s, "pid");
  pid_t child = fork();
  if (child < 0) fail(r, "phrase", "the shell wouldn't start");
  if (!child) {
    setsid();
    if (fork()) _exit(0);
    int none = open("/dev/null", O_RDONLY);
    if (none >= 0) dup2(none, 0);
    execl("/bin/sh", "sh", "-c", "echo $$ > \"$3\"; eval \"$0\" > \"$1\" 2>&1; echo $? > \"$2\"", command, log, code, pid, (char *)NULL);
    _exit(127);
  }
  waitpid(child, NULL, 0);
  val *rec = v_rec(r->a);
  put(r, rec, "id", given);
  return rec;
}

// What a started command has written so far, and its exit code once it ended.
static val *shell_output(run *r, val **a) {
  const char *id = str(r, a[0]);
  if (!*id || strspn(id, "0123456789") != strlen(id)) fail(r, "phrase", "that isn't the id of a started command");
  buf log = { 0 }, code = { 0 }, pid = { 0 };
  if (!pl_read(kept_at(r, id, "pid"), &pid)) {
    // It may only just have been started, before the shell has written anything.
    pl_sleep(1);
    if (!pl_read(kept_at(r, id, "pid"), &pid)) fail(r, "phrase", "no started command has that id");
  }
  // Whether it ended first, so all it wrote is read once it has.
  int ended = pl_read(kept_at(r, id, "code"), &code);
  pl_read(kept_at(r, id, "log"), &log);
  val *rec = v_rec(r->a);
  put(r, rec, "running", VBOOL(!ended));
  put(r, rec, "code", ended ? v_num(r->a, atoi(code.s ? code.s : "0")) : NOTHING);
  put_num(r, rec, "pid", atoi(pid.s ? pid.s : "0"));
  put(r, rec, "out", v_text(r->a, log.s ? log.s : "", log.n));
  buf_free(&log), buf_free(&code), buf_free(&pid);
  return rec;
}

// From /proc: each process's name, and the memory it holds now in MB.
static val *shell_processes(run *r, val **a) {
  val *out = v_list(r->a);
  DIR *d = opendir("/proc");
  double page = (double)sysconf(_SC_PAGESIZE) / 1048576.0;
  for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
    if (!isdigit((unsigned char)e->d_name[0])) continue;
    char path[64], name[256] = "";
    long pages = 0;
    snprintf(path, sizeof path, "/proc/%.32s/comm", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
    fclose(f);
    snprintf(path, sizeof path, "/proc/%.32s/statm", e->d_name);
    if ((f = fopen(path, "r"))) { if (fscanf(f, "%*d %ld", &pages) != 1) pages = 0; fclose(f); }
    val *one = v_rec(r->a);
    put_num(r, one, "pid", atoi(e->d_name));
    put(r, one, "name", v_cstr(r->a, name));
    put_num(r, one, "memory", (double)(long)(pages * page * 10 + 0.5) / 10);
    list_push(r->a, out, one);
  }
  if (d) closedir(d);
  return out;
}

static val *shell_stop(run *r, val **a) {
  double pid = a[0]->k == V_NUM ? a[0]->n : 0;
  if (pid < 1 || kill((pid_t)pid, SIGTERM)) fail(r, "phrase", "process %g couldn't be stopped", pid);
  return NOTHING;
}

////////// 4. CHAT //////////

// Asked of an app on this computer, when one is open (lingo_ask). A text alone is one question.
static val *chat_ask(run *r, val **a) {
  val *questions = a[0], *ask = v_rec(r->a), *got;
  if (questions->k == V_TEXT) { questions = v_list(r->a); list_push(r->a, questions, a[0]); }
  if (questions->k != V_LIST || !questions->l.n) fail(r, "type", "chat.ask takes questions, each a text or {question, choices}");
  put(r, ask, "questions", questions);
  if (!(got = lingo_ask(r, ask, a[1]->k == V_NUM ? a[1]->n : 120))) fail(r, "phrase", "No Onsay app is open on this computer to ask the person");
  return got;
}

// A line in the chat as a line of JSON on stdout, for an app to draw.
static val *chat_line(run *r, const char *kind, val *text) {
  const char *t = str(r, text);
  buf b = { 0 };
  buf_fmt(&b, "{\"show\":\"%s\",\"text\":", kind);
  buf_json_str(&b, t, strlen(t));
  buf_str(&b, "}\n");
  fwrite(b.s, 1, b.n, stdout);
  fflush(stdout);
  buf_free(&b);
  return NOTHING;
}

static val *chat_say(run *r, val **a) { return chat_line(r, "say", a[0]); }
static val *chat_step(run *r, val **a) { return chat_line(r, "step", a[0]); }

// A call that needs what this build can't reach, like a screen.
static val *not_here(run *r, val **a) {
  fail(r, "phrase", "This computer can't do that: it needs a screen, or something else this Linux build can't reach yet");
}

////////// 5. BASE_CALLS //////////

// What windows, controls and files are, written out where a call gives one.
#define WINDOW "{id,app,title,x,y,w,h,focused,minimized}"
#define CONTROL "{id,role,name,value,x,y,w,h,enabled,focused,can}"
#define FILE_ "{name,path,size,folder,changed}"

// Called once as the lingo client starts (onsay_lingo). Every call has the same name on every computer.
void base_calls(void) {
#define CALL(group, needs, line, fn) phrase_add(group, needs, line, fn);
#include "calls_every.h"
#undef CALL
#define CALL(group, needs, line, fn) phrase_add(group, needs, line, not_here);
#include "calls_device.h"
#undef CALL
}
