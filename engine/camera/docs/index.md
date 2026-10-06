+++
path = "/rendering/free-camera"
title = "Free camera (altitude-scaled navigation)"
kind = "subsystem"
status = "stable"
summary = "FreeCamera integrates WASD/mouse input into a camera pose whose speed is log-log interpolated by altitude above the ground, from a walking pace near the surface to an orbit-crossing dash far above it."
owner_module = "OrbitCamera"
keywords = ["camera", "free camera", "navigation", "speed", "altitude", "wasd", "fly"]
sources = [
  "engine/camera/include/orbit/camera/FreeCamera.hpp",
  "engine/camera/CMakeLists.txt",
]
symbols = ["FreeCameraConfig"]
invariants = [
  "Move speed is log-log interpolated by height above the ground directly below the camera, so the same input feels grounded near terrain and fast after climbing away from it.",
]
related = ["/editor/viewport"]
depends_on = ["/foundation/core", "/foundation/math"]
verify = [
  "ctest -R Orbit.Camera",
]
verified = "b0a0de7f"
+++


