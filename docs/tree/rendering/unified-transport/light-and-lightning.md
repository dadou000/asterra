+++
path = "/rendering/unified-transport/light-and-lightning"
title = "Stars, many lights, radiance caches and lightning (proposed)"
kind = "concept"
status = "planned"
summary = """
Proposed design for finite stellar disks and shared stellar visibility, energy-preserving hierarchical light selection for cities, versioned \
camera-independent radiance caches, charge-conserving lightning with a channel graph, and bounded transient illumination."""
keywords = ["star", "penumbra", "eclipse", "many lights", "lightcuts", "radiance cache", "lightning", "electrification", "discharge graph", "transient lighting", "city lights"]
related = ["/rendering/lighting", "/rendering/unified-transport"]
depends_on = ["/rendering/unified-transport/medium-and-clouds"]
invariants = [
  "Stars are finite angular emitters (alpha = asin(R/D)); direct visibility integrates transmission over the disk (optionally with limb darkening), never exponentiates averaged optical depth.",
  "Penumbra detail depends on blocker-to-receiver separation, not camera distance; each star keeps its own direction, spectrum, size, visibility and shadows.",
  "One stellar-visibility service serves terrain, clouds, water and atmosphere, combining planet-body occlusion, terrain horizon queries, nearby geometry and disk sampling.",
  "Emitters form a hierarchy (emitter, building, block, district, city) preserving spectral power, bounds and directionality; a merged downward streetlight group is not an isotropic point.",
  "A light is rejected only when a conservative bound is small and the sum of rejected contributions is accounted for; otherwise sample it probabilistically.",
  "Cache products (optical hierarchy, stellar visibility, local-source importance, directional radiance, diffuse scattering, surface transport, transient atlas) are versioned with independent invalidation: a light change does not invalidate extinction, a cloud change invalidates downstream visibility and scattering, camera motion invalidates only image-space history.",
  "Direct light is exact enough promptly; forward-scattered sunlight and narrow beams get explicit directional treatment, not low-order spherical harmonics alone.",
  "Lightning follows electrification (div(eps grad phi) = -rho_q) with conserved charge transfer; a discharge redistributes charge rather than resetting a meter.",
  "Lightning is a time-stamped segment graph with per-segment emission history reconstructed over the exposure interval; intracloud, cloud-to-ground and cloud-to-cloud flashes are distinct and long networks occur only within organized storm systems.",
  "A new flash never triggers a full-volume multiple-scattering solve and never blocks on indirect convergence; internal flashes light clouds even when the channel is hidden from the camera; buffers, queues and shader variants are preallocated.",
]
+++

## Lightning pipeline

Event generation (electric field, leader propagation, channel memory, current history) is separate from optical reconstruction. Visible channels are
evaluated in the current frame, direct and near-field scattering is injected into world-space regions at bounded cost, and higher-order scattering comes
from a compressed cached response with optional refinement. Unresolved distant segments merge into emission clusters that preserve energy and timing.
At extreme timewarp, flashes are queried from deterministic event data. Sprites and other transient luminous events use specialized mesospheric emission.

## Light selection

Bounded hierarchical selection (Lightcuts) with cached importance distributions (Disney Cache Points, ReGIR) reused by the world-space cache. Source
classes (stars, cities, street/vehicle clusters, emissive surfaces, lightning, fire) feed the same receiving-medium interface with different sampling.

## Scheduling

A priority scheduler gives frame-critical work (visible scene, direct light, first-response lightning) precedence over progressive refinement and
simulation, with explicit cost accounting, small interruptible batches, generation-tagged pages and frame-fence retirement. Large lighting changes publish
a coarse approximation first. Asynchronous compute is used only with real headroom.
