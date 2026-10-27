#!/usr/bin/env python3
"""Small host-side geometry sanity test for the documented rotation mapping.

This is a coordinate-contract test, not a replacement for visual camera testing.
"""

def rotate_out_to_source(src, angle):
    h, w = len(src), len(src[0])
    ow, oh = (h, w) if angle in (90, 270) else (w, h)
    out = [[None for _ in range(ow)] for _ in range(oh)]
    for oy in range(oh):
        for ox in range(ow):
            if angle == 0:
                sx, sy = ox, oy
            elif angle == 90:
                sx, sy = oy, h - 1 - ox
            elif angle == 180:
                sx, sy = w - 1 - ox, h - 1 - oy
            elif angle == 270:
                sx, sy = w - 1 - oy, ox
            else:
                raise ValueError(angle)
            out[oy][ox] = src[sy][sx]
    return out


def main():
    source = [["A", "B", "C", "D"], ["E", "F", "G", "H"]]
    expected = {
        0: [["A", "B", "C", "D"], ["E", "F", "G", "H"]],
        90: [["E", "A"], ["F", "B"], ["G", "C"], ["H", "D"]],
        180: [["H", "G", "F", "E"], ["D", "C", "B", "A"]],
        270: [["D", "H"], ["C", "G"], ["B", "F"], ["A", "E"]],
    }
    for angle, want in expected.items():
        got = rotate_out_to_source(source, angle)
        assert got == want, f"{angle} degree transform failed: {got!r}"
        print(f"{angle:3d}°: OK; output {len(got[0])}×{len(got)}")
    print("Rotation coordinate contract OK")


if __name__ == "__main__":
    main()
