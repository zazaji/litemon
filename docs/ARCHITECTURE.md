# Architecture

## Goals

LiteMon optimizes for one Linux workstation or laptop, not fleet observability. The architecture therefore minimizes background processes, dependencies, wakeups and write amplification.

## Components

### `litemon_core`

A static core library shared by GUI, collector and tests. It contains collectors, GPU parsing, path/config handling, SQLite persistence and diagnostics. This avoids the 1.x duplication where both executables compiled overlapping implementation files independently.

### `litemon-collector`

A Qt Core + Qt SQL process started by a systemd user service. It samples ordinary metrics frequently and expensive GPU metrics less frequently. It owns writes to the database.

### `litemon`

A Qt Widgets reader. It does not need to stay resident for history collection. Closing it releases GUI memory while the collector continues.

## Storage

SQLite runs in WAL mode with `synchronous=NORMAL`, busy timeout and bounded journal size. Two tables per domain balance detail against disk usage:

- `*_raw` — fine samples for the last `detailRetentionDays` calendar days (default 7, minimum 6)
- `*_5m` — compressed 5-minute averages for the long-term archive (default 365 days)

Per-core CPU utilization is its own domain (`cpu_cores_raw` / `cpu_cores_5m`, one row per core per sample) because the core count varies per machine; the CPU page shows one chart per core plus the aggregate chart. The schema is at version 4 (v4 added the per-core domain; v3 introduced the raw+5m layout with scaled integers).

Maintenance folds every completed 5-minute bucket into the archive before pruning, so the day rollover first compresses week-old detail rows and then clears them — nothing is deleted before it is compressed. A daily `VACUUM` gives the freed space back to the filesystem.

Metrics use scaled INTEGER columns (fixed-point) instead of REALs: SQLite has no float16 type and a REAL always costs 8 bytes, while small integers pack into 1–3 bytes — roughly 3–4x smaller with precision matched to display needs (NULL still means unknown):

| column group | scale | resolution |
|---|---|---|
| cpu / per-core / battery percent | x10 | 0.1% |
| temperature | x10 | 0.1 °C |
| load1 | x100 | 0.01 |
| memory / swap (MiB) | x1 | 1 MiB |
| network / disk rates (MiB/s) | x1000 | 0.001 MiB/s |
| disk usage (GiB) | x100 | 0.01 GiB |
| power (W) | x100 | 0.01 W |
| GPU frequency (MHz) | x1 | 1 MHz |

The schema includes an explicit `metadata.schema_version` entry so future migrations are deterministic.

## GPU strategy

GPU collection is deliberately vendor-aware because Linux exposes no single stable cross-vendor API.

NVIDIA:

1. Inspect PCI runtime power state.
2. If every NVIDIA display-class device is suspended, report suspended state without calling `nvidia-smi`.
3. Otherwise perform one bounded `nvidia-smi --query-gpu` call.

Intel:

1. Enumerate DRM cards and verify PCI vendor `0x8086`.
2. Read cheap sysfs counters first.
3. Prefer `xpu-smi` for supported Xe/Arc devices using PCI BDF selection.
4. Fall back to `intel_gpu_top -J` for i915 PMU metrics.
5. Never invent dedicated VRAM for integrated GPUs using shared memory.

## Failure model

Missing optional tools are normal, not fatal. Collector command execution is timeout-bounded. Invalid numeric values become NaN internally and SQL NULL on disk. UI formatting renders unavailable metrics as `—`.

## Data ownership

LiteMon writes only to its XDG-scoped directories and never modifies kernel tunables, GPU power controls, battery thresholds or driver configuration.
