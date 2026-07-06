"""Dimensional parameters for the T-Beam Supreme enclosure.

Coordinate convention (matches the LilyGo board DXF):
  origin = board corner at the SMA / OLED end, on the long edge
  opposite the buttons.
  X runs across the board width (0 .. 32.893), the button edge is
  at X = PCB_W.
  Y runs along the board length (0 .. 100.127), Y = 0 is the SMA /
  OLED end (antenna end, "up" in the holding orientation), Y = PCB_L
  is the USB-C / microSD end ("down").
  Z = 0 is the PCB top surface. Top-side components (OLED, M.2 core)
  grow +Z, the 18650 holder on the back grows -Z.

Provenance tags:
  OFFICIAL   measured from LilyGo's published DXF / STEP / schematic
             (Xinyuan-LilyGO/LilyGo-LoRa-Series, dimensions/ and
             schematic/T-Beam-S3-Supreme/)
  ESTIMATED  from photos, generic part datasheets, or community
             case notes. Verify against hardware before trusting
             for a tight fit.
"""

# PCB outline (OFFICIAL, DXF dimension entities)
PCB_W = 32.893
PCB_L = 100.127
PCB_CORNER_R = 2.0  # corner cutback is 2.03 x 2.03, modelled as R2
PCB_T = 1.6  # ESTIMATED, DXF does not state thickness

# Corner mounting holes (OFFICIAL): diameter 2.0 mm.
# Community cases use 2.2 mm self tapping screws in these.
MOUNT_HOLE_D = 2.0
MOUNT_HOLES = [
    (2.59, 2.37),
    (30.30, 2.37),
    (2.61, 97.67),
    (30.29, 97.72),
]

# Side actuated tactile buttons on the X = PCB_W long edge (OFFICIAL
# positions from DXF footprints, order confirmed by silkscreen).
# Plungers protrude past the board edge in +X.
BUTTON_EDGE_X = PCB_W
BUTTON_Y = {
    "reset": 50.7,
    "power": 59.0,
    "boot": 74.6,
}
BUTTON_FOOT_W = 4.75  # footprint extent along Y (OFFICIAL)
BUTTON_FOOT_D = 2.45  # footprint extent along X (OFFICIAL)
BUTTON_PLUNGER_PROTRUSION = 1.5  # past board edge, ESTIMATED (TS-019B type)
BUTTON_PLUNGER_W = 3.0  # ESTIMATED
BUTTON_HEIGHT_ABOVE_PCB = 3.0  # switch body top, ESTIMATED
BUTTON_TRAVEL = 0.25  # typical side tactile switch travel, ESTIMATED

# USB-C on the Y = PCB_L short edge (OFFICIAL footprint x 4.30..13.74)
USB_CENTER_X = 9.02
USB_W = 9.44  # footprint width along X
USB_H = 3.3  # receptacle height, generic part, ESTIMATED
USB_PROTRUSION = 0.9  # past board edge (OFFICIAL, from footprint)

# microSD (TF) slot, card inserts from the Y = PCB_L edge (OFFICIAL bbox)
SD_X_MIN, SD_X_MAX = 14.30, 28.43
SD_Y_MIN, SD_Y_MAX = 83.53, 99.56
SD_H = 2.0  # ESTIMATED

# OLED: 1.3 inch SH1106 128x64 at the Y = 0 end, glass covers roughly
# y 0..37 (OFFICIAL region), panel and PMMA cover heights ESTIMATED.
OLED_Y_MIN, OLED_Y_MAX = 0.5, 37.0
OLED_X_MIN, OLED_X_MAX = 1.5, 31.4
OLED_STACK_H = 4.5  # glass plus factory PMMA cover above PCB
OLED_ACTIVE_W = 29.42  # 1.3 inch panel active area, ESTIMATED
OLED_ACTIVE_H = 14.70

# M.2 core module region on the top side (OFFICIAL bbox from DXF).
# Socket stack height 8.5 mm is OFFICIAL ("M.2 B-KEY H8.5" on the
# schematic); the card and shields on top of it are ESTIMATED.
M2_X_MIN, M2_X_MAX = 0.0, 25.4
M2_Y_MIN, M2_Y_MAX = 43.4, 90.2
M2_STACK_H = 11.5

# SMA bulkhead at the Y = 0 edge (presence OFFICIAL, offsets ESTIMATED
# from photos; the barrel is not in the top silkscreen DXF).
SMA_CENTER_X = 6.0
SMA_D = 6.5
SMA_LEN_OUTSIDE = 8.0
SMA_CENTER_Z = 4.0  # barrel axis height above PCB, ESTIMATED

# 18650 holder on the back side (presence OFFICIAL, envelope ESTIMATED
# from generic drop-in frame holders; cell inserts radially from -Z).
BATT_W = 21.0  # across X
BATT_L = 78.0  # along Y
BATT_H = 15.0  # below the PCB underside
BATT_CENTER_X = PCB_W / 2
BATT_Y_MIN = 11.0

# Known good outer envelope: LilyGo's own injection molded shell
# measures 43.2 x 109.2 x 34.3 mm (OFFICIAL, measured from their STLs).
LILYGO_SHELL_W = 43.2
LILYGO_SHELL_L = 109.2
LILYGO_SHELL_T = 34.3

# Fit note: LilyGo changed a mold thickness by 0.8 mm in 2024 and broke
# community case fit at the SMA end. Leave extra tolerance there.
SMA_END_EXTRA_CLEARANCE = 1.0
