"""
Generate a 4x4 colour BMP and dump its matrix representation, cv::Mat
attributes, and raw byte layout into matrix_dump.txt so that beginners can
study them side by side.

Run:
    python generate_demo.py
Outputs:
    color_4x4.bmp     -- the true 4x4 colour image (16 colour squares once enlarged)
    color_4x4_x40.bmp -- the same image scaled up 40x for easier viewing
    matrix_dump.txt   -- matrix values + attributes + raw byte layout
"""
import os
import numpy as np
import cv2

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------
# 1) Build a 4x4 colour image by hand.
#    Note: OpenCV / NumPy store colour images in BGR order.
#    Each pixel is the three uint8 values [B, G, R].
# ---------------------------------------------------------------
# Colour definitions (BGR order).
RED   = [  0,   0, 255]
GREEN = [  0, 255,   0]
BLUE  = [255,   0,   0]
WHITE = [255, 255, 255]
BLACK = [  0,   0,   0]
YELLOW= [  0, 255, 255]   # R+G
CYAN  = [255, 255,   0]   # G+B
PINK  = [180, 105, 255]   # pink

# 4 rows x 4 cols, each cell holds one [B, G, R].
img = np.array([
    [RED,    GREEN,  BLUE,   WHITE ],   # row 0
    [BLACK,  YELLOW, CYAN,   PINK  ],   # row 1
    [RED,    RED,    GREEN,  GREEN ],   # row 2
    [BLUE,   BLUE,   WHITE,  BLACK ],   # row 3
], dtype=np.uint8)

print("img.shape =", img.shape)       # (4, 4, 3)
print("img.dtype =", img.dtype)       # uint8

# ---------------------------------------------------------------
# 2) Write the original 4x4 image and a 40x enlarged version (160x160).
# ---------------------------------------------------------------
cv2.imwrite(os.path.join(OUT_DIR, "color_4x4.bmp"), img)
big = cv2.resize(img, (160, 160), interpolation=cv2.INTER_NEAREST)
cv2.imwrite(os.path.join(OUT_DIR, "color_4x4_x40.bmp"), big)


def save_big(name, mat):
    """Scale 40x and save for clearer viewing."""
    out = cv2.resize(mat, (160, 160), interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(os.path.join(OUT_DIR, name), out)


# ---------------------------------------------------------------
# 2.1) Save each of the B / G / R channels as a "single-channel matrix".
#      -> Stored this way they always display as greyscale.
#      (An image viewer treats an H x W x 1 matrix as brightness: larger
#      numbers display brighter, smaller numbers darker.)
# ---------------------------------------------------------------
B, G, R = cv2.split(img)         # three 4x4 single-channel matrices (shape = 4x4)
save_big("channel_B_gray.bmp", B)
save_big("channel_G_gray.bmp", G)
save_big("channel_R_gray.bmp", R)

# ---------------------------------------------------------------
# 2.2) Place a single channel back into a 3-channel colour matrix with the
#      other two channels zeroed.
#      -> This is what pure R / pure G / pure B actually look like.
#      Used to show: "same numbers, different interpretation -> very
#      different colour on screen".
# ---------------------------------------------------------------
zero = np.zeros_like(B)

# OpenCV uses BGR order; channel 0 is B when merging back.
img_only_B = cv2.merge([B,    zero, zero])   # only B kept -> displays as blue
img_only_G = cv2.merge([zero, G,    zero])   # only G kept -> displays as green
img_only_R = cv2.merge([zero, zero, R])      # only R kept -> displays as red

save_big("channel_B_color.bmp", img_only_B)
save_big("channel_G_color.bmp", img_only_G)
save_big("channel_R_color.bmp", img_only_R)

# ---------------------------------------------------------------
# 3) Dump the matrix / attributes / byte layout to a text file.
# ---------------------------------------------------------------
lines = []
lines.append("=" * 72)
lines.append("Complete internal representation of a 4x4 colour image (color_4x4.bmp)")
lines.append("=" * 72)

lines.append("\n[Part 1] cv::Mat / numpy attribute reference")
lines.append("-" * 72)
lines.append(f"  shape          = {img.shape}        # (rows, cols, channels)")
lines.append(f"  ndim (~ dims)  = {img.ndim - 1}                # geometric dims = 2; channels are not a geometric dim")
lines.append(f"  rows           = {img.shape[0]}                # height")
lines.append(f"  cols           = {img.shape[1]}                # width")
lines.append(f"  channels()     = {img.shape[2]}                # numbers stored per cell (B, G, R)")
lines.append(f"  dtype          = {img.dtype}            # each number is 1 byte, 0..255")
lines.append(f"  elemSize       = {img.shape[2]} bytes          # 3 bytes per pixel cell")
lines.append(f"  total bytes    = {img.nbytes} bytes        # 4 * 4 * 3 = 48")

lines.append("\n[Part 2] Matrix view: each cell is a [B, G, R] triple")
lines.append("-" * 72)
lines.append("                col 0            col 1            col 2            col 3")
header = "      " + "-" * 16 + "+" + "-" * 16 + "+" + "-" * 16 + "+" + "-" * 16
lines.append("      +" + header[6:] + "+")
for r in range(4):
    row_cells = []
    for c in range(4):
        b, g, rr = img[r, c]
        row_cells.append(f"[{b:3d},{g:3d},{rr:3d}]")
    lines.append(f"row{r}  | " + "   | ".join(row_cells) + "   |")
    if r < 3:
        sep = "      +" + "-" * 16 + "+" + "-" * 16 + "+" + "-" * 16 + "+" + "-" * 16 + "+"
        lines.append(sep)
lines.append("      +" + header[6:] + "+")

lines.append("\n  Geometrically a 4-row x 4-column 2D matrix (dims/ndim = 2 geometrically).")
lines.append("  But each cell holds not a single number but [B, G, R] three uint8 values -> channels() = 3.")

lines.append("\n[Part 3] Per-channel view: split BGR into 3 greyscale matrices")
lines.append("-" * 72)
for ci, cname in enumerate(["B (blue channel)", "G (green channel)", "R (red channel)"]):
    lines.append(f"\n  {cname}:")
    for r in range(4):
        row = "    " + "  ".join(f"{img[r, c, ci]:3d}" for c in range(4))
        lines.append(row)

lines.append('\n  These three 4x4 "greyscale matrices" stacked together = the original colour image.')
lines.append('  Mathematically equivalent: "3 numbers per cell" = "3 matrices stacked".')

lines.append("\n[Part 4] How it is actually stored in memory (1D contiguous byte stream, HWC layout)")
lines.append("-" * 72)
lines.append("  addr  : byte value   pixel position   channel")
flat = img.flatten()
for addr, v in enumerate(flat):
    r = addr // (4 * 3)
    c = (addr % (4 * 3)) // 3
    ch = "BGR"[addr % 3]
    lines.append(f"  {addr:3d}   :  {v:3d}        row {r} col {c}        {ch}")
    if addr == 11:
        lines.append("  ... (same pattern: the 3 BGR bytes of one pixel are adjacent; the next pixel follows immediately)")
        # Skip the middle, show only the first 12 entries.
        break
lines.append(f"  {img.nbytes} bytes total (4 rows x 4 cols x 3 channels = 48)")

lines.append("\n[Part 5] Conclusion")
lines.append("-" * 72)
lines.append('  1) A colour image = a matrix with "geometric dims = 2 + 3 numbers per cell";')
lines.append("     equivalent to three greyscale matrices of the same size stacked together.")
lines.append('  2) cv::Mat::dims = 2 describes the two geometric dimensions (rows/cols);')
lines.append('     the channel count (3) is encoded in the element type (CV_8UC3 / Vec3b) and is not a geometric dim.')
lines.append('  3) Storing a vector per cell is mathematically valid -- this is called')
lines.append('     "matrix elements are vectors/tensors", equivalent to an H x W x C 3D array;')
lines.append("     OpenCV simply chose to fold the innermost C into the element type.")


# ---------------------------------------------------------------
# [Part 6] Six derived images: matrix / attributes / memory layout
# ---------------------------------------------------------------
def dump_mat(title, mat, note):
    """Append the attributes, numerical matrix and raw bytes of a cv::Mat / numpy matrix to `lines`."""
    lines.append("\n" + "=" * 72)
    lines.append(title)
    lines.append("=" * 72)
    lines.append(note)

    is_color = (mat.ndim == 3)
    rows = mat.shape[0]
    cols = mat.shape[1]
    chs = mat.shape[2] if is_color else 1
    cv_type = {
        (1, np.dtype("uint8")): "CV_8UC1 (single channel, 1 byte per cell)",
        (3, np.dtype("uint8")): "CV_8UC3 (three channels, 3 bytes per cell, BGR)",
    }[(chs, mat.dtype)]

    lines.append("\n  -- attributes --")
    lines.append(f"    shape       = {mat.shape}")
    lines.append(f"    ndim (geom) = 2                # rows x cols")
    lines.append(f"    rows        = {rows}")
    lines.append(f"    cols        = {cols}")
    lines.append(f"    channels()  = {chs}")
    lines.append(f"    dtype       = {mat.dtype}")
    lines.append(f"    type()      = {cv_type}")
    lines.append(f"    elemSize    = {chs} bytes")
    lines.append(f"    total bytes = {mat.nbytes}")

    lines.append("\n  -- numerical matrix (number in each cell) --")
    if is_color:
        for r in range(rows):
            cells = []
            for c in range(cols):
                b, g, rr = mat[r, c]
                cells.append(f"[{b:3d},{g:3d},{rr:3d}]")
            lines.append(f"    row {r}: " + "  ".join(cells))
    else:
        for r in range(rows):
            lines.append("    row %d: " % r + "  ".join(f"{mat[r, c]:3d}" for c in range(cols)))

    lines.append("\n  -- byte stream in memory (HWC contiguous layout) --")
    flat = mat.flatten()
    for addr, v in enumerate(flat):
        if is_color:
            pr = addr // (cols * chs)
            pc = (addr % (cols * chs)) // chs
            ch_name = "BGR"[addr % chs]
            lines.append(f"    addr {addr:3d}: value={v:3d}   pixel(row {pr}, col {pc}) channel {ch_name}")
        else:
            pr = addr // cols
            pc = addr % cols
            lines.append(f"    addr {addr:3d}: value={v:3d}   pixel(row {pr}, col {pc})")
    lines.append(f"    {mat.nbytes} bytes total")


dump_mat(
    "[Part 6-1] channel_B_gray.bmp -- single-channel B matrix (4x4x1)",
    B,
    "  Description: the B channel produced by cv::split, saved as a 1-channel greyscale image.\n"
    "               On screen the image viewer treats the values as brightness;\n"
    "               the red/green/blue sub-pixels light up equally with that brightness\n"
    "               -> R = G = B -> necessarily black/grey/white.",
)
dump_mat(
    "[Part 6-2] channel_G_gray.bmp -- single-channel G matrix (4x4x1)",
    G,
    "  Description: same as above but with the green channel values. Still displays as greyscale.",
)
dump_mat(
    "[Part 6-3] channel_R_gray.bmp -- single-channel R matrix (4x4x1)",
    R,
    "  Description: same as above but with the red channel values. Still displays as greyscale.",
)
dump_mat(
    "[Part 6-4] channel_B_color.bmp -- 3 channels (B kept, G/R all zero)",
    img_only_B,
    "  Description: the B channel values are identical to channel_B_gray.bmp, but here the matrix has 3 channels.\n"
    "               Only the blue sub-pixel lights up at brightness B; the red and green are off -> displays as pure blue.\n"
    "               Same B numbers, different channel interpretation, completely different colours.",
)
dump_mat(
    "[Part 6-5] channel_G_color.bmp -- 3 channels (G kept, B/R all zero)",
    img_only_G,
    "  Description: only the green sub-pixel lights up -> displays as pure green.",
)
dump_mat(
    "[Part 6-6] channel_R_color.bmp -- 3 channels (R kept, B/G all zero)",
    img_only_R,
    "  Description: only the red sub-pixel lights up -> displays as pure red.",
)


lines.append("\n" + "=" * 72)
lines.append("[Summary] single channel vs. three channels: identical numbers -> completely different display")
lines.append("=" * 72)
lines.append("  channel_B_gray.bmp  shape 4x4x1  -> displays as greyscale")
lines.append("  channel_B_color.bmp shape 4x4x3  -> displays as pure blue")
lines.append("  The 16 B-channel numbers are identical in both files; the only difference is the shape/channel count.")
lines.append("  -> Colour is not determined by the data alone; it is decided by")
lines.append("     'data + channel interpretation + screen sub-pixel emission rules' together.")


txt_path = os.path.join(OUT_DIR, "matrix_dump.txt")
with open(txt_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print(f"\nDone. See:")
print(f"  {txt_path}")
print(f"  {os.path.join(OUT_DIR, 'color_4x4.bmp')}        (original 4x4)")
print(f"  {os.path.join(OUT_DIR, 'color_4x4_x40.bmp')}    (scaled up 40x for easier viewing)")
