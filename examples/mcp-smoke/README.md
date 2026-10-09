# MCP smoke example project

This is a complete, openable Orbit project used by the live MCP crash smoke
test. Its authored planetary system contains a Sun-like primary star, an
Earth-like rocky planet named Asterra, and a Moon-scale Luna. The planet has a
physical atmosphere with procedural clouds, an ocean, analytic orbit, gravity,
rotation, and an authored terrain surface. Luna has the Moon's radius, mean
density, orbit, synchronous rotation, and 1.62 m/s² surface gravity, plus a
4.45-billion-year airless impact history. That history combines a seeded crater
population, degraded ancient multi-ring basins, younger ray craters, and broad
mare-like lava resurfacing. Both spherical bodies are ready for terrain editing
and the star includes its radiative emitter and photosphere.

The runner copies this directory before testing, so tool mutations never touch
the example source. Open `Project.orbit.toml` with Orbit Studio to explore the
world, or run the complete smoke sweep with
`python tools/tests/mcp_live_smoke.py --exe Orbit.exe`.
