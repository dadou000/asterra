# Orbit

Orbit is the custom C++23 engine for **Asterra**.

The `orbit` branch is intentionally independent of Godot. The previous Godot implementation remains available on the other Asterra branches as a reference while Orbit is developed from first principles.

## M0 — World Kernel

The first milestone establishes a strict modular engine layout, a native Windows runtime, C++23 build configuration, explicit platform boundaries, and the foundation for D3D12, the planetary world database, concentric terrain clipmaps, runtime editing, and procedural blueprints.

## Build

### One-click Windows build

Double-click `build_orbit.bat` from the repository root.

It configures CMake, builds `OrbitLauncher` + `OrbitSandbox`, and packages the runnable build into:

```text
dist/Orbit-Windows-Release/
├── OrbitLauncher.exe
├── OrbitSandbox.exe
└── symbols/
```

Useful command-line options:

```bat
build_orbit.bat run
build_orbit.bat debug run
build_orbit.bat rebuild
build_orbit.bat release nopause
```

### Manual build

Requirements:

- Windows 10/11
- Visual Studio 2022 with Desktop development with C++
- CMake 3.28+

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\apps\launcher\Debug\OrbitLauncher.exe
```

`OrbitLauncher` supervises the runtime and captures stdout/stderr into a timestamped session log. Logs are stored under `apps/launcher/<config>/logs` in a development build. If Orbit hits an unhandled Windows exception or `std::terminate`, the runtime also writes a crash report and minidump (`.dmp`) into the same log directory.

You can still launch `OrbitSandbox.exe` directly while developing; its crash handler falls back to a local `logs` directory when it is not started by the launcher.



### Sandbox camera controls

The terrain sandbox uses a free camera:

- `W/A/S/D` — move relative to camera heading
- Mouse — yaw/pitch look
- `Q/E` — move down/up
- `Left Shift` — movement boost
- `Esc` — quit

Mouse capture is released automatically when the window loses focus.

Read [docs/ORBIT_ARCHITECTURE.md](docs/ORBIT_ARCHITECTURE.md) before adding engine systems.
