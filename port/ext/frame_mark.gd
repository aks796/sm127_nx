# frame_mark.gd -- timestamps for the helper's frame breakdown (switch_port.gd).
#
# Two of these run under the helper: one first in every physics tick and every
# idle frame (lowest process_priority), one last in the idle frame (highest).
# Between them they split a frame into its physics ticks (scripts plus the
# physics server's step), idle processing, and what follows until the next
# frame begins: rendering and presenting.
extends Node

var last = false


func _ready():
	pause_mode = Node.PAUSE_MODE_PROCESS
	process_priority = 1000000000 if last else -1000000000
	set_physics_process(not last)


func _physics_process(_delta):
	get_parent()._mark_tick()


func _process(_delta):
	get_parent()._mark_idle(last)
