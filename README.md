## Dependency Management

This project uses vcpkg in manifest mode for dependency management. All dependencies are specified in the `vcpkg.json` file and will be automatically installed when building the project.

### Dependencies

The following dependencies are used:
- Boost (system, thread)
- spdlog
- fmt
- simdjson
- Eigen3
- uwebsockets
- OpenGL
- glfw3
- imgui (with glfw-binding and opengl3-binding)

### Building with vcpkg

The build scripts (`build.sh`, `build_linux.sh`, `build.ps1`) have been updated to use vcpkg in manifest mode. When you run these scripts, vcpkg will automatically install all dependencies specified in `vcpkg.json`.

**Note:** The `setup_dependencies.sh`, `setup_dependencies_linux.sh`, and `setup_dependencies.ps1` scripts are no longer needed when using vcpkg in manifest mode. They are kept for backward compatibility but are not used in the current build process.
