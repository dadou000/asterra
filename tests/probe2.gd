extends Node
func _ready() -> void:
	var e := Environment.new()
	for p in e.get_property_list():
		var n: String = p["name"]
		if n.begins_with("tonemap") or n.contains("adjustment") or n.contains("saturation"):
			print("%s = %s" % [n, e.get(n)])
	get_tree().quit()
