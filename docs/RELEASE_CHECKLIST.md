# Release checklist

- [ ] version updated in CMake and changelog
- [ ] GCC Debian 13 build passes
- [ ] Clang build passes
- [ ] CTest passes
- [ ] ASan passes
- [ ] UBSan passes
- [ ] fresh-install smoke test passes
- [ ] upgrade preserves the existing database
- [ ] database `--health-check` passes
- [ ] Intel i915 fixture/runtime check performed where hardware is available
- [ ] Intel Xe/Arc runtime check performed where hardware is available
- [ ] NVIDIA active runtime check performed where hardware is available
- [ ] NVIDIA suspended Optimus check confirms no forced wakeup
- [ ] laptop battery and no-battery desktop behavior checked
- [ ] `.deb` and `.tar.gz` artifacts install/launch correctly
- [ ] AppStream metadata validates
- [ ] release notes include user-visible changes and known limitations
