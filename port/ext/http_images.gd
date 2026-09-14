# http_images.gd -- extends the Level Share Square screens' image loader so
# WebP thumbnails and avatars load (see webp.gd). Applied by switch_port.gd to
# the node running the game's script; anything that is not WebP goes to the
# game's own handler unchanged.
extends "res://scenes/menu/level_portal/http/http_images.gd"

const Webp = preload("res://switch_port/ext/webp.gd")


func request_completed(result: int, response_code: int, headers: PoolStringArray, body: PoolByteArray, url: String):
	var image = Webp.decode(body)
	if image == null:
		.request_completed(result, response_code, headers, body, url)
		return
	# What the game's handler does with an image it could decode.
	var texture := ImageTexture.new()
	texture.create_from_image(image)
	cached_images[url] = texture
	loading = false
	emit_signal("image_loaded", url, texture)
	if image_queue.size() > 0:
		call_deferred("load_next_image")
