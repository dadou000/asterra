"""Reads the RIVR section of a planet .orbitbake file and reports the river graph's topology (read-only).

    python tools/river_graph_report.py examples/meridian/Bakes/<hash>.orbitbake
"""
import struct, sys, math, collections, pathlib

def sections(data):
    assert data[:8] == b"ORBITBAK", "not an orbitbake file"
    ver, count = struct.unpack_from("<II", data, 8); off = 16
    for _ in range(count):
        tag, _z, size, _h = struct.unpack_from("<IIQQ", data, off); off += 24
        yield tag.to_bytes(4, "little").decode("latin1"), data[off:off + size]; off += size

def rivers(payload):
    ver, rh, radius, nn, ns = struct.unpack_from("<IQdII", payload, 0); off = struct.calcsize("<IQdII")
    nodes = []
    for _ in range(nn):
        x, y, z, el, w, d, q, basin = struct.unpack_from("<dddffffI", payload, off); off += struct.calcsize("<dddffffI")
        nodes.append((x, y, z, el, w, d, q, basin))
    segs = [struct.unpack_from("<II", payload, off + 8 * i) for i in range(ns)]
    return radius, nodes, segs

def main(path):
    for tag, payload in sections(pathlib.Path(path).read_bytes()):
        if tag != "RIVR":
            continue
        radius, nodes, segs = rivers(payload)
        down = {u: d for u, d in segs}
        inflow = collections.Counter(d for _u, d in segs)
        heads = [i for i in range(len(nodes)) if inflow[i] == 0 and i in down]
        mouths = [i for i in range(len(nodes)) if i not in down]
        confl = [i for i, c in inflow.items() if c >= 2]
        print(f"nodes {len(nodes)}  segments {len(segs)}  headwaters {len(heads)}  confluences {len(confl)}  mouths {len(mouths)}")
        print("max inflows at one node", max(inflow.values()) if inflow else 0)
        basins = collections.Counter(n[7] for n in nodes)
        sizes = sorted(basins.values(), reverse=True)
        print(f"basins {len(basins)}  largest node counts {sizes[:8]}")
        # Strahler order
        order = {}
        def strahler(i):
            if i in order: return order[i]
            ups = ups_of[i]
            if not ups: order[i] = 1
            else:
                o = [strahler(u) for u in ups]; m = max(o)
                order[i] = m + 1 if o.count(m) >= 2 else m
            return order[i]
        ups_of = collections.defaultdict(list)
        for u, d in segs: ups_of[d].append(u)
        sys.setrecursionlimit(100000)
        for m in mouths: strahler(m)
        print("Strahler order histogram", dict(sorted(collections.Counter(order.values()).items())))
        q = sorted(n[6] for n in nodes); w = sorted(n[4] for n in nodes)
        print(f"discharge m3/s min {q[0]:.0f} median {q[len(q)//2]:.0f} max {q[-1]:.0f};  width m min {w[0]:.0f} max {w[-1]:.0f}")
        # longest path to mouth from each headwater
        def length(i):
            n = 0; 
            while i in down: i = down[i]; n += 1
            return n
        L = sorted((length(h) for h in heads), reverse=True)
        print("longest source->mouth reaches (segments)", L[:5])
        # largest basin: tributary structure at its mouth
        big = max(basins, key=basins.get)
        print("largest basin mouth node", big, "discharge", round(nodes[big][6]), "m3/s, upstream inflows", len(ups_of[big]))
        # mean spacing
        lens = [math.dist(nodes[u][:3], nodes[d][:3]) * radius for u, d in segs]
        print(f"reach length km mean {sum(lens)/len(lens)/1000:.0f} max {max(lens)/1000:.0f}")
if __name__ == "__main__": main(sys.argv[1])
