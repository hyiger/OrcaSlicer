# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Overview

OrcaSlicer (v2.4.0-dev) is an open-source FDM/SLA 3D slicer forked from Bambu Studio. C++17 codebase (~500k+ lines) using wxWidgets for GUI, CMake build system, cross-platform (Windows/macOS/Linux).

## Build Commands

### macOS
```bash
cmake --build build/arm64 --config RelWithDebInfo --target all --
```

### Linux
```bash
cmake --build build --config RelWithDebInfo --target all --
```

### Windows
```bash
cmake --build . --config %build_type% --target ALL_BUILD -- -m
```

### Build System Details
- CMake 3.13+ (max 3.31.x on Windows)
- Two-phase: dependencies built in `deps/build/`, then main app in `build/`
- Windows: Visual Studio generators; macOS: Xcode (Ninja with `-x`); Linux: Ninja
- Platform helper scripts: `build_release_macos.sh`, `build_linux.sh`, `build_release_vs2022.bat`
- Docker: `scripts/DockerBuild.sh` for reproducible Linux builds

## Testing

Catch2 framework. Tests in `tests/` organized by domain:

```bash
# All tests
cd build && ctest --output-on-failure

# Individual suites
ctest --test-dir ./tests/libslic3r    # 23 test files: geometry, formats, algorithms
ctest --test-dir ./tests/fff_print    # 12 test files: slicing, gcode, fill, support
ctest --test-dir ./tests/sla_print    # 4 test files: SLA raycast, support gen
ctest --test-dir ./tests/libnest2d    # 2D nesting
ctest --test-dir ./tests/slic3rutils  # Utility tests
```

Test data fixtures in `tests/data/`. `TEST_DATA_DIR` preprocessor macro provides path.

## Architecture

### Entry Point
- `src/OrcaSlicer.cpp` (438KB) - monolithic file containing `CLI` class and `main()`
- Supports both GUI mode (`GUI_Run()`) and CLI mode (headless slicing)
- CLI error codes defined at top (~40 error types)

### Core Library: `src/libslic3r/` (~444 files)

Platform-independent slicing engine. Key classes and their relationships:

```
Model (3D scene container)
  ├── ModelObject (each imported 3D object)
  │    ├── ModelVolume[] (mesh parts + modifiers)
  │    ├── ModelInstance[] (placement/transform instances)
  │    └── ModelConfigObject (per-object config overrides)
  └── ModelMaterial[] (shared materials)

Print (slicing orchestrator) ←→ Model
  ├── PrintObject[] (one per unique ModelObject)
  │    ├── Layer[] (sliced horizontal layers)
  │    │    └── LayerRegion[] (per-region data within layer)
  │    │         ├── slices → perimeters → fills
  │    │         └── thin_fills, fill_surfaces
  │    ├── SupportLayer[] (support structures)
  │    └── PrintInstance[] (placement data)
  ├── PrintRegion[] (groups of volumes sharing config)
  └── PrintConfig (global print settings)

GCode (output generator) ← consumes Layer data
  └── GCodeWriter (low-level G-code emission)
```

#### Subdirectories of `src/libslic3r/`:
| Directory | Files | Purpose |
|-----------|-------|---------|
| `GCode/` | 45 | G-code generation, cooling, pressure equalization, thumbnails, SeamPlacer, WipeTower |
| `Fill/` | 30 | Infill patterns: gyroid, honeycomb, lightning, grid, adaptive cubic, etc. |
| `SLA/` | 35 | Resin printing: pad, hollowing, support points, rasterization |
| `Format/` | 24 | File I/O: 3MF, BBS_3MF, AMF, STL, OBJ, STEP, SL1 |
| `Geometry/` | 19 | Voronoi, medial axis, convex hull, boolean ops |
| `Support/` | 15 | Tree supports + traditional support generation |
| `Arachne/` | 8 | Variable-width wall generation (skeletal trapezoidation) |
| `CSGMesh/` | 7 | Constructive solid geometry mesh operations |
| `Algorithm/` | 4 | Algorithmic utilities |
| `Execution/` | 3 | Threading execution policies |
| `Optimize/` | 3 | NLopt-based optimization |
| `Shape/` | 2 | Shape primitives |

#### Slicing Pipeline (sequential steps)

**Per-object steps** (PrintObjectStep enum):
1. `posSlice` - Mesh → layer cross-sections via TriangleMeshSlicer
2. `posPerimeters` - Wall generation (Arachne engine)
3. `posEstimateCurledExtrusions` - Curl detection for lift
4. `posPrepareInfill` - Surface type detection (top/bottom/internal), shell discovery
5. `posInfill` - Infill pattern generation
6. `posIroning` - Top surface smoothing
7. `posSupportMaterial` - Support structure generation
8. `posSimplifyPath` / `posSimplifyWall` / `posSimplifyInfill` - Path optimization

**Print-wide steps** (PrintStep enum):
1. `psWipeTower` / `psToolOrdering` - Multi-material sequencing
2. `psSkirtBrim` - First-layer adhesion features
3. `psSlicingFinished` - Completion marker
4. `psGCodeExport` - G-code output
5. `psConflictCheck` - Path conflict detection

### GUI: `src/slic3r/GUI/` (~395 files + 233 in subdirs)

wxWidgets-based application framework.

**Key components:**
- `MainFrame` - Main application window
- `Plater` - 3D model plate interface, central workspace
- `GLCanvas3D` / `3DScene` - OpenGL 3D rendering
- `Tab` - Settings tabs (Print, Filament, Printer)
- `ParamsPanel` - Parameter sidebar
- `PartPlate` - Multi-plate management
- `Monitor` - Printer monitoring
- `BackgroundSlicingProcess` - Async slicing engine
- `CalibrationPanel` - Printer calibration UI

**Subdirectories:**
| Directory | Files | Purpose |
|-----------|-------|---------|
| `Widgets/` | 79 | Custom UI controls (buttons, animations, AMS items) |
| `Gizmos/` | 53 | 3D manipulation tools (cut, paint, measure, brim ears) |
| `DeviceCore/` | 48 | Device communication (bed config, device control) |
| `Jobs/` | 37 | Async jobs (arrange, slice, export, send to printer) |
| `DeviceTab/` | 5 | Device UI (AMS humidity, firmware update) |
| `Printer/` | 5 | Printer integration (Bambu tunnel, streaming) |
| `dark_mode/` | 3 | Windows dark mode hooks |
| `LibVGCode/` | 2 | G-code visualization wrapper |

**Utilities:** `src/slic3r/Utils/` (101 files) - networking, HTTP, serial, update checking.

### G-code Visualization: `src/libvgcode/` (46 files)

Separate OpenGL library for real-time G-code toolpath rendering. Supports both OpenGL 3.2+ and GLES 2.0. Includes custom vertex/fragment shaders, layer management, color-coded extrusion roles.

## Configuration System

### PrintConfig (`src/libslic3r/PrintConfig.cpp` - 10,859 lines)

Settings defined via `this->add("key", type)` pattern:
```cpp
def = this->add("layer_height", coFloat);
def->label = L("Layer height");
def->tooltip = L("...");
def->sidetext = L("mm");
def->min = 0;
def->set_default_value(new ConfigOptionFloat(0.2));
```

Config types: `ConfigOptionFloat`, `ConfigOptionInt`, `ConfigOptionBool`, `ConfigOptionEnum<T>`, `ConfigOptionFloatOrPercent`, `ConfigOptionStrings`, etc.

Enum maps use `CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME)` macro.

### Config Class Hierarchy (`PrintConfig.hpp`)
```
StaticPrintConfig (compile-time base)
├── PrintObjectConfig
├── PrintRegionConfig
├── PrintConfig
│   └── GCodeConfig
└── FullPrintConfig (virtual, combines all)

DynamicPrintConfig (runtime config, extends DynamicConfig)
```

Key enums: `GCodeFlavor` (13 firmware types), `InfillPattern` (28 patterns), `SupportMaterialStyle` (7 types), `SeamPosition` (5 types), `WallSequence`, `IroningType`, 40+ more.

### Preset/Profile System

Profiles stored as JSON in `resources/profiles/` (~10,000 files across 64 manufacturers).

Three profile types:
- **machine** (`type: "machine_model"`) - printer definitions with bed model, nozzle sizes
- **filament** (`type: "filament"`) - material settings with temp ranges, flow ratios
- **process** (`type: "process"`) - print quality settings (layer height, speed, infill)

Profiles support inheritance via `"inherits": "parent_profile_name"`.

Core classes: `Preset` / `PresetBundle` in `src/libslic3r/Preset.hpp`. GUI integration via `PresetComboBoxes`, `SavePresetDialog`, `CreatePresetsDialog`.

Profile validation: `scripts/orca_extra_profile_check.py`

## Key Dependencies

| Library | Purpose |
|---------|---------|
| wxWidgets | Cross-platform GUI framework |
| Boost | Filesystem, threading, string algorithms |
| TBB | Intel Threading Building Blocks for parallelism |
| Eigen | Linear algebra and matrix operations |
| Clipper2 | 2D polygon clipping and offsetting |
| CGAL | Computational geometry (selective use, separate lib target) |
| OpenVDB | Volumetric data structures |
| libigl | Mesh processing |
| OpenGL | 3D rendering |
| libcurl / OpenSSL | Networking |
| NLopt | Nonlinear optimization |
| cereal | Serialization |

## Code Style

- C++17 with selective C++20 features
- `.clang-format` enforces: 4-space indent, 140-column limit
- `CamelCase` classes, `snake_case` functions/variables, `SCREAMING_CASE` constants
- `#pragma once` for header guards
- Smart pointers and RAII for memory management
- TBB for parallelization; be mindful of shared state
- Localization: `L()` marks strings for translation, `_()` translates at runtime

## Common Change Patterns

### Adding a New Print Setting
1. Define in `src/libslic3r/PrintConfig.cpp` with bounds, default, tooltip
2. Add member to appropriate config class in `PrintConfig.hpp`
3. Add UI controls in `src/slic3r/GUI/Tab*.cpp`
4. Update serialization if needed
5. Test with multiple printer profiles

### Modifying Slicing Algorithms
1. Core algorithms in `src/libslic3r/` subdirectories (Fill/, Support/, GCode/, Arachne/)
2. Profile performance-critical changes
3. Consider TBB threading implications
4. Validate against existing profiles
5. Add tests in `tests/libslic3r/` or `tests/fff_print/`

### Adding a Printer Profile
1. Create JSON in `resources/profiles/[manufacturer]/`
2. Add start/end G-code templates
3. Define bed model, nozzle sizes, material compatibility
4. Follow existing profile structure and `"inherits"` patterns
5. Validate with `scripts/orca_extra_profile_check.py`

### GUI Changes
1. GUI code in `src/slic3r/GUI/`
2. Custom widgets in `Widgets/` subdirectory
3. 3D gizmos in `Gizmos/` subdirectory
4. Support light/dark themes, DPI scaling, cross-platform
5. Use existing wxWidgets patterns from the codebase

## CI/CD

15 GitHub Actions workflows in `.github/workflows/`:
- **build_all.yml** - Main trigger (push to main/release, PRs, daily schedule)
- **build_orca.yml** - Actual compilation (Ubuntu/Windows/macOS, multi-arch)
- **build_deps.yml** - Dependency building
- **check_profiles.yml** - Profile JSON validation
- **check_locale.yml** - Localization verification
- **shellcheck.yml** - Shell script linting

## Codebase Navigation Tips

- Entry point: `src/OrcaSlicer.cpp` → `CLI::run()` → `GUI_Run()` or headless slicing
- Slicing pipeline: `src/libslic3r/Print.cpp` → `PrintObject::slice/make_perimeters/infill`
- All settings: `src/libslic3r/PrintConfig.cpp` (search by setting key name)
- GUI tabs: `src/slic3r/GUI/Tab.cpp` (Print/Filament/Printer settings UI)
- G-code generation: `src/libslic3r/GCode.cpp` + `src/libslic3r/GCode/` directory
- File formats: `src/libslic3r/Format/` (3MF.cpp, STL.cpp, STEP.cpp, etc.)
- Infill patterns: `src/libslic3r/Fill/` (one file per pattern)
- Support generation: `src/libslic3r/Support/` + `TreeSupport.cpp` in parent dir
