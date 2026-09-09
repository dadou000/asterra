from __future__ import annotations

import importlib
import importlib.metadata as metadata
import platform
import sys
from dataclasses import dataclass

WINDOWS_TENSORDICT_VERSION = "0.11.0"
RSL_RL_VERSION = "3.0.1"


@dataclass
class Check:
    name: str
    ok: bool
    detail: str


def import_check(module_name: str) -> Check:
    try:
        module = importlib.import_module(module_name)
        version = getattr(module, "__version__", "version unavailable")
        return Check(module_name, True, str(version))
    except Exception as exc:  # bootstrap diagnostic: report full import failure
        return Check(module_name, False, f"{type(exc).__name__}: {exc}")


def distribution_version(name: str) -> str | None:
    try:
        return metadata.version(name)
    except metadata.PackageNotFoundError:
        return None


def main() -> int:
    checks: list[Check] = []

    python_ok = sys.version_info[:2] == (3, 11)
    checks.append(
        Check(
            "python",
            python_ok,
            f"{platform.python_version()} ({sys.executable})",
        )
    )

    torch_check = import_check("torch")
    checks.append(torch_check)
    checks.append(import_check("isaaclab"))

    rsl_version = distribution_version("rsl-rl-lib")
    rsl_ok = rsl_version == RSL_RL_VERSION
    checks.append(
        Check(
            "rsl-rl-lib",
            rsl_ok,
            rsl_version if rsl_version is not None else "not installed",
        )
    )

    tensordict_version = distribution_version("tensordict")
    if platform.system() == "Windows":
        tensordict_ok = tensordict_version == WINDOWS_TENSORDICT_VERSION
        td_detail = tensordict_version if tensordict_version is not None else "not installed"
        if not tensordict_ok:
            td_detail += f" (Windows requires {WINDOWS_TENSORDICT_VERSION} for Isaac/RSL-RL stability)"
    else:
        tensordict_ok = tensordict_version is not None
        td_detail = tensordict_version if tensordict_version is not None else "not installed"
    checks.append(Check("tensordict", tensordict_ok, td_detail))

    # Do not import RSL-RL on Windows while TensorDict is known-unsafe. TensorDict
    # 0.12.x can terminate Python with 0xC0000005 before an exception is raised.
    if rsl_ok and tensordict_ok:
        checks.append(import_check("rsl_rl"))
    else:
        checks.append(Check("rsl_rl import", False, "skipped until dependency versions above are fixed"))

    if torch_check.ok:
        import torch

        cuda_ok = torch.cuda.is_available()
        if cuda_ok:
            detail = (
                f"{torch.cuda.get_device_name(0)} | "
                f"CUDA runtime {torch.version.cuda} | "
                f"VRAM {torch.cuda.get_device_properties(0).total_memory / (1024 ** 3):.1f} GiB"
            )
        else:
            detail = "torch imported but CUDA is unavailable"
        checks.append(Check("cuda", cuda_ok, detail))

    width = max(len(check.name) for check in checks)
    print("Asterra training stack check")
    print("=" * 72)
    for check in checks:
        state = "OK" if check.ok else "FAIL"
        print(f"{check.name:<{width}}  {state:<4}  {check.detail}")

    failures = [check for check in checks if not check.ok]
    if failures:
        print("\nTraining stack is not ready.")
        if platform.system() == "Windows" and not tensordict_ok:
            print(
                "Repair TensorDict with:\n"
                f'  "{sys.executable}" -m pip install --force-reinstall --no-deps '
                f'"tensordict=={WINDOWS_TENSORDICT_VERSION}"'
            )
        return 1

    print("\nTraining stack is ready for articulation, simulation and RSL-RL training.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
