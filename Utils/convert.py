import cv2
import numpy as np
import struct
import os
import sys
import math
import time


# ============================================================
# CONFIGURATION
# ============================================================

import argparse
WIDTH = 128
HEIGHT = 64

# OLED is much slower than normal video.
# 29 FPS matches the current playback target.
TARGET_FPS = 29

# Keyframe safety interval.
# A raw frame resets the XOR reference.
MAX_KEYFRAME_INTERVAL = 30

# Image processing
CROP_TO_ASPECT = True

# Adaptive threshold parameters.
# Increase BLOCK_SIZE for larger-scale lighting adaptation.
ADAPTIVE_BLOCK_SIZE = 15
ADAPTIVE_C = 4

# Small cleanup.
USE_MORPHOLOGY = True

# ============================================================
# STIK FORMAT
#
# Header: 16 bytes
#
# 0..3   : "STIK"
# 4..5   : width      uint16
# 6..7   : height     uint16
# 8      : FPS        uint8
# 9..12  : frame count uint32
# 13..15 : reserved
#
# Records:
#
# TYPE 0x01 = RAW FRAME
#   1 byte  type
#   2 bytes payload length
#   payload = 1024 bytes
#
# TYPE 0x02 = XOR-RLE DELTA
#   1 byte  type
#   2 bytes payload length
#   payload = RLE encoded XOR against previous frame
#
# RLE:
#
# control byte:
#
# 0xxxxxxx = ZERO RUN
#   length = control + 1
#
# 1xxxxxxx = LITERAL RUN
#   length = (control & 0x7F) + 1
#   followed by literal bytes
#
# ============================================================


FRAME_BYTES = (WIDTH * HEIGHT) // 8


def crop_to_2_1(frame):
    """Center-crop the source to the OLED's 2:1 aspect ratio."""

    h, w = frame.shape[:2]

    target_ratio = WIDTH / HEIGHT
    source_ratio = w / h

    if abs(source_ratio - target_ratio) < 0.01:
        return frame

    if source_ratio > target_ratio:
        # Too wide -> crop left/right
        new_w = int(h * target_ratio)
        x = (w - new_w) // 2
        return frame[:, x:x + new_w]

    else:
        # Too tall -> crop top/bottom
        new_h = int(w / target_ratio)
        y = (h - new_h) // 2
        return frame[y:y + new_h, :]


def preprocess(frame):
    """
    Convert a normal video frame into a high-detail
    128x64 monochrome image.
    """

    # --------------------------------------------------------
    # Grayscale
    # --------------------------------------------------------

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # --------------------------------------------------------
    # Crop to OLED aspect ratio
    # --------------------------------------------------------

    if CROP_TO_ASPECT:
        gray = crop_to_2_1(gray)

    # --------------------------------------------------------
    # High quality resize
    # --------------------------------------------------------

    gray = cv2.resize(
        gray,
        (WIDTH, HEIGHT),
        interpolation=cv2.INTER_LANCZOS4
    )

    # --------------------------------------------------------
    # Gentle contrast expansion
    #
    # This helps white outlines survive downscaling.
    # --------------------------------------------------------

    low, high = np.percentile(gray, (1.5, 98.5))

    if high > low + 5:
        gray = np.clip(
            (gray.astype(np.float32) - low)
            * 255.0 / (high - low),
            0,
            255
        ).astype(np.uint8)

    # --------------------------------------------------------
    # Adaptive threshold
    #
    # Better than a fixed threshold for scenes with
    # dark backgrounds and bright objects.
    # --------------------------------------------------------

    binary = cv2.adaptiveThreshold(
        gray,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY,
        ADAPTIVE_BLOCK_SIZE,
        ADAPTIVE_C
    )

    # --------------------------------------------------------
    # Very light cleanup
    #
    # Don't aggressively blur or erode.
    # Thin stickman outlines are important.
    # --------------------------------------------------------

    if USE_MORPHOLOGY:
        kernel = np.ones((2, 2), np.uint8)

        # Closing helps repair tiny breaks.
        binary = cv2.morphologyEx(
            binary,
            cv2.MORPH_CLOSE,
            kernel,
            iterations=1
        )

    return binary


def image_to_ssd1306(frame):
    """
    Convert 128x64 monochrome image into SSD1306 page format.

    1024 bytes:
      8 pages × 128 columns

    Each byte contains 8 vertical pixels.
    """

    packed = np.zeros(FRAME_BYTES, dtype=np.uint8)

    # Ensure pure binary.
    pixels = frame >= 128

    for page in range(HEIGHT // 8):

        y0 = page * 8

        for x in range(WIDTH):

            value = 0

            for bit in range(8):

                if pixels[y0 + bit, x]:
                    value |= (1 << bit)

            packed[page * WIDTH + x] = value

    return packed.tobytes()


def encode_rle(data):
    """
    Encode data using zero/literal RLE.

    This is deliberately simple so the ESP32 can decode it
    extremely quickly.
    """

    out = bytearray()

    i = 0
    n = len(data)

    while i < n:

        # ----------------------------------------------------
        # Zero run
        # ----------------------------------------------------

        if data[i] == 0:

            start = i

            while (
                i < n
                and data[i] == 0
                and i - start < 128
            ):
                i += 1

            length = i - start

            # 0xxxxxxx
            out.append(length - 1)

            continue

        # ----------------------------------------------------
        # Literal run
        # ----------------------------------------------------

        start = i

        while i < n and i - start < 128:

            if data[i] == 0:
                break

            i += 1

        length = i - start

        # 1xxxxxxx
        out.append(0x80 | (length - 1))
        out.extend(data[start:i])

    return bytes(out)


def decode_rle(encoded):
    """
    Decoder used internally to validate our output.
    """

    out = bytearray()

    i = 0

    while i < len(encoded):

        control = encoded[i]
        i += 1

        length = (control & 0x7F) + 1

        if control & 0x80:

            if i + length > len(encoded):
                raise ValueError("Invalid literal RLE")

            out.extend(encoded[i:i + length])
            i += length

        else:

            out.extend(b"\x00" * length)

    return bytes(out)


def xor_bytes(a, b):
    """
    XOR two 1024-byte frames.
    """

    aa = np.frombuffer(a, dtype=np.uint8)
    bb = np.frombuffer(b, dtype=np.uint8)

    return np.bitwise_xor(aa, bb).tobytes()


def validate_delta(previous, encoded):
    """
    Decode an XOR-RLE delta and reconstruct the frame.
    """

    delta = decode_rle(encoded)

    if len(delta) != FRAME_BYTES:
        raise ValueError(
            f"Delta decoded to {len(delta)} bytes, "
            f"expected {FRAME_BYTES}"
        )

    prev = np.frombuffer(previous, dtype=np.uint8)
    d = np.frombuffer(delta, dtype=np.uint8)

    reconstructed = np.bitwise_xor(prev, d)

    return reconstructed.tobytes()


def write_record(f, record_type, payload):
    """
    Write:

        uint8  type
        uint16 payload_length
        payload
    """

    if len(payload) > 65535:
        raise ValueError("Payload exceeds uint16 size")

    f.write(
        struct.pack(
            "<BH",
            record_type,
            len(payload)
        )
    )

    f.write(payload)


def format_bytes(value):
    if value < 1024:
        return f"{value} B"

    if value < 1024 * 1024:
        return f"{value / 1024:.2f} KB"

    return f"{value / (1024 * 1024):.2f} MB"


def main():
    parser = argparse.ArgumentParser(description="STIK VIDEO CONVERTER")
    parser.add_argument("input_video", help="Input video file")
    parser.add_argument("-o", "--output", help="Output file", required=True)
    args = parser.parse_args()

    INPUT_VIDEO = args.input_video
    OUTPUT_FILE = args.output
    if not OUTPUT_FILE.endswith('.bin'):
        OUTPUT_FILE += '.bin'

    print()
    print("=" * 64)
    print("        STIK VIDEO CONVERTER — HIGH DETAIL")
    print("=" * 64)
    print()

    if not os.path.exists(INPUT_VIDEO):
        print("ERROR:")
        print(f"Video not found:")
        print(INPUT_VIDEO)
        print()
        return

    cap = cv2.VideoCapture(INPUT_VIDEO)

    if not cap.isOpened():
        print("ERROR: Could not open video.")
        return

    source_fps = cap.get(cv2.CAP_PROP_FPS)
    source_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    source_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    source_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    duration = (
        source_frames / source_fps
        if source_fps > 0
        else 0
    )

    target_fps = 18

    output_frames = int(
        math.floor(duration * target_fps + 1e-6)
    )

    print(f"Input       : {INPUT_VIDEO}")
    print(f"Source      : {source_width}x{source_height}")
    print(f"Source FPS  : {source_fps:.6f}")
    print(f"Source frames: {source_frames}")
    print(f"Duration    : {duration:.3f} sec")
    print()
    print(f"Output      : {WIDTH}x{HEIGHT}")
    print(f"Output FPS  : {target_fps}")
    print(f"Output frames: {output_frames}")
    print(f"Raw frame   : {FRAME_BYTES} bytes")
    print()
    print("Processing...")
    print()

    # --------------------------------------------------------
    # First pass is NOT required.
    #
    # We already know duration/frame count from the container.
    # --------------------------------------------------------

    with open(OUTPUT_FILE, "wb") as out:

        # ----------------------------------------------------
        # Write 16-byte header.
        # ----------------------------------------------------

        header = struct.pack(
            "<4sHHBI3s",
            b"STIK",
            WIDTH,
            HEIGHT,
            target_fps,
            output_frames,
            b"\x00\x00\x00"
        )

        if len(header) != 16:
            raise RuntimeError(
                f"Header size is {len(header)}, expected 16"
            )

        out.write(header)

        previous_frame = None

        frame_index = 0
        raw_count = 0
        delta_count = 0

        raw_bytes = 0
        compressed_bytes = 0

        max_delta_size = 0

        start_time = time.time()

        next_output_time = 0.0

        source_index = -1

        while frame_index < output_frames:

            # ------------------------------------------------
            # Read sequentially.
            # ------------------------------------------------

            ok, frame = cap.read()

            if not ok:
                break

            source_index += 1

            current_time = source_index / source_fps

            # ------------------------------------------------
            # Timestamp selection.
            #
            # We output a frame when the current source frame
            # reaches the next desired output timestamp.
            # ------------------------------------------------

            if current_time + (0.5 / source_fps) < next_output_time:
                continue

            # ------------------------------------------------
            # Process image.
            # ------------------------------------------------

            processed = preprocess(frame)

            current = image_to_ssd1306(processed)

            # ------------------------------------------------
            # First frame MUST be raw.
            # ------------------------------------------------

            force_keyframe = (
                previous_frame is None
                or frame_index % MAX_KEYFRAME_INTERVAL == 0
            )

            if force_keyframe:

                write_record(
                    out,
                    0x01,
                    current
                )

                raw_count += 1
                raw_bytes += FRAME_BYTES + 3

            else:

                # --------------------------------------------
                # XOR against previous displayed frame.
                # --------------------------------------------

                delta = xor_bytes(
                    previous_frame,
                    current
                )

                encoded = encode_rle(delta)

                max_delta_size = max(
                    max_delta_size,
                    len(encoded)
                )

                # --------------------------------------------
                # If compression is actually worse than raw,
                # store a raw frame instead.
                # --------------------------------------------

                if len(encoded) >= FRAME_BYTES:

                    write_record(
                        out,
                        0x01,
                        current
                    )

                    raw_count += 1
                    raw_bytes += FRAME_BYTES + 3

                else:

                    write_record(
                        out,
                        0x02,
                        encoded
                    )

                    delta_count += 1
                    compressed_bytes += len(encoded) + 3

            # ------------------------------------------------
            # Validate reconstruction.
            # ------------------------------------------------

            if previous_frame is not None and not force_keyframe:

                if len(encoded) < FRAME_BYTES:

                    reconstructed = validate_delta(
                        previous_frame,
                        encoded
                    )

                    if reconstructed != current:
                        raise RuntimeError(
                            f"VALIDATION FAILED at frame "
                            f"{frame_index}"
                        )

            # ------------------------------------------------
            # Update reference.
            # ------------------------------------------------

            previous_frame = current

            frame_index += 1

            next_output_time = frame_index / TARGET_FPS

            # ------------------------------------------------
            # Progress
            # ------------------------------------------------

            if frame_index % 50 == 0 or frame_index == output_frames:

                elapsed = time.time() - start_time

                rate = (
                    frame_index / elapsed
                    if elapsed > 0
                    else 0
                )

                percent = (
                    frame_index * 100 / output_frames
                    if output_frames
                    else 0
                )

                print(
                    f"\r{percent:6.2f}% | "
                    f"{frame_index}/{output_frames} | "
                    f"{rate:.1f} frames/s",
                    end="",
                    flush=True
                )

    cap.release()

    # Rewrite header with exact generated frame count and target_fps
    with open(OUTPUT_FILE, "r+b") as out_fix:
        out_fix.seek(8)
        out_fix.write(struct.pack("<BI", target_fps, frame_index))

    final_size = os.path.getsize(OUTPUT_FILE)

    theoretical_raw = (
        frame_index * FRAME_BYTES
    )

    ratio = (
        theoretical_raw / final_size
        if final_size
        else 0
    )

    elapsed = time.time() - start_time

    print()
    print()
    print("=" * 64)
    print("CONVERSION COMPLETE")
    print("=" * 64)
    print()

    print(f"Output file       : {OUTPUT_FILE}")
    print(f"Frames generated  : {frame_index}")
    print(f"Resolution        : {WIDTH}x{HEIGHT}")
    print(f"FPS               : {target_fps}")
    print()
    print(f"Raw records       : {raw_count}")
    print(f"Delta records     : {delta_count}")
    print(f"Largest delta     : {max_delta_size} bytes")
    print()
    print(f"Raw video size    : {format_bytes(theoretical_raw)}")
    print(f"STIK size         : {format_bytes(final_size)}")
    print(f"Compression ratio : {ratio:.2f}x")
    print(f"Conversion time   : {elapsed:.1f} sec")
    print()

    if frame_index != output_frames:
        print("WARNING:")
        print(
            f"Expected {output_frames} frames but generated "
            f"{frame_index}."
        )
        print()

    print("Validation: PASSED")
    print()
    print("Ready for ESP32-CAM + PSRAM playback.")
    print()


if __name__ == "__main__":
    main()
    