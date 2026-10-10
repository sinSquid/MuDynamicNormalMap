# Local fixes

These changes build on upstream commit `2ae3f6e` (MuDynamicNormalMap 2.1.13).

## Runtime changes

- Preserve the actor ID before erasing an invalid actor from the active map.
- Protect the delayed callback queue and preserve the original N + 1 frame delay.
- Acquire the background detection gate before submission; release it when the
  task is destroyed, including exception paths.
- Check whether a condition is enabled before collecting geometry.
- Run TaskManager scene graph collection and material callbacks sequentially
  from the engine frame callback instead of dispatching them to TBB workers.
  Normal generation and compression remain asynchronous.
- Retain the source 3D root during generation. If it has been replaced before
  completion, discard the result and request a fresh update.
- Reject incomplete texture resources and recover a cached texture wrapper
  whose renderer texture is missing.

The material changes address identifiable concurrency and resource risks.
They have not been verified to eliminate the reported intermittent invisible
body parts or translucent/discolored skin in game.

## Building

Initialize the existing submodules, then use the `Release-NG` CMake preset.
The project requires Visual Studio 2022 C++ Build Tools, vcpkg, Python, and ISPC.
The current generated ISPC headers were produced with ISPC 1.31.0.

`build-local.cmd` is a convenience entry point for this workstation. Adjust its
Visual Studio, vcpkg and ISPC paths when using another machine. Toolchain
binaries, build output, inspection output and local deployment scripts are
excluded from this repository.

## Validation

The Release-NG build succeeded. The resulting DLL is x64 and has the same SKSE
exports and dependency set as the previous installed build. Existing compiler
warnings remain.

Standalone regression programs are in `tests/async_work_regression.cpp` and
`tests/cleanup_regression.cpp`. Compile them with a C++17 or newer compiler and
run with assertions enabled. They cover frame delay, reentrant enqueue,
concurrent producers, single-flight release and invalid actor cleanup; they do
not test Skyrim rendering.
