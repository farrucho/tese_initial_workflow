#!/usr/bin/env python3
"""Plot a time window from an ATCA raw capture."""

import argparse
import array
import json
import math
import pathlib
import sys


DEFAULT_CHANNELS = 16
DEFAULT_RATE = 2_000_000
BYTES_PER_VALUE = 4


def parse_channels(text, channel_count):
    if text.lower() == "all":
        return list(range(channel_count))
    try:
        channels = [int(value) for value in text.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("channels must be 'all' or comma-separated numbers") from error
    if not channels or len(channels) != len(set(channels)):
        raise argparse.ArgumentTypeError("channels must be a non-empty list without duplicates")
    if any(channel < 0 or channel >= channel_count for channel in channels):
        raise argparse.ArgumentTypeError(f"channel must be between 0 and {channel_count - 1}")
    return channels


def main():
    parser = argparse.ArgumentParser(description="Plot an ATCA raw capture")
    parser.add_argument("file", type=pathlib.Path, help="capture .bin file")
    parser.add_argument("--start", type=float, default=0.0, help="window start in seconds (default: 0)")
    parser.add_argument("--duration", type=float, default=0.01,
                        help="window duration in seconds (default: 0.01)")
    parser.add_argument("--channels", default="all", help="all or a list such as 0,1,4")
    parser.add_argument("--max-points", type=int, default=100_000,
                        help="maximum plotted points per channel (default: 100000)")
    parser.add_argument("--raw", action="store_true",
                        help="plot complete FPGA int32 words instead of ADC codes")
    parser.add_argument("--output", type=pathlib.Path,
                        help="save the plot (for example plot.png) instead of opening a window")
    args = parser.parse_args()

    if args.start < 0 or args.duration <= 0 or not math.isfinite(args.start + args.duration):
        parser.error("--start must be non-negative and --duration must be positive")
    if args.max_points < 100:
        parser.error("--max-points must be at least 100")

    metadata_path = pathlib.Path(str(args.file) + ".json")
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    channel_count = int(metadata.get("channels", DEFAULT_CHANNELS))
    rate = float(metadata.get("adc_rate_hz", DEFAULT_RATE))
    shift = int(metadata.get("fpga_left_shift_bits", 14))
    channels = parse_channels(args.channels, channel_count)
    frame_bytes = channel_count * BYTES_PER_VALUE
    file_size = args.file.stat().st_size
    if file_size % frame_bytes:
        raise SystemExit(f"invalid size: {file_size} is not divisible by {frame_bytes}")

    total_samples = file_size // frame_bytes
    first_sample = round(args.start * rate)
    requested_samples = max(1, round(args.duration * rate))
    if first_sample >= total_samples:
        raise SystemExit(f"start is outside capture ({total_samples / rate:.9g} s)")
    sample_count = min(requested_samples, total_samples - first_sample)

    with args.file.open("rb") as source:
        source.seek(first_sample * frame_bytes)
        values = array.array("i")
        values.frombytes(source.read(sample_count * frame_bytes))
    if sys.byteorder != "little":
        values.byteswap()
    if len(values) != sample_count * channel_count:
        raise SystemExit("capture ended while reading the requested window")

    stride = max(1, math.ceil(sample_count / args.max_points))
    sample_indices = range(0, sample_count, stride)
    times_ms = [(first_sample + index) * 1000.0 / rate for index in sample_indices]

    try:
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise SystemExit(
            "matplotlib is required. Install it with: python3 -m pip install matplotlib"
        ) from error

    columns = min(4, len(channels))
    rows = math.ceil(len(channels) / columns)
    figure, axes = plt.subplots(rows, columns, figsize=(4.2 * columns, 2.5 * rows),
                                squeeze=False, sharex=True)
    for axis, channel in zip(axes.flat, channels):
        channel_values = values[channel::channel_count]
        plotted = channel_values[::stride]
        if not args.raw:
            plotted = [value >> shift for value in plotted]
        axis.plot(times_ms, plotted, linewidth=0.7)
        axis.set_title(f"ch{channel:02d}")
        axis.grid(True, alpha=0.3)
        axis.set_ylabel("FPGA int32" if args.raw else "ADC code")
    for axis in list(axes.flat)[len(channels):]:
        axis.set_visible(False)
    for axis in axes[-1]:
        if axis.get_visible():
            axis.set_xlabel("Time (ms)")

    plotted_count = math.ceil(sample_count / stride)
    figure.suptitle(
        f"{args.file.name} — {sample_count / rate:.9g} s window, "
        f"{plotted_count} plotted points/channel"
    )
    figure.tight_layout()
    if args.output:
        figure.savefig(args.output, dpi=150)
        print(f"saved: {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
