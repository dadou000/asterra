from pathlib import Path
import re

ROOT = Path('.')

HEIGHTS = [100,250,450,700,1000,1350,1750,2200,2700,3250,3850,4500,5200,5950,6750,7600,8500,9400,10300,11200,12100,13000,13900,14800,15700,16600,17500,18400,19300,20200]
SIGMA = [0.987578,0.969233,0.945303,0.916219,0.882497,0.844720,0.803523,0.759572,0.713552,0.666144,0.618010,0.569783,0.522046,0.475328,0.430095,0.386741,0.345591,0.308819,0.275960,0.246597,0.220358,0.196912,0.175960,0.157237,0.140507,0.125556,0.112197,0.100259,0.089591,0.080058]
LAYER_WEIGHTS = [0.700583,0.685757,0.859964,1.018795,1.159806,1.281069,1.381213,1.459443,1.515535,1.549812,1.563101,1.556674,1.532182,1.491570,1.437001,1.370766,1.264000,1.129507,1.009324,0.901929,0.805962,0.720205,0.643574,0.575096,0.513904,0.459223,0.410361,0.366697,0.327680,0.309267]
WIND_TAU = [172800.0,172800.0,172800.0,190080.0,210816.0,235008.0,263250.0,299700.0,340200.0,384750.0,461113.0,546573.9,638608.7,753765.5,896772.4,1048717.2,1209600.0,1354269.8,1498939.5,1643609.3,1788279.1,1900800.0,1900800.0,1900800.0,1900800.0,1900800.0,1900800.0,1900800.0,1900800.0,1900800.0]
THERMAL_TAU = [172800.0,172800.0,172800.0,181440.0,191808.0,203904.0,217350.0,229500.0,243000.0,257850.0,279860.9,304278.3,330573.9,366455.2,414124.1,464772.4,518400.0,554567.4,590734.9,626902.3,663069.8,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0]
MOISTURE_TAU = [86400.0,86400.0,86400.0,95040.0,105408.0,117504.0,132300.0,156600.0,183600.0,213300.0,246991.3,283617.4,323060.9,366455.2,414124.1,464772.4,518400.0,554567.4,590734.9,626902.3,663069.8,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0,691200.0]
FALLBACK_SPEED = [1.000,1.000,1.000,1.044,1.097,1.158,1.228,1.301,1.383,1.472,1.552,1.637,1.728,1.816,1.899,1.987,2.080,2.134,2.189,2.243,2.298,2.340,2.340,2.340,2.340,2.340,2.340,2.340,2.340,2.340]


def fmt_float_array(values, indent='\t\t', per_line=6):
    lines = []
    for i in range(0, len(values), per_line):
        part = values[i:i+per_line]
        lines.append(indent + ', '.join(f'{v:.6f}f' for v in part) + (',' if i + per_line < len(values) else ''))
    return '\n'.join(lines)


def fmt_int_array(values, indent='\t\t', per_line=8):
    lines = []
    for i in range(0, len(values), per_line):
        part = values[i:i+per_line]
        lines.append(indent + ', '.join(f'{v}.0f' for v in part) + (',' if i + per_line < len(values) else ''))
    return '\n'.join(lines)


def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError(f'missing replacement target: {label}')
    return text.replace(old, new, 1)

# -----------------------------------------------------------------------------
# Native public constants / vertical coordinate
# -----------------------------------------------------------------------------
path = ROOT / 'native/weather/src/weather_native.h'
text = path.read_text(encoding='utf-8')
start = text.index('\tstatic constexpr int GLOBAL_W = 256;')
end_marker = '\tstatic constexpr std::array<float, LAYERS> DEFAULT_LAYER_WEIGHTS = {'
end = text.index(end_marker, start)
end = text.index('\n\t};', end) + len('\n\t};')
new_block = f'''\tstatic constexpr int GLOBAL_W = 1024;
\tstatic constexpr int GLOBAL_H = 512;
\tstatic constexpr int LOCAL_W = 192;
\tstatic constexpr int LOCAL_H = 192;
\tstatic constexpr int LAYERS = 30;
\tstatic constexpr int INTERFACES = LAYERS - 1;
\tstatic constexpr int HORIZON_SECTORS = 24;
\tstatic constexpr float PLANET_RADIUS_M = 3500000.0f;
\tstatic constexpr float LOCAL_CELL_M = 2200.0f;

\t// 30 non-uniform pressure/sigma levels. The lowest kilometre is densely
\t// sampled for boundary-layer physics, while the top reaches ~20.2 km.
\t// sigma ~= exp(-z/8 km), preserving the old six anchor levels closely.
\tstatic constexpr std::array<float, LAYERS> SIGMA = {{
{fmt_float_array(SIGMA)}
\t}};
\tstatic constexpr std::array<float, LAYERS> APPROX_HEIGHT_M = {{
{fmt_int_array(HEIGHTS)}
\t}};
\t// Pressure-thickness contribution normalized to a mean of one. The sum is
\t// exactly ~30, so column products can explicitly rescale to the old six-level
\t// optical calibration where required.
\tstatic constexpr std::array<float, LAYERS> DEFAULT_LAYER_WEIGHTS = {{
{fmt_float_array(LAYER_WEIGHTS)}
\t}};'''
text = text[:start] + new_block + text[end:]
text = replace_once(text,
    '\tint get_layer_count() const { return LAYERS; }\n',
    '\tint get_layer_count() const { return LAYERS; }\n\tint get_global_width() const { return GLOBAL_W; }\n\tint get_global_height() const { return GLOBAL_H; }\n\tfloat get_layer_height_m(int layer) const {\n\t\treturn layer >= 0 && layer < LAYERS ? APPROX_HEIGHT_M[layer] : 0.0f;\n\t}\n',
    'native getters')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Native core
# -----------------------------------------------------------------------------
path = ROOT / 'native/weather/src/weather_native.cpp'
text = path.read_text(encoding='utf-8')
text = text.replace('static constexpr float BOTTOM_AIR_MASS_KG_M2 = 1500.0f;',
                    'static constexpr float BOTTOM_AIR_MASS_KG_M2 = 242.223f; // 0..~190 m pressure slab.')

old_tau_re = re.compile(r'''static constexpr std::array<float, WeatherNative::LAYERS> WIND_DRAG_TAU_S = \{.*?\};\nstatic constexpr std::array<float, WeatherNative::LAYERS> THERMAL_RELAX_TAU_S = \{.*?\};\nstatic constexpr std::array<float, WeatherNative::LAYERS> MOISTURE_RELAX_TAU_S = \{.*?\};''', re.S)
new_tau = f'''static constexpr std::array<float, WeatherNative::LAYERS> WIND_DRAG_TAU_S = {{
{fmt_float_array(WIND_TAU, indent='\t', per_line=5)}
}};
static constexpr std::array<float, WeatherNative::LAYERS> THERMAL_RELAX_TAU_S = {{
{fmt_float_array(THERMAL_TAU, indent='\t', per_line=5)}
}};
static constexpr std::array<float, WeatherNative::LAYERS> MOISTURE_RELAX_TAU_S = {{
{fmt_float_array(MOISTURE_TAU, indent='\t', per_line=5)}
}};'''
text, n = old_tau_re.subn(new_tau, text, count=1)
if n != 1:
    raise RuntimeError('tau array block not replaced')

helper_target = '''static inline float sigma_temperature_factor(int layer) {
\treturn std::pow(WeatherNative::SIGMA[layer], KAPPA);
}
'''
helper_new = helper_target + '''
static inline int nearest_layer_for_height(float target_m) {
\tint best = 0;
\tfloat best_error = std::abs(WeatherNative::APPROX_HEIGHT_M[0] - target_m);
\tfor (int layer = 1; layer < WeatherNative::LAYERS; ++layer) {
\t\tfloat error = std::abs(WeatherNative::APPROX_HEIGHT_M[layer] - target_m);
\t\tif (error < best_error) {
\t\t\tbest_error = error;
\t\t\tbest = layer;
\t\t}
\t}
\treturn best;
}

static inline float old_six_level_column_scale() {
\treturn 6.0f / float(WeatherNative::LAYERS);
}
'''
text = replace_once(text, helper_target, helper_new, 'vertical helper')

# Bind runtime shape metadata in the original binder too (the Oklahoma wrapper
# has its own binder and is patched below).
text = replace_once(text,
    '\tClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);\n',
    '\tClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);\n\tClassDB::bind_method(D_METHOD("get_global_width"), &WeatherNative::get_global_width);\n\tClassDB::bind_method(D_METHOD("get_global_height"), &WeatherNative::get_global_height);\n\tClassDB::bind_method(D_METHOD("get_layer_height_m", "layer"), &WeatherNative::get_layer_height_m);\n',
    'core bind metadata')

# Initialization must scale with physical height, not layer ordinal.
text = text.replace('wave * (2.1f - 0.20f * layer)',
                    'wave * (2.1f - std::clamp(h / 12800.0f, 0.0f, 1.0f))')
text = text.replace('float upper = float(layer) / float(LAYERS - 1);',
                    'float upper = std::clamp(h / 12800.0f, 0.0f, 1.0f);', 1)
text = text.replace('layer * 0.37f', 'upper * 1.85f', 1)
text = text.replace('base_rh - 0.045f * float(layer) + wave * 0.035f',
                    'base_rh - 0.225f * upper + wave * 0.035f')
# Start clear; cloud should emerge from the prognostic water budget, not from a
# broad initialization seed.
text = text.replace('sat * 0.80f', 'sat * 0.96f', 1)
text = text.replace('supersat * (1.0f - ice_frac) * 0.55f', 'supersat * (1.0f - ice_frac) * 0.20f', 1)
text = text.replace('supersat * ice_frac * 0.55f', 'supersat * ice_frac * 0.20f', 1)

# Horizontal climatology: use height fractions so 30 levels do not become
# progressively colder/drier merely because their integer index is larger.
text = text.replace('+ 0.05f * std::pow(sinlat, 1.2f) - 0.045f * float(layer), 0.25f, 0.88f);',
                    '+ 0.05f * std::pow(sinlat, 1.2f) - 0.225f * std::clamp(height_m / 12800.0f, 0.0f, 1.0f), 0.25f, 0.88f);')
# Replace the second old ordinal upper fraction (inside horizontal_pass).
text = text.replace('float upper = float(layer) / float(LAYERS - 1);',
                    'float upper = std::clamp(height_m / 12800.0f, 0.0f, 1.0f);', 1)
# Preserve actual vertical derivative resolution.
text = text.replace('float dsigma = std::max(std::abs(SIGMA[above_layer] - SIGMA[below_layer]), 0.05f);',
                    'float dsigma = std::max(std::abs(SIGMA[above_layer] - SIGMA[below_layer]), 0.005f);')

# Parallel global horizontal rows. Scratch vectors must be row-private.
h_start = text.index('void WeatherNative::horizontal_pass(Atmosphere &a, bool is_global, float dt) {')
h_end = text.index('\n\nvoid WeatherNative::surface_pass', h_start)
h = text[h_start:h_end]
scratch = '''\talignas(32) int adv00_idx[8], adv10_idx[8], adv01_idx[8], adv11_idx[8];
\talignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
\talignas(32) float adv_fx[8], adv_fy[8], target_u_lane[8], target_v_lane[8];
\talignas(32) float adv00_sign[8], adv10_sign[8], adv01_sign[8], adv11_sign[8];
\talignas(32) float north_sign[8], south_sign[8];

'''
if scratch not in h:
    raise RuntimeError('horizontal scratch block missing')
h = h.replace(scratch, '', 1)
yloop = '\t\tfor (int y = 0; y < h; ++y) {\n'
private_scratch = '''\t\t#pragma omp parallel for schedule(static) if(is_global)
\t\tfor (int y = 0; y < h; ++y) {
\t\t\talignas(32) int adv00_idx[8], adv10_idx[8], adv01_idx[8], adv11_idx[8];
\t\t\talignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
\t\t\talignas(32) float adv_fx[8], adv_fy[8], target_u_lane[8], target_v_lane[8];
\t\t\talignas(32) float adv00_sign[8], adv10_sign[8], adv01_sign[8], adv11_sign[8];
\t\t\talignas(32) float north_sign[8], south_sign[8];
'''
if yloop not in h:
    raise RuntimeError('horizontal y loop missing')
h = h.replace(yloop, private_scratch, 1)
text = text[:h_start] + h + text[h_end:]

# Surface columns are independent.
s_start = text.index('void WeatherNative::surface_pass(Atmosphere &a, SurfaceState &surface, bool is_global, float dt) {')
s_end = text.index('\n\nvoid WeatherNative::vertical_pass', s_start)
s = text[s_start:s_end]
s = replace_once(s, '\tfor (int c = 0; c < surface.cells; ++c) {\n',
                 '\t#pragma omp parallel for schedule(static) if(is_global)\n\tfor (int c = 0; c < surface.cells; ++c) {\n', 'surface omp')
text = text[:s_start] + s + text[s_end:]

# Vertical interfaces remain ordered, but columns within each interface are independent.
v_start = text.index('void WeatherNative::vertical_pass(Atmosphere &a, bool is_global, float dt) {')
v_end = text.index('\n\nvoid WeatherNative::diagnose', v_start)
v = text[v_start:v_end]
v = v.replace('\t\tfor (int c = 0; c < a.cells; c += 8) {\n',
              '\t\t#pragma omp parallel for schedule(static) if(is_global)\n\t\tfor (int c = 0; c < a.cells; c += 8) {\n', 1)
# Physical-resolution scaling for the tropical subgrid footprint.
v = v.replace('// An ~86 km cell cannot resolve an eyewall.',
              '// A ~21.5 km L0 cell still cannot fully resolve an eyewall.')
v = v.replace('\t\t// Apply a smooth unresolved eyewall/heating footprint around only the best\n',
              '\t\tconst float coarse_cell_ratio = float(GLOBAL_W) / 256.0f;\n\t\t// Apply a smooth unresolved eyewall/heating footprint around only the best\n', 1)
v = v.replace('\t\t\t\tfor (int oy = -2; oy <= 2; ++oy) {\n',
              '\t\t\t\tint track_radius = std::max(2, int(std::round(2.0f * coarse_cell_ratio)));\n\t\t\t\tfor (int oy = -track_radius; oy <= track_radius; ++oy) {\n', 1)
v = v.replace('\t\t\t\t\tfor (int ox = -2; ox <= 2; ++ox) {\n',
              '\t\t\t\t\tfor (int ox = -track_radius; ox <= track_radius; ++ox) {\n', 1)
v = v.replace('\t\t\tfor (int oy = -3; oy <= 3; ++oy) {\n',
              '\t\t\tint footprint_radius = std::max(3, int(std::round(3.0f * coarse_cell_ratio)));\n\t\t\tfor (int oy = -footprint_radius; oy <= footprint_radius; ++oy) {\n', 1)
v = v.replace('\t\t\t\tfor (int ox = -3; ox <= 3; ++ox) {\n',
              '\t\t\t\tfor (int ox = -footprint_radius; ox <= footprint_radius; ++ox) {\n', 1)
v = v.replace('\t\t\t\t\tfloat radius_sq = float(ox * ox + oy * oy);\n',
              '\t\t\t\t\tfloat nx = float(ox) / coarse_cell_ratio;\n\t\t\t\t\tfloat ny = float(oy) / coarse_cell_ratio;\n\t\t\t\t\tfloat radius_sq = nx * nx + ny * ny;\n', 1)
# Tropical vertical structures by physical altitude, not integer layer number.
v = v.replace('for (int layer = 0; layer <= 2; ++layer) {\n\t\t\t\t\t\tint i = a.layer_offset(layer) + c;\n\t\t\t\t\t\ta.npressure[i] = std::clamp(a.npressure[i] - pressure_tendency\n\t\t\t\t\t\t\t* (1.0f - 0.18f * float(layer)), -6500.0f, 6500.0f);\n\t\t\t\t\t}',
'''for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {
\t\t\t\t\t\tint i = a.layer_offset(layer) + c;
\t\t\t\t\t\tfloat decay = 1.0f - 0.36f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);
\t\t\t\t\t\ta.npressure[i] = std::clamp(a.npressure[i] - pressure_tendency * decay, -6500.0f, 6500.0f);
\t\t\t\t\t}''', 1)
v = v.replace('for (int layer = 1; layer <= 3; ++layer) {\n\t\t\t\t\t\tint i = a.layer_offset(layer) + c;',
              'for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 5600.0f; ++layer) {\n\t\t\t\t\t\tif (APPROX_HEIGHT_M[layer] < 700.0f) continue;\n\t\t\t\t\t\tint i = a.layer_offset(layer) + c;', 1)
v = v.replace('for (int layer = 0; layer <= 2; ++layer) {\n\t\t\t\t\t\tint i = a.layer_offset(layer) + c;\n\t\t\t\t\t\tfloat vertical_decay = 1.0f - 0.22f * float(layer);',
              'for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {\n\t\t\t\t\t\tint i = a.layer_offset(layer) + c;\n\t\t\t\t\t\tfloat vertical_decay = 1.0f - 0.44f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);', 1)
v = v.replace('for (int interface_index = 0; interface_index <= 2; ++interface_index) {\n\t\t\t\t\t\tint i = a.interface_offset(interface_index) + c;',
              'for (int interface_index = 0; interface_index < INTERFACES && APPROX_HEIGHT_M[interface_index + 1] <= 5600.0f; ++interface_index) {\n\t\t\t\t\t\tint i = a.interface_offset(interface_index) + c;', 1)
v = v.replace('core_convection * (1.0f - 0.14f * float(interface_index))',
              'core_convection * (1.0f - 0.42f * std::clamp(APPROX_HEIGHT_M[interface_index + 1] / 5600.0f, 0.0f, 1.0f))', 1)
v = v.replace('int anvil_i = a.layer_offset(4) + c;',
              'int anvil_i = a.layer_offset(nearest_layer_for_height(8500.0f)) + c;', 1)
# Local mesocyclone/shear loops: first three old levels represented <=3.3 km.
v = v.replace('for (int layer = 0; layer <= 2; ++layer) {\n\t\t\t\t\tshear = std::max(shear, a.shear[a.layer_offset(layer) + c]);\n\t\t\t\t}',
              'for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {\n\t\t\t\t\tshear = std::max(shear, a.shear[a.layer_offset(layer) + c]);\n\t\t\t\t}', 1)
v = v.replace('for (int layer = 0; layer <= 2; ++layer) {\n\t\t\t\t\tint i = a.layer_offset(layer) + c;\n\t\t\t\t\tfloat vertical_decay = 1.0f - 0.25f * float(layer);',
              'for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {\n\t\t\t\t\tint i = a.layer_offset(layer) + c;\n\t\t\t\t\tfloat vertical_decay = 1.0f - 0.50f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);', 1)
v = v.replace('six sigma levels do', '30 sigma levels still do', 1)
text = text[:v_start] + v + text[v_end:]

# Diagnose layers can run concurrently. Make scratch arrays private to each layer.
d_start = text.index('void WeatherNative::diagnose(Atmosphere &a, bool is_global) {')
d_end = text.index('\n\nvoid WeatherNative::update_local_basis', d_start)
d = text[d_start:d_end]
dscratch = '''\talignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
\talignas(32) float north_sign[8], south_sign[8];

'''
if dscratch not in d:
    raise RuntimeError('diagnose scratch missing')
d = d.replace(dscratch, '', 1)
d = d.replace('\tfor (int layer = 0; layer < LAYERS; ++layer) {\n',
'''\t#pragma omp parallel for schedule(static) if(is_global)
\tfor (int layer = 0; layer < LAYERS; ++layer) {
\t\talignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
\t\talignas(32) float north_sign[8], south_sign[8];
''', 1)
text = text[:d_start] + d + text[d_end:]

# Pressure layers are independent reductions.
text = text.replace('\tfor (int layer = 0; layer < LAYERS; ++layer) {\n\t\tdouble weighted_sum = 0.0;\n',
                    '\t#pragma omp parallel for schedule(static)\n\tfor (int layer = 0; layer < LAYERS; ++layer) {\n\t\tdouble weighted_sum = 0.0;\n', 1)

# Output/diagnostic products use physical altitude anchors and preserve old
# six-layer column calibration.
text = text.replace('if (layer <= 3) max_rotation = std::max(max_rotation, std::abs(a.vorticity[i]));',
                    'if (WeatherNative::APPROX_HEIGHT_M[layer] <= 5600.0f) max_rotation = std::max(max_rotation, std::abs(a.vorticity[i]));')
text = text.replace('\t\tfor (int interface_index = 0; interface_index < WeatherNative::INTERFACES; ++interface_index) {',
                    '\t\tcondensate *= old_six_level_column_scale();\n\t\tfor (int interface_index = 0; interface_index < WeatherNative::INTERFACES; ++interface_index) {', 1)
text = text.replace('int l2 = a.layer_offset(2) + c;',
                    'int l2 = a.layer_offset(nearest_layer_for_height(3300.0f)) + c;', 1)
text = text.replace('for (int layer = 0; layer <= 3; ++layer) {\n\t\t\tfloat z = a.vorticity[a.layer_offset(layer) + c];',
                    'for (int layer = 0; layer < WeatherNative::LAYERS && WeatherNative::APPROX_HEIGHT_M[layer] <= 5600.0f; ++layer) {\n\t\t\tfloat z = a.vorticity[a.layer_offset(layer) + c];', 1)
text = text.replace('float div = a.divergence[a.layer_offset(1) + c];',
                    'float div = a.divergence[a.layer_offset(nearest_layer_for_height(1700.0f)) + c];', 1)
text = text.replace('float pv = a.potential_vorticity[a.layer_offset(4) + c];',
                    'float pv = a.potential_vorticity[a.layer_offset(nearest_layer_for_height(8500.0f)) + c];', 1)
text = text.replace('if (layer >= 3) upper_ice += a.ice[i];',
                    'if (WeatherNative::APPROX_HEIGHT_M[layer] >= 5600.0f) upper_ice += a.ice[i] * WeatherNative::DEFAULT_LAYER_WEIGHTS[layer];', 1)
text = text.replace('\t\tfloat polar_resolution = 1.0f;\n\t\tif (is_global) {',
                    '\t\tupper_ice *= old_six_level_column_scale();\n\t\tfloat polar_resolution = 1.0f;\n\t\tif (is_global) {', 1)
# products_rgba has another fixed l2.
text = text.replace('int l2 = atmosphere.layer_offset(2) + c;',
                    'int l2 = atmosphere.layer_offset(nearest_layer_for_height(3300.0f)) + c;', 1)

# OpenMP output loops. Limit substitutions to known static builders.
for func in ['weather_rgba_from', 'diagnostics_rgba_from', 'convective_rgba_from', 'surface_rgba_from', 'products_rgba_from']:
    pos = text.index(func)
    loop = text.index('\tfor (int c = 0;', pos)
    text = text[:loop] + '\t#pragma omp parallel for schedule(static)\n' + text[loop:]

# Upgrade comments that would otherwise lie about the active mesh.
text = text.replace('~86 km cell', '~21.5 km cell')
text = text.replace('six-level grid', '30-level grid')
text = text.replace('six global sigma levels', '30 global sigma levels')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Oklahoma sparse/severe wrapper
# -----------------------------------------------------------------------------
path = ROOT / 'native/weather/src/weather_native_oklahoma.cpp'
text = path.read_text(encoding='utf-8')
text = replace_once(text,
    '\tClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);\n',
    '\tClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);\n\tClassDB::bind_method(D_METHOD("get_global_width"), &WeatherNative::get_global_width);\n\tClassDB::bind_method(D_METHOD("get_global_height"), &WeatherNative::get_global_height);\n\tClassDB::bind_method(D_METHOD("get_layer_height_m", "layer"), &WeatherNative::get_layer_height_m);\n',
    'wrapper bind metadata')
text = text.replace('const int l0_off = a.layer_offset(0);\n\tconst int l1_off = a.layer_offset(1);\n\tconst int l2_off = a.layer_offset(2);',
'''const int l0_layer = 0;
\tconst int l1_layer = nearest_layer_for_height(1700.0f);
\tconst int l2_layer = nearest_layer_for_height(3300.0f);
\tconst int l0_off = a.layer_offset(l0_layer);
\tconst int l1_off = a.layer_offset(l1_layer);
\tconst int l2_off = a.layer_offset(l2_layer);''')
text = text.replace('const float fair_onset_rh = layer <= 1 ? 0.90f : 0.93f;',
                    'const float fair_onset_rh = APPROX_HEIGHT_M[layer] <= 2000.0f ? 0.90f : 0.93f;')
text = text.replace('const float fair_full_rh = layer <= 1 ? 0.985f : 0.995f;',
                    'const float fair_full_rh = APPROX_HEIGHT_M[layer] <= 2000.0f ? 0.985f : 0.995f;')
text = text.replace('const float liquid_tau = std::lerp(layer <= 1 ? 480.0f : 720.0f,',
                    'const float liquid_tau = std::lerp(APPROX_HEIGHT_M[layer] <= 2000.0f ? 480.0f : 720.0f,')
text = text.replace('const float base_rate = (is_global ? 2.4e-4f : 8.5e-4f) * original_convection;',
                    'const float base_rate = (is_global ? 2.4e-4f : 8.5e-4f) * original_convection * old_six_level_column_scale();')
text = text.replace('const float depth_factor = std::max(1.0f - 0.13f * float(interface_index), 0.45f);',
                    'const float depth_factor = std::max(1.0f - 0.52f * std::clamp(APPROX_HEIGHT_M[hi] / 12800.0f, 0.0f, 1.0f), 0.45f);')
text = text.replace('six-layer', '30-layer')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# OpenMP build
# -----------------------------------------------------------------------------
path = ROOT / 'native/weather/CMakeLists.txt'
text = path.read_text(encoding='utf-8')
old = 'target_link_libraries(asterra_weather PRIVATE godot-cpp)\n'
new = '''target_link_libraries(asterra_weather PRIVATE godot-cpp)

# The 1024x512x30 parent atmosphere is intentionally CPU-heavy. Keep AVX2 inside
# each worker and distribute independent rows/columns through OpenMP.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(asterra_weather PRIVATE OpenMP::OpenMP_CXX)
  target_compile_definitions(asterra_weather PRIVATE ASTERRA_WEATHER_OPENMP=1)
endif()
'''
text = replace_once(text, old, new, 'OpenMP CMake')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Runtime bridge
# -----------------------------------------------------------------------------
path = ROOT / 'scripts/weather/weather_system.gd'
text = path.read_text(encoding='utf-8')
text = text.replace('const GLOBAL_W := 256\nconst GLOBAL_H := 128', 'const GLOBAL_W := 1024\nconst GLOBAL_H := 512')
text = text.replace('const LAYERS := 6', 'const LAYERS := 30')
old_defaults = 'const DEFAULT_LAYER_WEIGHTS := [0.77, 0.90, 1.00, 1.07, 1.07, 1.19]'
new_defaults = 'const DEFAULT_LAYER_WEIGHTS := [' + ', '.join(f'{v:.6f}' for v in LAYER_WEIGHTS) + ']'
text = replace_once(text, old_defaults, new_defaults, 'GDScript layer weights')
text = text.replace('\t&"get_layer_count",\n', '\t&"get_layer_count",\n\t&"get_global_width",\n\t&"get_global_height",\n\t&"get_layer_height_m",\n', 1)
# Validate that a stale DLL cannot silently publish 256x128 arrays into 1024x512 textures.
needle = '''\t_native = instance
\tfor tuning_key: StringName in TUNING_KEYS:
'''
replacement = '''\tvar native_w := int(instance.call(&"get_global_width"))
\tvar native_h := int(instance.call(&"get_global_height"))
\tvar native_layers := int(instance.call(&"get_layer_count"))
\tif native_w != GLOBAL_W or native_h != GLOBAL_H or native_layers != LAYERS:
\t\tbackend_error = "WeatherNative grid mismatch: DLL reports %dx%dx%d, project expects %dx%dx%d; rebuild native/weather" % [native_w, native_h, native_layers, GLOBAL_W, GLOBAL_H, LAYERS]
\t\tpush_warning("WeatherSystem: %s" % backend_error)
\t\treturn
\t_native = instance
\tfor tuning_key: StringName in TUNING_KEYS:
'''
text = replace_once(text, needle, replacement, 'native shape validation')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Surface bridge: 1024x512 + worker-thread global geography build.
# -----------------------------------------------------------------------------
path = ROOT / 'scripts/weather/surface_energy_bridge.gd'
text = path.read_text(encoding='utf-8')
text = text.replace('const GLOBAL_W := 256\nconst GLOBAL_H := 128', 'const GLOBAL_W := 1024\nconst GLOBAL_H := 512')
text = text.replace('var _surface_fields_uploaded := false\n',
'''var _surface_fields_uploaded := false
var _global_building := false
var _global_task_id := -1
var _global_request := 0
''', 1)
start = text.index('func _upload_global_surface_fields() -> void:')
end = text.index('\n\nfunc _has_local_observer()', start)
new_global = '''func _upload_global_surface_fields() -> void:
\tif _native == null or not is_instance_valid(_native):
\t\t_bind_native()
\tif _native == null or not Planet.ready_state or Planet.grid == null or Planet.fields == null:
\t\treturn
\tif _global_building:
\t\treturn
\t_global_request += 1
\tvar request := _global_request
\t_global_building = true
\tvar task := func() -> void:
\t\tvar packed := _build_global_surface_fields()
\t\tcall_deferred("_finish_global_surface_fields", request, packed)
\t_global_task_id = WorkerThreadPool.add_task(task, true, "asterra_weather_surface_global")


static func _build_global_surface_fields() -> PackedFloat32Array:
\t# Four floats per 21.5 km global meteorology cell:
\t# elevation [m], free-water fraction, soil moisture, snow-free land albedo.
\tvar packed := PackedFloat32Array()
\tpacked.resize(GLOBAL_W * GLOBAL_H * 4)
\tfor y in GLOBAL_H:
\t\tvar lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GLOBAL_H)
\t\tvar sin_lat := sin(lat)
\t\tvar cos_lat := cos(lat)
\t\tfor x in GLOBAL_W:
\t\t\tvar lon := TAU * (float(x) + 0.5) / float(GLOBAL_W)
\t\t\tvar direction := Vector3(cos_lat * cos(lon), sin_lat, cos_lat * sin(lon))
\t\t\tvar offset := (x + y * GLOBAL_W) * 4
\t\t\tvar water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
\t\t\tvar moisture := clampf(Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
\t\t\tvar color := Planet.surface_color(direction)
\t\t\tvar luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
\t\t\tpacked[offset + 0] = lerpf(Planet.macro_height(direction), Planet.water_height(direction), water)
\t\t\tpacked[offset + 1] = water
\t\t\tpacked[offset + 2] = moisture
\t\t\tpacked[offset + 3] = clampf(luminance, 0.04, 0.65)
\treturn packed


func _finish_global_surface_fields(request: int, packed: PackedFloat32Array) -> void:
\tif _global_task_id >= 0:
\t\tWorkerThreadPool.wait_for_task_completion(_global_task_id)
\t\t_global_task_id = -1
\t_global_building = false
\tif request != _global_request or _native == null or not is_instance_valid(_native):
\t\treturn
\t_native.call(&"set_global_surface_fields", packed)
\t_surface_fields_uploaded = true
\t_local_uploaded_center = Vector3.ZERO
\t_local_requested_center = Vector3.ZERO
\t_local_request += 1
\t_push_solar_forcing()
\t_publish_surface_textures()
'''
text = text[:start] + new_global + text[end:]
# Invalidate any in-flight global build on a world replacement.
text = text.replace('\t_surface_fields_uploaded = false\n\t_local_uploaded_center',
                    '\t_surface_fields_uploaded = false\n\t_global_request += 1\n\t_local_uploaded_center', 1)
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Weather globe / wind sampler UI
# -----------------------------------------------------------------------------
path = ROOT / 'scripts/ui/weather_map.gd'
text = path.read_text(encoding='utf-8')
text = text.replace('const GLOBAL_W := 256\nconst GLOBAL_H := 128', 'const GLOBAL_W := 1024\nconst GLOBAL_H := 512')
# Replace six layer labels and fallback scaling with the actual 30-level coordinate.
name_start = text.index('const LAYER_NAMES := [')
name_end = text.index(']\nconst FALLBACK_LAYER_SPEED', name_start) + 1
layer_names = []
for i, h in enumerate(HEIGHTS):
    if h < 1000:
        label = f'L{i+1:02d}  ~{h / 1000.0:.2f} km'
    else:
        label = f'L{i+1:02d}  ~{h / 1000.0:.2f} km'
    layer_names.append(label)
new_names = 'const LAYER_NAMES := [\n' + ''.join(f'\t"{name}",\n' for name in layer_names) + ']'
text = text[:name_start] + new_names + text[name_end:]
fs_start = text.index('const FALLBACK_LAYER_SPEED := [')
fs_end = text.index(']\n', fs_start) + 1
new_fs = 'const FALLBACK_LAYER_SPEED := [' + ', '.join(f'{v:.3f}' for v in FALLBACK_SPEED) + ']'
text = text[:fs_start] + new_fs + text[fs_end:]
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Refinement scanner: same CPU cost as before, but samples the 21.5 km field.
# -----------------------------------------------------------------------------
path = ROOT / 'scripts/weather/weather_refinement_manager_v2.gd'
text = path.read_text(encoding='utf-8')
text = text.replace('The current L0 atmosphere is still coarse, so this controller scores precursor\n## environments rather than waiting for storm-scale magnitudes that an ~86 km\n## cell cannot produce.',
                    'L0 now runs at ~21.5 km with 30 vertical levels. This controller still scores\n## precursor environments; explicit storm dynamics belong to the child nests.')
text = text.replace('const CANDIDATE_TILE_W := 8\nconst CANDIDATE_TILE_H := 6',
                    'const CANDIDATE_TILE_W := 32\nconst CANDIDATE_TILE_H := 24\nconst SCAN_STRIDE := 4')
text = text.replace('for y: int in range(ty, y_end):\n\t\t\t\tfor x: int in range(tx, x_end):',
                    'for y: int in range(ty, y_end, SCAN_STRIDE):\n\t\t\t\tfor x: int in range(tx, x_end, SCAN_STRIDE):')
old_prev = '''\t_previous_pressure.resize(WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H)
\tfor c: int in range(WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H):
\t\t_previous_pressure[c] = weather[c * 4 + 3]
'''
new_prev = '''\t_previous_pressure.resize(WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H)
\tfor y: int in range(0, WeatherSystem.GLOBAL_H, SCAN_STRIDE):
\t\tfor x: int in range(0, WeatherSystem.GLOBAL_W, SCAN_STRIDE):
\t\t\tvar c := x + y * WeatherSystem.GLOBAL_W
\t\t\t_previous_pressure[c] = weather[c * 4 + 3]
'''
text = replace_once(text, old_prev, new_prev, 'refinement pressure history')
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Wind sampler parallel pack
# -----------------------------------------------------------------------------
path = ROOT / 'native/weather/src/weather_wind_sampler.cpp'
text = path.read_text(encoding='utf-8')
text = text.replace('\tfor (int c = 0; c < a.cells; ++c) {\n',
                    '\t#pragma omp parallel for schedule(static)\n\tfor (int c = 0; c < a.cells; ++c) {\n', 1)
path.write_text(text, encoding='utf-8')

# -----------------------------------------------------------------------------
# Severe probe dimensions/comments. Threshold retuning intentionally waits for
# observed behaviour on the final parent resolution.
# -----------------------------------------------------------------------------
path = ROOT / 'tests/weather_severe_probe.gd'
if path.exists():
    text = path.read_text(encoding='utf-8')
    text = text.replace('The 86 km global mesh', 'The ~21.5 km global mesh')
    text = text.replace('const GW := 256\nconst GH := 128', 'const GW := 1024\nconst GH := 512')
    path.write_text(text, encoding='utf-8')

print('Applied 1024x512x30 L0 atmosphere migration')
