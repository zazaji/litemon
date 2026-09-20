# GPU compatibility

## NVIDIA

Required for detailed data: the installed NVIDIA driver must provide `nvidia-smi`.

Typical fields:

- utilization
- VRAM used/total
- temperature
- board power
- graphics clock

On hybrid laptops LiteMon first checks `/sys/bus/pci/devices/*/power/runtime_status`. If the NVIDIA device is suspended, it is shown as suspended and `nvidia-smi` is not invoked, preventing the monitor from waking the dGPU.

If an NVIDIA VGA/3D device exists in sysfs but `nvidia-smi` is missing (driver tool not installed) or a query fails, the device is still reported with unknown metrics instead of disappearing, so it stays visible and selectable in the GUI. `nvidia-smi` bus IDs are canonicalized to the sysfs BDF form, keeping one stable ID per GPU across suspend/wake cycles.

## Intel

LiteMon supports both integrated and discrete Intel GPUs through a fallback chain.

### DRM/sysfs

Always preferred when a metric is exposed directly because it has the lowest overhead.

### XPU-SMI

When installed, LiteMon selects the exact device by PCI BDF. This is the preferred path for modern Xe/Arc hardware when the tool supports the requested fields.

### intel_gpu_top

`intel_gpu_top -J` provides PMU engine utilization and additional frequency/power data on supported i915 systems. Kernel perf permissions can restrict access; failure is treated as a missing optional metric.

## Multi-GPU

Every GPU is stored with a stable vendor-prefixed device ID. Intel and NVIDIA devices are never merged into one time series.
