// calls_device.h - the base calls that need a screen or a desktop: windows, controls, the clipboard, notifications, Onsay's panel, input and look.
// One call, one line: CALL(group, what it needs, its words, its function). A file that includes this says what CALL does,
// and calls_shared.c what WINDOW, CONTROL and FILE_ are. Every computer has every call under the same name, so a lingo
// written on one works on any other, and a call a device can't do says so when called.
CALL("window", "screen.read", "list() -> [" WINDOW "]: every window open on this computer, front to back, with its id, app, title and place", win_list)
CALL("window", "screen.read", "current() -> " WINDOW ": the one in front, never Onsay's own chat", win_front)
CALL("window", "screen.act", "open(target) -> " WINDOW "?: opens an app, file, folder or web address, like notepad, target being its name or path", win_open)
CALL("window", "screen.read", "wait_for(match, seconds=10) -> " WINDOW ": match {app} or {title}, part of the text is enough", win_wait)
CALL("window", "screen.act", "focus(id): brings a window to the front and gives it the keyboard", win_focus)
CALL("window", "screen.act", "close(id): closes a window", win_close)
CALL("window", "screen.act", "resize(id, how): minimizes, maximizes or restores a window, how being min, max or normal", win_size)
CALL("window", "screen.act", "move(id, x=nothing, y=nothing, w=nothing, h=nothing): moves a window on the screen or changes its size, in pixels", win_move)
CALL("controls", "screen.read", "find(win, match) -> [" CONTROL "]: match {name,role,value}, part of the text is enough", ui_find)
CALL("controls", "screen.read", "list(win, max=80) -> [" CONTROL "]: the buttons, boxes, menus and other controls in a window", ui_all)
CALL("controls", "screen.read", "read(win) -> text: every word of a page or document", ui_text)
CALL("controls", "screen.act", "press(control): presses a button or other control found with controls.find", ui_press)
CALL("controls", "screen.act", "fill(control, text): replaces the text in a box found with controls.find", ui_fill)
CALL("controls", "screen.act", "toggle(control): ticks or unticks a checkbox or switch", ui_toggle)
CALL("controls", "screen.act", "pick(control, option): a dropdown or list option, by name", ui_choose)
CALL("controls", "screen.act", "scroll(control, amount): scrolls a list or page inside a window, positive up", ui_scroll)
CALL("controls", "screen.read", "wait_for(win, match, seconds=10) -> " CONTROL ": waits until a control matching {name,role,value} shows in a window", ui_wait)
CALL("clipboard", "screen.read", "read() -> text: the text copied to the clipboard", clip_get)
CALL("clipboard", "screen.act", "write(text): copies text to the clipboard", clip_set)
CALL("notify", "none", "show(title, text): shows a Windows notification", note_toast)
CALL("panel", "none", "show(html, title=\"\", width=480, height=360): draws a small rounded window of Onsay's own on screen, beside him, "
     "showing html: a page, a table, a chart, a picture or an answer laid out. It stays until hidden or the person closes it, and "
     "showing again replaces it. Scripts in it run, with no reach to this computer", panel_show)
CALL("panel", "none", "hide(): takes the window away", panel_hide)
CALL("input", "none", "cursor(how=nothing) -> text: says which pointer Onsay clicks with, virtual or real, or switches it. It moves "
     "and clicks nothing. virtual, the default, never touches the user's mouse, and its clicks may be ignored by browsers, games "
     "and some apps. real moves and clicks the real mouse and works everywhere. Given how, it changes until changed again", mouse_mode)
CALL("input", "screen.act, screen.read", "click_text(words, window=nothing, nth=0) -> {x,y,text}: rings and clicks those words where "
     "they are on screen, read by OCR. nth picks a later one when they show more than once. Fails when they aren't there", input_click_text)
CALL("input", "screen.act, screen.read", "click_on(window, match) -> " CONTROL ": rings and clicks the first control in the window "
     "matching {name,role,value}", input_click_on)
CALL("input", "screen.act, screen.read", "type_into(window, match, text): clicks the control matching {name,role,value}, clears it and "
     "types text", input_type_into)
CALL("input", "screen.act", "type(text): types it where the cursor is, a new line as enter", input_type)
CALL("input", "screen.act, screen.read", "paste(text): puts text where the cursor is through the clipboard, fast for long text, then "
     "gives the clipboard back what it had", input_paste)
CALL("input", "screen.act", "shortcut(combo, times=1, gap=0.1): presses a combo like \"ctrl+s\", times over, gap seconds apart", input_shortcut)
CALL("input", "screen.act", "type_lines(lines): types each line of a list, pressing enter after each", input_type_lines)
CALL("input", "screen.act, screen.read", "scroll_to_text(words, window=nothing, tries=10) -> {x,y,text}?: scrolls down until those "
     "words are on screen and gives where they are, or nothing", input_scroll_to_text)
CALL("input", "screen.read", "wait_for_text(words, window=nothing, seconds=10) -> {x,y,text}?: waits until those words show on screen "
     "and gives where they are, or nothing when time runs out. Only for waiting on something to appear: it does nothing itself",
     input_wait_for_text)
CALL("input", "screen.act, screen.read", "copy_all(window) -> text: brings the window to the front, selects everything in it and copies "
     "it, and gives the text", input_copy_all)
CALL("input", "screen.read", "point_text(words, say=\"\", window=nothing, seconds=10) -> {x,y,w,h,text}: rings those words on screen "
     "and has Onsay beside them say say, to show the user something. Clicks nothing", input_point_text)
CALL("input", "screen.read", "point_on(window, match, say=\"\", seconds=10) -> " CONTROL ": rings the control matching "
     "{name,role,value} and has Onsay beside it say say. Clicks nothing", input_point_on)
CALL("input", "none", "point_at(x, y, w, h, say=\"\", seconds=10): rings that box on screen, in pixels, with Onsay beside it saying say",
     input_point_at)
CALL("input", "none", "stop_pointing(): takes the ring away", show_clear)
CALL("input", "screen.act, screen.read", "click_in(window, x, y, how=\"left\") -> {x,y}: clicks a place in a window, wherever the window "
     "is now: x, y from its top left in pixels, or between -1 and 1 a share of its size, 0.5 the middle, and below 0 back from its "
     "right or bottom. how is left, double or right. window may be a match like {app}, brought to the front first", input_click_in)
CALL("input", "screen.act, screen.read", "move_in(window=nothing, x=0.5, y=0.5, seconds=0) -> {x,y}: moves the mouse pointer, gliding "
     "it to a place in a window, or on the screen with no window, the middle unless given. x, y the same way as click_in, at a "
     "hand's pace or over seconds", input_move_in)
CALL("input", "screen.act, screen.read", "move_along(points, window=nothing, seconds=0) -> {x,y}: moves the mouse pointer through a list of "
     "places in turn, tracing a path or shape like a circle, each [x, y] the same way as click_in. At a hand's pace, or over seconds in all. "
     "Clicks nothing", input_move_along)
CALL("input", "screen.act, screen.read", "drag_in(window, x1, y1, x2, y2): drags from one place in a window to another, the same way as "
     "click_in", input_drag_in)
CALL("input", "screen.act, screen.read", "scroll_in(window, amount, x=0.5, y=0.5) -> {x,y}: scrolls at a place in a window, the middle "
     "unless given, positive up", input_scroll_in)
CALL("input", "screen.read", "position_in(window, x, y) -> {x,y}: the place on screen of a place in a window, without touching "
     "anything", input_where_in)
CALL("look", "screen.read", "screenshot(window=nothing, box=nothing) -> {path,x,y,w,h}: a picture of a window, or the main screen, "
     "saved as a PNG file. box {x,y,w,h} in the window's pixels keeps only that part", look_screenshot)
CALL("look", "screen.read", "elements(window=nothing, box=nothing) -> [{id,kind,name,x,y,w,h}]: everything on screen that can be told "
     "apart, numbered: the controls Windows knows, buttons, links, boxes, tabs, checkboxes and icons, with their names, and every "
     "line of text OCR reads. Places in the window's pixels for input.click_in, or the screen's when no window is given", look_elements)
CALL("look", "screen.read", "overview(window=nothing) -> {app,title,focused,pressable,fields}: a window in brief: what it is, what has "
     "the keyboard, and the names of what can be pressed and typed in. The front window when none is given", look_overview)
CALL("look", "screen.read", "focused(window=nothing) -> " CONTROL "?: the control that has the keyboard now", look_focused)
CALL("look", "screen.read", "page(window=nothing) -> text: every word of a window, from its own text, or read off the screen when it "
     "gives none", look_page)
CALL("look", "screen.read", "lines(window=nothing) -> [text]: the lines of text on screen, top to bottom, read by OCR. The whole screen "
     "when no window is given", look_lines)
CALL("look", "screen.read", "buttons(window=nothing, most=60) -> [{name,role,x,y}]: the buttons, links, tabs, menu items and boxes to "
     "tick, by name, with where to click them", look_pressable)
CALL("look", "screen.read", "fields(window=nothing) -> [{name,role,value,x,y}]: the boxes to type in and the dropdowns, with what is in "
     "them now", look_fields)
CALL("look", "screen.read", "near(words, window=nothing, distance=150) -> [text]: the other lines on screen within distance pixels of "
     "those words, closest first: what a label belongs to, or the value beside it", look_near)
CALL("look", "screen.read", "find_text(words, window=nothing) -> [{text,x,y,w,h}]: every line on screen with those words in it, and where "
     "it is", look_locate)
CALL("look", "screen.read", "open_windows() -> [{app,title}]: the windows open now, front to back, in brief", look_open_windows)
