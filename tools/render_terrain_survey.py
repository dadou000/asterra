"""Render the sampled heightfield exported by OrbitTerrainBenchmark (not a runtime screenshot)."""
import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource, LinearSegmentedColormap
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("heightfield")
parser.add_argument("output")
args = parser.parse_args()
data = np.genfromtxt(args.heightfield, delimiter=",", names=True)
n = len(np.unique(data["x_m"]))
x = data["x_m"].reshape(n, n) / 1000
y = data["y_m"].reshape(n, n) / 1000
z = data["height_m"].reshape(n, n) / 1000
cmap = LinearSegmentedColormap.from_list("terrain", ["#394a35", "#797c61", "#9a9386", "#e0dfd9", "#ffffff"])
light = LightSource(azdeg=315, altdeg=40)
rgb = light.shade(z, cmap=cmap, vert_exag=1, dx=x[0, 1]-x[0, 0], dy=y[1, 0]-y[0, 0], blend_mode="soft")
fig = plt.figure(figsize=(13, 6), facecolor="white", layout="constrained")
ax = fig.add_subplot(121)
ax.imshow(rgb, origin="lower", extent=[x.min(), x.max(), y.min(), y.max()])
contours = ax.contour(x, y, z, levels=np.arange(0, 8.1, .5), colors="black", linewidths=.35, alpha=.4)
ax.clabel(contours, inline=True, fontsize=7, fmt="%.1f km")
ax.set(xlabel="East (km)", ylabel="North (km)", title="Sampled summit region · 64 × 64 km")
ax2 = fig.add_subplot(122, projection="3d")
ax2.plot_surface(x, y, z, facecolors=rgb, rstride=2, cstride=2, shade=False, linewidth=0, antialiased=False)
ax2.set(xlabel="East (km)", ylabel="North (km)", zlabel="Elevation (km)", title="Terrain profile · 3× vertical exaggeration")
ax2.set_box_aspect((64, 64, max(float(np.ptp(z)), .1) * 3))
ax2.view_init(elev=30, azim=-125)
fig.savefig(args.output, dpi=150)
