# Testing

## Required gates

A release should pass:

1. CMake configure on Debian stable/trixie.
2. GCC release build.
3. Clang build.
4. CTest unit/integration suite.
5. ASan CTest run.
6. UBSan CTest run.
7. clang-format check.
8. clang-tidy or equivalent static analysis.
9. shell syntax checks for project scripts.
10. package creation smoke test.

## Hardware matrix

Where hardware is available, validate:

- Intel iGPU/i915
- Intel Xe/Arc
- NVIDIA desktop
- Intel + NVIDIA Optimus laptop with dGPU runtime suspended
- no-GPU VM
- laptop with battery
- desktop with no battery

GPU fixture parsers must be tested without requiring that hardware in CI.
