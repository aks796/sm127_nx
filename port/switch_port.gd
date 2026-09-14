# switch_port.gd -- sm127_nx's in-game helper for the Switch port (Godot 3.6).
#
# Registered by the wrapper as an autoload in assets/override.cfg. The NRO
# carries this folder (port/) in its romfs and serves it as res://switch_port/,
# so a new NRO brings every change here with it. No game file is changed.
#
# It never changes the game's controls: SM127 lets players remap everything in
# Options > Controls, and those bindings are the player's.
#
# It does what the Android build has no way to do on a Switch:
#
#  * Frame pacing. SM127's project sets debug/settings/fps/force_fps=60, so
#    Godot runs its own sleep-based frame limiter on top of vsync (the wrapper
#    presents with eglSwapInterval 1, always). Two 60 Hz clocks that are never
#    exactly equal drift against each other, and every so often a frame that
#    was ready is held past vblank: a repeated frame, then two physics steps
#    at once, which is the camera stutter. On hardware the smoothest window
#    still averaged 16.75 ms of engine time per frame -- a one-refresh floor --
#    at 1785 MHz. Caps at or above the display rate are cleared so vsync alone
#    paces the game; 30/40/50 chosen in Options are left alone. `fps_limiter 1`
#    in config.txt keeps the game's limiter.
#
#  * A controller cursor, OFF by default and never automatic. SM127's own
#    controller preset already covers gameplay and the menus (A confirms,
#    B backs out, + pauses), so the pause screen does not need a pointer and
#    no longer gets one.
#
#    It remains available for the level editor, which binds LMB and RMB
#    directly and acts on whatever is under the pointer: `cursor 1` in
#    config.txt, then R toggles it. Left stick moves, A/ZL left-click, Y/ZR
#    right-click, L recentres. Handheld, the touchscreen does the same job
#    without it.
#
#  * A level-card guard. A card loads its picture when its VisibilityEnabler2D
#    reports entering the viewport (decoration.gd, one-shot). A card still
#    without a texture half a second after it appears has
#    load_default_thumbnail() called directly -- the same call, idempotent if
#    the signal arrives late -- and the first few cards log what they were
#    given. On hardware the signal has always fired: the black cards were mip
#    chains (see the glGenerateMipmap shim in imports.c), so this is a guard.
#
#  * Pre-loading, so starting a level is not ten seconds of white screen.
#    The first level load of a session paid for ~950 resources at once (515
#    textures, each a WebP decode; 160 scripts; 73 scenes): 10.7 s on hardware,
#    against 1.5 s for a Retry of the same level. Measured in desktop Godot 3.6
#    with these assets, the same shape: 2.0 s cold, 0.27 s warm.
#
#    Pre-loading player.tscn plus every object scene while the player is in the
#    menus took that cold load to 279 ms (the exact per-level file list managed
#    259 ms), for +72 MB. It runs a few milliseconds per frame, leaf
#    dependencies first so no single frame has to load Mario's whole tree, and
#    starts under the game's own boot loading screen, where a hitch goes
#    unnoticed, and pauses whenever a level is running.
#
#    Two things keep it out of the way in the menus. Resources that scripts
#    preload() are invisible to get_dependencies(), so they used to arrive
#    inside one script compile: tilemap_loader.gd preloads SM127's two tilesets,
#    786 ms in a single step on desktop and a 5.6 s freeze on hardware. The larger,
#    generated_tiles.res, is a single TileSet that loads in one indivisible
#    step (466 ms on desktop), so both tilesets are loaded during boot instead,
#    before anything is on screen and on the boot clock. And no work
#    starts within half a second of a button press, so navigating stays smooth
#    and the loading happens in the pauses between. `prewarm 0` in config.txt turns it off.
#
#  * A scene cache (`scene_cache 0` in config.txt turns it off).
#    SM127 ships its scenes as text: 313 .tscn and 335 .tres survive the
#    Android export unconverted, and parsing one at the stock clock costs up to
#    ~0.7 s -- the repeating hitch when the pause menu reopens or a level
#    reloads. Keeping every instanced scene loaded removes that.
#
#    It is off by default because a held PackedScene also holds references to
#    every texture and resource it points at, so the cache pins a level's whole
#    asset set in memory for the rest of the session. On a console where a
#    level load is already the heaviest allocation the game makes, trading
#    memory for load time is the wrong way round until it has been measured on
#    hardware.
#
#  * Online. SM127's Level Share Square screens (browsing, sign-in, ratings,
#    comments, thumbnails) are HTTPRequests, which reach the network through
#    the wrapper's socket layer (net_shim.c). Two things here help them:
#
#    Each HTTPRequest runs on a worker thread (`http_threads 0` in config.txt
#    turns it off). Every request is a new TLS connection, and Godot computes
#    the handshake's key exchange and certificate checks inside one poll --
#    on the frame thread, a hitch per page and per thumbnail. request_completed
#    still arrives on the main thread, so the game's handlers are unchanged.
#
#    And each text field says what it is as it takes focus, through an
#    environment variable the wrapper intercepts, so the system keyboard masks
#    the password and offers @ for the email address. What the keyboard returns
#    comes back the same way and is set as the field's text here: typed in as
#    key events it could only ever be added to, never replace (_kbd_poll).
#
#    The screens' text fields get Godot's clear button, and the sign-in fields
#    are emptied each time the sign-in screen opens (_on_login_opened).
#
#    With a controller, SM127's own cursor clicks on those screens, and its
#    click on a text field never let go of the button, so every later click
#    went to that field (_release_cursor_click). When the keyboard closes, the
#    field gives focus back to the cursor, and OK in the search and page boxes
#    presses the button beside them (_kbd_poll).
#
#    Outcomes are logged by status and size, never by URL or content.
#
#  * Fixes to game scripts, without changing a game file:
#
#    Shine select moved two shines per stick push. It checks
#    Input.is_action_just_pressed() inside _input(), which stays true for every
#    event of the frame the action was pressed in, and a stick push is usually
#    an X-axis and a Y-axis event in the same frame. Its _input is switched off
#    and called from here at most once per frame (_forward_shine_input).
#
#    Level Share Square's thumbnails and avatars are WebP, which SM127 0.9.1
#    cannot decode; two small extensions of its image loaders do (ext/).
#
#    Level Share Square's page buttons could not be pressed with a controller:
#    SM127's cursor there takes focus back as A is released, which cancels the
#    press of any button that took focus. Those buttons' focus is switched off
#    (_under_portal).
#
#    Play and Save on a Level Share Square level re-encoded the whole level to
#    add the page's thumbnail address to it, which froze the Switch for 20 s on
#    a 674 KB level; the start of the code is rewritten instead
#    (ext/level_panel.gd).
#
#    The main menu's Discord button is hidden: Discord cannot be used in the
#    Switch's browser.
#
#  * Diagnostics, printed to sm127_debug.log: which scenes were instanced
#    during any frame over 100 ms, and every 30 s the average process /
#    physics time and draw calls.
#
# Settings come from user://sm127_nx.cfg, which the wrapper rewrites from
# config.txt at every boot.
extends CanvasLayer

const DEVICE = 0
# Godot 3 joypad indices, as the wrapper sends them (main.c s_btnmap), in
# Switch label order: A confirms, B backs out.
const JOY_A = 0
const JOY_Y = 3
const JOY_L = 4
const JOY_R = 5
const JOY_ZL = 6
const JOY_ZR = 7
const DEADZONE = 0.2
const SPEED_432P = 760.0      # px/s at full tilt, scaled with the window
const BASE_HEIGHT = 432.0     # SM127's design resolution is 768x432
const EXTERNAL_HEIGHT_720P = 40.0

var _enabled = true
var _manual = false
var _shown = false
var _placed = false
var _prev_r = false
var _prev_l = false
var _left = false
var _right = false
var _pos = Vector2.ZERO
var _sprite = null

var _scene_cache = {}          # filename -> PackedScene, held for the session
var _cache_scenes = true
# Every PERF_TRACE_S in a level: Mario's height, fps, draw calls, items. The
# wrapper writes a [pace] line (engine time, GL wait) at the same cadence into
# the same log. Reported on hardware: the game slows the higher Mario climbs.
# On desktop neither the view (draw calls fall from 101 to 71 going up Tutorial
# Hills) nor Mario's own queries (short fixed-length rays) reproduce that, so
# the next hardware log has to say which it is.
const PERF_TRACE_S = 5.0
var _trace_t = 0.0
var _trace_char = null
# Frame breakdown for the trace (ext/frame_mark.gd). Godot runs every physics
# tick a frame is behind before idle processing, up to 8 per frame: once one
# tick costs close to 16.7 ms, each slow frame asks for more ticks in the next,
# and a level locks at 133 ms a frame -- exactly what a large community level
# did on hardware at the stock clock. The tick count per frame shows that.
var _fm_tick_t0 = 0
var _fm_ticks = 0
var _fm_idle_t0 = 0
var _fm_idle_end = 0
var _fm_phys_us = 0
var _fm_proc_us = 0
var _fm_other_us = 0
var _fm_frames = 0
var _fm_ticks_total = 0

const DISPLAY_HZ = 60
var _keep_limiter = false
var _limiter_logged = false

# Online; see _on_http_request and _on_focus_changed. Indexed by HTTPRequest.Result.
const HTTP_RESULTS = ["ok", "chunked body size mismatch", "can't connect", "can't resolve",
	"connection error", "TLS handshake error", "no response", "body over size limit",
	"request failed", "can't open download file", "can't write download file",
	"too many redirects", "timed out"]
const HTTP_OK_REPORTS = 10             # failures are always logged
const KBD_HINT_ENV = "SM127NX_KBD"     # read by the wrapper's keyboard, swkbd_shim.c
const KBD_TEXT_ENV = "SM127NX_KBD_TEXT" # written by it: the result, as hex UTF-8
const KBD_SEQ_ENV = "SM127NX_KBD_SEQ"   # changes with every result
const KBD_STATUS_ENV = "SM127NX_KBD_STATUS" # "ok" or "cancel"
var _http_threads = true
var _http_ok_reported = 0
var _kbd_hint = ""
var _kbd_target = null                 # weakref: the text field being edited
var _kbd_seq = ""

const PORTAL_SCENE = "res://scenes/menu/level_portal/level_portal.tscn"
const LSS_CURSOR_SCRIPT = "res://scenes/menu/level_portal/cursor.gd"
var _portal_root = null                # weakref: the Level Share Square screens
var _lss_cursor = null                 # weakref: SM127's cursor on those screens
var _cursor_click_frame = -1           # frame of that cursor's last left press
var _clear_fields = []                 # weakrefs: fields with a clear button
var _swallow_release = false

const MAIN_MENU_SCENE = "res://scenes/menu/main_menu/main_menu.tscn"

const SHINE_PARENT_SCRIPT = "res://scenes/menu/shine_select/shine_parent.gd"
const SHINE_ACTIONS = ["ui_right", "ui_left", "ui_accept", "ui_cancel"]
var _shine_parent = null               # weakref
var _shine_frame = -1

# Game script -> port script that extends it; see _extend_script.
const SCRIPT_EXTENSIONS = {
	"res://scenes/menu/level_portal/http/http_images.gd": "res://switch_port/ext/http_images.gd",
	"res://scenes/menu/levels_list/misc/http_thumbnails.gd": "res://switch_port/ext/http_thumbnails.gd",
	"res://scenes/menu/level_portal/level_panel.gd": "res://switch_port/ext/level_panel.gd",
}

# Resources that scripts preload(), which get_dependencies() cannot see, so they
# used to arrive inside one blocking script compile (tilemap_loader.gd: 786 ms
# on desktop, a 5.6 s freeze in the menus on hardware). generated_tiles.res is a
# single TileSet whose load cannot be split, so these are loaded synchronously
# in _ready(), during boot, where the wait is expected -- see _prewarm_seed_now.
const PREWARM_SEEDS = [
	"res://assets/tiles/ids.tres",
	"res://generation/tileset_palettes.res",
	"res://assets/tiles/tiles.tres",
	"res://generation/generated_tiles.res",
]
const PREWARM_ROOTS = ["res://scenes/player/player.tscn"]
const PREWARM_QUIET_MS = 500          # no work this soon after a button press
const PREWARM_REPORT_USEC = 100000    # log any single step slower than this
const PREWARM_OBJECTS = "res://scenes/actors/objects"
const PREWARM_BUDGET_USEC = 12000     # per frame; one texture can overrun it
const PREWARM_DELAY_S = 1.0      # under the game's boot loading screen
var _prewarm_enabled = true
var _prewarm_started = false
var _prewarm_done = false
var _prewarm_paused_logged = false
var _prewarm_stack = []               # [path, dependencies_already_queued]
var _prewarm_seen = {}
var _prewarm_loader = null
var _prewarm_path = ""
var _last_input_ms = -100000
var _prewarm_held = []                # keeps everything in Godot's resource cache
var _prewarm_loaded = 0
var _prewarm_objects = 0
var _prewarm_t0 = 0
var _uptime = 0.0

var _cards = []                # level cards waiting for their thumbnail
var _cards_filled = 0
var _cards_reported = false
const CARD_GRACE_FRAMES = 30   # ~0.5 s: long enough for the signal to arrive

var _instanced = []            # scene files instanced since the last frame
const MAX_REPORTED = 12        # a level load instances thousands in one frame
var _instanced_total = 0
var _frame_usec = 0            # when _stats last ran, by the clock
var _prewarm_note = ""         # what pre-loading worked on since then
var _stat_frames = 0
var _stat_process = 0.0
var _stat_physics = 0.0
var _stat_items = 0.0
var _stat_calls = 0.0
var _stat_worst = 0.0
var _stat_time = 0.0


func _ready():
	layer = 128
	pause_mode = Node.PAUSE_MODE_PROCESS

	var cfg = ConfigFile.new()
	var ok = cfg.load("user://sm127_nx.cfg") == OK
	_enabled = bool(cfg.get_value("port", "cursor", true)) if ok else true
	var image_path = str(cfg.get_value("port", "cursor_image", "")) if ok else ""
	var height = float(cfg.get_value("port", "cursor_height", 0)) if ok else 0.0
	_cache_scenes = bool(cfg.get_value("port", "scene_cache", true)) if ok else true
	_prewarm_enabled = bool(cfg.get_value("port", "prewarm", true)) if ok else true
	_keep_limiter = bool(cfg.get_value("port", "fps_limiter", false)) if ok else false
	_http_threads = bool(cfg.get_value("port", "http_threads", true)) if ok else true
	_limiter_tick()
	if _prewarm_enabled:
		_prewarm_seed_now()
	get_tree().connect("node_added", self, "_on_node_added")
	get_viewport().connect("gui_focus_changed", self, "_on_focus_changed")
	var mark_script = load("res://switch_port/ext/frame_mark.gd")
	if mark_script != null:
		for last in [false, true]:
			var mark = Node.new()
			mark.set_script(mark_script)
			mark.last = last
			add_child(mark)

	_sprite = Sprite.new()
	_sprite.centered = false
	_sprite.visible = false
	add_child(_sprite)
	_setup_texture(image_path, height)


func _setup_texture(image_path, height):
	var window_h = max(OS.window_size.y, 1.0)
	if image_path != "":
		var img = Image.new()
		if img.load(image_path) == OK:
			var tex = ImageTexture.new()
			tex.create_from_image(img, Texture.FLAG_FILTER)
			_sprite.texture = tex
			var want_h = (height if height > 0 else EXTERNAL_HEIGHT_720P) * window_h / 720.0
			var s = want_h / max(tex.get_height(), 1)
			_sprite.scale = Vector2(s, s)
			return
	# The game's own cursor, at the game's pixel scale.
	var path = ProjectSettings.get_setting("display/mouse_cursor/custom_image")
	if path == null or str(path) == "":
		path = "res://assets/misc/cursor.png"
	var tex = load(str(path))
	if tex == null:
		_enabled = false
		return
	_sprite.texture = tex
	var hotspot = ProjectSettings.get_setting("display/mouse_cursor/custom_image_hotspot")
	if hotspot is Vector2:
		_sprite.offset = -hotspot
	var s = max(floor(window_h / BASE_HEIGHT), 1.0)
	if height > 0:
		s = height * window_h / 720.0 / max(tex.get_height(), 1)
	_sprite.scale = Vector2(s, s)


func _on_node_added(node):
	# The old options screen's FPS control (scenes/player/fps_cap.gd) shows the
	# cap by reading Engine.target_fps in its _ready, which runs after this
	# signal. Hand it the value the game set so it does not display 0; the next
	# _limiter_tick clears it again. The current options menu reads LocalSettings
	# and is unaffected either way.
	if not _keep_limiter and Engine.target_fps == 0 and node.has_method("increase_value") \
			and node.has_method("decrease_value") and node.get("fps_cap") != null:
		Engine.target_fps = DISPLAY_HZ

	# Decoration is the level card's picture; see _check_cards().
	if node.name == "Decoration" and node.has_method("load_default_thumbnail"):
		_cards.append([node, 0])

	_extend_script(node)
	# The main menu's Discord button opened Discord's invite page in the
	# Switch's browser, where Discord cannot be used. The wrapper does not open
	# Discord links either (web_applet.c); the editor's help pages have them.
	if node.name == "Discord" and node is BaseButton and node.owner != null \
			and node.owner.filename == MAIN_MENU_SCENE:
		node.visible = false
	if node.filename == PORTAL_SCENE:
		_portal_root = weakref(node)
	elif node is BaseButton and node.focus_mode != Control.FOCUS_NONE and _under_portal(node):
		node.focus_mode = Control.FOCUS_NONE
	if node is HTTPRequest:
		_on_http_request(node)
	# Not the 95 px go-to-page box: the button would take a third of it.
	if node is LineEdit and _in_portal(node) and node.get_parent().name != "GoToPage":
		_add_clear_button(node)
	elif node.name == "Login" and node.has_signal("screen_opened") and _in_portal(node):
		node.connect("screen_opened", self, "_on_login_opened", [node])
	var script = node.get_script()
	if script != null and script.resource_path == SHINE_PARENT_SCRIPT:
		node.connect("ready", self, "_on_shine_parent_ready", [node], CONNECT_ONESHOT)
	elif script != null and script.resource_path == LSS_CURSOR_SCRIPT:
		_lss_cursor = weakref(node)

	var fn = node.filename
	if fn == "":
		return
	if _instanced.size() < MAX_REPORTED:
		_instanced.append(fn)
	_instanced_total += 1
	if _cache_scenes and not _scene_cache.has(fn):
		# Already in Godot's resource cache while it is being instanced, so
		# this is a lookup, not a load; holding it keeps it there.
		_scene_cache[fn] = load(fn)


# Swap a game script for the port script that extends it (SCRIPT_EXTENSIONS).
# node_added comes after _enter_tree and before _ready, so no onready variable
# has been filled yet; script variables the scene itself set are carried over.
func _extend_script(node):
	var script = node.get_script()
	if script == null or not SCRIPT_EXTENSIONS.has(script.resource_path):
		return
	var extension = load(SCRIPT_EXTENSIONS[script.resource_path])
	if extension == null:
		return
	var kept = {}
	for p in node.get_property_list():
		if p.usage & PROPERTY_USAGE_SCRIPT_VARIABLE:
			kept[p.name] = node.get(p.name)
	node.set_script(extension)
	for name in kept:
		node.set(name, kept[name])


func _in_portal(node):
	return node.owner != null and node.owner.filename == PORTAL_SCENE


# With a controller, Level Share Square is driven by SM127's own cursor
# (cursor.gd; the screens' focus navigation is disabled in the scene). That
# cursor takes keyboard focus back on every input event, and a Button that loses
# focus while held cancels its press. A down: the click lands and the button
# takes focus. A up: the cursor takes focus back first -- press cancelled -- and
# the click release that follows has nothing to release. So the page buttons, the
# rating button and the description's button never fired from a controller; the
# level cards and the other buttons, which already had focus_mode 0, did. Focus
# does nothing for these buttons, so it is switched off, including on the page
# buttons the game creates for each page. Reproduced in desktop Godot 3.6: page 2
# loads with it off and not with it on.
func _under_portal(node):
	var root = _portal_root.get_ref() if _portal_root != null else null
	return root != null and root.is_a_parent_of(node)


# The x Godot draws at the right end of a field that has text. A press on it
# clears the field here, before the GUI sees the press: let through, it would
# focus the field first, and focusing a field opens the system keyboard.
func _add_clear_button(field):
	field.clear_button_enabled = true
	_clear_fields.append(weakref(field))


func _clear_button_under(pos):
	var i = _clear_fields.size() - 1
	while i >= 0:
		var field = _clear_fields[i].get_ref()
		if field == null:
			_clear_fields.remove(i)
		elif field.is_visible_in_tree() and field.editable and field.text != "":
			var local = field.get_global_transform_with_canvas().affine_inverse().xform(pos)
			var reach = field.get_icon("clear").get_width() + field.get_stylebox("normal").get_offset().x
			if local.y >= 0.0 and local.y < field.rect_size.y and local.x < field.rect_size.x \
					and local.x > field.rect_size.x - reach:
				return field
		i -= 1
	return null


# What was typed into the sign-in form otherwise stays for the whole session,
# including after leaving Level Share Square. A failed attempt keeps it: that
# stays on the same screen, so the details can be corrected.
func _on_login_opened(screen):
	for name in ["Email", "Password"]:
		var field = screen.find_node(name, true, false)
		if field is LineEdit:
			field.clear()


# SM127's shine select checks Input.is_action_just_pressed() inside _input(),
# which is true for every input event of the frame the action was pressed in.
# A stick push is usually an X-axis and a Y-axis event in the same frame (a
# thumb never pushes perfectly straight), so it moved two shines. Its own input
# callback is switched off; _forward_shine_input calls it at most once a frame,
# and only when one of its actions was just pressed -- the only case in which
# it does anything.
func _on_shine_parent_ready(node):
	node.set_process_input(false)
	_shine_parent = weakref(node)
	_shine_frame = -1


func _forward_shine_input(event):
	var node = _shine_parent.get_ref()
	if node == null or not node.is_inside_tree():
		_shine_parent = null
		return
	var frame = Engine.get_idle_frames()
	if frame == _shine_frame or not node.can_process():
		return
	for action in SHINE_ACTIONS:
		if Input.is_action_just_pressed(action):
			_shine_frame = frame
			node._input(event)
			return


# SM127's cursor on the Level Share Square screens (cursor.gd) clicks by turning
# A into a left press and release. A press on a text field gives the field focus,
# and while a text field has focus the cursor ignores every button but B -- A's
# release included. The press then never ends: Godot keeps sending every later
# press to the button it holds, so from then on each A with the cursor, on
# Featured, the search button or the password field, landed on that one text
# field again and opened its keyboard. Reproduced in desktop Godot 3.6. Here the
# release is sent in the same frame; the field asks for the keyboard again on
# it, which the wrapper ignores while that keyboard is already on its way.
func _release_cursor_click():
	var cursor = _lss_cursor.get_ref() if _lss_cursor != null else null
	var ev = InputEventMouseButton.new()
	ev.device = -1
	ev.button_index = BUTTON_LEFT
	ev.pressed = false
	if cursor != null:
		ev.position = cursor.get_event_pos()
		ev.global_position = ev.position
	Input.parse_input_event(ev)


# The system keyboard's result, handed over by the wrapper (swkbd_shim.c), set
# as the text of the field that asked for it, with the caret at the end.
#
# Then the keyboard is finished with the field, as Android's is after its Done
# key: OK arrives as Enter would (text_entered), Level Share Square's search and
# page boxes -- which have a button beside them and nothing on Enter -- press
# that button, and focus leaves the field. Left there, the cursor stayed frozen
# until B, the only button it listens to while a text field has focus.
func _kbd_poll():
	var seq = OS.get_environment(KBD_SEQ_ENV)
	if seq == _kbd_seq:
		return
	_kbd_seq = seq
	var field = _kbd_target.get_ref() if _kbd_target != null else null
	if field == null:
		print("[port] keyboard: the text field that asked for it is gone")
		return
	if OS.get_environment(KBD_STATUS_ENV) == "cancel":
		_kbd_done(field, "cancelled")
		return
	var hex = OS.get_environment(KBD_TEXT_ENV)
	var bytes = PoolByteArray()
	var i = 0
	while i + 1 < hex.length():
		bytes.append(("0x" + hex.substr(i, 2)).hex_to_int())
		i += 2
	var text = bytes.get_string_from_utf8()
	if field is LineEdit:
		if field.text != text:
			field.text = text
			field.emit_signal("text_changed", text)
		field.caret_position = text.length()
	elif field is TextEdit:
		if field.text != text:
			field.text = text
			field.emit_signal("text_changed")
		var last = field.get_line_count() - 1
		field.cursor_set_line(last)
		field.cursor_set_column(field.get_line(last).length())
	var how = "OK"
	if field is LineEdit:
		field.emit_signal("text_entered", field.text)
		if is_instance_valid(field) and _submit_portal_field(field):
			how = "OK, submitted"
	_kbd_done(field, how)


func _submit_portal_field(field):
	if field.name != "Query" or not _under_portal(field):
		return false
	var button = field.get_parent().get_node_or_null("Search")
	if not (button is BaseButton) or button.disabled or not button.is_visible_in_tree():
		return false
	button.emit_signal("pressed")
	return true


# What B does on those screens (cursor.gd): the field lets go of focus and the
# cursor takes it. A handler that moved focus somewhere on purpose is left alone.
func _kbd_done(field, how):
	if is_instance_valid(field) and field.has_focus():
		field.release_focus()
	var cursor = _lss_cursor.get_ref() if _lss_cursor != null else null
	if cursor != null and cursor.is_visible_in_tree() and cursor.get_focus_owner() == null:
		cursor.grab_focus()
	print("[port] keyboard %s" % how)


# node_added fires before the node's _ready, so before any request() call.
# use_threads can only change while the client is idle; a node re-entering the
# tree mid-request keeps what it has.
func _on_http_request(node):
	if _http_threads and not node.use_threads \
			and node.get_http_client_status() == HTTPClient.STATUS_DISCONNECTED:
		node.use_threads = true
	if not node.is_connected("request_completed", self, "_on_http_completed"):
		node.connect("request_completed", self, "_on_http_completed", [node.name])


# Status and size only: URLs carry level and account ids, bodies carry tokens.
func _on_http_completed(result, code, _headers, body, who):
	var ok = result == HTTPRequest.RESULT_SUCCESS and code >= 200 and code < 400
	if ok:
		if _http_ok_reported >= HTTP_OK_REPORTS:
			return
		_http_ok_reported += 1
	var what = HTTP_RESULTS[result] if result >= 0 and result < HTTP_RESULTS.size() else str(result)
	var more = " (later successes are not logged)" if ok and _http_ok_reported == HTTP_OK_REPORTS else ""
	print("[port] http %s: %s, HTTP %d, %d bytes%s" % [who, what, code, body.size(), more])


# Viewport emits gui_focus_changed before the control's focus notification,
# which is where Godot asks for the keyboard (order checked in desktop 3.6).
func _on_focus_changed(control):
	if _cursor_click_frame == Engine.get_idle_frames() and (control is LineEdit or control is TextEdit) \
			and _under_portal(control):
		_cursor_click_frame = -1
		call_deferred("_release_cursor_click")
	var hint = ""
	_kbd_target = weakref(control) if (control is LineEdit or control is TextEdit) else null
	if control is LineEdit:
		var guide = control.placeholder_text
		if control.secret:
			hint = "password|" + guide
		elif "mail" in guide.to_lower():
			hint = "email|" + guide
		else:
			hint = "text|" + guide
	elif control is TextEdit:
		hint = "multiline|"
	if hint != _kbd_hint:
		_kbd_hint = hint
		OS.set_environment(KBD_HINT_ENV, hint)


# The tilesets, synchronously. This runs inside Godot's Main::start, before the
# first frame is presented and while the CPU is still on the boot clock, so the
# seconds it costs on hardware are added to the boot wait rather than frozen
# into a menu. Held, so the interactive walk later finds them cached.
func _prewarm_seed_now():
	var t = OS.get_ticks_msec()
	var n = 0
	for p in PREWARM_SEEDS:
		if ResourceLoader.exists(p):
			var r = load(p)
			if r != null:
				_prewarm_held.append(r)
				n += 1
	print("[port] prewarm: %d tileset resources loaded during boot in %d ms" % [n, OS.get_ticks_msec() - t])


# Queue the roots: the player first (Mario alone is ~280 of the files a level
# load reads, the same for every level), then every object scene, sorted.
func _prewarm_build():
	var roots = []
	for r in PREWARM_SEEDS:
		if ResourceLoader.exists(r):
			roots.append(r)
	for r in PREWARM_ROOTS:
		roots.append(r)
	var objs = []
	var left = []
	var d := Directory.new()
	if d.open(PREWARM_OBJECTS) == OK:
		d.list_dir_begin(true, true)
		var n = d.get_next()
		while n != "":
			var p = "%s/%s/%s.tscn" % [PREWARM_OBJECTS, n, n]
			if ResourceLoader.exists(p):
				if _makes_noise_on_load(p):
					left.append(n)
				else:
					objs.append(p)
			n = d.get_next()
		d.list_dir_end()
	objs.sort()
	if left.size() > 0:
		print("[port] prewarm: %s left to the levels that use them (noise textures)" % str(left))
	_prewarm_objects = objs.size()
	roots += objs
	# Reverse onto a stack so the player is expanded first.
	for i in range(roots.size() - 1, -1, -1):
		_prewarm_stack.append([roots[i], false])


# toxic_gas.tscn holds seven NoiseTextures, 1024 px and seamless, and loading it
# builds all seven in one frame: 654 ms on desktop. On hardware, the only stall
# in the menus nothing else explained -- 3.7 s -- came just before pre-loading
# finished, where this scene falls. A scene like that is left out, and loads with
# a level that uses it, behind the level's transition.
func _makes_noise_on_load(path):
	var f = File.new()
	if f.open(path, File.READ) != OK:
		return false
	var text = f.get_buffer(f.get_len()).get_string_from_utf8()
	f.close()
	return text.find('type="NoiseTexture"') >= 0


# One frame's worth. Depth-first, dependencies before the resource that needs
# them: by the time a scene itself is loaded everything it points at is already
# cached, so it parses in small steps instead of one long frame.
func _prewarm_step():
	var start = OS.get_ticks_usec()
	while OS.get_ticks_usec() - start < PREWARM_BUDGET_USEC:
		if _prewarm_loader != null:
			_prewarm_note = _prewarm_path
			var t_poll = OS.get_ticks_usec()
			var err = _prewarm_loader.poll()
			_prewarm_report("step", _prewarm_path, OS.get_ticks_usec() - t_poll)
			if err == ERR_FILE_EOF:
				var res = _prewarm_loader.get_resource()
				if res != null:
					_prewarm_held.append(res)
					_prewarm_loaded += 1
				_prewarm_loader = null
			elif err != OK:
				_prewarm_loader = null
			continue

		if _prewarm_stack.empty():
			_prewarm_done = true
			print("[port] prewarm: done, %d resources in %.1f s" % [
				_prewarm_loaded, (OS.get_ticks_msec() - _prewarm_t0) / 1000.0])
			return

		var item = _prewarm_stack.pop_back()
		var path = item[0]
		_prewarm_note = path
		if item[1]:
			if not ResourceLoader.has_cached(path):
				var t_open = OS.get_ticks_usec()
				_prewarm_loader = ResourceLoader.load_interactive(path)
				_prewarm_path = path
				_prewarm_report("open", path, OS.get_ticks_usec() - t_open)
			continue
		if _prewarm_seen.has(path):
			continue
		_prewarm_seen[path] = true
		_prewarm_stack.append([path, true])
		var t_deps = OS.get_ticks_usec()
		var deps = ResourceLoader.get_dependencies(path)
		_prewarm_report("dependency scan", path, OS.get_ticks_usec() - t_deps)
		for dep in deps:
			var dp = str(dep).split("::")[0]
			if dp.begins_with("res://") and not _prewarm_seen.has(dp):
				_prewarm_stack.append([dp, false])


# Any single piece of pre-loading slow enough to be felt, by name, so the next
# log says exactly what to split up rather than just that a frame was slow.
func _prewarm_report(what, path, usec):
	if usec > PREWARM_REPORT_USEC:
		print("[port] prewarm: %d ms in one %s: %s" % [usec / 1000, what, path])


func _input(event):
	if event is InputEventMouseButton and event.button_index == BUTTON_LEFT:
		# cursor.gd marks its clicks device -1; see _release_cursor_click.
		_cursor_click_frame = Engine.get_idle_frames() if event.pressed and event.device == -1 else -1
		if event.pressed:
			var field = _clear_button_under(event.position)
			if field != null:
				field.clear()
				_swallow_release = true
				get_tree().set_input_as_handled()
				return
		elif _swallow_release:
			_swallow_release = false
			get_tree().set_input_as_handled()
			return
	if _shine_parent != null:
		_forward_shine_input(event)
	if (event is InputEventJoypadButton and event.pressed) \
			or (event is InputEventJoypadMotion and abs(event.axis_value) > 0.5) \
			or (event is InputEventScreenTouch and event.pressed) \
			or (event is InputEventKey and event.pressed):
		_last_input_ms = OS.get_ticks_msec()


func _prewarm_tick():
	var cs = get_tree().current_scene
	var in_level = cs != null and (cs.name == "Player" or cs.name == "Editor")
	if in_level:
		# Never compete with gameplay; carry on next time the menus are up.
		if _prewarm_started and not _prewarm_paused_logged:
			_prewarm_paused_logged = true
			print("[port] prewarm: a level started with %d resources pre-loaded; pausing" % _prewarm_loaded)
		return
	if _uptime < PREWARM_DELAY_S:
		return
	if not _prewarm_started:
		_prewarm_started = true
		_prewarm_t0 = OS.get_ticks_msec()
		_prewarm_build()
		print("[port] prewarm: pre-loading the player and %d object scenes in the menus" % _prewarm_objects)
	_prewarm_paused_logged = false
	if OS.get_ticks_msec() - _last_input_ms < PREWARM_QUIET_MS:
		return
	_prewarm_step()


func _mark_tick():
	if _fm_ticks == 0:
		_fm_tick_t0 = OS.get_ticks_usec()
	_fm_ticks += 1


func _mark_idle(last):
	var now = OS.get_ticks_usec()
	if not last:
		_fm_idle_t0 = now
		if _fm_ticks > 0:
			_fm_phys_us += now - _fm_tick_t0
			if _fm_idle_end > 0:
				_fm_other_us += _fm_tick_t0 - _fm_idle_end
		elif _fm_idle_end > 0:
			_fm_other_us += now - _fm_idle_end
		return
	_fm_proc_us += now - _fm_idle_t0
	_fm_idle_end = now
	_fm_frames += 1
	_fm_ticks_total += _fm_ticks
	_fm_ticks = 0


func _frame_split():
	if _fm_frames == 0:
		return ""
	var n = float(_fm_frames)
	var s = ", per frame: physics %.1f ms over %.1f ticks, process %.1f ms, render+present %.1f ms" % [
		_fm_phys_us / 1000.0 / n, _fm_ticks_total / n, _fm_proc_us / 1000.0 / n, _fm_other_us / 1000.0 / n]
	_fm_phys_us = 0
	_fm_proc_us = 0
	_fm_other_us = 0
	_fm_frames = 0
	_fm_ticks_total = 0
	return s


func _perf_trace(delta):
	_trace_t += delta
	if _trace_t < PERF_TRACE_S:
		return
	_trace_t = 0.0
	var split = _frame_split()
	var cs = get_tree().current_scene
	if cs == null or cs.name != "Player":
		_trace_char = null
		return
	if _trace_char == null or not is_instance_valid(_trace_char):
		_trace_char = cs.find_node("Character", true, false)
		if _trace_char == null:
			return
	var y = _trace_char.global_position.y
	var where = "y %d" % int(y)
	# Height above the bottom of the current area, when the game says where that is.
	var singleton = get_node_or_null("/root/Singleton")
	var cld = singleton.get("CurrentLevelData") if singleton != null else null
	if cld != null and cld.get("level_data") != null and cld.get("area") != null:
		var areas = cld.level_data.get("areas")
		if areas != null and cld.area < areas.size() and areas[cld.area].get("settings") != null:
			var bounds = areas[cld.area].settings.get("bounds")
			if bounds is Rect2:
				where = "height %d px" % int(bounds.end.y * 32.0 - y)
	print("[port] trace: %s, %d fps, 2D draw calls %d, items %d, nodes %d%s" % [
		where, Engine.get_frames_per_second(),
		int(Performance.get_monitor(Performance.RENDER_2D_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_2D_ITEMS_IN_FRAME)),
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)), split])


# Clear a frame cap that only duplicates vsync. Checked every frame because the
# game re-applies it whenever the FPS option is loaded or changed.
func _limiter_tick():
	if _keep_limiter:
		return
	var cap = Engine.target_fps
	if cap >= DISPLAY_HZ:
		Engine.target_fps = 0
		if not _limiter_logged:
			_limiter_logged = true
			print("[port] frame limiter: target_fps %d cleared, vsync paces the game (fps_limiter 1 keeps it)" % cap)


# One line per level card saying what it was actually given, so "the icons are
# black" stops being a guess. Logged for the first few cards only.
var _card_reports = 0

func _report_card(node):
	if _card_reports >= 4:
		return
	_card_reports += 1
	var info = node.get("level_info")
	var thumb = node.get("thumbnail")
	var fore = node.get("foreground")
	var t = null
	var fg = null
	if thumb != null and is_instance_valid(thumb):
		t = thumb.texture
	if fore != null and is_instance_valid(fore):
		fg = fore.texture
	var name_s = "?"
	if info != null:
		name_s = str(info.get("level_name"))
	print("[port] card %-18s sky=%s fg=%s | sky_tex=%s %s | fg_tex=%s %s | fg_vis=%s mod=%s" % [
		name_s,
		("?" if info == null else str(info.get("thumbnail_sky"))),
		("?" if info == null else str(info.get("thumbnail_background"))),
		("NULL" if t == null else t.get_class()),
		("-" if t == null else str(t.get_size())),
		("NULL" if fg == null else fg.get_class()),
		("-" if fg == null else str(fg.get_size())),
		("?" if fore == null else str(fore.visible)),
		("?" if fore == null else str(fore.modulate))])


# Fill in any thumbnail the viewport_entered one-shot did not deliver.
func _check_cards():
	var i = _cards.size() - 1
	while i >= 0:
		var entry = _cards[i]
		var node = entry[0]
		if not is_instance_valid(node):
			_cards.remove(i)
			i -= 1
			continue

		entry[1] += 1
		if entry[1] < CARD_GRACE_FRAMES:
			i -= 1
			continue
		_cards.remove(i)

		_report_card(node)

		# Only when it is genuinely still blank: a card that got its picture
		# from the signal, or from the HTTP thumbnail loader, is left alone.
		var thumb = node.get("thumbnail")
		if thumb != null and is_instance_valid(thumb) and thumb.texture == null \
				and node.get("level_info") != null:
			node.load_default_thumbnail()
			_cards_filled += 1
			if not _cards_reported:
				_cards_reported = true
				print("[port] level card thumbnail was still empty after %d frames; "
					% CARD_GRACE_FRAMES
					+ "filling it in directly (VisibilityEnabler2D.viewport_entered "
					+ "did not fire on this platform)")
		i -= 1


# Slow frames are timed by the clock: Godot's delta here never reported more
# than ~150 ms, and a 20 s freeze on hardware showed up as frames of 141 and
# 123 ms. Pre-loading's part in one is named, for stalls nothing else explains.
func _stats(delta):
	var now = OS.get_ticks_usec()
	var frame = (now - _frame_usec) / 1000000.0 if _frame_usec > 0 else delta
	_frame_usec = now
	var during = (", pre-loading " + _prewarm_note) if _prewarm_note != "" else ""
	_prewarm_note = ""
	_stat_frames += 1
	_stat_time += delta
	_stat_process += Performance.get_monitor(Performance.TIME_PROCESS)
	_stat_physics += Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)
	_stat_items += Performance.get_monitor(Performance.RENDER_2D_ITEMS_IN_FRAME)
	_stat_calls += Performance.get_monitor(Performance.RENDER_2D_DRAW_CALLS_IN_FRAME)
	_stat_worst = max(_stat_worst, frame)
	if frame > 0.1 and _instanced_total > 0:
		# Only the first few names: writing thousands of paths to the SD card on
		# every slow frame of a level load costs more than the frame it reports.
		print("[port] slow frame %d ms%s; %d scenes instanced%s" % [
			int(frame * 1000.0), during, _instanced_total,
			(", first: " + str(_instanced)) if _instanced.size() > 0 else ""])
	elif frame > 0.1:
		print("[port] slow frame %d ms%s" % [int(frame * 1000.0), during])
	_instanced.clear()
	_instanced_total = 0
	if _stat_time >= 30.0:
		var n = float(_stat_frames)
		print("[port] 30s: %.1f fps, process %.2f ms, physics %.2f ms (per-second peaks), 2D items %d, 2D draw calls %d, worst %d ms, nodes %d, scenes cached %d" % [
			n / _stat_time, _stat_process / n * 1000.0, _stat_physics / n * 1000.0,
			int(_stat_items / n), int(_stat_calls / n), int(_stat_worst * 1000.0),
			int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)), _scene_cache.size()])
		_stat_frames = 0
		_stat_time = 0.0
		_stat_process = 0.0
		_stat_physics = 0.0
		_stat_items = 0.0
		_stat_calls = 0.0
		_stat_worst = 0.0


func _process(delta):
	_kbd_poll()
	_stats(delta)
	_limiter_tick()
	_perf_trace(delta)
	_uptime += delta
	if _prewarm_enabled and not _prewarm_done:
		_prewarm_tick()
	# The game writes the thumbnails during its own startup, after this autoload
	# is ready, so poll for them rather than racing it. Gives up after a minute.
	if _cards.size() > 0:
		_check_cards()
	if not _enabled:
		return

	var r = Input.is_joy_button_pressed(DEVICE, JOY_R)
	if r and not _prev_r:
		_manual = not _manual
	_prev_r = r

	var want = _manual   # never automatic: the pause menu is controller-driven
	if want != _shown:
		_show(want)
	if not _shown:
		return

	var size = get_viewport().get_visible_rect().size

	var l = Input.is_joy_button_pressed(DEVICE, JOY_L)
	if l and not _prev_l:
		_move_to(size / 2.0)
	_prev_l = l

	var v = Vector2(Input.get_joy_axis(DEVICE, JOY_AXIS_0), Input.get_joy_axis(DEVICE, JOY_AXIS_1))
	var m = v.length()
	if m > DEADZONE:
		var k = min((m - DEADZONE) / (1.0 - DEADZONE), 1.0)
		_move_to(_pos + v / m * k * k * SPEED_432P * (size.y / 720.0) * delta)

	var left = Input.is_joy_button_pressed(DEVICE, JOY_A) \
		or Input.is_joy_button_pressed(DEVICE, JOY_ZL)
	if left != _left:
		_set_button(BUTTON_LEFT, left)

	var right = Input.is_joy_button_pressed(DEVICE, JOY_Y) \
		or Input.is_joy_button_pressed(DEVICE, JOY_ZR)
	if right != _right:
		_set_button(BUTTON_RIGHT, right)


func _mask():
	var m = 0
	if _left:
		m |= BUTTON_MASK_LEFT
	if _right:
		m |= BUTTON_MASK_RIGHT
	return m


func _show(on):
	_shown = on
	_sprite.visible = on
	if on:
		if not _placed:
			_placed = true
			_pos = get_viewport().get_visible_rect().size / 2.0
		_move_to(_pos)   # let the GUI know where the pointer is
	else:
		# Never leave a button stuck down when the cursor goes away.
		if _left:
			_set_button(BUTTON_LEFT, false)
		if _right:
			_set_button(BUTTON_RIGHT, false)


func _move_to(p):
	var size = get_viewport().get_visible_rect().size
	p.x = clamp(p.x, 0.0, size.x - 1.0)
	p.y = clamp(p.y, 0.0, size.y - 1.0)
	var ev = InputEventMouseMotion.new()
	ev.position = p
	ev.global_position = p
	ev.relative = p - _pos
	ev.button_mask = _mask()
	_pos = p
	_sprite.position = p
	Input.parse_input_event(ev)


func _set_button(index, on):
	if index == BUTTON_LEFT:
		_left = on
	else:
		_right = on
	var ev = InputEventMouseButton.new()
	ev.button_index = index
	ev.pressed = on
	ev.position = _pos
	ev.global_position = _pos
	ev.button_mask = _mask()
	Input.parse_input_event(ev)
