# MXWrite

MXWrite is a small C++20 video-writing library built directly on FFmpeg. It is
the recording backend used by ACMX2 and ACMXVK. Applications submit RGBA video
frames while MXWrite performs color conversion, encoding, packet writing, and
container finalization on a background thread.

MXWrite supports software and hardware encoders, sequential or explicit frame
timestamps, queue backpressure, CUDA-resident input, 10-bit HDR output, encoder
introspection, and audio remuxing. The available codecs ultimately depend on
the FFmpeg build installed on the system.

## Features

- Tightly packed RGBA8 host-frame input
- Sequential constant-frame-rate timestamps or explicit presentation timestamps
- Asynchronous encoding with bounded queueing
- Optional no-drop backpressure for offline rendering
- CRF/CQ quality control or target bitrate control
- Automatic H.264/HEVC NVENC selection with software fallback
- Exact selection of any compatible FFmpeg video encoder
- Direct CUDA RGBA ingestion when CUDA/NVENC support is available
- HEVC Main10 HDR output with BT.2020 PQ or HLG signaling
- Preservation of mastering-display and content-light metadata
- Runtime encoder and encoder-option enumeration
- Encoded duration, frame-count, byte-count, and hardware-status queries
- Audio-stream remuxing from an existing media file
- Optional nanobind Python module

## Requirements

- A C++20 compiler
- CMake 3.10 or newer
- FFmpeg development packages providing:
  - `libavcodec` 58 or newer
  - `libavformat` 58 or newer
  - `libavutil` 56 or newer
  - `libswscale` 5 or newer
- POSIX threads or the platform's equivalent C++ thread implementation
- Optional: CUDA Toolkit for CUDA device-frame ingestion
- Optional: Python 3.8 or newer and nanobind for the Python module

Typical packages are:

```bash
# Debian or Ubuntu
sudo apt install cmake pkg-config libavcodec-dev libavformat-dev \
    libavutil-dev libswscale-dev

# Arch Linux
sudo pacman -S cmake pkgconf ffmpeg

# macOS
brew install cmake pkgconf ffmpeg
```

On Windows, MXWrite can be built in an MSYS2 UCRT64 or MinGW64 environment
with the corresponding FFmpeg and CMake packages installed.

## Building and installing

From the root of the `acidcam-gpu` repository:

```bash
cmake -S MXWrite -B build/mxwrite -DCMAKE_BUILD_TYPE=Release
cmake --build build/mxwrite --parallel
cmake --install build/mxwrite
```

MXWrite builds as a static library by default. To build a shared library
instead, configure with `-DSHARED=ON`:

```bash
cmake -S MXWrite -B build/mxwrite-shared \
    -DCMAKE_BUILD_TYPE=Release -DSHARED=ON
cmake --build build/mxwrite-shared --parallel
cmake --install build/mxwrite-shared
```

Use `-DSHARED=OFF`, or omit the option, for the default static library. Shared
builds install the `.so` or `.dylib` into the library directory. On Windows,
the DLL is installed into the binary directory and its import library into the
library directory.

### Python module

The nanobind extension is disabled by default. Enable it with
`-DPYTHON_MODULE=ON`:

```bash
python3 -m pip install nanobind
cmake -S MXWrite -B build/mxwrite-python \
    -DCMAKE_BUILD_TYPE=Release \
    -DPYTHON_MODULE=ON
cmake --build build/mxwrite-python --parallel
cmake --install build/mxwrite-python
```

This builds the `mxwrite_ext` module and links it with MXWrite. CMake first
looks for an installed nanobind package and then asks the selected Python
interpreter for nanobind's CMake directory. By default, installation places
the extension in
`lib/python<major>.<minor>/site-packages` beneath `CMAKE_INSTALL_PREFIX`.
Override that location with `-DMXWRITE_PYTHON_INSTALL_DIR=<directory>`.
The default static MXWrite build is recommended for a self-contained Python
extension. `-DPYTHON_MODULE=OFF`, or omitting the option, leaves Python and
nanobind out of configuration entirely. When combined with `-DSHARED=ON`, the
installed extension uses a relative loader path to the MXWrite shared library
on Linux and macOS; Windows installations also place the MXWrite DLL beside
the extension.

The Python example generates an animated RGBA test pattern, queries the
available FFmpeg encoders and their options, writes a video, and validates the
reported frame count, duration, and output size. NumPy is required to supply
frames to the module:

```bash
python3 -m pip install numpy
PYTHONPATH=build/mxwrite-python \
    python3 MXWrite/examples/python_example.py \
    --output /tmp/mxwrite-python-example.mp4
```

Pass `--explicit-pts` to exercise `open_ts()` and `write_at_pts()`. Use
`--help` to see the resolution, frame-rate, encoder, preset, and CRF options.

MXWrite can also be built and installed directly through pip from the
repository checkout. The FFmpeg development packages listed above must already
be installed because pip does not provide those native system libraries:

```bash
python3 -m pip install ./MXWrite
python3 -c "import mxwrite_ext; print(len(mxwrite_ext.available_video_encoders()))"
```

The pip build uses scikit-build-core to invoke CMake, enables the nanobind
module automatically, and installs NumPy as its runtime dependency.

CMake automatically enables `MXWRITE_HAS_CUDA_COPY` when it finds the CUDA
Toolkit. This definition changes the layout of `Writer`, so every translation
unit using `mxwrite.hpp` must receive the same definition as the library. The
exported `MXWrite::mxwrite` CMake target propagates it automatically. Consumers
of a CUDA-enabled installation must also make `CUDA::cudart` available, normally
with `find_package(CUDAToolkit REQUIRED)`.

To use the installed CMake package:

```cmake
find_package(MXWrite CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE MXWrite::mxwrite)
```

The repository's top-level ACMX Pcons build compiles the same MXWrite source
directly and propagates the matching CUDA definition when CUDA is enabled.

## Basic use

```cpp
#include <cstdint>
#include <mxwrite.hpp>
#include <vector>

int main() {
    constexpr int WIDTH = 1280;
    constexpr int HEIGHT = 720;

    EncodeOptions options;
    options.codec = "libx264";
    options.preset = "medium";
    options.crf = 18;
    options.block_when_full = true;

    Writer writer;
    if (!writer.open("output.mp4", WIDTH, HEIGHT, 30.0F, options)) {
        return 1;
    }

    std::vector<std::uint8_t> rgba(WIDTH * HEIGHT * 4, 255);
    for (int frame = 0; frame < 300; ++frame) {
        // Fill rgba with the next tightly packed R, G, B, A frame.
        writer.write(rgba.data());
    }

    writer.close();
    return 0;
}
```

Calling `close()` drains queued frames, flushes the encoder, writes the
container trailer, and releases FFmpeg resources. The destructor closes an
open writer automatically, but explicit `close()` makes output lifetime clear.

## Input and timing

Host input passed to `write()` or `write_at_pts()` must be tightly packed RGBA8
with exactly four bytes per pixel and dimensions matching `open()`. MXWrite
copies the data before the call returns, so the caller may reuse its buffer.

There are two timestamp styles:

| Method | Behavior |
| --- | --- |
| `write(frame)` | Assigns sequential PTS values `0, 1, 2, ...` at the configured frame rate. |
| `write_at_pts(frame, pts)` | Uses an explicit PTS measured in output-frame ticks. Values must move forward; stale or duplicate slots are discarded. Gaps are preserved. |
| `write_hdr_rgba16_at_pts(frame, pts)` | Explicit-PTS equivalent for encoded HDR RGBA16 input. |
| `write_cuda_rgba_at_pts(frame, stride, pts)` | Explicit-PTS equivalent for CUDA input. |

`open_ts()` remains as a compatibility entry point. It enables the low-delay
threading path, but `write_ts()` currently aliases `write()` and therefore uses
sequential timestamps. Applications that already have source timestamps should
use `write_at_pts()` directly.

Common fractional rates are represented exactly, including 23.976, 29.97,
47.952, 59.94, and 119.88 FPS. Other rates are converted to a reduced rational
with milliframe precision.

## Encoder configuration

`EncodeOptions` provides the primary controls:

| Field | Default | Purpose |
| --- | --- | --- |
| `codec` | `auto` | Encoder policy or exact FFmpeg encoder name. |
| `preset` | `medium` | Speed/quality preset. x264-style names are mapped to NVENC `p1` through `p7`. |
| `tune` | empty | Software or hardware tuning mode. |
| `crf` | `18` | CRF for software encoders or CQ for NVENC, clamped to 0–51. |
| `bit_rate` | `0` | Target bits per second. A positive value selects bitrate-based operation instead of built-in CRF/CQ. |
| `ffmpeg_options` | empty | Additional FFmpeg-style encoder and muxer options. |
| `realtime` | `false` | Enables low-latency encoder settings. |
| `block_when_full` | `false` | Blocks the producer rather than dropping frames when encoding falls behind. |

For example, 10 Mbit/s target bitrate encoding is:

```cpp
EncodeOptions options;
options.codec = "libx264";
options.bit_rate = 10'000'000;
```

This is equivalent in intent to FFmpeg's `-b:v 10M`. Leave `bit_rate` at zero
to use `crf` or NVENC `cq` instead.

### Codec selection

- `auto` prefers NVENC and falls back to software. It selects H.264 through
  3840x2160 and HEVC for dimensions larger than 3840 pixels wide or 2160 pixels
  high.
- `nvenc` uses the same resolution policy and reports a software fallback if
  the selected NVENC encoder is unavailable or cannot initialize CUDA.
- `software`, `cpu`, or `x264` forces the resolution-selected software codec.
- `h264`, `libx264`, `hevc`, `h265`, and `libx265` select their corresponding
  software paths explicitly.
- An exact registered encoder name selects that FFmpeg encoder without
  collapsing it to H.264. Examples can include `libsvtav1`, `libvpx-vp9`,
  `prores_ks`, `ffv1`, `h264_qsv`, `hevc_vaapi`, or `h264_videotoolbox`.

Registration does not guarantee that a hardware encoder can run. Its driver,
device, FFmpeg hardware support, and accepted pixel formats must also be
available. Exact hardware-frame-only selections fail with a diagnostic if
their device context cannot be created.

### Extra FFmpeg options

`ffmpeg_options` accepts familiar option spelling and supports quoted values:

```cpp
EncodeOptions options;
options.codec = "hevc_nvenc";
options.ffmpeg_options =
    "-preset p6 -tune lossless -profile:v rext -pix_fmt yuv444p";
```

Options in this string override matching built-in choices. MXWrite uses
libavcodec and libavformat directly; the string must contain only video encoder
or muxer options, not an FFmpeg command, input filename, or output filename.
Unsupported options cause `open()` to fail with a diagnostic.

The legacy overload below is retained for existing applications:

```cpp
writer.open("output.mp4", width, height, fps, "24");
```

It parses the string as CRF and preserves the original ultrafast,
zero-latency, realtime behavior. New code should prefer `EncodeOptions`.

## Queue behavior and offline rendering

Encoding runs on a background `std::jthread`. In normal mode the pending queue
holds up to 120 frames. If the producer outruns the encoder after the queue is
full, new frames are dropped and MXWrite periodically reports the count.

For file conversion, headless rendering, or any workflow where every submitted
frame must be encoded, enable backpressure before opening or at runtime:

```cpp
EncodeOptions options;
options.block_when_full = true;

// Or after open():
writer.set_block_when_full(true);
```

Backpressure keeps one frame pending and blocks the producer until the encoder
has room. This prevents frame loss and naturally paces offline processing to
the encoder's throughput.

## CUDA input

When MXWrite is compiled with CUDA support and a compatible CUDA hardware
encoder is active, a CUDA RGBA8 image can be submitted without downloading it
to host memory:

```cpp
if (!writer.write_cuda_rgba(device_rgba, device_stride, false)) {
    // Direct CUDA ingestion is unavailable; use a host-frame fallback.
}
```

The source must be an RGBA8 CUDA device allocation. `src_stride` is its row
pitch in bytes, and `bottom_up` requests a vertical flip during the device copy.
The method returns `false` when the writer is closed or direct CUDA ingestion
is unavailable. Explicit timestamps are supported by
`write_cuda_rgba_at_pts()`.

## HDR output

Set `options.hdr.enabled` to select the dedicated HDR path. It currently uses
software `libx265`, HEVC Main10, and `yuv420p10le`. The output is tagged with
BT.2020 primaries, BT.2020 non-constant-luminance matrix coefficients, and PQ
by default. Setting `color_trc` to FFmpeg's HLG value produces HLG signaling.

```cpp
EncodeOptions options;
options.hdr.enabled = true;
options.hdr.color_primaries = AVCOL_PRI_BT2020;
options.hdr.color_trc = AVCOL_TRC_SMPTE2084;
options.hdr.color_space = AVCOL_SPC_BT2020_NCL;
options.hdr.color_range = AVCOL_RANGE_MPEG;

Writer writer;
if (!writer.open("output-hdr.mkv", width, height, fps, options)) {
    return 1;
}

writer.write_hdr_rgba16(encoded_bt2020_rgba16);
writer.close();
```

`write_hdr_rgba16()` expects tightly packed, little-endian, unsigned-normalized
RGBA16 whose RGB channels are already encoded as BT.2020 PQ or HLG. MXWrite
converts that signal to limited-range YUV420P10 without applying another
transfer function. RGBA8 submitted through `write()` is supported as an SDR
compatibility input and is placed at the 100-nit reference level in a PQ
signal.

Raw FFmpeg mastering-display and content-light side-data payloads can be placed
in `options.hdr.mastering_display` and `options.hdr.content_light`. MXWrite
copies them to the output stream when possible.

## Encoder discovery

Applications can build encoder controls from the linked FFmpeg installation:

```cpp
#include <iostream>

for (const EncoderInfo &encoder : available_video_encoders()) {
    std::cout << encoder.name << " | " << encoder.codec_name << " | "
              << encoder.pixel_formats << '\n';
}

for (const EncoderOptionInfo &option : video_encoder_options("libx265")) {
    std::cout << '-' << option.name << ": " << option.help << '\n';
}
```

`EncoderInfo` reports the exact encoder name, codec family, human-readable
name, supported pixel formats, hardware classification, and experimental
status. `EncoderOptionInfo` exposes FFmpeg option types, defaults, ranges,
named choices, and help text.

## Progress information

While a writer is open, applications may query:

- `is_open()` for writer state
- `is_hardware_encode()` for active encoder classification
- `get_frame_count()` for the accepted output timeline length
- `get_bytes_written()` for the muxer's current logical byte position
- `get_duration()` for encoded duration in seconds
- `get_block_when_full()` for the current queue policy

For explicit PTS writes, `get_frame_count()` is the highest accepted PTS plus
one, so it includes intentional timeline gaps rather than only counting calls.

## Copying audio into an encoded video

`transfer_audio(source, destination)` remuxes the first audio stream from a
source media file into an already encoded destination video. Video packets and
audio packets are copied without re-encoding, audio is clipped to the video
duration, and the destination is replaced through a temporary file.

```cpp
writer.close();
transfer_audio("source-with-audio.mp4", "rendered-video.mp4");
```

The helper supports common containers including MP4, MKV, MOV, AVI, MPEG-TS,
MPEG, FLV, 3GP, WMV, ASF, and VOB. The source must contain an audio stream and
the destination must contain a video stream. Calls are serialized internally.

## Container and pixel-format notes

The output container is selected from the filename extension by libavformat.
The selected encoder must be valid for that container. For software encoding,
MXWrite chooses a compatible system-memory pixel format and converts from RGBA
with libswscale. Additional `-pix_fmt` requests are honored only when the
encoder and the implemented upload path support them.

## Example

`opencv_example/` contains a small camera program demonstrating the legacy
`open()` and `open_ts()` entry points. Build it after installing MXWrite and
OpenCV:

```bash
cmake -S MXWrite/opencv_example -B build/mxwrite-example
cmake --build build/mxwrite-example --parallel
./build/mxwrite-example/opencv_ex 0 0
```

The first argument is the camera index. The second is `0` for normal mode or
`1` for the low-delay compatibility mode.

## License

MXWrite is part of the Acid Cam project and is distributed under the repository's
License.
