from __future__ import annotations

import importlib
import platform
import sys
from dataclasses import dataclass


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
    checks.append(import_check("rsl_rl"))

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
        return 1

    print("\nTraining stack is ready for the articulation/simulation implementation.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
