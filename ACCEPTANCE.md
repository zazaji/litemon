# LiteMon 2.0 acceptance record

Date: 2026-09-16

## Scope completed

- native Qt 6 single-machine monitor architecture retained
- background collector separated from on-demand GUI
- Intel + NVIDIA multi-GPU collection retained
- NVIDIA runtime-suspend protection retained
- persistent XDG settings added
- configurable sampling and retention added
- stable XDG data/config/cache paths added
- SQLite schema version metadata, integrity check and WAL checkpoint added
- diagnostics JSON and health-check CLI added
- CSV history export added
- GUI settings/diagnostics/about actions added
- AppStream/icon/desktop packaging completed
- CMake shared core, presets, sanitizer switches and CPack added
- CI, release workflow, repository governance and contributor docs added

## Validation executed in this environment

PASS:

- `tests/test_intel_json.py`
- `tests/test_sql.py`
- project metadata/XML validation
- shell syntax validation for every `scripts/*.sh`
- CMake parses project until dependency resolution

Environment limitation:

- Qt 6 development package is not installed in the execution environment, so native C++ linking/CTest binaries cannot be executed here. CMake stops at `find_package(Qt6 6.4 ...)` with the expected missing-package error.

## Target-machine release gate

On Debian 13 with `qt6-base-dev` and `libqt6sql6-sqlite` installed, run:

```bash
./scripts/build.sh release
./build/release/litemon-collector --health-check
./build/release/litemon-collector --snapshot
```

For Intel hardware install `intel-gpu-tools`; for NVIDIA use the driver-provided `nvidia-smi`.
