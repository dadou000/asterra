+++
path = "/apps/build-cli"
title = "Orbit build CLI"
kind = "subsystem"
status = "stable"
summary = "The command-line front end of BuildService: --profile, --output, --validate, --cook (default), --package and --no-clean."
keywords = ["build cli", "orbitbuild", "cook", "package", "profile", "headless build"]
sources = [
  "apps/build/src/Main.cpp",
  "apps/build/CMakeLists.txt",
]
symbols = []
invariants = [
  "--validate writes no build products; --cook validates then cooks (the default); --package cooks and assembles an OrbitPlayer package; --no-clean refuses to replace an existing output directory.",
]
related = ["/apps/build-service"]
depends_on = ["/apps/build-service", "/authoring/documents", "/foundation/platform"]
verify = [
  "ctest -R Orbit.BuildCli",
]
verified = "b0a0de7f"
+++


