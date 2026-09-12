# Orbit

Orbit is the custom C++23 engine for **Asterra**.

The `orbit` branch is intentionally independent of Godot. The previous Godot implementation remains available on the other Asterra branches as a reference while Orbit is developed from first principles.

## M0 — World Kernel

The first milestone establishes a strict modular engine layout, a native Windows runtime, C++23 build configuration, explicit platform boundaries, and the foundation for D3D12, the planetary world database, concentric terrain clipmaps, runtime editing, and procedural blueprints.

## Build

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

Read [docs/ORBIT_ARCHITECTURE.md](docs/ORBIT_ARCHITECTURE.md) before adding engine systems.
