"""Cloud experiment: a cumulonimbus with an anvil at the terminator, for long shadows.

Drives a running Orbit Studio over its JSON-RPC port (ORBIT_RPC_HOST/PORT, default
127.0.0.1:4320) with existing methods only: it selects the planet, sets the simulation
clock to just before sunset at lat 0 / lon 0, puts the camera 9 km up looking about 70
degrees away from the sun (so the shadows rake sideways across the screen) and places a
cloud-lab cumulonimbus 45 km ahead. The sun is the real one, so the terrain, the
atmosphere and the cloud shadows all agree.

    python tools/cloud_terminator_experiment.py            # set it up
    python tools/cloud_terminator_experiment.py --hour 12.5 --yaw 310   # sun behind the cloud
    python tools/cloud_terminator_experiment.py --off      # remove the lab cloud

Sunset at this spot is at about 12.9 h of simulation time (the sun is on the camera's
310 degree heading at 12.5 h). Everything is transient view/clock state.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import time

HOST = os.environ.get("ORBIT_RPC_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORBIT_RPC_PORT", "4320"))
VIEW = "studio.primary"


def rpc(method: str, params: dict | None = None, timeout: float = 30.0):
    request = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
        sock.sendall(json.dumps(request).encode() + b"\n")
        buffer = b""
        while True:
            while b"\n" not in buffer:
                chunk = sock.recv(65536)
                if not chunk:
                    raise RuntimeError("Orbit Studio closed the connection")
                buffer += chunk
            line, _, buffer = buffer.partition(b"\n")
            message = json.loads(line)
            if message.get("id") == 1:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message["result"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hour", type=float, default=12.2, help="simulation time in hours")
    parser.add_argument("--yaw", type=float, default=20.0, help="camera heading, degrees")
    parser.add_argument("--altitude", type=float, default=9000.0, help="camera height, m")
    parser.add_argument("--distance", type=float, default=45000.0, help="cloud distance, m")
    parser.add_argument("--radius", type=float, default=7000.0, help="cloud radius, m")
    parser.add_argument("--seed", type=int, default=3)
    parser.add_argument(
        "--cirrus-sheet", type=float, default=0.8,
        help="thin cirrus on the shadow side of the storm (0 = none)",
    )
    parser.add_argument("--off", action="store_true", help="remove the lab cloud and stop")
    args = parser.parse_args()

    if args.off:
        rpc("view.terrain_layers_set", {"id": VIEW, "cloud_lab": {"enabled": False}})
        return

    rpc("time.set", {"time_microseconds": int(args.hour * 3600 * 1e6)})

    # Camera on the +X axis (lat 0, lon 0): up = +X, north = +Y, east = -Z.
    pose = rpc("viewport.pose_get", {"id": VIEW})
    pose.update(
        observer=[6371000.0 + args.altitude, 0.0, 0.0],
        up=[1, 0, 0],
        north=[0, 1, 0],
        east=[0, 0, -1],
        yaw=math.radians(args.yaw),
        pitch=0.0,
        id=VIEW,
    )
    # "restored" is only true when the observer actually moved, so it is not checked here;
    # if the planet is not the selected target the pose is ignored (selection.set first).
    rpc("viewport.pose_set", pose)
    time.sleep(2.0)

    lab = rpc(
        "view.terrain_layers_set",
        {
            "id": VIEW,
            "clouds": True,
            "cloud_lab": {
                "enabled": True,
                "type": 1.0,          # cumulonimbus
                "coverage": 0.9,
                "cirrus": 1.0,        # anvil
                "precipitation": 0.3,
                "radius_meters": args.radius,
                "height_scale": 1.0,
                "distance_meters": args.distance,
                "maturity": 0.6,
                "organisation": 0.6,
                "density": 1.0,
                "cirrus_sheet": args.cirrus_sheet,
                "seed": args.seed,
                "sun_override": False,  # the real sun lights the cloud and the ground
                "place": True,
            },
        },
    )["cloud_lab"]
    print(f"placed cloud (place_serial {lab['place_serial']}) at hour {args.hour}, yaw {args.yaw}")


if __name__ == "__main__":
    main()
