#!/usr/bin/env python3
"""Analyze captured PBM frame for timing issues"""

def analyze_pbm(filename):
    with open(filename, 'r') as f:
        lines = f.readlines()

    # Parse header
    if lines[0].strip() != 'P1':
        print("ERROR: Not a P1 PBM file")
        return

    # Skip comment line
    i = 1
    while lines[i].startswith('#'):
        i += 1

    # Get dimensions
    dims = lines[i].strip().split()
    width, height = int(dims[0]), int(dims[1])
    print(f"Image: {width}x{height}")
    print()

    # Analyze pixel data
    pixel_lines = lines[i+1:]

    total_ones = 0
    total_pixels = 0
    line_stats = []

    for line_no, line in enumerate(pixel_lines[:height]):
        pixels = line.strip().split()
        ones = pixels.count('1')
        zeros = pixels.count('0')
        total = len(pixels)

        total_ones += ones
        total_pixels += total
        line_stats.append((line_no, ones, total))

    print(f"Total pixels: {total_pixels}")
    print(f"White pixels (1s): {total_ones} ({100*total_ones/total_pixels:.1f}%)")
    print(f"Black pixels (0s): {total_pixels - total_ones} ({100*(total_pixels-total_ones)/total_pixels:.1f}%)")
    print()

    # Find first and last lines with content
    first_content = None
    last_content = None
    for line_no, ones, total in line_stats:
        if ones > 0:
            if first_content is None:
                first_content = line_no
            last_content = line_no

    print(f"First line with content: {first_content}")
    print(f"Last line with content: {last_content}")
    print()

    # Show lines with most content
    print("Lines with most white pixels:")
    sorted_lines = sorted(line_stats, key=lambda x: x[1], reverse=True)
    for line_no, ones, total in sorted_lines[:10]:
        print(f"  Line {line_no}: {ones}/{total} pixels ({100*ones/total:.1f}%)")

    # Check for line length consistency
    print()
    print("Checking for glitches...")
    for line_no, ones, total in line_stats:
        if total != width:
            print(f"  WARNING: Line {line_no} has {total} pixels (expected {width})")

if __name__ == "__main__":
    analyze_pbm("frame.pbm")
