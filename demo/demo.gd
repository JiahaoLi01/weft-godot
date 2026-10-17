# The demo: a full-screen WeftView with the demo page (res://ui), a turning cube the page labels by an anchor, and the
# game's side of the page's messages (a click it acknowledges, a call it completes, its state as a view model). Clicks
# the page does not take reach _unhandled_input: they are the game's, and the cube changes colour.
#
# godot --path demo -- --screenshot=<file.png> saves a frame after the page has drawn, then quits.
extends Node3D

@onready var view: WeftView = $Ui/WeftView
@onready var cube: MeshInstance3D = $Cube
@onready var camera: Camera3D = $Camera3D

var cube_material := StandardMaterial3D.new()
var game_clicks := 0
var frames := 0
var counted := 0.0
var fps := 0.0
var shot := ""
var shot_frames := 0


func _ready() -> void:
	cube_material.albedo_color = Color(0.85, 0.55, 0.25)
	cube.material_override = cube_material
	view.message_received.connect(_on_message)
	view.call_received.connect(_on_call)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--screenshot="):
			shot = arg.trim_prefix("--screenshot=")


func _process(delta: float) -> void:
	cube.rotate(Vector3(0.35, 1, 0).normalized(), delta * 0.8)
	frames += 1
	counted += delta
	if counted >= 0.25:
		fps = frames / counted
		frames = 0
		counted = 0.0
		send_state()
	# The label stands on the cube's top, wherever the camera sees it (CSS px are the full-screen View's units).
	var top := cube.global_position + Vector3.UP * 0.9
	var flags := 2 if camera.is_position_behind(top) else 0
	view.write_anchors(PackedInt64Array([1]), PackedVector2Array([camera.unproject_position(top)]),
		PackedFloat32Array([1.0]), PackedFloat32Array([camera.global_position.distance_to(top)]), PackedInt32Array([flags]))
	if shot != "":
		shot_frames += 1
		if shot_frames == 90:
			get_viewport().get_texture().get_image().save_png(shot)
			get_tree().quit()


func _unhandled_input(event: InputEvent) -> void:
	# A click the page did not take (off its elements) is the game's.
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		game_clicks += 1
		cube_material.albedo_color = Color.from_hsv(fmod(game_clicks * 0.17, 1.0), 0.65, 0.95)
		view.emit_event("gameClick", {"count": game_clicks})


func _on_message(name: String, value: Variant) -> void:
	if name == "clicked":
		# The page's button: acknowledged with its count.
		view.emit_event("ack", value)


func _on_call(call_id: int, name: String, _value: Variant) -> void:
	if name == "greet":
		view.complete_call(call_id, true, "Godot %s 回答了" % Engine.get_version_info().string)


func send_state() -> void:
	var hit: Dictionary = view.hit_test(view.get_local_mouse_position())
	view.set_model("game", {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"fps": roundi(fps),
		"scale": view.device_scale,
		"overPage": view.is_pointer_over_page(),
		"cursor": hit.cursor,
		"gameClicks": game_clicks,
	})
