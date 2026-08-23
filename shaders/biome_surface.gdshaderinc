// Distance-adaptive, textureless biome material synthesis.
//
// The chunk stream provides four continuous material weights instead of a hard
// biome ID. Geometry and pixels therefore agree at ecological boundaries, and a
// single shader serves the entire planet without texture-array indirection.

// Close-range PBR scans. Both source materials are CC0 Poly Haven assets stored
// locally in the project, so runtime rendering never depends on the network.
uniform sampler2D u_ground_grass_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_ground_grass_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_ground_grass_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_ground_soil_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_ground_soil_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_ground_soil_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_forest_floor_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_forest_floor_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_forest_floor_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_forest_canopy_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform float u_ground_grass_metres = 2.0;
uniform float u_ground_soil_metres = 1.3;
uniform float u_forest_floor_metres = 2.0;
uniform float u_forest_canopy_metres = 2.0;

vec3 biome_albedo(vec3 base, vec4 surface, float slope, float coarse_noise,
		float fine_noise, float near_weight) {
	float vegetation = surface.r;
	float wetness = surface.g;
	float sand = surface.b;
	float frost = surface.a;

	// These are absolute albedos and have to stay on the same scale as
	// BIOME_ALBEDO. When the biome palette was three times brighter they were in
	// proportion; against physical values they would otherwise overpower the
	// biome entirely and drag every vegetated surface to khaki, because dry soil
	// really is two to three times brighter than a canopy.
	vec3 mineral = mix(vec3(0.085, 0.068, 0.050), vec3(0.300, 0.245, 0.165), sand);
	vec3 humus = vec3(0.052, 0.040, 0.026);
	vec3 snow = mix(vec3(0.64, 0.72, 0.78), vec3(0.93, 0.965, 0.985),
		0.55 + fine_noise * 0.30);

	// Preserve the climate-baked palette at landscape scale and reveal physical
	// surface constituents only as the camera approaches the ground. Mineral
	// ground shows through the canopy, not over it: a closed canopy hides its own
	// soil almost completely, so exposure is gated on vegetation rather than
	// applied everywhere the soil happens to be sandy.
	float expose = clamp(sand * (1.0 - vegetation * 1.15), 0.0, 1.0);
	vec3 composed = mix(base, mineral, expose * 0.55 * near_weight);
	composed = mix(composed, humus, vegetation * wetness * 0.22 * near_weight);
	float snow_hold = frost * (1.0 - smoothstep(0.13, 0.48, slope));
	composed = mix(composed, snow, snow_hold * (0.18 + 0.72 * near_weight));

	float grain = coarse_noise * 0.075 + fine_noise * 0.055 * near_weight;
	composed *= 1.0 + grain;
	composed *= 1.0 - wetness * (0.09 + 0.20 * near_weight);
	return max(composed, vec3(0.008));
}

float biome_roughness(vec4 surface, float slope) {
	float snow_hold = surface.a * (1.0 - smoothstep(0.13, 0.48, slope));
	float rough = mix(0.92, 0.78, surface.b);
	rough = mix(rough, 0.62, surface.g * 0.55);
	rough = mix(rough, 0.96, snow_hold);
	return clamp(rough, 0.45, 0.98);
}

// Screen-derivative bump mapping needs no additional procedural samples: the
// same fine signal used for albedo produces a coherent micro-normal.
vec3 biome_micro_normal(vec3 world_pos, vec3 world_normal, float height_signal,
		float strength) {
	vec3 dpdx = dFdx(world_pos);
	vec3 dpdy = dFdy(world_pos);
	float dhdx = dFdx(height_signal);
	float dhdy = dFdy(height_signal);
	vec3 r1 = cross(dpdy, world_normal);
	vec3 r2 = cross(world_normal, dpdx);
	float determinant = dot(dpdx, r1);
	// Mikkelsen-style derivative reconstruction. Keeping the determinant as a
	// scale instead of dividing by it avoids magnifying precision noise on tiny
	// or edge-on triangles; its sign preserves winding orientation.
	float orientation = determinant < 0.0 ? -1.0 : 1.0;
	vec3 gradient = orientation * (r1 * dhdx + r2 * dhdy);
	return normalize(abs(determinant) * world_normal - gradient * strength);
}

// ---------------------------------------------------------------- close range ---
/// Ground material as seen from a few metres, modulating the baked biome albedo
/// rather than replacing it so the surface stays continuous from orbit down to
/// standing on it.
///
/// Three things make real ground read as ground, and none of them is colour:
///
///  * Clumping. Vegetation, gravel and soil aggregates are discrete objects with
///    gaps between them. Fractal noise alone is continuous everywhere and always
///    reads as blurred cloud, so the tuft field here is cellular.
///  * Tuft-to-gap contrast. The gap between two clumps is not a darker green, it
///    is in shadow -- typically a third of the brightness of the lit top.
///  * A hue split between the two. Sunlit foliage is lit by a warm sun and skews
///    yellow; shaded foliage is lit almost entirely by blue sky and skews cyan.
///    Varying brightness alone still reads as one flat material.
///
/// `dpos` must be the precision-safe detail position from ground_detail, not a
/// planet-space coordinate. `bump` returns a height signal for normal
/// perturbation by the caller.
/// `bump` and `rough` are inout, not out. They arrive carrying whatever material
/// already answered for this fragment -- the canopy crowns, or the forest floor --
/// and an `out` parameter would discard that outright however small this
/// material's own weight was. Under a stand that meant the floor's litter relief
/// was replaced wholesale by open-ground tufts and, wherever the cell had any
/// aeolian weight at all, by the directional sand ripple, which is where the
/// diagonal combing across a forest floor came from.
/// Resolve individual grass blades in a stable tangent plane. One jittered,
/// curved blade occupies each 4 cm cell; a 3x3 search removes cell-edge pops.
/// The blade is a tapered signed-distance capsule, so screen footprint widens
/// its edge instead of allowing sub-pixel blades to sparkle.
float grass_blade_layer(vec2 p, float pixel_m, out float tone, out float relief) {
	const float cell = 0.040;
	vec2 home = floor(p / cell);
	float aa = max(pixel_m * 0.72, 0.00055);
	float strongest = 0.0;
	tone = 0.5;
	relief = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec2 lattice = home + vec2(float(x), float(y));
			vec3 rnd = gd_hash3(vec3(lattice, 37.0));
			vec2 centre = (lattice + mix(vec2(0.12), vec2(0.88), rnd.xy)) * cell;
			float angle = rnd.z * TAU;
			vec2 axis = vec2(cos(angle), sin(angle));
			vec2 across = vec2(-axis.y, axis.x);
			vec2 delta = p - centre;
			vec2 q = vec2(dot(delta, across), dot(delta, axis));
			float blade_len = mix(0.028, 0.078, rnd.y);
			float along = clamp(q.y / blade_len + 0.5, 0.0, 1.0);
			float bend = (rnd.x - 0.5) * 0.010 * along * along;
			float half_width = mix(0.0012, 0.0028, rnd.x)
				* mix(1.0, 0.24, along);
			float dx = abs(q.x - bend) - half_width;
			float dy = abs(q.y) - blade_len * 0.5;
			float sd = length(max(vec2(dx, dy), vec2(0.0)))
				+ min(max(dx, dy), 0.0);
			float blade = 1.0 - smoothstep(-aa, aa, sd);
			if (blade > strongest) {
				strongest = blade;
				tone = rnd.z;
			}
			relief = max(relief, blade * (0.30 + 0.70 * along));
		}
	}
	// Once a blade is smaller than a few pixels it becomes the continuous sward
	// beneath it; fading prevents both shimmer and a distance ring.
	return strongest * (1.0 - smoothstep(0.010, 0.028, pixel_m));
}

vec3 ground_close(vec3 base, vec4 surface, float slope, vec3 dpos, vec3 up, float pixel_m,
		float strength, inout float bump, inout float rough) {
	float vegetation = surface.r;
	float wetness = surface.g;
	float sand = surface.b;
	float frost = surface.a;

	// Separate physical scales. Bare patches are metres across, the sward varies
	// over decimetres, and actual soil aggregates are centimetres/millimetres.
	// Mixing those into one fBm was the source of the enlarged blurry mottling.
	float patch = gd_fbm_band(dpos, 7.5, 0.45, pixel_m, 0.56);
	float clump = gd_fbm_band(dpos, 0.75, 0.075, pixel_m, 0.52);
	float grain = gd_fbm_band(dpos, 0.065, 0.004, pixel_m, 0.48);
	float patch01 = clamp(0.5 + patch * 1.85, 0.0, 1.0);
	float clump01 = clamp(0.5 + clump * 1.65, 0.0, 1.0);
	float grain01 = clamp(0.5 + grain * 1.7, 0.0, 1.0);

	vec3 grass_t;
	vec3 grass_b;
	gd_basis(up, grass_t, grass_b);
	vec2 grass_uv = vec2(dot(dpos, grass_t), dot(dpos, grass_b));
	float blade_tone;
	float blade_relief;
	float blade = grass_blade_layer(grass_uv, pixel_m, blade_tone, blade_relief);

	// --- vegetation -------------------------------------------------------
	float lit = smoothstep(0.12, 0.88, clump01);
	// Channel multipliers, applied to a base whose red is already below its
	// green. Boosting red hardest to make foliage "warmer" inverts that
	// relationship and turns every green surface olive, which is the single
	// easiest way to make a living landscape look dead. Sunlit foliage gains
	// most in green; shaded foliage keeps relatively more blue because the only
	// light reaching it is sky.
	vec3 veg_lit = mix(vec3(0.052, 0.125, 0.018),
		vec3(0.092, 0.205, 0.030), clamp(wetness * 0.70 + clump01 * 0.30, 0.0, 1.0));
	vec3 veg_shade = mix(vec3(0.018, 0.042, 0.011),
		vec3(0.026, 0.068, 0.016), wetness);
	vec3 veg = mix(veg_shade, veg_lit, lit);
	// Senescent patches, in drifts rather than uniformly. Straw is an absolute
	// colour: dead grass is the same near-neutral tan whatever it grew from.
	float dry = smoothstep(0.58, 0.88, patch01) * (1.0 - wetness * 0.9);
	veg = mix(veg, vec3(0.205, 0.158, 0.060), dry * 0.60);

	// --- mineral ground ---------------------------------------------------
	// Also absolute, and on the BIOME_ALBEDO scale: soil colour comes from the
	// mineral in it, not from whatever happens to be growing on top.
	// Humus and loam are explicitly brown, not grey mineral tinted by the biome.
	// The wet end stays chromatic as it darkens, so moist temperate soil does not
	// turn into black/green camouflage beside the grass.
	vec3 humus = vec3(0.060, 0.027, 0.009);
	vec3 loam = mix(vec3(0.155, 0.071, 0.020), vec3(0.225, 0.125, 0.045), sand);
	vec3 soil = mix(humus, loam, grain01);
	float grit = smoothstep(0.76, 0.94,
		gd_value(dpos / 0.018 + vec3(23.0, 7.0, 61.0)));
	soil = mix(soil, vec3(0.245, 0.208, 0.160), grit * (0.08 + sand * 0.22));

	// Vegetation cannot hold a steep face, and thin cover lets the ground show.
	// A closed canopy shows almost none of its own floor, so this has to fall to
	// zero well before vegetation does.
	//
	// Two failure modes to avoid, and the old form had both. `slope` comes from
	// the interpolated mesh normal, so a bare linear coefficient on it prints the
	// triangulation across every sunlit hillside as pale faceted blobs -- hence
	// the smoothstep, which is flat over the range where a facet lives. And what
	// a forest floor shows between its trees is dark leaf litter, not the pale
	// mineral subsoil a metre underneath it: exposing that under a canopy was
	// putting bright grey-white patches all over a temperate rainforest.
	float grass_suit = clamp(vegetation * 1.55, 0.0, 1.0)
		* (1.0 - smoothstep(0.22, 0.68, slope));
	float exposure = patch01 * 0.82 + (grain01 - 0.5) * 0.10
		+ smoothstep(0.20, 0.68, slope) * 0.32;
	float threshold = mix(0.38, 0.72, grass_suit);
	float bare = smoothstep(threshold - 0.09, threshold + 0.11, exposure);
	// A grazed/open temperate forest at vegetation 0.44 now retains roughly a
	// quarter brown ground, while dense wet cover still closes most of the gaps.
	bare = clamp(bare + (1.0 - grass_suit) * 0.12, 0.0, 1.0);
	vec3 thatch = mix(vec3(0.043, 0.035, 0.018), veg_shade, clump01 * 0.58);
	vec3 ground = mix(mix(thatch, veg, 0.62 + clump01 * 0.30), soil, bare);

	// Resolved blades sit above the sward and disappear inside genuine dirt
	// patches. Hue varies blade by blade, with brighter yellow-green tips.
	vec3 blade_col = mix(vec3(0.025, 0.082, 0.012),
		vec3(0.105, 0.225, 0.032), blade_tone);
	blade_col = mix(blade_col, vec3(0.165, 0.145, 0.050), dry * 0.55);
	ground = mix(ground, blade_col, blade * (1.0 - bare) * grass_suit * 0.94);

	// --- aeolian sand -----------------------------------------------------
	if (sand > 0.02) {
		// Ripples are a travelling wave, so they are directional and much more
		// regular than noise. A ridged field along one axis is what makes sand
		// read as sand instead of beige gravel.
		float ripple = gd_ridged(dpos * vec3(1.0, 0.30, 1.0) * 6.5, 1.0, pixel_m * 6.5);
		vec3 sand_col = mix(vec3(0.300, 0.245, 0.160), vec3(0.455, 0.390, 0.275), ripple);
		ground = mix(ground, sand_col, clamp(sand * 1.25, 0.0, 1.0));
		grain = mix(grain, ripple - 0.5, clamp(sand * 1.25, 0.0, 1.0));
	}

	// --- snow -------------------------------------------------------------
	float snow_hold = frost * (1.0 - smoothstep(0.16, 0.52, slope));
	if (snow_hold > 0.02) {
		float drift = gd_fbm(dpos, 2.2, pixel_m, 0.55);
		// Wind-packed snow is not white, it is faintly blue in its own shadow
		// because the only light reaching a hollow is sky.
		vec3 snow = mix(vec3(0.58, 0.64, 0.74), vec3(0.86, 0.89, 0.93), drift + 0.5);
		ground = mix(ground, snow, snow_hold);
	}

	// Wet ground is darker and glossier than the same material dry.
	float damp = wetness * (0.45 + 0.55 * smoothstep(0.72, 0.22, patch01));
	ground *= 1.0 - damp * mix(0.10, 0.27, bare);

	float material_height = clump * 0.20 + grain * mix(0.22, 0.46, bare)
		+ blade_relief * (1.0 - bare) * grass_suit * 0.48 - bare * 0.08;
	bump = mix(bump, material_height, strength);
	// Wet ground is glossier than the same material dry -- but foliage is not a
	// wet slab. Rain beads and scatters on leaves; it is bare soil, rock and sand
	// that take on a sheen. Applying the gloss regardless of what is growing took
	// roughness under a temperate rainforest down to 0.6, which put a hard white
	// specular lobe on every sun-facing slope: the brightest thing in the whole
	// landscape, sitting on the darkest surface in it.
	rough = mix(rough, clamp(mix(0.94, 0.70, sand) - damp * 0.34 * bare
		+ snow_hold * 0.10, 0.55, 0.99), strength);
	return mix(base, max(ground, vec3(0.004)), strength);
}

/// Texture-backed open-ground material. Fine-scale colour, blade structure,
/// aggregate relief, and roughness all come from photographic PBR scans.
/// Two irrationally-related, rotated projections are blended by a much broader
/// sample from the scan itself. This breaks the repeated square without adding a
/// second, generated pattern that becomes visible when the detail mips out.
vec3 ground_close_textured(vec3 base, vec4 surface, float slope, vec3 dpos, vec3 up,
		float pixel_m, vec3 material_tint, float strength, inout float bump, inout float rough,
		inout vec3 normal_offset) {
	float vegetation = surface.r;
	float wetness = surface.g;
	float sand = surface.b;
	float frost = surface.a;

	vec3 tangent;
	vec3 bitangent;
	gd_basis(up, tangent, bitangent);
	vec2 metres = vec2(dot(dpos, tangent), dot(dpos, bitangent));
	float gs = max(u_ground_grass_metres, 0.01);
	float ss = max(u_ground_soil_metres, 0.01);
	vec2 grass_uv0 = metres / gs;
	vec2 grass_uv1 = vec2(0.80 * metres.x - 0.60 * metres.y,
		0.60 * metres.x + 0.80 * metres.y) / (gs * 1.371) + vec2(0.317, 0.731);
	vec2 grass_uvm = vec2(0.36 * metres.x + 0.93 * metres.y,
		-0.93 * metres.x + 0.36 * metres.y) / (gs * 9.73) + vec2(0.613, 0.127);
	vec2 soil_uv0 = vec2(0.91 * metres.x + 0.41 * metres.y,
		-0.41 * metres.x + 0.91 * metres.y) / ss + vec2(0.173, 0.683);
	vec2 soil_uv1 = vec2(0.26 * metres.x - 0.97 * metres.y,
		0.97 * metres.x + 0.26 * metres.y) / (ss * 1.517) + vec2(0.817, 0.269);
	vec2 soil_uvm = vec2(0.68 * metres.x - 0.73 * metres.y,
		0.73 * metres.x + 0.68 * metres.y) / (ss * 11.31) + vec2(0.431, 0.887);

	vec3 grass_macro = texture(u_ground_grass_albedo, grass_uvm).rgb;
	vec3 soil_macro = texture(u_ground_soil_albedo, soil_uvm).rgb;
	float grass_mix = smoothstep(0.06, 0.28,
		dot(grass_macro, vec3(0.18, 0.67, 0.15)));
	float soil_mix = smoothstep(0.025, 0.16,
		dot(soil_macro, vec3(0.26, 0.58, 0.16)));

	// Once one pixel covers more than a metre, the rotated detail projections,
	// tangent-space normals, roughness maps, and centimetre sand/snow synthesis
	// are below the reconstruction footprint. Sampling them anyway was the main
	// cost of an aircraft view. The scan's broad projection is still a real
	// texture and its mip chain retains the colour frequencies the pixel can see.
	if (pixel_m > 1.25) {
		vec3 grass_far = grass_macro * mix(0.94, 1.06, grass_mix) * material_tint;
		vec3 soil_far = soil_macro * mix(0.93, 1.07, soil_mix) * material_tint;
		grass_far *= mix(vec3(0.94, 1.04, 0.91), vec3(0.78, 0.91, 0.76),
			wetness * 0.62);
		soil_far *= mix(vec3(1.08, 0.94, 0.82), vec3(0.70, 0.61, 0.54),
			wetness * 0.72);
		float grass_suit_far = clamp(vegetation * 1.55, 0.0, 1.0)
			* (1.0 - smoothstep(0.22, 0.68, slope));
		float ecological_bare_far = smoothstep(0.18, 0.88,
			(1.0 - grass_suit_far) + sand * 0.22);
		float opening_far = smoothstep(0.26, 0.76, 1.0 - grass_mix);
		float bare_far = clamp(ecological_bare_far * 0.58
			+ opening_far * ecological_bare_far * 0.42, 0.0, 1.0);
		vec3 ground_far = mix(grass_far, soil_far, bare_far);
		if (sand > 0.02) {
			vec3 sand_far = mix(vec3(0.300, 0.245, 0.160),
				vec3(0.455, 0.390, 0.275), soil_mix) * material_tint;
			ground_far = mix(ground_far, sand_far, clamp(sand * 1.25, 0.0, 1.0));
		}
		float snow_far = frost * (1.0 - smoothstep(0.16, 0.52, slope));
		ground_far = mix(ground_far, vec3(0.76, 0.81, 0.88), snow_far);
		rough = mix(rough, clamp(mix(0.91, 0.72, wetness * bare_far), 0.55, 0.98),
			strength);
		return mix(base, max(ground_far, vec3(0.004)), strength);
	}
	vec3 grass = mix(texture(u_ground_grass_albedo, grass_uv0).rgb,
		texture(u_ground_grass_albedo, grass_uv1).rgb, 0.22 + grass_mix * 0.56);
	vec3 soil = mix(texture(u_ground_soil_albedo, soil_uv0).rgb,
		texture(u_ground_soil_albedo, soil_uv1).rgb, 0.20 + soil_mix * 0.58);
	// The broad colour drift is intentionally subtle. It keeps hectares from
	// averaging to a repeated checker while preserving the scan's real colour.
	grass *= mix(0.94, 1.06, grass_mix);
	soil *= mix(0.93, 1.07, soil_mix);
	// Carry the 600 m--90 km value/hue composition into the photographic
	// constituents. Previously the scans replaced the already-tinted base, so
	// all lower-frequency colour structure vanished wherever texture weight was
	// close to one.
	grass *= material_tint;
	soil *= material_tint;

	vec3 grass_n0 = texture(u_ground_grass_normal, grass_uv0).xyz * 2.0 - 1.0;
	vec3 grass_n1 = texture(u_ground_grass_normal, grass_uv1).xyz * 2.0 - 1.0;
	grass_n1.xy = vec2(0.80 * grass_n1.x + 0.60 * grass_n1.y,
		-0.60 * grass_n1.x + 0.80 * grass_n1.y);
	vec3 soil_n0 = texture(u_ground_soil_normal, soil_uv0).xyz * 2.0 - 1.0;
	vec3 soil_n1 = texture(u_ground_soil_normal, soil_uv1).xyz * 2.0 - 1.0;
	soil_n1.xy = vec2(0.26 * soil_n1.x + 0.97 * soil_n1.y,
		-0.97 * soil_n1.x + 0.26 * soil_n1.y);
	vec3 grass_n = normalize(mix(grass_n0, grass_n1, 0.22 + grass_mix * 0.56));
	vec3 soil_n = normalize(mix(soil_n0, soil_n1, 0.20 + soil_mix * 0.58));
	float grass_rough = mix(texture(u_ground_grass_roughness, grass_uv0).r,
		texture(u_ground_grass_roughness, grass_uv1).r, 0.22 + grass_mix * 0.56);
	float soil_rough = mix(texture(u_ground_soil_roughness, soil_uv0).r,
		texture(u_ground_soil_roughness, soil_uv1).r, 0.20 + soil_mix * 0.58);

	// Preserve the climate response without repainting the scans. Moist foliage
	// is a little darker/cooler; damp soil darkens strongly but remains brown.
	grass *= mix(vec3(0.94, 1.04, 0.91), vec3(0.78, 0.91, 0.76), wetness * 0.62);
	soil *= mix(vec3(1.08, 0.94, 0.82), vec3(0.70, 0.61, 0.54), wetness * 0.72);

	// The grass scan already contains real exposed earth. Only move toward the
	// separate mud scan when ecology says grass should fail; do not stamp a
	// generated dirt mask over healthy grass, which was the altitude-visible
	// leopard pattern in the previous material.
	float grass_suit = clamp(vegetation * 1.55, 0.0, 1.0)
		* (1.0 - smoothstep(0.22, 0.68, slope));
	float failure = (1.0 - grass_suit) + sand * 0.22;
	float ecological_bare = smoothstep(0.18, 0.88, failure);
	// Variation comes from the photographic grass cover at a broad mip, so it
	// remains organic without becoming a field of algorithmic blobs.
	float opening = smoothstep(0.26, 0.76, 1.0 - grass_mix);
	float bare = clamp(ecological_bare * 0.58
		+ opening * ecological_bare * 0.42, 0.0, 1.0);

	vec3 ground = mix(grass, soil, bare);
	vec3 map_n = normalize(mix(grass_n, soil_n, bare));
	vec3 mapped_world = normalize(tangent * map_n.x + bitangent * map_n.y + up * map_n.z);
	normal_offset = mix(normal_offset, mapped_world - up, strength);
	float map_rough = mix(grass_rough, soil_rough, bare);
	map_rough = clamp(map_rough - wetness * bare * 0.24, 0.34, 1.0);
	rough = mix(rough, map_rough, strength);

	// Existing specialist layers remain: aeolian sand and frost are biome
	// processes, not substitutes for the grass/soil scans.
	if (sand > 0.02) {
		float ripple = gd_ridged(dpos * vec3(1.0, 0.30, 1.0) * 6.5,
			1.0, pixel_m * 6.5);
		vec3 sand_col = mix(vec3(0.300, 0.245, 0.160), vec3(0.455, 0.390, 0.275), ripple);
		sand_col *= material_tint;
		ground = mix(ground, sand_col, clamp(sand * 1.25, 0.0, 1.0));
		bump = mix(bump, ripple - 0.5, clamp(sand * 1.25, 0.0, 1.0) * strength);
	}
	float snow_hold = frost * (1.0 - smoothstep(0.16, 0.52, slope));
	if (snow_hold > 0.02) {
		float drift = gd_fbm(dpos, 2.2, pixel_m, 0.55);
		vec3 snow = mix(vec3(0.58, 0.64, 0.74), vec3(0.86, 0.89, 0.93), drift + 0.5);
		ground = mix(ground, snow, snow_hold);
	}
	return mix(base, max(ground, vec3(0.004)), strength);
}

/// Temperate forest material used both below and above the canopy transition.
/// Keeping one photographic source across camera heights prevents the material
/// identity from changing at 24 m or fading into procedural crown noise at
/// 200--900 m. The above-canopy view is colour-graded darker/greener, but retains
/// the same spatial evidence from the scan.
vec3 forest_surface_textured(vec3 base, vec3 dpos, vec3 up, float wetness,
		float canopy_view, vec3 material_tint, float strength, inout float rough,
		inout vec3 normal_offset, out float sunfleck) {
	vec3 tangent;
	vec3 bitangent;
	gd_basis(up, tangent, bitangent);
	vec2 metres = vec2(dot(dpos, tangent), dot(dpos, bitangent));
	float fs = max(u_forest_floor_metres, 0.01);
	vec2 uv0 = metres / fs;
	vec2 uv1 = vec2(0.60 * metres.x - 0.80 * metres.y,
		0.80 * metres.x + 0.60 * metres.y) / (fs * 1.429) + vec2(0.237, 0.791);
	vec2 uvm = vec2(0.87 * metres.x + 0.49 * metres.y,
		-0.49 * metres.x + 0.87 * metres.y) / (fs * 10.91) + vec2(0.719, 0.193);
	vec3 macro = texture(u_forest_floor_albedo, uvm).rgb;
	float macro_luma = dot(macro, vec3(0.24, 0.62, 0.14));
	float blend = 0.20 + 0.58 * smoothstep(0.07, 0.32, macro_luma);
	vec3 forest = mix(texture(u_forest_floor_albedo, uv0).rgb,
		texture(u_forest_floor_albedo, uv1).rgb, blend);
	forest *= mix(0.94, 1.06, smoothstep(0.06, 0.34, macro_luma));
	forest *= mix(vec3(1.04, 1.00, 0.94), vec3(0.78, 0.88, 0.75), wetness * 0.58);
	forest *= material_tint;

	// The canopy uses the real leafy scan that was previously (incorrectly)
	// routed to ordinary grass. Two rotated projections keep a stand from reading
	// as a tiled square while the floor scan remains visible below the canopy.
	float cs = max(u_forest_canopy_metres, 0.01);
	vec2 cuv0 = metres / cs + vec2(0.119, 0.463);
	vec2 cuv1 = vec2(0.73 * metres.x + 0.68 * metres.y,
		-0.68 * metres.x + 0.73 * metres.y) / (cs * 1.613) + vec2(0.827, 0.251);
	vec2 cuvm = vec2(0.42 * metres.x - 0.91 * metres.y,
		0.91 * metres.x + 0.42 * metres.y) / (cs * 12.37) + vec2(0.347, 0.917);
	vec3 canopy_macro = texture(u_forest_canopy_albedo, cuvm).rgb;
	float canopy_blend = 0.22 + 0.56 * smoothstep(0.035, 0.24,
		dot(canopy_macro, vec3(0.16, 0.72, 0.12)));
	vec3 canopy = mix(texture(u_forest_canopy_albedo, cuv0).rgb,
		texture(u_forest_canopy_albedo, cuv1).rgb, canopy_blend);
	canopy *= mix(0.94, 1.06, canopy_blend);
	canopy *= mix(vec3(1.02, 1.00, 0.96), vec3(0.82, 0.91, 0.80), wetness * 0.45);
	// The scan was captured in open diffuse light, while an aerial forest patch
	// is the self-shadowed top of a layered canopy. Calibrate its value without
	// manufacturing colour detail: all spatial structure still comes from the
	// photograph and the ecological stand mask.
	// The scan contains both leaves and a large amount of exposed brown litter.
	// Its distant mip therefore averages to olive/brown, even though a forest
	// stand seen from above is dominated by chlorophyll. Calibrate the aerial
	// response channel-wise while retaining every spatial variation from the
	// photograph; the forest floor below is intentionally left unaltered.
	canopy *= vec3(0.34, 0.82, 0.27);
	canopy *= material_tint;
	vec3 material = mix(forest, canopy, canopy_view);

	vec3 n0 = texture(u_forest_floor_normal, uv0).xyz * 2.0 - 1.0;
	vec3 n1 = texture(u_forest_floor_normal, uv1).xyz * 2.0 - 1.0;
	n1.xy = vec2(0.60 * n1.x + 0.80 * n1.y,
		-0.80 * n1.x + 0.60 * n1.y);
	vec3 map_n = normalize(mix(n0, n1, blend));
	vec3 mapped_world = normalize(tangent * map_n.x + bitangent * map_n.y + up * map_n.z);
	// Ground aggregate normals are useful below the canopy. From above, the
	// terrain's kilometre-scale normal is authoritative until tree geometry is
	// introduced, so do not fake crown geometry with a ground normal map.
	float normal_w = strength * (1.0 - canopy_view);
	normal_offset = mix(normal_offset, mapped_world - up, normal_w);
	float map_rough = mix(texture(u_forest_floor_roughness, uv0).r,
		texture(u_forest_floor_roughness, uv1).r, blend);
	rough = mix(rough, clamp(map_rough, 0.55, 1.0), strength);
	sunfleck = mix(0.16, 0.48, smoothstep(0.06, 0.34, macro_luma));
	return mix(base, max(material, vec3(0.004)), strength);
}

// --------------------------------------------------------------- forest floor ---
/// The floor of a closed stand, seen from under the canopy.
///
/// The crown model above is a top-down one: it draws what a forest looks like
/// from an aircraft. Standing in a wood you are *underneath* that, and what is
/// actually in front of you is litter, moss, roots and fallen wood in deep shade
/// with sunflecks through it. Painting the aerial view onto the ground plane at
/// eye height is why standing in this forest looked like standing on a
/// photograph of one.
///
/// Returns the floor albedo and, through `sunfleck`, the fraction of direct sun
/// that reaches it -- the caller multiplies the sun's visibility by that, which
/// is what puts moving pools of light on a forest floor instead of a flat
/// darkening.
vec3 forest_floor(vec3 base, vec3 dpos, float pixel_m, float wetness,
		out float sunfleck, out float bump) {
	// Litter is a mat of discrete pieces, not a continuous field: leaves, needles
	// and twigs at a hand's width, over a coarser drift of the same.
	float drift = gd_fbm(dpos, 3.4, pixel_m, 0.55);
	// Two clump scales rather than one. A single cellular field at one spacing
	// reads as cobbles however it is coloured, because every clump is the same
	// size; litter is a mixture of leaf, needle and twig sizes and needs at least
	// two. Both are domain-warped, because a Voronoi distance field tiles the
	// plane exactly -- every cell the same size, no overlaps and no gaps -- and
	// unwarped it reads as scales however it is weighted.
	vec3 lw = dpos + vec3(
		gd_value(dpos * 2.7),
		gd_value(dpos * 2.7 + vec3(19.0, 0.0, 0.0)),
		gd_value(dpos * 2.7 + vec3(0.0, 41.0, 0.0))) * 0.075;
	float leaves = 1.0 - gd_cells(lw * 9.0);
	float fines = 1.0 - gd_cells(lw * 31.0);
	// Grain: an fBm from a hand's width down to the millimetre. This is what
	// carries the relief, not the cellular fields -- fBm has no lattice to show
	// and no characteristic size to repeat, which is exactly what a mat of mixed
	// debris looks like at arm's length.
	float grit = gd_fbm(dpos, 0.09, pixel_m, 0.55);
	// Balanced so no one scale can saturate the clamp. `drift` at 1.05 spanned the
	// whole range on its own, so both clump scales were being added to a field
	// already pinned at 0 or 1 and contributed nothing -- which is why adding
	// pixels changed nothing: the fine detail was being generated and then thrown
	// away by a clamp. A field that is uniform is a uniform surface however many
	// octaves went into it.
	float duff = clamp(0.5 + drift * 0.62
		+ (leaves - 0.5) * 0.38 + (fines - 0.5) * 0.34 + grit * 0.30, 0.0, 1.0);
	// Absolute colours, on the BIOME_ALBEDO scale. Leaf litter is brown and
	// surprisingly bright against a canopy; the humus under it is nearly black.
	vec3 litter = mix(vec3(0.070, 0.052, 0.032), vec3(0.190, 0.140, 0.076), duff);
	// Moss and low ferns take the damp, shaded ground between the litter drifts.
	float moss_mask = smoothstep(0.62, 0.18, duff) * clamp(wetness * 1.5, 0.0, 1.0);
	vec3 moss = vec3(0.030, 0.062, 0.024);
	vec3 floor_col = mix(litter, moss, moss_mask * 0.75);
	// Fallen wood. This has to be genuinely sparse: a ridged field is a connected
	// network by construction, so thresholding it loosely does not give scattered
	// logs, it gives a brushed-metal grain combed across the whole floor. Gating
	// it on a separate low-frequency mask is what turns the network into a few
	// isolated pieces of it.
	float log_where = smoothstep(0.62, 0.86,
		gd_fbm(dpos, 7.0, pixel_m, 0.5) + 0.5);
	float logs = smoothstep(0.86, 0.97,
		gd_ridged(dpos * vec3(1.0, 0.45, 0.30) * 1.6, 1.0, pixel_m * 1.6))
		* log_where;
	floor_col = mix(floor_col, vec3(0.088, 0.070, 0.048), logs * 0.7);

	// Sunflecks. A closed canopy passes a few per cent of direct sun through
	// gaps, in sharp-edged patches a metre or two across that are the brightest
	// thing on the floor by an order of magnitude.
	float gaps = gd_fbm_band(dpos, 14.0, 2.2, pixel_m, 0.5) * 2.2 + 0.5;
	// Not zero between the flecks: some sun arrives having been scattered by the
	// leaves it passed through rather than missing them, and a canopy is not an
	// opaque lid.
	sunfleck = mix(0.34, 1.0, smoothstep(0.66, 0.92, gaps));
	// Relief. Litter is loose material lying on the ground, not paint on it, and
	// without a height signal the whole floor is lit as a flat plane -- which at
	// 4K is exactly the "more pixels, same blur" failure, because contrast at the
	// scale of a leaf comes almost entirely from its own shading and not from its
	// colour. Logs stand proud of the drifts they lie on.
	// Weighted toward the smooth drift and the finest clumps rather than the
	// mid-scale cellular field. A single cellular field carrying the relief
	// tiles the floor with same-sized cones -- lizard skin, not litter -- because
	// every clump is the same size and every gap between them is the same depth.
	bump = drift * 0.50 + grit * 0.95 + (leaves - 0.5) * 0.16 + logs * 0.45;
	return mix(base, floor_col, 0.92);
}

// --------------------------------------------------------------- forest cover ---
/// Fall in the cell's `frost` weight per metre of altitude.
///
/// `frost` is packed as (3.5 C - mean temperature) / 17.5 C, and air cools at
/// the environmental lapse rate of about 6.5 C per kilometre, so a metre of
/// height is worth 0.0065 / 17.5 of frost.
const float FOREST_FROST_PER_M = 3.714e-4;

/// Where trees actually stand, and the smooth expectation of that.
///
/// A forest is not a colour a biome has, it is a population with a distribution.
/// Trees crowd into sheltered, damp, gently sloping ground, thin out on exposed
/// ridges where soil will not stay, and stop altogether at a treeline. Painting
/// the whole biome one green is what makes a forest read as a lawn, and it is
/// why the boundary between forest and not-forest has to be a field rather than
/// a biome edge.
///
/// Two coordinates, because no single one can carry the whole range. `pd` is
/// planet-anchored and exact to a few centimetres, which is plenty for anything
/// measured in kilometres but useless below a metre; `dpos` is the local detail
/// coordinate, exact to micrometres but wrapped every GD_ORIGIN_WRAP metres and
/// therefore unusable for anything larger than a few hundred. The mask is
/// assembled from bands on whichever of the two can actually represent them.
///
/// `alt_anomaly` is the fragment's height above the local mean surface, in
/// metres. It is what makes the treeline a contour on a mountainside instead of
/// a step at the edge of the eight-kilometre climate cell that averaged over it,
/// and it simultaneously thins the stand on ridges and thickens it in hollows.
vec2 forest_cover(vec3 pd, vec3 dpos, float pixel_m, float vegetation,
		float wetness, float slope, float frost, float alt_anomaly) {
	// Domain warp before anything else, at both scales. Unwarped noise thresholds
	// into round blobs; a real stand edge is fingered and re-entrant because it
	// follows drainage lines and aspect. One cheap octave per axis is enough --
	// the warp only has to break the circularity, not carry detail of its own.
	vec3 warp_c = vec3(
		gd_value((pd + vec3(311.0, 0.0, 0.0)) / 7300.0),
		gd_value((pd + vec3(0.0, 907.0, 0.0)) / 7300.0),
		gd_value((pd + vec3(0.0, 0.0, 1733.0)) / 7300.0)) - 0.5;
	vec3 wc = pd + warp_c * 4600.0;
	vec3 warp_f = vec3(
		gd_value((dpos + vec3(11.3, 0.0, 0.0)) / 165.0),
		gd_value((dpos + vec3(0.0, 37.1, 0.0)) / 165.0),
		gd_value((dpos + vec3(0.0, 0.0, 73.7)) / 165.0)) - 0.5;
	vec3 wf = dpos + warp_f * 120.0;

	// Three bands covering twenty kilometres down to fifteen metres: the extent
	// of a forest region, the stands inside it, and the groves inside those.
	// The old mask started at 300 m, which is below one pixel from three
	// kilometres up -- so from any altitude worth the name the entire landcover
	// pattern simply vanished and the planet went back to being painted by the
	// eight-kilometre climate grid alone. Whatever the viewing distance, at least
	// one of these bands is resolvable.
	float region = gd_fbm_band(wc, 21000.0, 850.0, pixel_m, 0.62);
	float stand = gd_fbm_band(wf, 640.0, 85.0, pixel_m, 0.56);
	float grove = gd_fbm_band(wf, 52.0, 15.0, pixel_m, 0.55);

	// Cross-fade between the bands by whether each is resolvable at all, and
	// normalise by the length of the weight vector rather than its sum. The bands
	// are independent, so their variances add in quadrature; dividing by the sum
	// would shrink the mask spread as more of them come alive, which moves the
	// realised cover with viewing distance and draws a ring on the ground where
	// a band switches on.
	vec3 w = vec3(
		gd_octave_weight(21000.0, pixel_m) * 0.62,
		gd_octave_weight(640.0, pixel_m) * 0.30,
		gd_octave_weight(52.0, pixel_m) * 0.16);
	float mask = 0.5 + dot(vec3(region, stand, grove), w)
		/ max(length(w), 1e-4) * 2.4;

	// Deliberately no mesh-slope term in the threshold. `slope` comes from the
	// interpolated mesh normal, which is only C0 across a triangle, and feeding
	// it into the threshold stamps the tessellation onto the stand edges as
	// faceted blobs tens of metres across. Steepness is applied after the
	// threshold instead, where a facet becomes a slight brightness change rather
	// than a hard edge.
	float suit = clamp(vegetation * 1.55, 0.0, 1.0);
	suit *= mix(0.50, 1.0, clamp(wetness * 1.7, 0.0, 1.0));
	// Treeline, and shelter. Air cools with height, so the limit of tree growth
	// lies along a contour of altitude -- it wraps around a peak and reaches down
	// a cold valley. Taking it from the cell frost weight alone put it on the
	// climate grid instead, which is a straight line across a mountainside. The
	// same anomaly does the other half of the job for free: ground standing above
	// its surroundings is windswept and thin-soiled, ground below collects both
	// water and the soil that came off the ridge.
	float local_frost = clamp(frost + alt_anomaly * FOREST_FROST_PER_M, 0.0, 1.0);
	suit *= 1.0 - smoothstep(0.40, 0.76, local_frost);
	suit *= mix(1.15, 0.74, smoothstep(-120.0, 160.0, alt_anomaly));
	suit = clamp(suit, 0.0, 1.0);

	// Suitability sets *how much* of the landscape carries forest, by moving the
	// threshold; the mask decides *where*. Multiplying by suitability instead
	// spreads a uniform half-cover haze over everything, which is a forest
	// nowhere rather than a forest somewhere -- and it is the difference between
	// a stand with an edge and a green tint.
	// Even in ideal conditions a landscape is not wall-to-wall trees. Bogs, rock,
	// water, avalanche tracks, burns and blowdowns keep a fifth or so of any real
	// forest open, and letting suitability drive the threshold all the way down
	// produced a single continuous green blanket with no edges anywhere in it --
	// which is the thing that reads as a painted texture rather than as woodland.
	float t = mix(0.80, 0.37, suit);
	// Analytic antialiasing. A fixed feather is fine where the mask changes
	// slowly across a pixel and useless where it does not: at distance the same
	// stand edge crosses the threshold within a pixel or two, and a hard step in
	// a procedural field aliases exactly like a hard step in a texture. Widening
	// the feather to at least the mask own screen-space gradient makes the edge
	// resolve smoothly at every range without softening it up close.
	// A real stand edge is sharp -- a few tens of metres from closed canopy to
	// open ground -- so the feather has to be narrow against the mask's own
	// spread of about 0.27. At 0.19 it covered most of that spread, which turned
	// every boundary into a gradient and the whole landcover field into an even
	// green wash with no edges in it anywhere. What widens it is distance, where
	// a stand edge really does cross the threshold inside one pixel and a hard
	// step would alias exactly like a hard step in a texture; `aa` covers the
	// screen-space part of the same problem.
	float aa = max(fwidth(mask) * 1.4, 0.0005);
	float feather = max(clamp(0.05 + pixel_m * 0.0016, 0.05, 0.42), aa);
	// Trees will not hold a steep face, but that has to attenuate an already
	// resolved stand rather than decide where its edge falls.
	float steep = mix(1.0, 0.22, smoothstep(0.20, 0.58, slope));
	float cover = smoothstep(t - feather, t + feather, mask) * steep;

	// How much of this neighbourhood carries trees, as a smooth field rather
	// than a realised one. The mask is an fBm sum which is very close to normal
	// about 0.5 with a standard deviation near 0.27, so the fraction of it above
	// the threshold follows from the threshold alone -- no second evaluation and
	// no spatial filtering needed. The albedo uses this to normalise, which is
	// what lets the biome palette survive as the average of what gets drawn.
	float expected = clamp(0.5 - (t - 0.5) / 0.667, 0.0, 1.0) * steep;
	return vec2(cover, expected);
}

/// Apply canopy over the biome albedo, conserving the biome albedo as the mean.
///
/// A stand is darker than the clearing beside it, so both have to exist either
/// side of the baked colour: painting canopy over the biome colour alone leaves
/// forest and clearing differing only in texture. But the correction has to be
/// normalised by how much forest there actually is, and it was not. Open ground
/// was a flat 1.95x the biome albedo whether the mask put trees next to it or
/// not -- so every region the mask declined to forest, which is every region
/// with low suitability, was simply painted twice as bright and slightly warm.
/// From orbit that is a continent-sized pale tan wash sitting over ground that
/// the climate model had already coloured correctly, and it was the single
/// largest source of the washed-out look.
///
/// Here the tints are relative to the stand average and the result is divided by
/// the average tint the local cover implies, so the mean over any patch comes
/// back to `base` by construction. Open ground still sits above the biome colour
/// and canopy below; there is simply no longer a free brightness multiplier
/// riding along with it.
vec3 forest_albedo(vec3 base, float cover, float expected, vec3 pd, vec3 dpos,
		float pixel_m, inout float bump) {
	// Reflectance of each component relative to the stand average.
	const vec3 OPEN_TINT = vec3(1.52, 1.38, 1.06);
	const vec3 CROWN_TINT = vec3(0.74, 0.90, 0.68);
	const vec3 GAP_TINT = vec3(0.30, 0.38, 0.36);
	// Fraction of a closed canopy that is lit crown rather than shadowed gap.
	const float LIT_FRACTION = 0.56;
	vec3 canopy_mean = mix(GAP_TINT, CROWN_TINT, LIT_FRACTION);

	if (cover < 0.004 && expected < 0.004) return base * OPEN_TINT / OPEN_TINT;

	// Individual crowns, roughly ten metres across. This is the texture that
	// makes a forest legible from the air -- the reason woodland reads as
	// broccoli and grassland does not.
	// Band-limited fBm rather than a cellular field. Crowns really are discrete
	// objects and cellular is the honest model for them, but a cube lattice is
	// visible as soon as anything darkens its interiors, and at this range the
	// crown texture reads as mottling regardless. A field with no lattice cannot
	// show one.
	float crowns = gd_fbm_band(dpos, 26.0, 8.0, pixel_m, 0.5) * 2.6 + 0.5;
	float lit = smoothstep(0.18, 0.86, clamp(crowns, 0.0, 1.0));
	vec3 canopy = mix(GAP_TINT, CROWN_TINT, lit);
	// Species mosaic. A forest is never one green: conifer stands are dark and
	// blue-green, broadleaf stands lighter and distinctly yellow-green, and the
	// two interlock at scales from a few hundred metres to a few kilometres --
	// which from the air is most of what makes woodland read as woodland rather
	// than as a green surface.
	//
	// The coarse term runs on the planet-anchored coordinate. `dpos` wraps every
	// GD_ORIGIN_WRAP metres, so anything kilometre-scale evaluated on it tiles
	// the species map visibly across the landscape; it is only safe for the fine
	// term, which is far below the wrap.
	float species = (gd_value(pd / 2900.0) - 0.5) * 0.62
		+ (gd_value(dpos / 340.0) - 0.5) * 0.38;
	const vec3 CONIFER = vec3(0.80, 0.95, 0.80);
	const vec3 BROADLEAF = vec3(1.26, 1.14, 0.70);
	canopy *= mix(CONIFER, BROADLEAF, smoothstep(-0.20, 0.20, species));

	vec3 tint = mix(OPEN_TINT, canopy, cover);
	vec3 mean_tint = mix(OPEN_TINT, canopy_mean, expected);
	bump = mix(bump, crowns, cover * 0.85);
	return base * tint / max(mean_tint, vec3(0.25));
}
