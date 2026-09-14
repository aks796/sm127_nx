# webp.gd -- WebP bodies for SM127's HTTP image loaders (sm127_nx).
#
# Level Share Square now serves level thumbnails and avatars as WebP. SM127
# 0.9.1 decodes a downloaded image as PNG when it has a PNG header and as JPEG
# otherwise, so every WebP one fails ("Image failed to load") -- on every
# platform, not just this one. Godot 3.6 decodes WebP itself; the extensions
# next to this file only route those bodies to it and leave everything else to
# the game's own code.
extends Reference


static func is_webp(body: PoolByteArray) -> bool:
	# "RIFF" .... "WEBP"
	return body.size() >= 12 and body[0] == 0x52 and body[1] == 0x49 and body[2] == 0x46 \
		and body[3] == 0x46 and body[8] == 0x57 and body[9] == 0x45 and body[10] == 0x42 \
		and body[11] == 0x50


# The decoded image, or null when the body is not WebP (or not valid WebP).
static func decode(body: PoolByteArray) -> Image:
	if not is_webp(body):
		return null
	var image := Image.new()
	if image.load_webp_from_buffer(body) != OK:
		return null
	return image
