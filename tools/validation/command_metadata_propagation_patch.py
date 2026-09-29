from pathlib import Path


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label} anchor count {count}')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')


authoring = Path('engine/editor_model/src/AuthoringCommands.cpp')
replace_once(
    authoring,
    '''            commands::CommandParameter{\n                .name = "material",\n                .kind = commands::CommandValueKind::String,\n                .required = true\n            }\n''',
    '''            commands::CommandParameter{\n                .name = "material",\n                .kind = commands::CommandValueKind::String,\n                .required = true,\n                .displayName = "Material Asset",\n                .description =\n                    "Project material asset identifier assigned to the selected compatible object."\n            }\n''',
    'Assign Material parameter')

replace_once(
    authoring,
    '''            {\n                .name = "start_handle",\n                .kind = commands::CommandValueKind::Vector3,\n                .required = false\n            },\n            {\n                .name = "end_handle",\n                .kind = commands::CommandValueKind::Vector3,\n                .required = false\n            }\n''',
    '''            {\n                .name = "start_handle",\n                .kind = commands::CommandValueKind::Vector3,\n                .required = false,\n                .displayName = "Start Handle",\n                .description =\n                    "Cubic Bezier start control-handle offset in path-network space.",\n                .unit = "m"\n            },\n            {\n                .name = "end_handle",\n                .kind = commands::CommandValueKind::Vector3,\n                .required = false,\n                .displayName = "End Handle",\n                .description =\n                    "Cubic Bezier end control-handle offset in path-network space.",\n                .unit = "m"\n            }\n''',
    'Bezier parameters')

material = Path('engine/editor_model/src/MaterialAuthoringCommands.cpp')
replace_once(
    material,
    '''            {\n                .name = "decal",\n                .kind = commands::CommandValueKind::String,\n                .required = true\n            },\n            {\n                .name = "latitude",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            },\n            {\n                .name = "longitude",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            },\n            {\n                .name = "width",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            },\n            {\n                .name = "height",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            },\n            {\n                .name = "rotation",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            },\n            {\n                .name = "opacity",\n                .kind = commands::CommandValueKind::Float,\n                .required = true\n            }\n''',
    '''            {\n                .name = "decal",\n                .kind = commands::CommandValueKind::String,\n                .required = true,\n                .displayName = "Decal Asset",\n                .description =\n                    "Project decal asset identifier to attach to the selected celestial body."\n            },\n            {\n                .name = "latitude",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Latitude",\n                .description =\n                    "Surface latitude used to place the decal.",\n                .unit = "rad"\n            },\n            {\n                .name = "longitude",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Longitude",\n                .description =\n                    "Surface longitude used to place the decal.",\n                .unit = "rad"\n            },\n            {\n                .name = "width",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Width",\n                .description =\n                    "Physical decal width. The command requires a positive value.",\n                .unit = "m"\n            },\n            {\n                .name = "height",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Height",\n                .description =\n                    "Physical decal height. The command requires a positive value.",\n                .unit = "m"\n            },\n            {\n                .name = "rotation",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Rotation",\n                .description =\n                    "Rotation around the decal surface normal.",\n                .unit = "deg"\n            },\n            {\n                .name = "opacity",\n                .kind = commands::CommandValueKind::Float,\n                .required = true,\n                .displayName = "Opacity",\n                .description =\n                    "Decal opacity from fully transparent (0) to fully opaque (1).",\n                .minimum = 0.0,\n                .maximum = 1.0\n            }\n''',
    'Attach Decal parameters')

volume = Path('engine/editor_model/src/VolumeAuthoringCommands.cpp')
replace_once(
    volume,
    '''        .parameters = {\n            {.name="preset",.kind=commands::CommandValueKind::String,.required=false}\n        },\n''',
    '''        .parameters = {\n            {\n                .name = "preset",\n                .kind = commands::CommandValueKind::String,\n                .required = false,\n                .displayName = "Preset",\n                .description =\n                    "Production preset: Empty, Smoke, Fire, Fog, Dust, Snow, or Surface Flow.",\n                .defaultValue =\n                    commands::CommandValue{std::string{"Empty"}}\n            }\n        },\n''',
    'Create Volume preset')

replace_once(
    volume,
    '''        .parameters = {\n            {.name="kind",.kind=commands::CommandValueKind::String,.required=true}\n        },\n''',
    '''        .parameters = {\n            {\n                .name = "kind",\n                .kind = commands::CommandValueKind::String,\n                .required = true,\n                .displayName = "Source Type",\n                .description =\n                    "Source adapter: Brush, Texture / Mask, Terrain, Spline, Mesh / SDF, Collision Proxy, Particles, Object Motion, or World Motion."\n            }\n        },\n''',
    'Add Volume Source kind')

replace_once(
    volume,
    '''        .parameters = {\n            {.name="kind",.kind=commands::CommandValueKind::String,.required=true}\n        },\n''',
    '''        .parameters = {\n            {\n                .name = "kind",\n                .kind = commands::CommandValueKind::String,\n                .required = true,\n                .displayName = "Effector Type",\n                .description =\n                    "Effector type: Obstacle, Drag, Wind, Temperature, or Dissipation."\n            }\n        },\n''',
    'Add Volume Effector kind')

replace_once(
    volume,
    '''        .parameters = {\n            {.name="position",.kind=commands::CommandValueKind::Vector3,.required=true},\n            {.name="radius",.kind=commands::CommandValueKind::Float,.required=false},\n            {.name="strength",.kind=commands::CommandValueKind::Float,.required=false}\n        },\n''',
    '''        .parameters = {\n            {\n                .name = "position",\n                .kind = commands::CommandValueKind::Vector3,\n                .required = true,\n                .displayName = "World Position",\n                .description =\n                    "World-space center of the persistent terrain-paint source stroke.",\n                .unit = "m"\n            },\n            {\n                .name = "radius",\n                .kind = commands::CommandValueKind::Float,\n                .required = false,\n                .displayName = "Radius",\n                .description =\n                    "Terrain-paint source radius. Negative values are clamped to zero by the command.",\n                .unit = "m",\n                .defaultValue = commands::CommandValue{f64{2.0}},\n                .minimum = 0.0\n            },\n            {\n                .name = "strength",\n                .kind = commands::CommandValueKind::Float,\n                .required = false,\n                .displayName = "Strength",\n                .description =\n                    "Scalar source strength written into the authored terrain-paint stroke.",\n                .defaultValue = commands::CommandValue{f64{1.0}}\n            }\n        },\n''',
    'Paint Volume Terrain Source parameters')
