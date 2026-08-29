# Asterra 19-body ragdoll experiment

This branch is intentionally stripped to the minimum needed to validate the humanoid physics experiment.

Kept:

- `assets/character/asterrahuman.glb` and its character assets — canonical visual/skin source for the later binding pass.
- `experiments/locomotion_19body/` — neural locomotion experiment/configuration.
- `scripts/ragdoll_test.gd` — isolated 19-rigid-body ragdoll physics test.
- `scenes/ragdoll_test.tscn` — only launch scene.

Everything from the game runtime, terrain, weather, player, UI, shaders, tests and editor tooling has been removed from this branch so it cannot affect the articulation test.

## First gate: raw ragdoll

Open the project in Godot and press **F6/F5**. The 19-body proxy starts above a rigid floor and is completely passive: gravity + collisions + hard joint limits only. There are no motors, animation forces, balance controllers, terrain queries or neural inference.

Controls:

- **R** — rebuild/reset the ragdoll above the floor.
- **Right mouse drag** — orbit camera.
- **Mouse wheel** — zoom.
- **Esc** — release an active camera drag.

Expected behavior: the ragdoll falls, contacts the floor, articulates at its joints and settles without joints separating or the constraint solver exploding. Some awkward folding is normal because no muscle torques exist yet.

The physics proxy intentionally uses simple visible primitives in this first gate. Once the passive articulation is verified, the next gate is to fit its dimensions/anchors to the exact `asterrahuman.glb` skeleton manifest and then bind the skin to the physical pose.
