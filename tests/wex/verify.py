#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import sys
from PIL import Image

def hex_to_rgb(hex_str):
    hex_str = hex_str.lstrip('#')
    if hex_str.startswith('0x') or hex_str.startswith('0X'):
        hex_str = hex_str[2:]
    if len(hex_str) == 8: # ARGB or RGBA, take RGB
        hex_str = hex_str[2:]
    return tuple(int(hex_str[i:i+2], 16) for i in (0, 2, 4))

def color_dist(c1, c2):
    return max(abs(c1[0] - c2[0]), abs(c1[1] - c2[1]), abs(c1[2] - c2[2]))

def check_rect(image_path, x, y, w, h, expected_hex, tolerance=5):
    img = Image.open(image_path).convert('RGB')
    expected = hex_to_rgb(expected_hex)
    
    # Check interior sample points
    sample_points = [
        (x + 5, y + 5),
        (x + w - 5, y + 5),
        (x + 5, y + h - 5),
        (x + w - 5, y + h - 5),
        (x + w // 2, y + h // 2),
    ]
    
    for px, py in sample_points:
        if px < 0 or px >= img.width or py < 0 or py >= img.height:
            print(f"FAIL: Sample point ({px}, {py}) is outside image bounds {img.size}")
            return False
        c = img.getpixel((px, py))
        if color_dist(c, expected) > tolerance:
            print(f"FAIL: Inside rect ({x},{y},{w},{h}) at ({px},{py}): expected {expected}, got {c} (dist={color_dist(c, expected)})")
            return False

    # Check exterior points just outside boundary (should differ)
    exterior_points = []
    if x - 2 >= 0:
        exterior_points.append((x - 2, y + h // 2))
    if y - 2 >= 0:
        exterior_points.append((x + w // 2, y - 2))
    if x + w + 2 < img.width:
        exterior_points.append((x + w + 2, y + h // 2))
    if y + h + 2 < img.height:
        exterior_points.append((x + w // 2, y + h + 2))
        
    for px, py in exterior_points:
        c = img.getpixel((px, py))
        if color_dist(c, expected) <= tolerance:
            print(f"FAIL: Exterior point ({px},{py}) unexpectedly matched rect color {expected} (got {c})")
            return False

    print(f"PASS: Rect ({x},{y},{w},{h}) matches {expected} (tolerance={tolerance})")
    return True

def check_alpha(image_path, x, y, w, h, exp_r, exp_g, exp_b, tolerance=8):
    img = Image.open(image_path).convert('RGB')
    expected = (int(exp_r), int(exp_g), int(exp_b))
    
    sample_points = [
        (x + 50, y + 50),
        (x + w - 50, y + 50),
        (x + 50, y + h - 50),
        (x + w // 2, y + h // 2),
    ]
    for px, py in sample_points:
        c = img.getpixel((px, py))
        d = color_dist(c, expected)
        if d > tolerance:
            print(f"FAIL: Alpha rect at ({px},{py}): expected {expected}, got {c} (dist={d} > {tolerance})")
            return False
            
    print(f"PASS: Alpha rect at ({x},{y},{w},{h}) matches {expected} within tolerance {tolerance}")
    return True

def check_pixel(image_path, x, y, expected_hex, tolerance=5):
    img = Image.open(image_path).convert('RGB')
    expected = hex_to_rgb(expected_hex)
    c = img.getpixel((x, y))
    d = color_dist(c, expected)
    if d > tolerance:
        print(f"FAIL: Pixel at ({x},{y}): expected {expected}, got {c} (dist={d})")
        return False
    print(f"PASS: Pixel at ({x},{y}) matches {expected} (got {c})")
    return True

def check_thumb_subsurface(image_path, green_x, green_y, red_x, red_y, tolerance=5):
    # Multi-surface thumbnail: the green subsurface centre must be green and a
    # red pixel outside the subsurface but inside the thumbnail must be red.
    img = Image.open(image_path).convert('RGB')
    green = hex_to_rgb('00FF00')
    red = hex_to_rgb('FF0000')

    gc = img.getpixel((green_x, green_y))
    gd = color_dist(gc, green)
    if gd > tolerance:
        print(f"FAIL: Subsurface centre ({green_x},{green_y}): expected green {green}, got {gc} (dist={gd})")
        return False

    rc = img.getpixel((red_x, red_y))
    rd = color_dist(rc, red)
    if rd > tolerance:
        print(f"FAIL: Red pixel outside subsurface ({red_x},{red_y}): expected red {red}, got {rc} (dist={rd})")
        return False

    print(f"PASS: Subsurface centre ({green_x},{green_y}) green and red pixel ({red_x},{red_y}) red (tolerance={tolerance})")
    return True

def check_range(image_path, x, y, min_val, max_val, channel=None):
    img = Image.open(image_path).convert('RGB')
    if x < 0 or x >= img.width or y < 0 or y >= img.height:
        print(f"FAIL: Pixel ({x}, {y}) outside image bounds {img.size}")
        return False
    c = img.getpixel((x, y))
    vals = [c[channel]] if channel is not None else [c[0], c[1], c[2]]
    for v in vals:
        if v < min_val or v > max_val:
            print(f"FAIL: Pixel at ({x},{y}) value {c} out of range [{min_val}, {max_val}]")
            return False
    print(f"PASS: Pixel at ({x},{y}) value {c} in range [{min_val}, {max_val}]")
    return True

def check_average(image_path, x, y, w, h, min_val, max_val, channel=None):
    img = Image.open(image_path).convert('RGB')
    if x < 0 or y < 0 or x + w > img.width or y + h > img.height:
        print(f"FAIL: Rect ({x}, {y}, {w}, {h}) exceeds image bounds {img.size}")
        return False
    total = [0.0, 0.0, 0.0]
    count = w * h
    for py in range(y, y + h):
        for px in range(x, x + w):
            c = img.getpixel((px, py))
            total[0] += c[0]
            total[1] += c[1]
            total[2] += c[2]
    avg = [total[0] / count, total[1] / count, total[2] / count]
    vals = [avg[channel]] if channel is not None else avg
    for v in vals:
        if v < min_val or v > max_val:
            print(f"FAIL: Rect ({x},{y},{w},{h}) average {avg} out of range [{min_val}, {max_val}]")
            return False
    print(f"PASS: Rect ({x},{y},{w},{h}) average {avg} in range [{min_val}, {max_val}]")
    return True

def compare_images(image1_path, image2_path, tolerance=0):
    img1 = Image.open(image1_path).convert('RGB')
    img2 = Image.open(image2_path).convert('RGB')
    if img1.size != img2.size:
        print(f"FAIL: Image sizes differ: {img1.size} vs {img2.size}")
        return False
    diff_count = 0
    for y in range(img1.height):
        for x in range(img1.width):
            c1 = img1.getpixel((x, y))
            c2 = img2.getpixel((x, y))
            if color_dist(c1, c2) > tolerance:
                diff_count += 1
    if diff_count > 0:
        print(f"FAIL: Images differ ({diff_count} pixels differ with tol={tolerance})")
        return False
    print(f"PASS: Images match completely (tol={tolerance})")
    return True

def main():
    if len(sys.argv) < 3:
        print("Usage: verify.py <check-rect|check-alpha|check-pixel|check-thumb-subsurface|check-range|compare-images> <img_path> [args...]")
        sys.exit(1)
        
    cmd = sys.argv[1]
    img = sys.argv[2]
    
    if cmd == "check-rect":
        x = int(sys.argv[3])
        y = int(sys.argv[4])
        w = int(sys.argv[5])
        h = int(sys.argv[6])
        col = sys.argv[7]
        tol = int(sys.argv[8]) if len(sys.argv) > 8 else 5
        res = check_rect(img, x, y, w, h, col, tol)
    elif cmd == "check-alpha":
        x = int(sys.argv[3])
        y = int(sys.argv[4])
        w = int(sys.argv[5])
        h = int(sys.argv[6])
        r = int(sys.argv[7])
        g = int(sys.argv[8])
        b = int(sys.argv[9])
        tol = int(sys.argv[10]) if len(sys.argv) > 10 else 8
        res = check_alpha(img, x, y, w, h, r, g, b, tol)
    elif cmd == "check-pixel":
        x = int(sys.argv[3])
        y = int(sys.argv[4])
        col = sys.argv[5]
        tol = int(sys.argv[6]) if len(sys.argv) > 6 else 5
        res = check_pixel(img, x, y, col, tol)
    elif cmd == "check-thumb-subsurface":
        green_x = int(sys.argv[3])
        green_y = int(sys.argv[4])
        red_x = int(sys.argv[5])
        red_y = int(sys.argv[6])
        tol = int(sys.argv[7]) if len(sys.argv) > 7 else 5
        res = check_thumb_subsurface(img, green_x, green_y, red_x, red_y, tol)
    elif cmd == "check-range":
        x = int(sys.argv[3])
        y = int(sys.argv[4])
        min_val = int(sys.argv[5])
        max_val = int(sys.argv[6])
        channel = int(sys.argv[7]) if len(sys.argv) > 7 else None
        res = check_range(img, x, y, min_val, max_val, channel)
    elif cmd in ("check-average", "average"):
        x = int(sys.argv[3])
        y = int(sys.argv[4])
        w = int(sys.argv[5])
        h = int(sys.argv[6])
        min_val = float(sys.argv[7])
        max_val = float(sys.argv[8])
        channel = int(sys.argv[9]) if len(sys.argv) > 9 else None
        res = check_average(img, x, y, w, h, min_val, max_val, channel)
    elif cmd == "compare-images":
        img2 = sys.argv[3]
        tol = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        res = compare_images(img, img2, tol)
    else:
        print(f"Unknown command {cmd}")
        sys.exit(1)
        
    sys.exit(0 if res else 1)

if __name__ == '__main__':
    main()

