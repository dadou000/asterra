#!/usr/bin/env python3
"""CM1 harness for the Orbit SC-01 weather experiments.

  cm1_lab.py build  --cm1 <CM1 checkout> [--jobs N]
  cm1_lab.py run    --cm1 <CM1 checkout> --preset quick|standard|fine --out <dir> [--minutes M] [--threads N]
  cm1_lab.py export <cm1out.nc> <out.orbitwx>

`build` and `run` use the stock CM1 supercell case (config_files/supercell:
Weisman-Klemp sounding, quarter-circle hodograph, 1 K warm bubble); only grid
spacing, domain size, time step and output cadence change per preset. `export`
turns CM1's NetCDF output into the .orbitwx interchange format that
orbit_weather_lab (and later Studio) reads.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

# name -> (nx, ny, nz, dx, dz). 128 km square domain, 20 km deep, so the
# presets line up cell-for-cell with the fast core's power-of-two grids.
PRESETS = {
    "quick": (64, 64, 40, 2000.0, 500.0),
    "standard": (128, 128, 40, 1000.0, 500.0),
    "fine": (256, 256, 80, 500.0, 250.0),
}

# CM1 variable -> .orbitwx field.
EXPORT_FIELDS = [
    ("th", "th"), ("qv", "qv"), ("qc", "qc"), ("qr", "qr"), ("qi", "qi"),
    ("qs", "qs"), ("qg", "qg"), ("uinterp", "u"), ("vinterp", "v"),
    ("winterp", "w"), ("zvort", "zvort"),
]


def build(cm1: Path, jobs: int) -> Path:
    src = cm1 / "src"
    env = dict(os.environ)
    nc = subprocess.run(["nf-config", "--prefix"], capture_output=True, text=True)
    prefix = nc.stdout.strip() or "/usr"
    # Newer linkers drop libraries listed before the objects; put them after
    # by overriding LINKOPTS with --no-as-needed.
    cmd = ["make", f"-j{jobs}", "FC=gfortran", "USE_OPENMP=true", "USE_NETCDF=true",
           f"NETCDFBASE={prefix}", "LINKOPTS=-Wl,--no-as-needed -lnetcdff -lnetcdf"]
    subprocess.run(cmd, cwd=src, env=env, check=True)
    exe = cm1 / "run" / "cm1.exe"
    if not exe.exists():
        raise SystemExit("CM1 build did not produce run/cm1.exe")
    return exe


def patch_namelist(text: str, values: dict) -> str:
    for key, value in values.items():
        pattern = re.compile(rf"^(\s*{key}\s*=\s*)[^,\n]*(,?)", re.M)
        if not pattern.search(text):
            raise SystemExit(f"namelist key '{key}' not found")
        text = pattern.sub(lambda m: f"{m.group(1)}{value}{m.group(2)}", text, count=1)
    return text


def run(cm1: Path, preset: str, out: Path, minutes: float, threads: int) -> Path:
    nx, ny, nz, dx, dz = PRESETS[preset]
    case = cm1 / "run" / "config_files" / "supercell"
    out.mkdir(parents=True, exist_ok=True)
    namelist = (case / "namelist.input").read_text()
    namelist = patch_namelist(namelist, {
        "nx": nx, "ny": ny, "nz": nz, "dx": f"{dx:.1f}", "dy": f"{dx:.1f}",
        "dz": f"{dz:.1f}", "dtl": f"{dx / 150.0:.3f}", "adapt_dt": 1,
        "timax": f"{minutes * 60.0:.1f}", "tapfrq": "600.0", "output_format": 2,
        "output_filetype": 1, "statfrq": "60.0", "prclfrq": "-1.0",
    })
    (out / "namelist.input").write_text(namelist)
    shutil.copy(cm1 / "run" / "LANDUSE.TBL", out / "LANDUSE.TBL")
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    start = time.time()
    with open(out / "run.log", "w") as log:
        subprocess.run([str(cm1 / "run" / "cm1.exe")], cwd=out, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    wall = time.time() - start
    sim = minutes * 60.0
    print(f"CM1 {preset}: simulated {sim:.0f} s in {wall:.1f} s wall "
          f"({sim / wall:.1f}x real time, {threads} threads)")
    (out / "wall_seconds.txt").write_text(f"{wall:.3f}\n")
    return out / "cm1out.nc"


def export(nc_path: Path, out_path: Path) -> None:
    import numpy as np
    from netCDF4 import Dataset

    ds = Dataset(nc_path)
    times = [float(t) for t in ds["time"][:]]
    zh = [float(z) * 1000.0 for z in ds["zh"][:]]  # CM1 reports km
    xh = ds["xh"][:]
    nx, ny, nz = len(xh), len(ds["yh"][:]), len(zh)
    dx = float(xh[1] - xh[0]) * 1000.0
    dy = float(ds["yh"][1] - ds["yh"][0]) * 1000.0
    fields = [(src, dst) for src, dst in EXPORT_FIELDS if src in ds.variables]
    umove = float(ds["umove"][0]) if "umove" in ds.variables else 0.0
    vmove = float(ds["vmove"][0]) if "vmove" in ds.variables else 0.0
    with open(out_path, "wb") as f:
        f.write(b"ORBITWX1\n")
        f.write(b"source cm1\n")
        f.write(f"nx {nx}\nny {ny}\nnz {nz}\ndx {dx:g}\ndy {dy:g}\n".encode())
        f.write(f"move {umove:g} {vmove:g}\n".encode())
        f.write(("zh " + " ".join(f"{z:g}" for z in zh) + "\n").encode())
        f.write(("times " + " ".join(f"{t:g}" for t in times) + "\n").encode())
        f.write(("fields " + " ".join(dst for _, dst in fields) + "\n\n").encode())
        for ti in range(len(times)):
            for src, _ in fields:
                data = np.asarray(ds[src][ti], dtype="<f4")
                if data.shape != (nz, ny, nx):
                    raise SystemExit(f"{src}: unexpected shape {data.shape}")
                f.write(np.ascontiguousarray(data).tobytes())
    print(f"wrote {out_path}: {len(times)} frames, {len(fields)} fields, {nx}x{ny}x{nz}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--cm1", type=Path, required=True)
    b.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    r = sub.add_parser("run")
    r.add_argument("--cm1", type=Path, required=True)
    r.add_argument("--preset", choices=sorted(PRESETS), default="quick")
    r.add_argument("--out", type=Path, required=True)
    r.add_argument("--minutes", type=float, default=120.0)
    r.add_argument("--threads", type=int, default=os.cpu_count() or 2)
    e = sub.add_parser("export")
    e.add_argument("nc", type=Path)
    e.add_argument("out", type=Path)
    args = parser.parse_args()
    if args.cmd == "build":
        build(args.cm1, args.jobs)
    elif args.cmd == "run":
        nc = run(args.cm1, args.preset, args.out, args.minutes, args.threads)
        export(nc, args.out / f"cm1_{args.preset}.orbitwx")
    else:
        export(args.nc, args.out)


if __name__ == "__main__":
    sys.exit(main())
