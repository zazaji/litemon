# Changelog

## Unreleased

### Added
- per-core CPU monitoring: collector samples each core, storage keeps a `cpu_cores_raw`/`cpu_cores_5m` domain (schema v4, migrated automatically), and the CPU page shows one chart per core plus the aggregate chart
- main-window status bar shows the database path and total size (MB, 1 decimal)
- daily VACUUM reclaims space after retention pruning

### Changed
- storage schema v3: two tables per domain — fine `*_raw` detail (default last 7 calendar days, minimum 6) plus compressed `*_5m` 5-minute-average archive (default 365 days); legacy 1-minute/10-minute tables are folded into the archive on migration
- metric columns are scaled INTEGERs (fixed-point) instead of REALs, roughly 3–4x smaller at 0.1%/0.1 °C-grade precision; NULL still means unknown
- retention settings simplified to detail days + archive days (old config keys are still honored as fallback)

## 2.0.0 - 2026-09-16

### Added
- shared `litemon_core` library
- stable XDG paths
- persistent settings and configurable sampling/retention
- database schema metadata and integrity check
- collector diagnostics and health-check CLI
- CSV history export and diagnostics export in GUI
- AppStream metadata and scalable application icon
- CMake presets, sanitizer options, static-analysis configuration and packaging metadata
- contributor/security/testing/architecture documentation

### Changed
- retention is no longer hard-coded in database maintenance
- collector uses configured GPU policy and maintenance cadence
- project version raised to 2.0.0

### Fixed
- prevented GUI/collector database path divergence caused by inconsistent Qt application metadata
- kept NVIDIA runtime-suspend behavior to avoid unnecessary Optimus wakeups
