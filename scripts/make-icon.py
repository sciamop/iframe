#!/usr/bin/env python3
"""Generates the iFrame icon: a chamfered, all-45-degree cursor pointing up and to the right.

Style follows the Pinstle mark: one flat color, every edge at 0/45/90 degrees, corners clipped.
Outputs Design/icon.svg; scripts/build-icons.sh renders every app icon from it.
"""
import math, os, sys

S = 1024
ACCENT = "#2EE6D6"                        # glyph
BG = ("#0A3A3F", "#05202A", "#020B0F")    # background radial gradient: center, middle, edge
r2 = math.sqrt(2)
D = (1 / r2, 1 / r2)    # cursor axis: tip (top-left) -> tail (bottom-right)
N = (1 / r2, -1 / r2)   # perpendicular, toward top-right

def add(p, v, k=1.0): return (p[0] + v[0] * k, p[1] + v[1] * k)
def poly(points, **attrs):
    a = " ".join(f'{k.replace("_", "-")}="{v}"' for k, v in attrs.items())
    return f'<polygon points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in points)}" {a}/>'

def cursor(tip, leg, stem_w, stem_len, wing_cut, tail_cut):
    """Right-isosceles arrowhead (legs along the axes) plus a stem along the diagonal."""
    tx, ty = tip
    a = (tx, ty + leg)            # bottom wing
    b = (tx + leg, ty)            # right wing
    mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    h = stem_w / 2
    s_lo, s_hi = add(mid, N, -h), add(mid, N, h)          # stem meets hypotenuse
    e_lo, e_hi = add(s_lo, D, stem_len), add(s_hi, D, stem_len)
    c = wing_cut
    k = tail_cut
    return [
        tip,
        (a[0], a[1] - c), (a[0] + c, a[1] - c),            # bottom wing: flat horizontal cut
        s_lo,
        (e_lo[0] - k, e_lo[1] - k), (e_lo[0] + k, e_lo[1] - k),   # tail: horizontal cut
        (e_hi[0] - k, e_hi[1] + k), (e_hi[0] - k, e_hi[1] - k),   # tail: vertical cut
        s_hi,
        (b[0] - c, b[1] + c), (b[0] - c, b[1]),            # right wing: flat vertical cut
    ]

FILL = 0.58   # fraction of the canvas the cursor's larger dimension spans

def build(dark=True):
    body = cursor((0, 0), 452, stem_w=150, stem_len=300, wing_cut=64, tail_cut=40)
    # Point up and to the right: mirror horizontally, then scale and center on the canvas
    # (nudged slightly so the visual mass, which sits toward the tip, reads as centered).
    body = [(-x, y) for x, y in body]
    xs, ys = [p[0] for p in body], [p[1] for p in body]
    k = FILL * S / max(max(xs) - min(xs), max(ys) - min(ys))
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    body = [((x - cx) * k + S / 2 - 6, (y - cy) * k + S / 2 + 6) for x, y in body]

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {S} {S}">',
           '<defs>',
           '<radialGradient id="bg" cx="62%" cy="32%" r="85%">'
           f'<stop offset="0" stop-color="{BG[0]}"/><stop offset="0.55" stop-color="{BG[1]}"/>'
           f'<stop offset="1" stop-color="{BG[2]}"/></radialGradient>',
           '<linearGradient id="sheen" x1="1" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.24"/>'
           '<stop offset="0.45" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>',
           '</defs>']
    if dark:
        out.append(f'<rect width="{S}" height="{S}" fill="url(#bg)"/>')
    out.append(poly(body, fill=ACCENT))
    out.append(poly(body, fill="url(#sheen)"))
    out.append('</svg>')
    return "\n".join(out)

if __name__ == "__main__":
    root = os.path.join(os.path.dirname(__file__), "..", "Design")
    os.makedirs(root, exist_ok=True)
    open(os.path.join(root, "icon.svg"), "w").write(build(dark=True))
    open(os.path.join(root, "icon-transparent.svg"), "w").write(build(dark=False))
    print("wrote Design/icon.svg, Design/icon-transparent.svg")
