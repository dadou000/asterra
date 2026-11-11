+++
path = "/foundation"
title = "Foundation"
kind = "section"
status = "stable"
summary = """
The lowest layers every other module builds on: core primitives, math, the job system, simulation time and reference frames, \
the platform layer (window, input, dialogs), platform services (Steam and friends) and the runtime session. Dependencies point \
only downward: nothing here may include a higher layer."""
keywords = ["foundation", "core", "math", "jobs", "time", "frames", "platform", "base layer", "primitives"]
related = ["/rules/architecture"]

[routes]
"types, assert, log, StrongId, thread names, profiler" = "core"
"vectors, matrices, rigid transforms" = "math"
"background work, worker pools, priorities, job telemetry" = "jobs"
"simulation time, pause, step, clock" = "time"
"reference frames, precision far from the origin" = "frames"
"window, keyboard layouts, text input, DPI, file dialogs, crash handler" = "platform"
"Steam, achievements, stats, platform provider" = "platform-services"
"runtime window + device + swapchain, resize" = "runtime-session"
+++

`Core` sits at the bottom; `Math`, `Time`, `Frames` and `Jobs` depend only on it (and each other as listed in
each node's `depends_on`). The platform layer wraps the OS; third-party SDKs stay behind adapters
(`/rules/architecture`).
