// calls_device.h - the base calls that need a screen or a desktop: windows, controls, the clipboard, notifications, input and look.
// One call, one line: CALL(group, what it needs, its words, its function). A file that includes this says what CALL does,
// and calls_shared.c what WINDOW, CONTROL and FILE_ are. Every computer has every call under the same name, so a lingo
// written on one works on any other, and a call a device can't do says so when called.
CALL("window", "screen.read", "list() -> [" WINDOW "]: every window open on this computer, front to back, with its id, app, title and place", win_list)
CALL("window", "screen.read", "current() -> " WINDOW ": the one in front, never Onsay's own chat", win_front)
CALL("window", "screen.act", "open(target: app) -> " WINDOW "?: opens an app, file, folder or web address, like notepad, target being its name or path", win_open)
CALL("window", "screen.read", "wait_for(match: record, seconds: number=10) -> " WINDOW ": match {app} or {title}, part of the text is enough", win_wait)
CALL("window", "screen.act", "focus(id: whole): brings a window to the front and gives it the keyboard", win_focus)
CALL("window", "screen.act", "close(id: whole): closes a window", win_close)
CALL("window", "screen.act", "resize(id: whole, how: text): minimizes, maximizes or restores a window, how being min, max or normal", win_size)
CALL("window", "screen.act", "move(id: whole, x: number=nothing, y: number=nothing, w: number=nothing, h: number=nothing): moves a window on the screen or changes its size, in pixels", win_move)
CALL("controls", "screen.read", "find(win: window, match: record) -> [" CONTROL "]: match {name,role,value}, part of the text is enough", ui_find)
CALL("controls", "screen.read", "list(win: window, max: whole=80) -> [" CONTROL "]: the buttons, boxes, menus and other controls in a window", ui_all)
CALL("controls", "screen.read", "read(win: window) -> text: every word of a page or document", ui_text)
CALL("controls", "screen.act", "press(control: control): presses a button or other control found with controls.find", ui_press)
CALL("controls", "screen.act", "fill(control: control, text: text): replaces the text in a box found with controls.find", ui_fill)
CALL("controls", "screen.act", "toggle(control: control): ticks or unticks a checkbox or switch", ui_toggle)
CALL("controls", "screen.act", "pick(control: control, option: text): a dropdown or list option, by name", ui_choose)
CALL("controls", "screen.act", "scroll(control: control, amount: number): scrolls a list or page inside a window, positive up", ui_scroll)
CALL("controls", "screen.read", "wait_for(win: window, match: record, seconds: number=10) -> " CONTROL ": waits until a control matching {name,role,value} shows in a window", ui_wait)
CALL("clipboard", "screen.read", "read() -> text: the text copied to the clipboard", clip_get)
CALL("clipboard", "screen.act", "write(text: text): copies text to the clipboard", clip_set)
CALL("notify", "none", "show(title: text, text: text): shows a Windows notification", note_toast)
CALL("input", "none", "cursor(how: text=nothing) -> text: says which pointer Onsay clicks with, virtual or real, or switches it. It moves "
     "and clicks nothing. virtual, the default, never touches the user's mouse, and its clicks may be ignored by browsers, games "
     "and some apps. real moves and clicks the real mouse and works everywhere. Given how, it changes until changed again. "
     "Headless, in input.modes, never uses the real mouse, but for an action it has to do in front", mouse_mode)
CALL("input", "none", "modes(headless: bool=nothing, no_follow: bool=nothing) -> {headless: bool, no_follow: bool}: says how Onsay's "
     "input acts, or changes it, kept until changed again. It moves and clicks nothing. headless, on by default: nothing on screen "
     "moves, clicks and keys go to the app in the background, no window comes to the front and the user's mouse and keyboard "
     "are never taken; some apps ignore that, and what can't be done that way is done in front for that one action, the pointer "
     "showing, with a step saying why, then headless again. no_follow, on by default: no "
     "pointer or ring goes to where Onsay clicks or types, and the chat stays open while he works", input_modes)
CALL("input", "screen.act, screen.read", "click_text(words: text, window: window=nothing, nth: whole=0) -> {x: number, y: number, text: text}: rings and clicks those words where "
     "they are on screen, read by OCR. nth picks a later one when they show more than once. Fails when they aren't there", input_click_text)
CALL("input", "screen.act, screen.read", "click_on(window: window, match: record) -> " CONTROL ": rings and clicks the first control in the window "
     "matching {name,role,value}", input_click_on)
CALL("input", "screen.act, screen.read", "type_into(window: window, match: record, text: text): clicks the control matching {name,role,value}, clears it and "
     "types text", input_type_into)
CALL("input", "screen.act", "type(text: text): types it where the cursor is, a new line as enter. Headless, into the window last clicked in", input_type)
CALL("input", "screen.act, screen.read", "paste(text: text): puts text where the cursor is through the clipboard, fast for long text, then "
     "gives the clipboard back what it had. Headless, into the window last clicked in", input_paste)
CALL("input", "screen.act", "shortcut(combo: combo, times: whole=1, gap: number=0.1): presses a combo like \"ctrl+s\", times over, gap seconds apart. Headless, keys held together bring the app forward", input_shortcut)
CALL("input", "screen.act", "type_lines(lines: [text]): types each line of a list, pressing enter after each", input_type_lines)
CALL("input", "screen.act, screen.read", "scroll_to_text(words: text, window: window=nothing, tries: whole=10) -> {x: number, y: number, text: text}?: scrolls down until those "
     "words are on screen and gives where they are, or nothing", input_scroll_to_text)
CALL("input", "screen.read", "wait_for_text(words: text, window: window=nothing, seconds: number=10) -> {x: number, y: number, text: text}?: waits until those words show on screen "
     "and gives where they are, or nothing when time runs out. Only for waiting on something to appear: it does nothing itself",
     input_wait_for_text)
CALL("input", "screen.act, screen.read", "copy_all(window: window) -> text: brings the window to the front, selects everything in it and copies "
     "it, and gives the text. Headless, it reads the text instead and leaves the clipboard as it was", input_copy_all)
CALL("input", "screen.read", "point_text(words: text, say: text=\"\", window: window=nothing, seconds: number=10) -> {x: number, y: number, w: number, h: number, text: text}: rings those words on screen "
     "and has Onsay beside them say say, to show the user something. Clicks nothing", input_point_text)
CALL("input", "screen.read", "point_on(window: window, match: record, say: text=\"\", seconds: number=10) -> " CONTROL ": rings the control matching "
     "{name,role,value} and has Onsay beside it say say. Clicks nothing", input_point_on)
CALL("input", "none", "point_at(x: number, y: number, w: number, h: number, say: text=\"\", seconds: number=10): rings that box on screen, in pixels, with Onsay beside it saying say",
     input_point_at)
CALL("input", "none", "stop_pointing(): takes the ring away", show_clear)
CALL("input", "screen.act, screen.read", "click_in(window: window, x: number, y: number, how: text=\"left\") -> {x: number, y: number}: clicks a place in a window, wherever the window "
     "is now: x, y from its top left in pixels, or between -1 and 1 a share of its size, 0.5 the middle, and below 0 back from its "
     "right or bottom. how is left, double or right. window may be a match like {app}, brought to the front first unless headless", input_click_in)
CALL("input", "screen.act, screen.read", "move_in(window: window=nothing, x: number=0.5, y: number=0.5, seconds: number=0) -> {x: number, y: number}: moves the mouse pointer, gliding "
     "it to a place in a window, or on the screen with no window, the middle unless given. x, y the same way as click_in, at a "
     "hand's pace or over seconds", input_move_in)
CALL("input", "screen.act, screen.read", "move_along(points: [[number]], window: window=nothing, seconds: number=0) -> {x: number, y: number}: moves the mouse pointer through a list of "
     "places in turn, tracing a path or shape like a circle, each [x, y] the same way as click_in. At a hand's pace, or over seconds in all. "
     "Clicks nothing", input_move_along)
CALL("input", "screen.act, screen.read", "drag_in(window: window, x1: number, y1: number, x2: number, y2: number): drags from one place in a window to another, the same way as "
     "click_in. Headless, it brings the app forward for the drag", input_drag_in)
CALL("input", "screen.act, screen.read", "scroll_in(window: window, amount: number, x: number=0.5, y: number=0.5) -> {x: number, y: number}: scrolls at a place in a window, the middle "
     "unless given, positive up", input_scroll_in)
CALL("input", "screen.read", "position_in(window: window, x: number, y: number) -> {x: number, y: number}: the place on screen of a place in a window, without touching "
     "anything", input_where_in)
CALL("look", "screen.read", "screenshot(window: window=nothing, box: record=nothing) -> {path: path, x: number, y: number, w: number, h: number}: a picture of a window, or the main screen, "
     "saved as a PNG file. box {x,y,w,h} in the window's pixels keeps only that part", look_screenshot)
CALL("look", "screen.read", "elements(window: window=nothing, box: record=nothing) -> [{id: whole, kind: text, name: text, x: number, y: number, w: number, h: number}]: everything on screen that can be told "
     "apart, numbered: the controls Windows knows, buttons, links, boxes, tabs, checkboxes and icons, with their names, and every "
     "line of text OCR reads. Places in the window's pixels for input.click_in, or the screen's when no window is given", look_elements)
CALL("look", "screen.read", "overview(window: window=nothing) -> {app: text, title: text, focused: any, pressable: [text], fields: [text]}: a window in brief: what it is, what has "
     "the keyboard, and the names of what can be pressed and typed in. The front window when none is given", look_overview)
CALL("look", "screen.read", "focused(window: window=nothing) -> " CONTROL "?: the control that has the keyboard now", look_focused)
CALL("look", "screen.read", "page(window: window=nothing) -> text: every word of a window, from its own text, or read off the screen when it "
     "gives none", look_page)
CALL("look", "screen.read", "lines(window: window=nothing) -> [text]: the lines of text on screen, top to bottom, read by OCR. The whole screen "
     "when no window is given", look_lines)
CALL("look", "screen.read", "buttons(window: window=nothing, most: whole=60) -> [{name: text, role: text, x: number, y: number}]: the buttons, links, tabs, menu items and boxes to "
     "tick, by name, with where to click them", look_pressable)
CALL("look", "screen.read", "fields(window: window=nothing) -> [{name: text, role: text, value: any, x: number, y: number}]: the boxes to type in and the dropdowns, with what is in "
     "them now", look_fields)
CALL("look", "screen.read", "near(words: text, window: window=nothing, distance: number=150) -> [text]: the other lines on screen within distance pixels of "
     "those words, closest first: what a label belongs to, or the value beside it", look_near)
CALL("look", "screen.read", "find_text(words: text, window: window=nothing) -> [{text: text, x: number, y: number, w: number, h: number}]: every line on screen with those words in it, and where "
     "it is", look_locate)
CALL("look", "screen.read", "open_windows() -> [{app: text, title: text}]: the windows open now, front to back, in brief", look_open_windows)
