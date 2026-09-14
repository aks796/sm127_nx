# ext/level_panel.gd -- Level Share Square's level page (level_panel.gd), with
# Play and Save no longer re-encoding the whole level. Swapped in for the game's
# script by switch_port.gd; the game file is unchanged.
extends "res://scenes/menu/level_portal/level_panel.gd"


# add_info_to_level copies the page's name, author, description and thumbnail
# address into the level wherever the level still has the default, then called
# get_encoded_level_data() to rebuild the level code. That encoder appends to one
# String a piece at a time and every append copies the whole string so far, so
# its cost grows with the square of the level's size: a 674 KB community level,
# whose data had no thumbnail address, took 3.7 s on desktop and froze the Switch
# for 20 s when Play was pressed.
#
# Those four values open the code -- format version, the four percent-encoded,
# then the areas -- so here the opening is written the way the encoder writes it
# and the rest of the code is kept as it came. Codes in an older format still go
# through the game's own path, which also converts them.
func add_info_to_level(page_info: LSSLevelPage) -> LSSLevelPage:
	var level_info: LevelInfo = page_info.level_info
	var code: String = level_info.level_code
	var rest: int = _areas_start(code)
	if rest < 0 or level_info.level_data == null:
		return .add_info_to_level(page_info)

	var level_changed := false
	for array in keys_defaults:
		if level_info.level_data[array[0]] == array[2]:
			level_info.level_data[array[0]] = page_info[array[1]]
			level_changed = true

	if level_changed:
		var data: LevelData = level_info.level_data
		level_info.level_code = "%s,%s,%s,%s,%s," % [
			LevelData.current_format_version,
			data.name.percent_encode(),
			data.author.percent_encode(),
			data.description.percent_encode(),
			data.thumbnail_url.percent_encode()] + code.substr(rest)
		page_info.level_info = level_info
		page_info.level_code = level_info.level_code

	return page_info


# Where the areas begin (just past the fifth comma), or -1 when the code is not in
# the current format or not shaped as the encoder writes it.
static func _areas_start(code: String) -> int:
	if code.get_slice(",", 0) != LevelData.current_format_version:
		return -1
	var at := -1
	for _i in range(5):
		at = code.find(",", at + 1)
		if at < 0:
			return -1
	if code.substr(at + 1, 1) != "[":
		return -1
	return at + 1
