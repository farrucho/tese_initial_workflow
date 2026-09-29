#!/usr/bin/env python3
import argparse
import array
import json
import math
import pathlib
import sys

CHANNELS = 16
BYTES_PER_VALUE = 4


def main():
    parser = argparse.ArgumentParser(description="Inspect an ATCA raw capture")
    parser.add_argument("file", type=pathlib.Path)
    args = parser.parse_args()
    size = args.file.stat().st_size
    frame_bytes = CHANNELS * BYTES_PER_VALUE
    if size % frame_bytes:
        raise SystemExit(f"invalid size: {size} is not divisible by {frame_bytes}")

    metadata_path = pathlib.Path(str(args.file) + ".json")
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else None
    count = [0] * CHANNELS
    total = [0] * CHANNELS
    total2 = [0] * CHANNELS
    minimum = [None] * CHANNELS
    maximum = [None] * CHANNELS

    with args.file.open("rb") as source:
        while chunk := source.read(frame_bytes * 65536):
            values = array.array("i")
            values.frombytes(chunk)
            if sys.byteorder != "little":
                values.byteswap()
            for index, value in enumerate(values):
                channel = index % CHANNELS
                count[channel] += 1
                total[channel] += value
                total2[channel] += value * value
            for channel in range(CHANNELS):
                channel_values = values[channel::CHANNELS]
                if channel_values:
                    lo, hi = min(channel_values), max(channel_values)
                    minimum[channel] = lo if minimum[channel] is None else min(minimum[channel], lo)
                    maximum[channel] = hi if maximum[channel] is None else max(maximum[channel], hi)

    samples = size // frame_bytes
    print(f"file: {args.file}")
    print(f"bytes: {size}")
    print(f"samples/channel: {samples}")
    if metadata:
        print(f"metadata complete: {metadata.get('complete')}")
        print(f"capture duration: {metadata.get('captured_duration_s')} s")
    print("channel          min          max             mean          stddev")
    for channel in range(CHANNELS):
        if not count[channel]:
            print(f"ch{channel:02d} no samples")
            continue
        mean = total[channel] / count[channel] if count[channel] else math.nan
        variance = total2[channel] / count[channel] - mean * mean if count[channel] else math.nan
        stddev = math.sqrt(max(0.0, variance))
        print(f"ch{channel:02d} {minimum[channel]:12d} {maximum[channel]:12d} "
              f"{mean:16.3f} {stddev:15.3f}")


if __name__ == "__main__":
    main()
