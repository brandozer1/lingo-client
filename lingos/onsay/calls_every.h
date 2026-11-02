// calls_every.h - the base calls of every computer that can run this program: files, commands and chat.
// One call, one line: CALL(group, what it needs, its words, its function). A file that includes this says what CALL does,
// and calls_shared.c what WINDOW, CONTROL and FILE_ are. Every computer has every call under the same name, so a lingo
// written on one works on any other, and a call a device can't do says so when called.
CALL("files", "files.read", "read(path: path) -> text: the text of a file on this computer", fs_read)
CALL("files", "files.write", "write(path: path, text: text): writes a whole text file, replacing what is there", fs_write)
CALL("files", "files.write", "add_to(path: path, text: text): adds text to the end of a file", fs_append)
CALL("files", "files.read, files.write", "edit(path: path, find: text, with: text, all: bool=false) -> {replaced: whole}: exact words in a file replaced by others, refused when they "
     "are there more than once unless all is true", fs_edit)
CALL("files", "files.read", "exists(path: path) -> bool: whether a file or folder is there", fs_exists)
CALL("files", "files.read", "list(folder: folder, pattern: pattern=\"*\") -> [" FILE_ "]: the files and folders in a folder, like the desktop or downloads", fs_list)
CALL("files", "files.read", "find(folder: folder, pattern: pattern=\"*\") -> [" FILE_ "]: finds files and folders by name in a folder and every folder inside it", fs_find)
CALL("files", "files.read", "search(folder: folder, words: text, pattern: pattern=\"*\", most: whole=100) -> [{path: path, line: whole, text: text}]: every line holding those words, in any case, in the "
     "text files of a folder and every folder inside, or those whose names match pattern", fs_search)
CALL("files", "files.write", "copy(from: path, to: path): copies a file or folder, into to if it is a folder", fs_copy)
CALL("files", "files.write", "move(from: path, to: path): moves or renames a file or folder, into to if it is a folder", fs_move)
CALL("files", "files.write", "delete(path: path): deletes a file or folder, to the Recycle Bin or the trash", fs_delete)
CALL("files", "files.write", "make_folder(path: path): makes a new folder", fs_folder)
CALL("commands", "run", "run(command: command) -> {code: whole, out: text}: runs any command or script in this computer's shell, Windows PowerShell on Windows and sh elsewhere, and gives its exit code and output. For anything no other function does", shell_run)
CALL("commands", "none", "running() -> [{pid: whole, name: text, memory: number}]: the programs and processes running now, memory in MB", shell_processes)
CALL("commands", "run", "stop(pid: whole): ends a running program or process", shell_stop)
CALL("commands", "run", "start(command: command) -> {id: text}: starts a command in the background and goes on, for one that takes long or keeps running, like a server. commands.output gives what it has written", shell_start)
CALL("commands", "none", "output(id: text) -> {running: bool, code?: whole, pid: whole, out: text}: what a command begun with commands.start has written so far, whether it still runs, and its exit code once it ended", shell_output)
CALL("chat", "none", "ask(questions: list, seconds: number=120) -> [text]?: asks the person questions in Onsay's chat on this computer and waits for "
     "their answers, in order, so the work goes on with them. Each question is text, answered by typing, or {question, choices}, "
     "answered by picking one. Nothing when nobody answered in seconds. Fails when no Onsay app is open to ask", chat_ask)
CALL("chat", "none", "say(text: text): tells the person something in Onsay's chat while the work goes on, on whichever computer they asked from", chat_say)
CALL("chat", "none", "step(text: text): shows what is being done now as a step under the person's message in Onsay's chat, on whichever computer they asked from. "
     "Say one as each part of a long job starts: it also tells Onsay the work is still going", chat_step)
