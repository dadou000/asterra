# MCP smoke example project

This is a complete, openable Orbit project used by the live MCP crash smoke
test. Its authored planetary system contains a Sun-like primary star, an
Earth-like rocky planet named Asterra, and its Luna-like moon. The planet has a
physical atmosphere with procedural clouds, an ocean, analytic orbit, gravity,
rotation, and an authored terrain surface. The moon also has orbit, gravity,
rotation, and terrain. Both spherical bodies are ready for terrain editing and
the star includes its radiative emitter and photosphere.

The runner copies this directory before testing, so tool mutations never touch
the example source. Open `Project.orbit.toml` with Orbit Studio to explore the
world, or run the complete smoke sweep with
`python tools/tests/mcp_live_smoke.py --exe Orbit.exe`.
