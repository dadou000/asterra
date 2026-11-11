# Shared semantic UI colors

- Existing owner: `OrbitEditorUi` owns the code-native vector icon renderer and
  shared toolbar/navigation presentation. Keep category-to-color policy there;
  panels map their domain elements to shared categories.
- Primary insertion points: `EditorUi.hpp` exposes `ElementCategory` and its
  palette API; `Toolbar.cpp` maps toolbar icons and explorer row icons;
  `NavigationTabs.cpp` maps workspace icons.
- Canonical state: colors are presentation-only constants. Selection, hover,
  disabled state, and domain status remain separate and are never inferred from
  category color.
- Preserve neutral panel surfaces and text, blue active-selection treatment,
  readable contrast, and non-color cues. Do not add per-panel palette copies.
- Palette: meshes `#6EA8FE`, procedurals `#B197FC`, lighting `#F6C85F`,
  simulation `#55C6B3`, VFX `#E879F9`, SFX `#F28B82`, shading `#8B9BFF`,
  celestial `#5CC8E8`, plugins `#A6B0C3`, planning `#79C98A`; neutral remains
  theme text gray.
- Native UI changes follow the central Studio generation refresh.
