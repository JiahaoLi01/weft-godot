# The extension's tests in a game window on the graphics driver Godot runs (--rendering-driver vulkan or d3d12): a
# WeftView's pixels read back from its texture, its messages, events, view models and calls, pointer input (given and
# through Godot's GUI, with clicks off the page's elements passing to what is under it), url() images, page folders,
# script errors, resizing, and a runtime started again. Prints "FAIL <test>: <what>" for each failed check and quits with
# the number of them (an error of the script itself is Godot's "SCRIPT ERROR", which scripts/RunTests.ps1 counts).
#
#   godot --path <project> --rendering-driver vulkan [--render-thread separate] -s res://test_runner.gd
extends SceneTree

const WIDTH := 64
const HEIGHT := 32
const ROOT := "#root{position:relative;width:100%;height:100%;margin:0}"
const BOX := "document.getElementById('root').appendChild(document.createElement('div')).className = 'a';"
const FOLDER := "user://weft-tests"

var weft: Object
var failures := 0
var current := ""
var messages: Array = []
var diagnostics: Array = []
var before: Dictionary = {}
var backstop_clicks := 0


func _initialize() -> void:
	weft = Engine.get_singleton("Weft")
	# A test that never finishes ends the run.
	create_timer(120.0).timeout.connect(func() -> void:
		printerr("FAIL %s: timed out" % current)
		quit(100))
	_run.call_deferred()


func _run() -> void:
	print("weft-godot tests: weft %s on %s, %s" % [weft.get_build(), RenderingServer.get_current_rendering_driver_name(),
		"the main thread rendering" if RenderingServer.is_on_render_thread() else "a separate render thread"])
	await _draws_the_page_top_row_first()
	await _premultiplied_and_encoded()
	await _messages_events_models_and_calls()
	await _pointer_input_and_hit_tests()
	await _gui_input_passes_off_the_page()
	await _images_by_url()
	await _page_folders()
	await _script_errors_are_diagnostics()
	await _resizes_with_the_control()
	await _runtime_starts_again()
	print("weft-godot tests: %d failures" % failures)
	quit(failures)


# ---- Helpers ----

func begin(name: String) -> void:
	current = name
	messages.clear()
	diagnostics.clear()
	before = weft.get_render_stats()
	print("RUN %s" % name)


func check(condition: bool, what: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL %s: %s" % [current, what])


func frames(count: int) -> void:
	for i in count:
		await process_frame


func make(script: String, css: String, folder := "") -> Control:
	var view: Control = ClassDB.instantiate("WeftView")
	view.size = Vector2(WIDTH, HEIGHT)
	view.message_received.connect(func(name: String, value: Variant) -> void: messages.append({"name": name, "value": value}))
	view.call_received.connect(func(id: int, name: String, value: Variant) -> void:
		messages.append({"name": name, "value": value, "call": id}))
	view.diagnostic_received.connect(func(kind: int, text: String) -> void: diagnostics.append({"kind": kind, "text": text}))
	view.log_diagnostics = false
	view.load_source(script, ROOT + css, folder)
	root.add_child(view)
	return view


# Waits until the View's texture shows a draw (30 frames at most), then two more frames.
func drawn(view: Control) -> void:
	for i in 30:
		await process_frame
		var stats: Dictionary = weft.get_render_stats()
		if view.get_texture().texture_rd_rid.is_valid() and stats.draws > before.draws:
			break
	await frames(2)


func no_diagnostics() -> void:
	check(diagnostics.is_empty(), "diagnostics: %s" % [diagnostics])


# The View's draws since the test began, on Godot's driver, none failed.
func no_failed_draws() -> void:
	var stats: Dictionary = weft.get_render_stats()
	check(stats.api == RenderingServer.get_current_rendering_driver_name(), "weft draws with %s" % stats.api)
	check(stats.failures == before.failures, "a draw failed: %s" % stats.last_failure)
	check(stats.draws > before.draws, "nothing was drawn")


# The texture's pixel (R, G, B, A) at a texel (x right, y down from the top row), read on the render thread (Godot's
# separate render thread too).
func pixel(view: Control, x: int, y: int) -> Array:
	var texture: Texture2D = view.get_texture()
	var rid: RID = texture.texture_rd_rid
	if not rid.is_valid():
		return [-1, -1, -1, -1]
	var read := [PackedByteArray()]
	RenderingServer.call_on_render_thread(func() -> void:
		read[0] = RenderingServer.get_rendering_device().texture_get_data(rid, 0))
	RenderingServer.force_sync()
	var bytes: PackedByteArray = read[0]
	var at := (y * texture.get_width() + x) * 4
	if at < 0 or at + 3 >= bytes.size():
		return [-2, -2, -2, -2]
	# 8-bit BGRA.
	return [bytes[at + 2], bytes[at + 1], bytes[at], bytes[at + 3]]


func near(expected: Array, actual: Array, tolerance: int, what: String) -> void:
	var close := true
	for i in 4:
		close = close and absi(int(expected[i]) - int(actual[i])) <= tolerance
	check(close, "%s: expected %s, got %s" % [what, expected, actual])


func message(name: String) -> Dictionary:
	for m in messages:
		if m.name == name:
			return m
	return {}


func done(view: Control) -> void:
	view.queue_free()
	await frames(2)


# ---- Tests ----

func _draws_the_page_top_row_first() -> void:
	begin("draws_the_page_top_row_first")
	var view := make(BOX, ".a{position:absolute;left:0;top:0;width:32px;height:16px;background-color:#ff0000}")
	await drawn(view)
	no_diagnostics()
	no_failed_draws()
	check(view.get_texture().get_width() == WIDTH and view.get_texture().get_height() == HEIGHT, "the texture is the View's size")
	near([255, 0, 0, 255], pixel(view, 8, 4), 0, "the box at the page's top left")
	near([0, 0, 0, 0], pixel(view, 48, 4), 0, "the page's top right, cleared")
	near([0, 0, 0, 0], pixel(view, 8, 27), 0, "the page's bottom left, cleared")
	await done(view)


func _premultiplied_and_encoded() -> void:
	begin("premultiplied_and_encoded")
	var view := make("const root = document.getElementById('root');" +
		"for (const name of ['a', 'b']) root.appendChild(document.createElement('div')).className = name;",
		".a{position:absolute;left:0;top:0;width:32px;height:16px;background-color:rgb(128,128,128)}" +
		".b{position:absolute;left:32px;top:0;width:32px;height:16px;background-color:rgba(255,255,255,0.5)}")
	await drawn(view)
	no_diagnostics()
	near([128, 128, 128, 255], pixel(view, 8, 8), 1, "grey as weft encodes it (sRGB)")
	near([128, 128, 128, 128], pixel(view, 48, 8), 1, "white at half opacity, premultiplied")
	await done(view)


func _messages_events_models_and_calls() -> void:
	begin("messages_events_models_and_calls")
	var view := make(
		"weft.engine.on('ping', v => weft.engine.send('pong', {n: v.n + 1, half: v.n / 2, list: [1, 'two', true, null]}));" +
		"weft.engine.model('player').subscribe(v => weft.engine.send('model', v));" +
		"weft.engine.on('ask', () => weft.engine.call('add', {a: 1, b: 2}).then(r => weft.engine.send('sum', r)," +
		" e => weft.engine.send('refused', e.message)));" +
		"weft.engine.on('where', v => weft.engine.send('there', v));", "")
	view.call_received.connect(func(id: int, name: String, value: Variant) -> void:
		if name == "add":
			view.complete_call(id, true, value.a + value.b))
	await frames(2)
	check(view.emit_event("ping", {"n": 41}), "emit ping")
	check(view.set_model("player", {"hp": 7, "name": "兵"}), "set the model")
	check(view.emit_event("ask"), "emit ask")
	check(view.emit_event("where", {"at": Vector2(1.5, 2), "tint": Color(1, 0, 0, 1)}), "emit where")
	await frames(4)
	no_diagnostics()
	var pong := message("pong")
	check(not pong.is_empty(), "pong: %s" % [messages])
	if not pong.is_empty():
		check(pong.value.n == 42 and typeof(pong.value.n) == TYPE_INT, "pong's n stays an integer: %s" % [pong.value])
		check(is_equal_approx(pong.value.half, 20.5), "pong's half is a float: %s" % [pong.value])
		check(pong.value.list == [1, "two", true, null], "pong's list: %s" % [pong.value.list])
		check(not pong.has("call"), "pong is no call")
	var model := message("model")
	check(not model.is_empty() and model.value == {"hp": 7, "name": "兵"}, "the model: %s" % [model])
	var add := message("add")
	check(not add.is_empty() and add.has("call") and add.value == {"a": 1, "b": 2}, "the call: %s" % [add])
	var sum := message("sum")
	check(not sum.is_empty() and sum.value == 3, "the call's result: %s" % [messages])
	var there := message("there")
	# A page's number is an integer when it is whole (2.0 comes back as 2).
	check(not there.is_empty() and is_equal_approx(there.value.at.x, 1.5) and is_equal_approx(there.value.at.y, 2.0)
		and is_equal_approx(there.value.tint.r, 1.0) and is_equal_approx(there.value.tint.a, 1.0),
		"vectors and colors as objects: %s" % [there])
	await done(view)


func _pointer_input_and_hit_tests() -> void:
	begin("pointer_input_and_hit_tests")
	var view := make(
		"const b = document.getElementById('root').appendChild(document.createElement('div'));" +
		"b.className = 'button';" +
		"b.addEventListener('click', e => weft.engine.send('clicked', {x: e.clientX, y: e.clientY}));",
		".button{position:absolute;left:8px;top:8px;width:16px;height:16px;background-color:#00f;cursor:pointer}")
	await frames(3)
	var over: Dictionary = view.hit_test(Vector2(16, 16))
	check(over.element, "an element under the button's centre")
	check(over.cursor == "pointer", "its cursor: %s" % over.cursor)
	check(not view.hit_test(Vector2(48, 16)).element, "the root beside it")
	view.send_pointer(0, Vector2(16, 16))
	view.send_pointer(1, Vector2(16, 16), 1)
	view.send_pointer(2, Vector2(16, 16), 0)
	await frames(2)
	no_diagnostics()
	var clicked := message("clicked")
	check(not clicked.is_empty() and clicked.value.x == 16 and clicked.value.y == 16, "a click on the button: %s" % [messages])
	await done(view)


func _gui_input_passes_off_the_page() -> void:
	begin("gui_input_passes_off_the_page")
	var backstop := Control.new()
	backstop.set_anchors_preset(Control.PRESET_FULL_RECT)
	backstop.mouse_filter = Control.MOUSE_FILTER_STOP
	backstop_clicks = 0
	backstop.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.pressed:
			backstop_clicks += 1)
	root.add_child(backstop)
	var view := make(
		"const b = document.getElementById('root').appendChild(document.createElement('div'));" +
		"b.className = 'button';" +
		"b.addEventListener('click', e => weft.engine.send('clicked', {x: e.clientX, y: e.clientY}));",
		".button{position:absolute;left:8px;top:8px;width:16px;height:16px;background-color:#00f;cursor:pointer}")
	view.position = Vector2(100, 50)
	await frames(3)
	click(view.position + Vector2(16, 16))
	await frames(2)
	var clicked := message("clicked")
	check(not clicked.is_empty() and clicked.value.x == 16 and clicked.value.y == 16, "a click on the button: %s" % [messages])
	check(backstop_clicks == 0, "the click on the button stays on the page")
	messages.clear()
	click(view.position + Vector2(48, 16))
	await frames(2)
	check(message("clicked").is_empty(), "no click on the page beside the button")
	check(backstop_clicks == 1, "the click beside the button goes to the Control under the View (%d)" % backstop_clicks)
	no_diagnostics()
	await done(view)
	backstop.queue_free()
	await frames(1)


func click(at: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = at
	motion.global_position = at
	root.push_input(motion)
	for pressed in [true, false]:
		var button := InputEventMouseButton.new()
		button.button_index = MOUSE_BUTTON_LEFT
		button.pressed = pressed
		button.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
		button.position = at
		button.global_position = at
		root.push_input(button)


func _images_by_url() -> void:
	begin("images_by_url")
	var folder := FOLDER + "/images"
	DirAccess.make_dir_recursive_absolute(folder)
	var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color(0, 1, 0, 1))
	check(image.save_png(folder + "/green.png") == OK, "the image is written")
	var view := make("const root = document.getElementById('root');" +
		"for (const name of ['found', 'missing']) root.appendChild(document.createElement('div')).className = name;",
		".found{position:absolute;left:0;top:0;width:16px;height:16px;background-image:url(green.png);background-size:100% 100%}" +
		".missing{position:absolute;left:32px;top:0;width:16px;height:16px;background-image:url(missing.png)}", folder)
	await drawn(view)
	no_failed_draws()
	near([0, 255, 0, 255], pixel(view, 8, 8), 0, "the image the page names")
	var missing := false
	for d in diagnostics:
		missing = missing or (d.kind == 4 and d.text.contains("missing.png"))
	check(missing, "a diagnostic of the missing image: %s" % [diagnostics])
	await done(view)


func _page_folders() -> void:
	begin("page_folders")
	var folder := FOLDER + "/folder"
	DirAccess.make_dir_recursive_absolute(folder)
	write(folder + "/page.json", '{"scripts": ["first.js", "second.js"], "stylesheet": "look.css"}')
	write(folder + "/first.js", "globalThis.boxName = 'a';")
	write(folder + "/second.js", "document.getElementById('root').appendChild(document.createElement('div')).className = globalThis.boxName;")
	write(folder + "/look.css", ROOT + ".a{position:absolute;left:0;top:0;width:32px;height:16px;background-color:#0000ff}")
	var view: Control = ClassDB.instantiate("WeftView")
	view.size = Vector2(WIDTH, HEIGHT)
	view.diagnostic_received.connect(func(kind: int, text: String) -> void: diagnostics.append({"kind": kind, "text": text}))
	view.page = folder
	root.add_child(view)
	await drawn(view)
	no_diagnostics()
	near([0, 0, 255, 255], pixel(view, 8, 4), 0, "the folder's page, its scripts in order")
	await done(view)


func write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()


func _script_errors_are_diagnostics() -> void:
	begin("script_errors_are_diagnostics")
	var view := make("throw new Error('kaput');", "")
	await frames(2)
	var found := false
	for d in diagnostics:
		found = found or (d.kind == 1 and d.text.contains("kaput"))
	check(found, "the script's error: %s" % [diagnostics])
	await done(view)


func _resizes_with_the_control() -> void:
	begin("resizes_with_the_control")
	var view := make(BOX, ".a{position:absolute;left:0;top:0;width:32px;height:16px;background-color:#ff0000}")
	await drawn(view)
	view.device_scale = 2.0
	view.size = Vector2(40, 20)
	before = weft.get_render_stats()
	for i in 30:
		await process_frame
		if view.get_texture().get_width() == 80:
			break
	await frames(2)
	no_diagnostics()
	no_failed_draws()
	check(view.get_texture().get_width() == 80 and view.get_texture().get_height() == 40,
		"the texture is the Control's size at scale 2: %d x %d" % [view.get_texture().get_width(), view.get_texture().get_height()])
	near([255, 0, 0, 255], pixel(view, 16, 8), 0, "the box at scale 2")
	near([0, 0, 0, 0], pixel(view, 70, 10), 0, "beside the box")
	await done(view)


func _runtime_starts_again() -> void:
	begin("runtime_starts_again")
	var first := make(BOX, ".a{position:absolute;left:0;top:0;width:64px;height:32px;background-color:#ff0000}")
	await drawn(first)
	weft.stop()
	check(not weft.is_running(), "the runtime stopped")
	await frames(2)
	# Texture2DRD keeps its texture_rd_rid when it lets go of the texture; its size goes.
	check(first.get_texture().get_width() == 0, "a View of a stopped runtime shows nothing")
	before = weft.get_render_stats()
	check(weft.start() == OK, "the runtime starts again")
	var second := make(BOX, ".a{position:absolute;left:0;top:0;width:64px;height:32px;background-color:#0000ff}")
	second.position = Vector2(0, 40)
	await drawn(second)
	await drawn(first)
	no_diagnostics()
	no_failed_draws()
	near([0, 0, 255, 255], pixel(second, 8, 8), 0, "a new View of the second runtime")
	near([255, 0, 0, 255], pixel(first, 8, 8), 0, "the View in the tree, made again with its page")
	await done(first)
	await done(second)
