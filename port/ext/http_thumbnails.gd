# http_thumbnails.gd -- extends the levels list's thumbnail loader for WebP
# (see webp.gd). Applied by switch_port.gd to the node running the game's script.
#
# That loader also caches each downloaded thumbnail next to its level, naming
# the file .png or .jpg by header -- so a WebP thumbnail was saved as .jpg, and
# Image.load() failed on it every time the list opened. WebP thumbnails are
# saved as PNG instead, and a .jpg cached by an earlier session that is really
# WebP is converted the first time it is needed.
extends "res://scenes/menu/levels_list/misc/http_thumbnails.gd"

const Webp = preload("res://switch_port/ext/webp.gd")


func request_completed(result: int, response_code: int, headers: PoolStringArray, body: PoolByteArray, url: String):
	var image = Webp.decode(body)
	if image == null:
		.request_completed(result, response_code, headers, body, url)
		return
	var texture := ImageTexture.new()
	texture.create_from_image(image)
	cached_images[url] = texture
	loading = false
	emit_signal("image_loaded", url, texture)
	if image_queue.size() > 0:
		call_deferred("load_next_image")

	var level_id: String = ids_queue.pop_front()
	var path: String = level_list_util.get_level_thumbnail_path(level_id, list_handler.working_folder, false)
	if image.save_png(path + ".png") != OK:
		printerr("Error saving level thumbnail as PNG: " + path)


func load_next_image():
	if image_queue.size() > 0 and not loading and ids_queue.size() > 0:
		var cached: String = level_list_util.get_level_thumbnail_path(ids_queue[0], list_handler.working_folder)
		if cached.ends_with(".jpg"):
			_convert_webp_jpg(cached)
	.load_next_image()


func _convert_webp_jpg(path: String):
	var file := File.new()
	if file.open(path, File.READ) != OK:
		return
	var body := file.get_buffer(file.get_len())
	file.close()
	var image = Webp.decode(body)
	if image == null:
		return
	if image.save_png(path.get_basename() + ".png") == OK:
		Directory.new().remove(path)
