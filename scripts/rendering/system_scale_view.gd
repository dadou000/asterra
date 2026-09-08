class_name SystemScaleView
extends RefCounted
## System-scale far-body view compression (M7 of the seamless-multi-planet design).
##
## During interplanetary travel the other planets and the star sit ~1 AU away.
## Placing their far-LOD proxy meshes at their true render positions blows the
## scene bounds to ~1e11 m -- which wrecks near-terrain depth precision and makes
## the directional-light shadow frustum culler choke (the `!res` warning seen
## since M4). This maps a body's true camera distance through a smooth log-style
## compression into a fixed budget `[NEAR_M, VIEW_FAR_M)` and returns the matching
## uniform scale so the proxy's ANGULAR size on screen is exactly preserved. The
## near camera then keeps a bounded far plane and one depth buffer.
##
## This is the "parallax layer at log distance" approach. A true dual-viewport
## composite (system camera in a SubViewport, blended behind the near camera) is
## the heavier alternative kept as a future option; the shared depth buffer here
## is fine because remapped bodies always land near the far plane, well behind any
## near terrain.

## Bodies closer than this render un-remapped (a concurrent moon, the body you
## are leaving before you clear its space). Must exceed the near camera's own
## family far so a close body is never yanked.
const NEAR_M: float = 12_000_000.0
## Everything beyond NEAR_M is compressed to land inside this. The near camera's
## far is pinned at least this high so every remapped body is in view.
const VIEW_FAR_M: float = 22_000_000.0
## Distance over which the compression e-folds. ~5e10 keeps a 1 AU body a
## comfortable margin inside VIEW_FAR_M while a body a few million km out barely
## moves.
const COMPRESS_SCALE_M: float = 5.0e10


## Remapped render distance for a proxy whose true camera distance is `d_true`.
static func compressed_distance(d_true: float) -> float:
	if d_true <= NEAR_M:
		return maxf(d_true, 1.0)
	var span: float = VIEW_FAR_M * 0.92 - NEAR_M
	var over: float = d_true - NEAR_M
	return NEAR_M + span * (1.0 - exp(-over / COMPRESS_SCALE_M))


## Uniform scale multiplier that keeps the proxy's angular size unchanged after
## compressed_distance(): a body pulled to half its distance is drawn at half its size.
static func scale_for(d_true: float) -> float:
	var d: float = maxf(d_true, 1.0)
	return compressed_distance(d) / d


## Near-camera far plane: whatever the active body's own family needs (bounded by
## its family-frame radius + the player's altitude over it), but never below
## VIEW_FAR_M so every remapped far body stays in view. Remapped bodies never push
## this up -- they are all inside VIEW_FAR_M by construction -- so it can never
## reach AU scale.
static func near_far(active_family_need_m: float) -> float:
	return maxf(active_family_need_m, VIEW_FAR_M)
