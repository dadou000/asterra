extends "res://scripts/ragdoll_training_topology.gd"
## Runtime glue for the exact 54-DOF training-topology viewport.


func _ready() -> void:
	super._ready()
	_policy_server_path = (
		_repo_root.path_join("experiments")
		.path_join("locomotion_19body")
		.path_join("training")
		.path_join("scripts")
		.path_join("policy_inference_server_training.py")
	)
	print("Asterra viewport: using direct 54-DOF training-topology policy bridge")
