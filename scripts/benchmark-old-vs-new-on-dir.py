#!/usr/bin/env python3
#
# Compare libdeflate raw DEFLATE compression and decompression throughput
# between two builds of the 'benchmark' program across all files in a directory
# at compression levels 1 through 12.
#
# Example:
#   ./scripts/benchmark-old-vs-new-on-dir.py ~/bin/benchmark-old ~/bin/benchmark-new ~/data/corpus/

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LEVELS = range(1, 13)
WARMUP_SIZE = 1048576
MAX_CHUNK_SIZE = 0xFFFFFFFF

FILE_RESULT_RE = re.compile(
    r"^\tCompressed (\d+) => (\d+) bytes \([0-9.]+%\)\n"
    r"\tCompression time: (\d+) ms \((\d+) MB/s\)\n"
    r"\tDecompression time: (\d+) ms \((\d+) MB/s\)$",
    re.MULTILINE,
)


def resolve_binary(path_str: str) -> str:
    expanded = os.path.expanduser(path_str)
    if os.sep in expanded or (os.altsep and os.altsep in expanded):
        abspath = os.path.abspath(expanded)
        if not os.path.isfile(abspath):
            sys.exit(f"error: binary not found: {path_str}")
        if not os.access(abspath, os.X_OK):
            sys.exit(f"error: binary is not executable: {path_str}")
        return abspath
    found = shutil.which(expanded)
    if found is None:
        sys.exit(f"error: binary not found: {path_str}")
    return os.path.abspath(found)


def collect_files(directory: str) -> list[tuple[str, int]]:
    dir_path = Path(os.path.expanduser(directory))
    if not dir_path.is_dir():
        sys.exit(f"error: not a directory: {directory}")

    files: list[tuple[str, int]] = []
    for name in sorted(os.listdir(dir_path)):
        if name.startswith("."):
            continue
        path = dir_path / name
        if not path.is_file():
            continue
        size = path.stat().st_size
        if size == 0:
            print(f"warning: skipping empty file: {path}", file=sys.stderr)
            continue
        path_str = str(path)
        if path_str.startswith("-"):
            path_str = os.path.join(".", path_str)
        files.append((path_str, size))

    if not files:
        sys.exit(f"error: no non-empty regular files found in {directory}")
    return files


def make_warmup_data(files: list[tuple[str, int]], target_size: int) -> bytes:
    chunks: list[bytes] = []
    total = 0
    for path, _size in files:
        with open(path, "rb") as f:
            data = f.read(target_size - total)
        if data:
            chunks.append(data)
            total += len(data)
            if total >= target_size:
                break
    buf = b"".join(chunks)
    if not buf:
        return b"\x00" * target_size
    if len(buf) < target_size:
        repeats = (target_size + len(buf) - 1) // len(buf)
        buf = (buf * repeats)[:target_size]
    return buf


def estimate_throughput_mbps(uncompressed_bytes: int, ms: int, mbps: int) -> float:
    """
    Recover throughput in MB/s (10^6 bytes/s) by intersecting the elapsed-time
    intervals implied by both the truncated integer millisecond count and the
    truncated integer MB/s value printed by 'benchmark'.  This preserves high
    precision for both fast runs (small ms, large MB/s) and slow runs such as
    level 12 compression (large ms, single-digit MB/s).
    """
    mb = uncompressed_bytes / 1e6
    t_ms_lo = ms / 1000.0
    t_ms_hi = (ms + 1) / 1000.0

    if mbps > 0:
        t_rate_lo = mb / (mbps + 1)
        t_rate_hi = mb / mbps
        t_lo = max(t_ms_lo, t_rate_lo)
        t_hi = min(t_ms_hi, t_rate_hi)
        if t_lo <= t_hi and t_hi > 0.0:
            return mb / (0.5 * (t_lo + t_hi))

    if ms > 0:
        return mb / ((ms + 0.5) / 1000.0)
    if mbps > 0:
        return float(mbps)
    if uncompressed_bytes > 0:
        # Both ms and mbps truncated to 0, so t < 0.001 s and mb / t < 1 MB/s
        # (i.e. t > mb).  Use the midpoint of [mb, 0.001) when mb < 0.001.
        t_est = 0.5 * (mb + 0.001) if mb < 0.001 else 0.0005
        return mb / t_est
    raise ValueError(
        f"unable to determine throughput from {uncompressed_bytes} bytes, "
        f"{ms} ms, {mbps} MB/s"
    )


def run_benchmark_once(
    binary: str,
    level: int,
    warmup_file: str,
    files: list[str],
    chunk_size: int,
) -> list[tuple[float, float]]:
    cmd = [binary, f"-{level}", "-e", "-s", str(chunk_size), warmup_file, *files]

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed (exit {proc.returncode}): {' '.join(cmd)}\n"
            f"{proc.stderr.strip()}"
        )

    matches = FILE_RESULT_RE.findall(proc.stdout)
    expected = 1 + len(files)
    if len(matches) != expected:
        raise RuntimeError(
            f"expected {expected} file results from {' '.join(cmd)}, "
            f"got {len(matches)}\noutput:\n{proc.stdout}"
        )

    results: list[tuple[float, float]] = []
    for u_bytes_s, _c_bytes_s, c_ms_s, c_mbps_s, d_ms_s, d_mbps_s in matches[1:]:
        u_bytes = int(u_bytes_s)
        c_tp = estimate_throughput_mbps(u_bytes, int(c_ms_s), int(c_mbps_s))
        d_tp = estimate_throughput_mbps(u_bytes, int(d_ms_s), int(d_mbps_s))
        results.append((c_tp, d_tp))
    return results


def update_best(
    best_c: list[float],
    best_d: list[float],
    offset: int,
    results: list[tuple[float, float]],
) -> None:
    for j, (c_tp, d_tp) in enumerate(results):
        idx = offset + j
        if c_tp > best_c[idx]:
            best_c[idx] = c_tp
        if d_tp > best_d[idx]:
            best_d[idx] = d_tp


def geomean(ratios: list[float]) -> float:
    return math.exp(math.fsum(math.log(r) for r in ratios) / len(ratios))


def format_pct_change(pct: float) -> str:
    if abs(pct) < 0.005:
        pct = 0.0
    return f"{pct:+.2f}%"


def format_extremum(files: list[str], ratios: list[float], pick_max: bool) -> str:
    idx, ratio = (max if pick_max else min)(enumerate(ratios), key=lambda x: x[1])
    name = os.path.basename(files[idx]).replace("|", "\\|")
    pct_str = format_pct_change((ratio - 1.0) * 100.0)
    return f"{pct_str} ({name})"


def print_table(
    title: str,
    rows: list[tuple[int, str, str]],
    align_right: bool = True,
) -> None:
    print(f"### {title}")
    print()
    c_hdr = "Compression Throughput"
    d_hdr = "Decompression Throughput"
    c_w = max(len(c_hdr), *(len(c) for _, c, _ in rows))
    d_w = max(len(d_hdr), *(len(d) for _, _, d in rows))
    if align_right:
        print(f"| Level | {c_hdr:>{c_w}s} | {d_hdr:>{d_w}s} |")
        print(f"|------:|{'-' * (c_w + 1)}:|{'-' * (d_w + 1)}:|")
        for level, comp_str, decomp_str in rows:
            print(f"| {level:5d} | {comp_str:>{c_w}s} | {decomp_str:>{d_w}s} |")
    else:
        print(f"| Level | {c_hdr:<{c_w}s} | {d_hdr:<{d_w}s} |")
        print(f"|------:|:{'-' * (c_w + 1)}|:{'-' * (d_w + 1)}|")
        for level, comp_str, decomp_str in rows:
            print(f"| {level:5d} | {comp_str:<{c_w}s} | {decomp_str:<{d_w}s} |")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare libdeflate DEFLATE compression and decompression "
            "throughput between two benchmark binaries across levels 1-12."
        )
    )
    parser.add_argument("benchmark_old", help="path to old benchmark binary")
    parser.add_argument("benchmark_new", help="path to new benchmark binary")
    parser.add_argument("directory", help="directory of input files to benchmark")
    parser.add_argument(
        "-n",
        "--iterations",
        type=int,
        default=3,
        help="number of interleaved A/B iterations per level (default: 3)",
    )
    parser.add_argument(
        "-s",
        "--chunk-size",
        type=int,
        default=None,
        help="chunk size in bytes passed to benchmark -s (default: each file's size)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print per-file throughput changes to stderr",
    )
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("--iterations must be >= 1")
    if args.chunk_size is not None and not (1 <= args.chunk_size <= MAX_CHUNK_SIZE):
        parser.error(f"--chunk-size must be between 1 and {MAX_CHUNK_SIZE}")
    return args


def main() -> None:
    args = parse_args()
    bench_old = resolve_binary(args.benchmark_old)
    bench_new = resolve_binary(args.benchmark_new)
    file_entries = collect_files(args.directory)
    files = [path for path, _size in file_entries]
    file_sizes = [size for _path, size in file_entries]

    warmup_target = (
        min(args.chunk_size, WARMUP_SIZE)
        if args.chunk_size is not None
        else min(max(file_sizes), WARMUP_SIZE)
    )
    warmup_data = make_warmup_data(file_entries, warmup_target)
    tty_progress = sys.stderr.isatty() and not args.verbose

    geomean_rows: list[tuple[int, str, str]] = []
    max_rows: list[tuple[int, str, str]] = []
    min_rows: list[tuple[int, str, str]] = []

    with tempfile.TemporaryDirectory(prefix="libdeflate_bench_") as tmpdir:
        warmup_path = os.path.join(tmpdir, "warmup")
        with open(warmup_path, "wb") as warmup_fp:
            warmup_fp.write(warmup_data)

        small_warmup_paths: dict[int, str] = {}
        if args.chunk_size is None:
            for size in set(file_sizes):
                if size < warmup_target:
                    p = os.path.join(tmpdir, f"warmup_{size}")
                    with open(p, "wb") as fp:
                        fp.write(warmup_data[:size])
                    small_warmup_paths[size] = p

        try:
            for level in LEVELS:
                best_old_c = [0.0] * len(files)
                best_old_d = [0.0] * len(files)
                best_new_c = [0.0] * len(files)
                best_new_d = [0.0] * len(files)

                for it in range(args.iterations):
                    if tty_progress:
                        print(
                            f"\rBenchmarking level {level}/12 "
                            f"(iteration {it + 1}/{args.iterations})...",
                            end="",
                            file=sys.stderr,
                            flush=True,
                        )

                    # Alternate old/new order across iterations to reduce drift.
                    pair = (
                        [
                            (bench_old, best_old_c, best_old_d),
                            (bench_new, best_new_c, best_new_d),
                        ]
                        if it % 2 == 0
                        else [
                            (bench_new, best_new_c, best_new_d),
                            (bench_old, best_old_c, best_old_d),
                        ]
                    )
                    if args.chunk_size is not None:
                        for binary, best_c, best_d in pair:
                            res = run_benchmark_once(
                                binary, level, warmup_path, files, args.chunk_size
                            )
                            update_best(best_c, best_d, 0, res)
                    else:
                        for i, (path, size) in enumerate(file_entries):
                            chunk_size = min(size, MAX_CHUNK_SIZE)
                            w_path = small_warmup_paths.get(size, warmup_path)
                            for binary, best_c, best_d in pair:
                                res = run_benchmark_once(
                                    binary, level, w_path, [path], chunk_size
                                )
                                update_best(best_c, best_d, i, res)

                comp_ratios = [
                    new_c / old_c for old_c, new_c in zip(best_old_c, best_new_c)
                ]
                decomp_ratios = [
                    new_d / old_d for old_d, new_d in zip(best_old_d, best_new_d)
                ]

                if args.verbose:
                    print(f"Level {level}:", file=sys.stderr)
                    for path, old_c, new_c, cr, old_d, new_d, dr in zip(
                        files,
                        best_old_c,
                        best_new_c,
                        comp_ratios,
                        best_old_d,
                        best_new_d,
                        decomp_ratios,
                    ):
                        name = os.path.basename(path)
                        c_pct = format_pct_change((cr - 1.0) * 100.0)
                        d_pct = format_pct_change((dr - 1.0) * 100.0)
                        print(
                            f"  {name}: compress {old_c:.1f} -> {new_c:.1f} MB/s ({c_pct}), "
                            f"decompress {old_d:.1f} -> {new_d:.1f} MB/s ({d_pct})",
                            file=sys.stderr,
                        )

                comp_geomean_pct = (geomean(comp_ratios) - 1.0) * 100.0
                decomp_geomean_pct = (geomean(decomp_ratios) - 1.0) * 100.0
                geomean_rows.append(
                    (
                        level,
                        format_pct_change(comp_geomean_pct),
                        format_pct_change(decomp_geomean_pct),
                    )
                )
                max_rows.append(
                    (
                        level,
                        format_extremum(files, comp_ratios, pick_max=True),
                        format_extremum(files, decomp_ratios, pick_max=True),
                    )
                )
                min_rows.append(
                    (
                        level,
                        format_extremum(files, comp_ratios, pick_max=False),
                        format_extremum(files, decomp_ratios, pick_max=False),
                    )
                )
        except RuntimeError as exc:
            if tty_progress:
                print("\r" + " " * 60 + "\r", end="", file=sys.stderr, flush=True)
            sys.exit(f"error: {exc}")

    if tty_progress:
        print("\r" + " " * 60 + "\r", end="", file=sys.stderr, flush=True)

    print_table("Geometric mean", geomean_rows, align_right=True)
    print()
    print_table("Best case (maximum)", max_rows, align_right=False)
    print()
    print_table("Worst case (minimum)", min_rows, align_right=False)


if __name__ == "__main__":
    main()
