# Installation

Xylem is a header-centric C++17 library. It has zero external library dependencies, relying only on standard library features and the helper library `xic`.

## PlatformIO (ESP32 / ARM)
To use Xylem in a PlatformIO project, add the dependency to your `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    xipid/Xylem
build_flags =
    -std=c++17
```

## CMake (Linux / macOS / Windows)
If you are developing a C++ application with CMake, you can add Xylem by checking it out as a subdirectory:

```cmake
# Add xylem library (located in sibling directory or vendor folder)
add_subdirectory(path/to/xylem)

# Link against Xylem
target_link_libraries(your_target PRIVATE Xylem)
```

Ensure your C++ standard compiler is set to C++17 or higher:
```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```
