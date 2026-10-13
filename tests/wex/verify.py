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
        print("Usage: verify.py <check-rect|check-alpha|check-pixel|compare-images> <img_path> [args...]")
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

