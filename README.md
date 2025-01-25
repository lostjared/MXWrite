# MXWrite - simple static library for writing RGBA video
# FFmpeg-Based Video Writer

This repository provides a C++ class (`Writer`) that uses the [FFmpeg](https://ffmpeg.org/) libraries to write raw RGBA frames to an MP4 (or TS) file in H.264 format. It supports both a straightforward, frame-by-frame workflow (`open()`, `write()`, and `close()`) and a timestamp-based workflow (`open_ts()`, `write_ts()`, and `close()`).  

## Table of Contents

1. [Overview](#overview)
2. [Dependencies](#dependencies)
3. [Building](#building)
4. [Usage](#usage)
   - [Basic Initialization (`open`/`write`)](#basic-initialization-openwrite)
   - [Timestamp-Based Writing (`open_ts`/`write_ts`)](#timestamp-based-writing-open_tswrite_ts)
5. [Key Implementation Details](#key-implementation-details)
6. [License](#license)

---

## Overview

`Writer` is a C++ class that simplifies encoding and writing video frames to a container file. You can:
- Open an output file (MP4 or TS container).
- Write raw RGBA data (passed in as a pointer) to the file, converting it to YUV420P under the hood.
- Optionally use hardware acceleration (CUDA) if available.
- Close and finalize the file properly.

It can handle both:
- **Frame-by-frame mode**: For applications where you generate or capture frames at a known rate.
- **Timestamp-based mode**: For applications that require precise PTS (presentation timestamp) control, typically when your source frames arrive at irregular intervals.

---

## Dependencies

### Required Libraries

1. **FFmpeg** – The code specifically uses the following components:
   - `libavcodec`
   - `libavformat`
   - `libavutil`
   - `libswscale`
2. **Threads** (C++11 and above) – Uses `<thread>` and `<mutex>` from the standard library.
3. A **C++14 (or higher)** compatible compiler.

To install FFmpeg development libraries on your platform:
- **Ubuntu / Debian**:
  ```bash
  sudo apt-get update
  sudo apt-get install ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
  ```
- **Windows**: 
  - Install via [vcpkg](https://github.com/microsoft/vcpkg), [MSYS2](https://www.msys2.org/), or download and build FFmpeg from source.
- **macOS**: 
  ```bash
  brew install ffmpeg
  ```

---

## Building

Below is one straightforward way to build the project with **CMake**.

1. **Create a `CMakeLists.txt`** in your project that includes `ffwrite.cpp` (or wherever this code lives), and links to the appropriate FFmpeg libraries. For example:

   ```cmake
   cmake_minimum_required(VERSION 3.10)
   project(FFmpegWriterDemo CXX)

   # Find FFmpeg libraries (example, might need adjusting to your environment)
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(FFMPEG REQUIRED 
       libavcodec 
       libavformat 
       libavutil 
       libswscale 
   )

   add_library(ffwriter STATIC
       ffwrite.cpp   # or whatever filename you have for the .cpp file
   )
   target_include_directories(ffwriter PUBLIC 
       ${FFMPEG_INCLUDE_DIRS}
   )
   target_link_libraries(ffwriter PUBLIC
       ${FFMPEG_LIBRARIES}
       pthread       # On Linux-like systems
   )

   # Optionally create an executable to test usage
   add_executable(writer_test main.cpp)
   target_link_libraries(writer_test PRIVATE ffwriter)
   ```

2. **Configure and build**:
   ```bash
   mkdir -p build
   cd build
   cmake ..
   cmake --build .
   ```
3. This will produce either a static library (`libffwriter.a`) or a shared library (`libffwriter.so`) depending on how you configure `add_library`. It will also produce the `writer_test` executable if you created a test target.

---

## Usage

You can include the `ffwrite.hpp` header in your own C++ files and link against the library. Below is a quick example:

```cpp
#include "ffwrite.hpp"
#include <vector>

int main() {
    Writer writer;
    // 1. Open an output MP4
    bool ok = writer.open("output.mp4", 1280, 720, 30.0f, 2500);  // width=1280, height=720, fps=30, bitrate=2500 kbps
    if (!ok) {
        return 1;
    }
    // 2. Prepare or capture frames in RGBA format.
    // For demonstration, we'll just create a dummy buffer of size width * height * 4.
    std::vector<uint8_t> dummyRGBA(1280 * 720 * 4, 255); // all white
    // 3. Write frames
    for (int i = 0; i < 60; ++i) { // e.g., 2 seconds of video at 30 FPS
        writer.write(dummyRGBA.data());
    }
    // 4. Close and finalize
    writer.close();
    return 0;
}
```
