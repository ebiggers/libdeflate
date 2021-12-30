#!/usr/bin/env python3

import csv
import os
import re
import subprocess
import statistics
import sys

SCRIPTDIR = os.path.dirname(sys.argv[0])
BENCHMARK_PROG = os.path.join(SCRIPTDIR, "..", "benchmark")
NTRIES = 3

def usage():
    print("""
Usage: corpus_util.py run CORPUS_DIR
       corpus_util.py analyze BENCHMARK_RESULTS_CSV
       corpus_util.py compare SUMMARY_1_CSV SUMMARY_2_CSV""",
         file=sys.stderr)
    sys.exit(2)

def run_benchmark_prog(path, level, use_libz):
    args = [BENCHMARK_PROG, path, f'-{level}']
    if use_libz:
        args.extend(['-C', 'libz'])
    cspeed = -1
    for _ in range(NTRIES):
        output = subprocess.check_output(args)
        output = output.decode('utf-8')
        match = re.search('Compressed ([0-9]+) => ([0-9]+) bytes', output)
        usize = int(match.group(1))
        csize = int(match.group(2))
        match = re.search('Compression time: [0-9]+ ms \\(([0-9]+) MB/s\\)',
                          output)
        cspeed = max(cspeed, int(match.group(1)))
    return (usize, csize, cspeed)

def benchmark_file_level(writer, path, level):
    (usize, csize, cspeed) = run_benchmark_prog(path, level, False)
    (libz_usize, libz_csize, libz_cspeed) = run_benchmark_prog(path, level, True)
    assert usize == libz_usize
    abs_comp_ratio = csize / usize
    rel_comp_ratio = csize / libz_csize
    rel_comp_time = libz_cspeed / cspeed
    writer.writerow({'file': os.path.basename(path),
                     'level': f'{level}',
                     'abs_comp_ratio': f'{abs_comp_ratio}',
                     'rel_comp_ratio': f'{rel_comp_ratio}',
                     'rel_comp_time': f'{rel_comp_time}'})

def run(corpus_dir):
    fieldnames = ['file', 'level', 'abs_comp_ratio',
                  'rel_comp_ratio', 'rel_comp_time']
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    for filename in os.listdir(corpus_dir):
        path = os.path.join(corpus_dir, filename)
        for level in range(1, 10):
            benchmark_file_level(writer, path, level)

def avg_rel_comp_ratio(rows):
    v = statistics.mean([row['rel_comp_ratio'] for row in rows])
    return f'{v:.6}'

def avg_rel_comp_time(rows):
    v = statistics.mean([row['rel_comp_time'] for row in rows])
    return f'{v:.6}'

def worse_comp_ratio(rows):
    v = 1 + len([row for row in rows if row['rel_comp_ratio'] > 1]) / len(rows)
    return f'{v:.6}'

def worse_comp_time(rows):
    v = 1 + len([row for row in rows if row['rel_comp_time'] > 1]) / len(rows)
    return f'{v:.6}'

def analyze(benchmark_results_csv):
    all_rows = []
    rows_by_file = {}
    rows_by_level = {}
    with open(benchmark_results_csv) as f:
        reader = csv.DictReader(f)
        for row in reader:
            filename = row['file']
            level = row['level']
            for field in ['abs_comp_ratio', 'rel_comp_ratio', 'rel_comp_time']:
                row[field] = float(row[field])
            all_rows.append(row)
            rows_by_file.setdefault(filename, []).append(row)
            rows_by_level.setdefault(level, []).append(row)

    writer = csv.writer(sys.stdout)
    writer.writerow(['name', 'value'])

    writer.writerow(['all_avg_rel_comp_ratio', avg_rel_comp_ratio(all_rows)])
    writer.writerow(['all_avg_rel_comp_time', avg_rel_comp_time(all_rows)])

    writer.writerow(['all_worse_comp_ratio', worse_comp_ratio(all_rows)])
    writer.writerow(['all_worse_comp_time', worse_comp_time(all_rows)])

    for (filename, file_rows) in sorted(rows_by_file.items()):
        writer.writerow([f'file_{filename}_avg_rel_comp_ratio',
                        avg_rel_comp_ratio(file_rows)])
    for (filename, file_rows) in sorted(rows_by_file.items()):
        writer.writerow([f'file_{filename}_avg_rel_comp_time',
                        avg_rel_comp_time(file_rows)])

    for (filename, file_rows) in sorted(rows_by_file.items()):
        writer.writerow([f'file_{filename}_worse_comp_ratio',
                        worse_comp_ratio(file_rows)])
    for (filename, file_rows) in sorted(rows_by_file.items()):
        writer.writerow([f'file_{filename}_worse_comp_time',
                        worse_comp_time(file_rows)])

    for (level, level_rows) in sorted(rows_by_level.items()):
        writer.writerow([f'level{level}_avg_rel_comp_ratio',
                        avg_rel_comp_ratio(level_rows)])
    for (level, level_rows) in sorted(rows_by_level.items()):
        writer.writerow([f'level{level}_avg_rel_comp_time',
                        avg_rel_comp_time(level_rows)])

    for (level, level_rows) in sorted(rows_by_level.items()):
        writer.writerow([f'level{level}_worse_comp_ratio',
                        worse_comp_ratio(level_rows)])
    for (level, level_rows) in sorted(rows_by_level.items()):
        writer.writerow([f'level{level}_worse_comp_time',
                        worse_comp_time(level_rows)])

def load_summary(summary_csv):
    summary = {}
    with open(summary_csv) as f:
        reader = csv.DictReader(f)
        for row in reader:
            name = row['name']
            value = float(row['value'])
            summary[name] = value

    return summary


def compare(summary_1_csv, summary_2_csv):
    summary_1 = load_summary(summary_1_csv)
    summary_2 = load_summary(summary_2_csv)

    names = set(summary_1.keys()) & set(summary_2.keys())
    comparisons = {}
    for name in names:
        val1 = summary_1[name]
        val2 = summary_2[name]
        comparisons[name] = (val1, val2, val2 / val1)

    for (k, v) in sorted(comparisons.items(), key=lambda item: item[1][2]):
        print(f'{k}: {v[0]} => {v[1]} ({v[2]})')

args = sys.argv[1:]
if len(args) < 1:
    usage()

if args[0] == 'run':
    if len(args) != 2:
        usage()
    run(args[1])
elif args[0] == 'analyze':
    if len(args) != 2:
        usage()
    analyze(args[1])
elif args[0] == 'compare':
    if len(args) != 3:
        usage()
    compare(args[1], args[2])
else:
    usage()
