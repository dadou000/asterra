# Cloud light-volume refresh skip

## Behavior
The cloud sun-optical-depth cache (3 cascades x 32 layers x 96x96 voxels) is no longer refreshed every frame when nothing that feeds it changed. In a static scene this saves about 0.7 ms of the `Clouds` GPU pass (about 74 -> 78 fps in an interleaved A/B); while the camera or the sun moves nothing is skipped, so the image is unchanged. `ORBIT_CLOUD_VOLUME_ALWAYS_REFRESH=1` restores the old schedule.

## Existing owner
- Module: `engine/celestial_clouds`
- Class/service: `CloudRenderer` (`UpdateLightVolume`, `LightVolume`)
- Canonical state: the light-volume buffer and its per-voxel refresh schedule (valid voxels every 6th frame, far cascade every 12th, missing every 3rd, indexed by `frameIndex % 4096`)

## Primary insertion point
- File: `engine/celestial_clouds/src/CloudRenderer.cpp`
- Symbol/function: `LightVolumeInputFingerprint`, `kLightVolumeSettleFrames = 24`, the skip logic in `UpdateLightVolume`
- Reason: the refresh is a pure function of the constants, the cloud field and the volume buffer; hashing exactly those inputs and skipping once they have been unchanged for a full schedule period (24 frames, longer than the 12-frame far-cascade period and the frame-index wrap) is exact.

## Secondary touch points
- `engine/studio_ui/src/StudioViewportRenderer.cpp`: the `.CloudLightVolume` update is its own graph pass declaring the cloud target, so its GPU time is attributed separately from the ray march.
- `tests/CloudLightVolumeTests.cpp` (gpu label): a lock-step reference volume against a skipping one, byte-compared after static, sun-moved, camera-moved, field-changed and frame-wrap phases.
- `engine/celestial_clouds/docs/shadows-and-light-volume.md`: invariants, verify and diagnose updated.

## Must not be implemented in
- The shader schedule: the cheap, exact decision is made on the CPU from the input fingerprint.
- A time-based or approximate skip: any approximation would make the cache differ from the reference.

## Data/control flow
cloud constants + field fingerprint + buffer address -> `LightVolumeInputFingerprint` -> `settledFrames` counter -> `UpdateLightVolume` skips the dispatch (or runs the normal schedule)

## Validation
- [x] Existing authority remains canonical (same buffer, same schedule when inputs change)
- [x] No duplicate state/controller was introduced
- [x] `Orbit.CloudLightVolume` is byte-exact in all phases and fails with a 3- or 12-frame settle window (the 12-frame failure is the `frameIndex % 4096` wrap case)
- [x] Interleaved A/B on one binary; absolute fps drifts between sessions, so only interleaved comparisons count
