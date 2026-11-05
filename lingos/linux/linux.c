// linux.c - the base calls on Linux and Android, built into the lingo client for Linux (build.ps1) and into the Android app
// (onsay-mobile). The same names as on every computer (../onsay/calls_every.h and calls_device.h): files, commands and chat
// on Linux, where a call this build can't do yet says so. The phone only prompts and shows, so on Android it has chat
// alone, for a lingo working on another computer to show its progress there.
//
// 1. helpers       paths, and why a file was refused
// 2. files         exists, list, find, copy, move, delete: what is Linux's own about them
// 3. commands      run, start, running, stop
// 4. base_calls()  every call joined to its function, called as the lingo client starts (onsay_lingo)
// What is the same on every computer, like files.search and chat, is in ../onsay/calls_shared.c.
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
#define SLASH "/"
#include "calls_shared.c"

#define WHOLE 4096   // the longest path
#define MOST_OUT (4u << 20)   // the most of a command's output kept, in bytes
#define RUN_SECONDS 600   // the longest commands.run waits
#ifdef __ANDROID__
#define SHELL "/system/bin/sh"
#else
#define SHELL "/bin/sh"
#endif

////////// 1. HELPERS //////////

static const char *home_folder(void) {
  const char *home = getenv("HOME");
  return home && *home ? home : "/";
}

// A path as the person means it: ~ for home, downloads, desktop or documents for those folders, otherwise from home,
// with . and .. worked out, and checked with the core for need, when given.
static val *path_of(run *r, val *p, const char *need) {
  const char *given = str(r, p);
  const char *folder = NULL;
  size_t first = strcspn(given, "/");
  if (first == 1 && given[0] == '~')
    folder = "";
  else if (first == 9 && !strncasecmp(given, "downloads", 9))
#ifdef __ANDROID__
    folder = "/Download";   // Android's own name for it
#else
    folder = "/Downloads";
#endif
  else if (first == 7 && !strncasecmp(given, "desktop", 7))
    folder = "/Desktop";
  else if (first == 9 && !strncasecmp(given, "documents", 9))
    folder = "/Documents";

  buf joined = { 0 };
  if (folder)
    buf_fmt(&joined, "%s%s%s", home_folder(), folder, given + first);
  else if (given[0] == '/')
    buf_str(&joined, given);
  else
    buf_fmt(&joined, "%s/%s", home_folder(), given);

  // Each part in turn: . is dropped, and .. takes the last one back.
  char *text = joined.s ? joined.s : "/";
  buf whole = { 0 };
  for (char *part = strtok(text, "/"); part; part = strtok(NULL, "/")) {
    if (!strcmp(part, ".")) continue;
    if (!strcmp(part, "..")) {
      while (whole.n && whole.s[--whole.n] != '/')
        ;
      if (whole.s) whole.s[whole.n] = 0;
      continue;
    }
    buf_chr(&whole, '/');
    buf_str(&whole, part);
  }
  val *out = whole.n ? v_text(r->a, whole.s, whole.n) : v_cstr(r->a, "/");
  buf_free(&joined);
  buf_free(&whole);
  if (need) may(r, need, out->t.s);
  return out;
}

static void file_failed(run *r, const char *what, const char *path) {
  const char *why = strerror(errno);
  if (errno == ENOENT) why = "it doesn't exist";
  if (errno == EACCES || errno == EPERM) why = "this computer denied access";
  fail(r, "phrase", "can't %s %s: %s", what, path, why);
}

////////// 2. FILES //////////

static val *fs_exists(run *r, val **a) {
  struct stat s;
  return VBOOL(stat(path_of(r, a[0], "files.read")->t.s, &s) == 0);
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
  val *p = path_of(r, a[0], "files.read");
  val *out = v_list(r->a);
  struct stat s;
  if (stat(p->t.s, &s) || !S_ISDIR(s.st_mode)) fail(r, "phrase", "there is no folder %s", p->t.s);
  look_in(r, p->t.s, a[1]->k == V_NOTHING ? "*" : str(r, a[1]), deep, out);
  if (*r->stop) fail(r, "stopped", "Stopped by the person");
  return out;
}

// Into to if it is a folder, otherwise as to, with the folders above it made. Only a file can be copied, as on Windows.
static void copy_or_move(run *r, val **a, int move) {
  val *from = path_of(r, a[0], move ? "files.write" : "files.read");
  val *to = path_of(r, a[1], "files.write");
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
  if (!pl_read(from->t.s, &b)) file_failed(r, move ? "move" : "copy", from->t.s);
  int ok = pl_write(target, b.s ? b.s : "", b.n);
  buf_free(&b);
  if (!ok) file_failed(r, move ? "move" : "copy", from->t.s);
  chmod(target, s.st_mode);
  if (move && unlink(from->t.s)) file_failed(r, "move", from->t.s);
}

// To the trash in the person's home, so they can always get it back, the same promise as the Recycle Bin.
static val *fs_delete(run *r, val **a) {
  val *p = path_of(r, a[0], "files.write");
  struct stat s;
  if (lstat(p->t.s, &s)) fail(r, "phrase", "there is nothing at %s", p->t.s);
  buf trash = { 0 };
  buf into = { 0 };
  buf_fmt(&trash, "%s/.local/share/Trash/files", home_folder());
  pl_folder(trash.s);
  const char *name = strrchr(p->t.s, '/');
  buf_fmt(&into, "%s/%s.%ld", trash.s, name ? name + 1 : "item", (long)pl_now());
  int ok = !rename(p->t.s, into.s);
  buf_free(&trash);
  buf_free(&into);
  if (!ok) file_failed(r, "move to the trash", p->t.s);
  return NOTHING;
}

////////// 3. COMMANDS //////////

// The command in the shell, in a process group of its own so a stop ends all of it, its output read until it ends.
static val *shell_run(run *r, val **a) {
  const char *command = str(r, a[0]);
  int pipes[2];
  if (pipe(pipes)) fail(r, "phrase", "the shell wouldn't start");
  pid_t child = fork();
  if (child < 0) {
    close(pipes[0]);
    close(pipes[1]);
    fail(r, "phrase", "the shell wouldn't start");
  }
  if (!child) {
    setpgid(0, 0);
    dup2(pipes[1], 1);
    dup2(pipes[1], 2);
    close(pipes[0]);
    close(pipes[1]);
    int none = open("/dev/null", O_RDONLY);
    if (none >= 0) dup2(none, 0);
    execl(SHELL, "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(pipes[1]);

  buf out = { 0 };
  double until = pl_now() + RUN_SECONDS;
  int open_ = 1;
  int status = 0;
  int ended = 0;
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
    if (open_ && poll(&watch, 1, 0) > 0) {
      ssize_t got = read(pipes[0], chunk, sizeof chunk);
      if (got > 0 && out.n < MOST_OUT) buf_add(&out, chunk, (size_t)got);
      if (got <= 0) open_ = 0;
    } else {
      int never = 0;
      lingo_wait(&never, r->stop, 0.05);   // other requests are answered while it runs
    }
    if (!ended && waitpid(child, &status, WNOHANG) == child) ended = 1;
    if (ended && open_ && poll(&watch, 1, 0) == 0) open_ = 0;   // what it left running keeps the pipe, but it is done
  }
  close(pipes[0]);
  val *rec = v_rec(r->a);
  put_num(r, rec, "code", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
  trim_end(&out);
  put(r, rec, "out", v_text(r->a, out.s ? out.s : "", out.n));
  buf_free(&out);
  return rec;
}

static const char *temp_folder(run *r) {
  const char *temp = getenv("TMPDIR");
  buf b = { 0 };
  buf_fmt(&b, "%s/", temp && *temp ? temp : "/tmp");
  const char *out = v_text(r->a, b.s, b.n)->t.s;
  buf_free(&b);
  return out;
}

// In the background, on after this call returns. Two forks, so it never waits on this program.
static val *shell_start(run *r, val **a) {
  const char *command = str(r, a[0]);
  const char *id = started_id(r);
  const char *log = kept_at(r, id, "log");
  const char *code = kept_at(r, id, "code");
  const char *pid = kept_at(r, id, "pid");
  pid_t child = fork();
  if (child < 0) fail(r, "phrase", "the shell wouldn't start");
  if (!child) {
    setsid();
    if (fork()) _exit(0);
    int none = open("/dev/null", O_RDONLY);
    if (none >= 0) dup2(none, 0);
    execl(SHELL, "sh", "-c", "echo $$ > \"$3\"; eval \"$0\" > \"$1\" 2>&1; echo $? > \"$2\"", command, log, code,
        pid, (char *)NULL);
    _exit(127);
  }
  waitpid(child, NULL, 0);
  val *rec = v_rec(r->a);
  put(r, rec, "id", v_cstr(r->a, id));
  return rec;
}

// From /proc: each process's name, and the memory it holds now in MB.
static val *shell_processes(run *r, val **a) {
  val *out = v_list(r->a);
  DIR *d = opendir("/proc");
  double page = (double)sysconf(_SC_PAGESIZE) / 1048576.0;
  for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
    if (!isdigit((unsigned char)e->d_name[0])) continue;
    char path[64];
    char name[256] = "";
    long pages = 0;
    snprintf(path, sizeof path, "/proc/%.32s/comm", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
    fclose(f);
    snprintf(path, sizeof path, "/proc/%.32s/statm", e->d_name);
    if ((f = fopen(path, "r"))) {
      if (fscanf(f, "%*d %ld", &pages) != 1) pages = 0;
      fclose(f);
    }
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

// One line of JSON on stdout for an app to read, or on Android handed to the app around the core.
static void show(buf *line) {
#ifdef __ANDROID__
  buf answer = { 0 };
  pl_device("show", line->s, &answer);
  buf_free(&answer);
  buf_free(line);
  return;
#endif
  buf_chr(line, '\n');
  fwrite(line->s, 1, line->n, stdout);
  fflush(stdout);
  buf_free(line);
}

#ifndef __ANDROID__
// A call that needs what this build can't reach, like a screen.
static val *not_here(run *r, val **a) {
  fail(r, "phrase",
      "This computer can't do that: it needs a screen, or something else this Linux build can't reach yet");
}
#endif

////////// 4. BASE_CALLS //////////

// Called once as the lingo client starts (onsay_lingo). Every call has the same name on every computer.
void base_calls(void) {
#ifdef __ANDROID__
#define CALL(group, needs, line, fn)   if (!strcmp(group, "chat")) phrase_add(group, needs, line, fn);
#include "calls_every.h"
#undef CALL
#else
#define CALL(group, needs, line, fn) phrase_add(group, needs, line, fn);
#include "calls_every.h"
#undef CALL
#define CALL(group, needs, line, fn) phrase_add(group, needs, line, not_here);
#include "calls_device.h"
#undef CALL
#endif
}
