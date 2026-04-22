# External Dependencies

Runtime dependencies are vendored via CMake `FetchContent` during configure.

Current stage-1 dependency policy:

- `SDL3` is fetched from its upstream repository.
- `Vulkan SDK` is expected to be installed locally on Windows.

