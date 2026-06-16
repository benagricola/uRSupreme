# Generates: (1) a zoomed icon sheet PNG, (2) messenger page mockup
# PNGs at 4x, (3) /tmp/oled_messenger_screen_design.md with the same
# glyphs emitted as GFX drawBitmap-ready byte arrays. The pixel
# strings below ARE the proposal - 8x8, 1 bit, no antialiasing.
from PIL import Image, ImageDraw

GLYPHS = {
    # name: 8 rows of 8 chars, '#' = lit
    "envelope": [
        "########",
        "#......#",
        "##....##",
        "#.#..#.#",
        "#..##..#",
        "#......#",
        "########",
        "........",
    ],
    "inbox": [  # incoming message: down-arrow into tray
        "...##...",
        "...##...",
        ".######.",
        "..####..",
        "...##...",
        "#......#",
        "#......#",
        "########",
    ],
    "send": [  # outbox: up-arrow out of the tray (inbox mirrored)
        "...##...",
        "..####..",
        ".######.",
        "...##...",
        "...##...",
        "#......#",
        "#......#",
        "########",
    ],
    "pin": [  # map pin: round head, hole, tapered point (centred)
        "........",
        "..####..",
        ".######.",
        ".##..##.",
        ".######.",
        "..####..",
        "...##...",
        "...##...",
    ],
    "compass": [  # 4-point compass rose (centred)
        "...##...",
        "...##...",
        "..####..",
        "########",
        "########",
        "..####..",
        "...##...",
        "...##...",
    ],
    "thermo": [
        "..##....",
        ".#..#...",
        ".#.##...",
        ".#.##...",
        ".#..#...",
        "#....#..",
        "#.##.#..",
        ".####...",
    ],
    "battery": [
        "........",
        ".######.",
        ".#....#.",
        ".#.##.##",
        ".#.##.##",
        ".#....#.",
        ".######.",
        "........",
    ],
    "live": [  # refresh arrow: open circle + arrowhead
        "..###...",
        ".#...#..",
        "#.....##",
        "#....###",
        "#......#",
        ".#...#..",
        "..###...",
        "........",
    ],
    "clock": [
        ".#####..",
        "#..#..#.",
        "#..#..#.",
        "#..####.",
        "#.....#.",
        "#.....#.",
        ".#####..",
        "........",
    ],
    "check": [
        "........",
        ".......#",
        "......#.",
        ".....#..",
        "#...#...",
        ".#.#....",
        "..#.....",
        "........",
    ],
    "cross": [
        "........",
        ".#....#.",
        "..#..#..",
        "...##...",
        "...##...",
        "..#..#..",
        ".#....#.",
        "........",
    ],
    "person": [
        "...##...",
        "..####..",
        "..####..",
        "...##...",
        "..####..",
        ".######.",
        "########",
        "........",
    ],
    "cursor": [  # list cursor, filled triangle
        "#.......",
        "##......",
        "###.....",
        "####....",
        "###.....",
        "##......",
        "#.......",
        "........",
    ],
    "house": [  # home: the status page
        "...##...",
        "..####..",
        ".######.",
        "########",
        ".#....#.",
        ".#.##.#.",
        ".#.##.#.",
        ".######.",
    ],
    "antenna": [  # broadcast mast with waves: the radio page
        "...##...",
        ".#.##.#.",
        "#..##..#",
        "...##...",
        "...##...",
        "...##...",
        "..####..",
        ".######.",
    ],
}


# ---------- spinner: rotating-arc frames ----------
# 8 segments clockwise; each frame blanks two adjacent ones (a quarter
# gap). Animate at ~4 Hz: frame = (millis() / 250) & 3.
_SEGS = {
    "T":  [(2, 0), (3, 0), (4, 0)],
    "TR": [(5, 1)],
    "R":  [(6, 2), (6, 3), (6, 4)],
    "BR": [(5, 5)],
    "B":  [(2, 6), (3, 6), (4, 6)],
    "BL": [(1, 5)],
    "L":  [(0, 2), (0, 3), (0, 4)],
    "TL": [(1, 1)],
}
# Delivered = the check glyph, simply inverted pixel-for-pixel.
GLYPHS["check2"] = ["".join("#" if c == "." else "." for c in row)
                    for row in GLYPHS["check"]]

_ORDER = ["T", "TR", "R", "BR", "B", "BL", "L", "TL"]
def _spin_frame(gap_at):
    grid = [["."] * 8 for _ in range(8)]
    for i, name in enumerate(_ORDER):
        if name in (_ORDER[gap_at % 8], _ORDER[(gap_at + 1) % 8]):
            continue
        for (x, y) in _SEGS[name]:
            grid[y][x] = "#"
    return ["".join(r) for r in grid]
for f in range(4):
    GLYPHS[f"spin{f}"] = _spin_frame(f * 2)

WHITE = 255
BLACK = 0

def draw_glyph(img, gx, gy, rows, scale=1, color=WHITE):
    px = img.load()
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c == '#':
                for dy in range(scale):
                    for dx in range(scale):
                        px[gx + x * scale + dx, gy + y * scale + dy] = color

# ---------- 1. icon sheet ----------
names = list(GLYPHS.keys())
cols, cell, zoom = 5, 90, 8
rows_n = (len(names) + cols - 1) // cols
sheet = Image.new("L", (cols * cell, rows_n * cell + 10), 30)
d = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    cx, cy = (i % cols) * cell, (i // cols) * cell
    d.rectangle([cx + 8, cy + 8, cx + 8 + 8 * zoom + 1, cy + 8 + 8 * zoom + 1], outline=90)
    draw_glyph(sheet, cx + 9, cy + 9, GLYPHS[n], scale=zoom)
    draw_glyph(sheet, cx + 8 + 8 * zoom - 4, cy + 76, GLYPHS[n], scale=1)  # 1x actual
    d.text((cx + 8, cy + 74), n, fill=200)
sheet.save("/tmp/oled_shots/icon_sheet.png")

# ---------- 2. page mockups ----------
# Drawn at 2x panel resolution (128x256 for the 64x128 panel) so PIL's
# ~11 px default font stands in for the real 6x8/Picopixel fonts at
# panel scale. Glyphs draw at scale 2 = real 8x8.
PW, PH = 64, 128
S = 2
W, H = PW * S, PH * S

def panel():
    return Image.new("L", (W, H), BLACK)

def header(img, dr, glyph, title):
    # Titles are ALWAYS UPPERCASE, no punctuation. Glyph and text are
    # both vertically centred in the 11 px bar.
    dr.rectangle([0, 0, W - 1, 11 * S - 1], fill=WHITE)
    draw_glyph(img, 2 * S, 1 * S + 1, GLYPHS[glyph], scale=S, color=BLACK)
    dr.text((13 * S, 6), title.upper(), fill=BLACK)

def hline(dr, y):
    dr.line([(0, y * S), (W - 1, y * S)], fill=WHITE)

def hints(img, dr, lines):
    # Inverted like the header: white block, black text, with a 1 px
    # black gap above so it reads as a bar, not a bleed.
    top = PH - 7 * len(lines) - 3
    dr.rectangle([0, top * S, W - 1, H - 1], fill=WHITE)
    y = top + 2
    for l in lines:
        dr.text((2 * S, y * S), l, fill=BLACK)
        y += 7

def out(img, name):
    img.resize((W * 2, H * 2), Image.NEAREST).save(f"/tmp/oled_shots/{name}.png")

# LIST page
img = panel(); dr = ImageDraw.Draw(img)
header(img, dr, "envelope", "MESSAGES")
rows = [("cursor", "envelope", None,   "OK"),
        (None,     "envelope", "pin",  "Pickup"),
        (None,     "envelope", "live", "Live pos")]
y = 15
for cur, g1, g2, label in rows:
    if cur: draw_glyph(img, 1 * S, y * S, GLYPHS[cur], scale=S)
    draw_glyph(img, 9 * S, y * S, GLYPHS[g1], scale=S)
    if g2: draw_glyph(img, 18 * S, y * S, GLYPHS[g2], scale=S)
    dr.text((28 * S, y * S + 2), label, fill=WHITE)
    y += 11
hints(img, dr, ["Tap PWR: pick", "Tap BOOT: next", "Hold BOOT: back"])
out(img, "mock_list")

# CONFIRM page
img = panel(); dr = ImageDraw.Draw(img)
header(img, dr, "send", "SEND")
dr.text((2 * S, 13 * S), "Need pickup", fill=WHITE)
draw_glyph(img, 2 * S, 21 * S, GLYPHS["person"], scale=S)
dr.text((12 * S, 21 * S + 2), "33bd7c44", fill=WHITE)
hline(dr, 32)
dr.text((2 * S, 36 * S), "Please come", fill=WHITE)
dr.text((2 * S, 44 * S), "get me.", fill=WHITE)
draw_glyph(img, 2 * S, 58 * S, GLYPHS["pin"], scale=S)
draw_glyph(img, 13 * S, 58 * S, GLYPHS["battery"], scale=S)
draw_glyph(img, 24 * S, 58 * S, GLYPHS["live"], scale=S)
dr.text((34 * S, 58 * S + 2), "15m", fill=WHITE)
hints(img, dr, ["Hold PWR: send", "Hold BOOT: back"])
out(img, "mock_confirm")

# RESULT page (finding route -> delivered)
for status, line2, glyph, fname in [("Finding", "route", "clock", "mock_result_finding"),
                                    ("Delivered", None, "check2", "mock_result_delivered")]:
    img = panel(); dr = ImageDraw.Draw(img)
    header(img, dr, "send", "STATUS")
    draw_glyph(img, (PW - 10) * S, 1 * S + 1, GLYPHS["spin0"], scale=S, color=BLACK)
    draw_glyph(img, 3 * S, 20 * S, GLYPHS[glyph], scale=S)
    dr.text((15 * S, 20 * S + 2), status, fill=WHITE)
    if line2:
        dr.text((15 * S, 28 * S + 2), line2, fill=WHITE)
    hints(img, dr, ["Tap ANY: close"])
    out(img, fname)

# INCOMING message page
img = panel(); dr = ImageDraw.Draw(img)
header(img, dr, "inbox", "LR-TESTER")
dr.text((2 * S, 14 * S), "On my way,", fill=WHITE)
dr.text((2 * S, 22 * S), "see you in", fill=WHITE)
dr.text((2 * S, 30 * S), "ten.", fill=WHITE)
draw_glyph(img, 2 * S, 42 * S, GLYPHS["pin"], scale=S)
dr.text((12 * S, 42 * S + 2), "51.50, -0.12", fill=WHITE)
hints(img, dr, ["Tap ANY: close"])
out(img, "mock_incoming")

# ---------- 3. emit the C arrays ----------
def xbm_bytes(rows):
    out = []
    for row in rows:
        b = 0
        for x, c in enumerate(row):
            if c == '#':
                b |= 0x80 >> x   # GFX drawBitmap: MSB = leftmost pixel
        out.append(b)
    return out

with open("/tmp/oled_glyph_arrays.h", "w") as f:
    f.write("// 8x8 OLED glyphs, GFX drawBitmap format (MSB = leftmost).\n")
    f.write("// Generated from the pixel art in oled_icons_gen.py.\n\n")
    for n, rows in GLYPHS.items():
        ba = ", ".join(f"0x{b:02X}" for b in xbm_bytes(rows))
        f.write(f"static const uint8_t GLYPH_{n.upper()}[8] PROGMEM = {{ {ba} }};\n")
print("generated")

# ---------- animated result page (live spinner in the header) ----------
frames = []
for f in range(4):
    img = panel(); dr = ImageDraw.Draw(img)
    header(img, dr, "send", "STATUS")
    draw_glyph(img, (PW - 10) * S, 1 * S + 1, GLYPHS[f"spin{f}"], scale=S, color=BLACK)
    draw_glyph(img, 3 * S, 20 * S, GLYPHS["clock"], scale=S)
    dr.text((15 * S, 20 * S + 2), "Finding", fill=WHITE)
    dr.text((15 * S, 28 * S + 2), "route", fill=WHITE)
    hints(img, dr, ["Tap ANY: close"])
    frames.append(img.resize((W * 2, H * 2), Image.NEAREST).convert("P"))
frames[0].save("/tmp/oled_shots/mock_result_live.gif", save_all=True,
               append_images=frames[1:], duration=250, loop=0)

# Spinner frame strip for the sheet.
strip = Image.new("L", (4 * 90, 100), 30)
ds = ImageDraw.Draw(strip)
for f in range(4):
    cx = f * 90
    ds.rectangle([cx + 8, cy_s := 8, cx + 8 + 65, 8 + 65], outline=90)
    draw_glyph(strip, cx + 9, 9, GLYPHS[f"spin{f}"], scale=8)
    draw_glyph(strip, cx + 8 + 60 - 4, 76, GLYPHS[f"spin{f}"], scale=1)
    ds.text((cx + 8, 74), f"spin{f}", fill=200)
strip.save("/tmp/oled_shots/spinner_frames.png")

# 16x16 glyphs for the sensor screens' primary visuals (the main
# status screen's icon scale). Same pixel-art-is-source rule.
GLYPHS16 = {
    "sat": [  # comm satellite: body, solar panels, downlink wave
        "......####......",
        "......####......",
        "###...####...###",
        "###..######..###",
        "###..######..###",
        "###..######..###",
        "###...####...###",
        "......####......",
        "......####......",
        ".......##.......",
        "....#..##..#....",
        "...#...##...#...",
        "..#....##....#..",
        ".......##.......",
        "......####......",
        ".....######.....",
    ],
    "fix_none": [  # hollow crosshair: searching
        ".......##.......",
        ".......##.......",
        ".....######.....",
        "....##....##....",
        "...##......##...",
        "..##........##..",
        "##...........##",
        "##...........##",
        "##...........##",
        "..##........##..",
        "...##......##...",
        "....##....##....",
        ".....######.....",
        ".......##.......",
        ".......##.......",
        "................",
    ],
    "fix_ok": [  # solid-centre crosshair: locked
        ".......##.......",
        ".......##.......",
        ".....######.....",
        "....##....##....",
        "...##..##..##...",
        "..##..####..##..",
        "##...######...##",
        "##...######...##",
        "##...######...##",
        "..##..####..##..",
        "...##..##..##...",
        "....##....##....",
        ".....######.....",
        ".......##.......",
        ".......##.......",
        "................",
    ],
}

def emit16(name, rows):
    out = []
    for r in rows:
        r = r.ljust(16, ".")
        b0 = b1 = 0
        for i in range(8):
            if r[i] == "#": b0 |= 0x80 >> i
            if r[8 + i] == "#": b1 |= 0x80 >> i
        out.append(b0); out.append(b1)
    body = ", ".join(f"0x{b:02X}" for b in out)
    return f"static const uint8_t GLYPH16_{name.upper()}[32] PROGMEM = {{ {body} }};"

with open("/tmp/oled_glyph16_arrays.h", "w") as f:
    for n, rows in GLYPHS16.items():
        f.write(emit16(n, rows) + "\n")

# render the 16x16 set onto the icon sheet's bottom row

sheet16 = Image.new("RGB", (3 * 80 + 20, 110), (20, 20, 20))
d16 = ImageDraw.Draw(sheet16)
for i, (n, rows) in enumerate(GLYPHS16.items()):
    gx, gy = 10 + i * 80, 10
    for ry, row in enumerate(rows):
        for rx, c in enumerate(row):
            if c == "#":
                d16.rectangle([gx + rx * 4, gy + ry * 4, gx + rx * 4 + 3, gy + ry * 4 + 3], fill=(255, 255, 255))
    d16.text((gx, gy + 70), n, fill=(180, 180, 180))
sheet16.save("/tmp/oled_shots/icon_sheet16.png")
print("16x16 set ->", "/tmp/oled_shots/icon_sheet16.png")
