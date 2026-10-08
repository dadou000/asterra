# Studio project-root path fallback

## Existing owner
- `ContentService` owns normalization of the project content root.

## Primary insertion point
- `engine/content/src/ContentService.cpp` project-root initialization.

## State authority
- `ProjectDocument` remains authoritative for the project root. `ContentService` stores only its resolved path for mount and derived-cache access.

## Must not be implemented in
- A second project-path service, project browser, or Studio-only asset-root cache.

## Behavior
- Prefer `weakly_canonical` when the operating system permits it.
- If canonical resolution reports an error, retain the absolute lexically normalized path so a project that Studio can already open does not fail during content-service construction.
- Keep per-entry canonicalization and project-mount containment checks intact; the fallback only covers root initialization.

## Validation
- Launch `Orbit.exe` with the repository's `examples/mcp-smoke` project and confirm `project.info` and the loopback RPC endpoint remain available.
