#!/usr/bin/env python3

import argparse
import math
from pathlib import Path

try:
    import numpy as np
except ImportError as error:
    raise SystemExit("NumPy is required: python3 -m pip install numpy") from error

try:
    import mxwrite_ext
except ImportError as error:
    raise SystemExit(
        "Could not import mxwrite_ext. Build with -DPYTHON_MODULE=ON and set "
        "PYTHONPATH to the directory containing the built module."
    ) from error


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Generate an animated test video with the MXWrite Python module."
    )
    parser.add_argument("--output", type=Path, default=Path("mxwrite-python-example.mp4"))
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=180)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--codec", default="libx264")
    parser.add_argument("--preset", default="veryfast")
    parser.add_argument("--crf", type=int, default=20)
    parser.add_argument(
        "--explicit-pts",
        action="store_true",
        help="Exercise open_ts() and write_at_pts() instead of sequential writes.",
    )
    return parser.parse_args()


def validate_arguments(args):
    if args.width <= 0 or args.height <= 0:
        raise SystemExit("Width and height must be positive.")
    if args.fps <= 0.0:
        raise SystemExit("FPS must be positive.")
    if args.frames <= 0:
        raise SystemExit("Frame count must be positive.")
    if not 0 <= args.crf <= 51:
        raise SystemExit("CRF must be between 0 and 51.")


def make_frame(x_coordinates, y_coordinates, frame_index, frame_count):
    phase = 2.0 * math.pi * frame_index / frame_count
    x_wave = np.sin(x_coordinates * 2.0 * math.pi + phase)
    y_wave = np.cos(y_coordinates * 2.0 * math.pi - phase * 1.7)
    diagonal = np.sin(
        (x_coordinates + y_coordinates) * 3.0 * math.pi + phase * 2.3
    )

    frame = np.empty(
        (y_coordinates.shape[0], x_coordinates.shape[1], 4), dtype=np.uint8
    )
    frame[:, :, 0] = np.clip((x_wave + 1.0) * 127.5, 0, 255).astype(np.uint8)
    frame[:, :, 1] = np.clip((y_wave + 1.0) * 127.5, 0, 255).astype(np.uint8)
    frame[:, :, 2] = np.clip((diagonal + 1.0) * 127.5, 0, 255).astype(
        np.uint8
    )
    frame[:, :, 3] = 255
    return frame


def main():
    args = parse_arguments()
    validate_arguments(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    encoders = mxwrite_ext.available_video_encoders()
    matching_encoders = [encoder for encoder in encoders if encoder.name == args.codec]
    print(f"MXWrite reported {len(encoders)} video encoder(s).")
    if matching_encoders:
        encoder = matching_encoders[0]
        mode = "hardware" if encoder.hardware else "software"
        print(f"Using {encoder.name} ({encoder.long_name}, {mode}).")
        options = mxwrite_ext.video_encoder_options(encoder.name)
        print(f"The encoder exposes {len(options)} configurable option(s).")
    elif args.codec not in {
        "auto",
        "software",
        "cpu",
        "x264",
        "h264",
        "hevc",
        "h265",
        "nvenc",
    }:
        names = ", ".join(encoder.name for encoder in encoders[:12])
        raise SystemExit(
            f"Requested encoder '{args.codec}' is unavailable. "
            f"Available encoders include: {names}"
        )

    encode_options = mxwrite_ext.EncodeOptions()
    encode_options.codec = args.codec
    encode_options.preset = args.preset
    encode_options.crf = args.crf
    encode_options.block_when_full = True

    writer = mxwrite_ext.Writer()
    open_method = writer.open_ts if args.explicit_pts else writer.open
    if not open_method(
        str(args.output), args.width, args.height, float(args.fps), encode_options
    ):
        raise SystemExit(f"MXWrite could not open {args.output}")

    x_coordinates = np.linspace(0.0, 1.0, args.width, dtype=np.float32)[None, :]
    y_coordinates = np.linspace(0.0, 1.0, args.height, dtype=np.float32)[:, None]

    try:
        for frame_index in range(args.frames):
            frame = make_frame(
                x_coordinates, y_coordinates, frame_index, args.frames
            )
            if args.explicit_pts:
                writer.write_at_pts(frame, frame_index)
            else:
                writer.write(frame)
    finally:
        writer.close()

    frame_count = writer.get_frame_count()
    duration = writer.get_duration()
    byte_count = writer.get_bytes_written()
    file_size = args.output.stat().st_size if args.output.is_file() else 0
    expected_duration = args.frames / args.fps

    if frame_count != args.frames:
        raise SystemExit(f"Expected {args.frames} frames, but MXWrite reported {frame_count}.")
    if not math.isclose(
        duration, expected_duration, rel_tol=0.0, abs_tol=1.0 / args.fps
    ):
        raise SystemExit(
            f"Expected about {expected_duration:.3f}s, "
            f"but MXWrite reported {duration:.3f}s."
        )
    if file_size == 0:
        raise SystemExit(f"MXWrite did not create a non-empty file at {args.output}.")

    print(
        f"PASS: wrote {frame_count} frames, {duration:.3f}s, "
        f"{file_size} filesystem bytes to {args.output}"
    )
    print(f"MXWrite's muxer byte counter reported {byte_count} bytes.")


if __name__ == "__main__":
    main()
