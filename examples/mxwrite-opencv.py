#!/usr/bin/env python3

import argparse
import math
import sys
import time
from pathlib import Path

try:
    import cv2
except ImportError as error:
    raise SystemExit("OpenCV is required: python3 -m pip install opencv-python") from error

try:
    import numpy as np
except ImportError as error:
    raise SystemExit("NumPy is required: python3 -m pip install numpy") from error

try:
    import mxwrite_ext
except ImportError as error:
    raise SystemExit("Could not import mxwrite_ext. Build with -DPYTHON_MODULE=ON and set PYTHONPATH to the directory containing the built module.") from error


def parse_arguments():
    parser = argparse.ArgumentParser(description="Capture video from a file or webcam and encode it with MXWrite using timestamps.")

    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="Input video file.")
    source.add_argument("--camera", type=int, help="Webcam device index, for example --camera 0.")

    parser.add_argument("--output", type=Path, default=Path("mxwrite-output.mp4"))
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--frames", type=int, default=0, help="Maximum number of frames. 0 means unlimited.")
    parser.add_argument("--codec", default="libx264")
    parser.add_argument("--preset", default="veryfast")
    parser.add_argument("--crf", type=int, default=20)
    parser.add_argument("--normalize-pts", action="store_true")

    return parser.parse_args()


def validate_arguments(args):
    if args.input is not None and not args.input.is_file():
        raise SystemExit(f"Input file does not exist: {args.input}")

    if args.width <= 0 or args.height <= 0:
        raise SystemExit("Width and height must be positive.")

    if args.fps <= 0.0:
        raise SystemExit("FPS must be positive.")

    if args.frames < 0:
        raise SystemExit("Frame count cannot be negative.")

    if not 0 <= args.crf <= 51:
        raise SystemExit("CRF must be between 0 and 51.")


def fourcc_to_string(value):
    value = int(value)

    if value <= 0:
        return "unknown"

    return "".join(chr((value >> (8 * i)) & 0xff) for i in range(4))


def open_file_capture(filename):
    capture = cv2.VideoCapture(str(filename), cv2.CAP_FFMPEG)

    if capture.isOpened():
        return capture

    capture.release()
    capture = cv2.VideoCapture(str(filename))

    if not capture.isOpened():
        raise SystemExit(f"Could not open input video: {filename}")

    return capture


def open_camera_capture(index, width, height, fps):
    if sys.platform.startswith("linux"):
        print("Opening camera with Linux V4L2 backend.")

        capture = cv2.VideoCapture(index, cv2.CAP_V4L2)

        if not capture.isOpened():
            raise SystemExit(f"Could not open camera {index} using V4L2")

        fourcc = cv2.VideoWriter_fourcc("M", "J", "P", "G")

        capture.set(cv2.CAP_PROP_FOURCC, fourcc)
        capture.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        capture.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        capture.set(cv2.CAP_PROP_FPS, fps)
    else:
        print("Opening camera with the default OpenCV backend.")

        capture = cv2.VideoCapture(index)

        if not capture.isOpened():
            raise SystemExit(f"Could not open camera {index}")

        capture.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        capture.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        capture.set(cv2.CAP_PROP_FPS, fps)

    actual_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = float(capture.get(cv2.CAP_PROP_FPS))
    actual_fourcc = capture.get(cv2.CAP_PROP_FOURCC)

    try:
        backend = capture.getBackendName()
    except cv2.error:
        backend = "unknown"

    print(f"Camera backend: {backend}")
    print(f"Camera format: {fourcc_to_string(actual_fourcc)}")
    print(f"Camera resolution: {actual_width}x{actual_height}")
    print(f"Camera FPS: {actual_fps:.3f}")

    if actual_width != width or actual_height != height:
        print(f"Warning: requested {width}x{height}, but camera negotiated {actual_width}x{actual_height}.")

    if actual_fps > 0.0 and not math.isclose(actual_fps, fps, rel_tol=0.01, abs_tol=0.1):
        print(f"Warning: requested {fps:.3f} FPS, but camera negotiated {actual_fps:.3f} FPS.")

    return capture


def get_file_timestamp(capture, source_fps, frame_index):
    if hasattr(cv2, "CAP_PROP_PTS"):
        pts = capture.get(cv2.CAP_PROP_PTS)

        if math.isfinite(pts) and pts >= 0.0:
            return pts / source_fps, "PTS"

    position_ms = capture.get(cv2.CAP_PROP_POS_MSEC)

    if math.isfinite(position_ms) and position_ms >= 0.0:
        return position_ms / 1000.0, "POS_MSEC"

    return frame_index / source_fps, "FRAME_INDEX"


def configure_encoder(args):
    encoders = mxwrite_ext.available_video_encoders()
    matching_encoders = [encoder for encoder in encoders if encoder.name == args.codec]

    print(f"MXWrite reported {len(encoders)} video encoder(s).")

    if matching_encoders:
        encoder = matching_encoders[0]
        mode = "hardware" if encoder.hardware else "software"

        print(f"Using {encoder.name} ({encoder.long_name}, {mode}).")

        options = mxwrite_ext.video_encoder_options(encoder.name)

        print(f"The encoder exposes {len(options)} configurable option(s).")

    elif args.codec not in {"auto", "software", "cpu", "x264", "h264", "hevc", "h265", "nvenc"}:
        names = ", ".join(encoder.name for encoder in encoders[:12])
        raise SystemExit(f"Requested encoder '{args.codec}' is unavailable. Available encoders include: {names}")

    encode_options = mxwrite_ext.EncodeOptions()
    encode_options.codec = args.codec
    encode_options.preset = args.preset
    encode_options.crf = args.crf
    encode_options.block_when_full = True

    return encode_options


def main():
    args = parse_arguments()
    validate_arguments(args)

    args.output.parent.mkdir(parents=True, exist_ok=True)

    is_camera = args.camera is not None

    if is_camera:
        capture = open_camera_capture(args.camera, args.width, args.height, args.fps)
    else:
        capture = open_file_capture(args.input)

    window_name = "MXWrite Capture"

    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)

    try:
        source_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
        source_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
        source_fps = float(capture.get(cv2.CAP_PROP_FPS))

        if source_width <= 0 or source_height <= 0:
            raise SystemExit(f"Invalid capture resolution: {source_width}x{source_height}")

        if not math.isfinite(source_fps) or source_fps <= 0.0:
            source_fps = args.fps

        output_fps = args.fps if is_camera else source_fps

        try:
            backend = capture.getBackendName()
        except cv2.error:
            backend = "unknown"

        print()
        print(f"Backend: {backend}")
        print(f"Resolution: {source_width}x{source_height}")
        print(f"Capture FPS: {source_fps:.6f}")
        print(f"Output time base: 1/{output_fps:.6f}")

        if is_camera:
            print(f"Camera: {args.camera}")

            if sys.platform.startswith("linux"):
                print("Linux camera mode: V4L2 + MJPEG")
            else:
                print("Camera mode: default OpenCV backend")

            print("Timestamp source: monotonic clock")
        else:
            print(f"Input: {args.input}")

        print("Press Escape to stop capture.")

        encode_options = configure_encoder(args)
        writer = mxwrite_ext.Writer()

        if not writer.open_ts(str(args.output), source_width, source_height, float(output_fps), encode_options):
            raise SystemExit(f"MXWrite could not open {args.output}")

        frame_index = 0
        first_timestamp = None
        start_time = None
        last_pts = None
        timestamp_source = None

        try:
            while True:
                if args.frames > 0 and frame_index >= args.frames:
                    break

                success, bgr_frame = capture.read()

                if not success:
                    if is_camera:
                        print("Camera capture failed.")

                    break

                if is_camera:
                    capture_time = time.monotonic()

                    if start_time is None:
                        start_time = capture_time

                    timestamp = capture_time - start_time
                    current_source = "MONOTONIC"
                else:
                    timestamp, current_source = get_file_timestamp(capture, source_fps, frame_index)

                    if first_timestamp is None:
                        first_timestamp = timestamp

                    if args.normalize_pts:
                        timestamp -= first_timestamp

                if timestamp_source != current_source:
                    timestamp_source = current_source
                    print(f"Timestamp source: {timestamp_source}")

                output_pts = int(round(timestamp * output_fps))

                if last_pts is not None and output_pts < last_pts:
                    print(f"Warning: non-monotonic PTS at frame {frame_index}: {output_pts} < {last_pts}")
                    output_pts = last_pts

                rgba_frame = cv2.cvtColor(bgr_frame, cv2.COLOR_BGR2RGBA)

                writer.write_at_pts(rgba_frame, output_pts)

                cv2.imshow(window_name, bgr_frame)

                key = cv2.waitKey(1) & 0xff

                if key == 27:
                    print("Escape pressed. Stopping capture.")
                    break

                last_pts = output_pts

                if frame_index < 10 or frame_index % 100 == 0:
                    print(f"frame={frame_index:6d} time={timestamp:10.6f}s pts={output_pts:8d}")

                frame_index += 1

        except KeyboardInterrupt:
            print("\nCapture stopped.")

        finally:
            writer.close()

        frame_count = writer.get_frame_count()
        duration = writer.get_duration()
        byte_count = writer.get_bytes_written()
        file_size = args.output.stat().st_size if args.output.is_file() else 0

        if file_size == 0:
            raise SystemExit(f"MXWrite did not create a non-empty file at {args.output}.")

        print()
        print(f"PASS: wrote {frame_count} frames, {duration:.3f}s, {file_size} filesystem bytes to {args.output}")
        print(f"MXWrite's muxer byte counter reported {byte_count} bytes.")

        if last_pts is not None:
            print(f"Last video PTS: {last_pts} ({last_pts / output_fps:.6f}s)")

    finally:
        capture.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
