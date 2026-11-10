# Live MCP crash smoke test

- Existing owner: StudioApplication owns the loopback JSON-RPC listener used by the MCP adapter.
- Primary insertion point: its DevServer configuration; `ORBIT_RPC_PORT` must let an isolated smoke run use a private port without taking over a user's Studio session.
- Canonical state: the existing RPC dispatcher and MCP adapter remain authoritative; the smoke runner only issues calls against a copied disposable project.
- Do not duplicate: RPC method handlers, MCP wrapper policy, or the checked-in example project's state. The fixture is copied for every run.
- Reflection and verification: saving the app implementation uses the central native-generation fallback; the smoke runner builds/runs the unified Orbit executable and reports calls that fail or terminate the process.
