"""Offscreen PNG renders of the case around the board.

Renders tessellated STLs with VTK (needs libosmesa6 for headless GL).
Case parts are drawn translucent so the board placement inside is
visible for design review.
"""

from pathlib import Path

# (name, color, opacity) for each board reference part
BOARD_STYLE = {
    "pcb": ((0.05, 0.45, 0.22), 1.0),
    "oled": ((0.08, 0.08, 0.10), 1.0),
    "pmma": ((0.85, 0.92, 0.98), 0.5),
    "headers": ((0.12, 0.12, 0.12), 1.0),
    "m2_core": ((0.45, 0.47, 0.52), 1.0),
    "sd_slot": ((0.72, 0.72, 0.72), 1.0),
    "usb_c": ((0.75, 0.75, 0.78), 1.0),
    "buttons": ((0.85, 0.15, 0.15), 1.0),
    "sma": ((0.85, 0.70, 0.20), 1.0),
    "battery_holder": ((0.20, 0.25, 0.65), 1.0),
}

CASE_STYLE = {
    "front_shell": ((0.62, 0.70, 0.78), 0.45),
    "back_shell": ((0.55, 0.62, 0.55), 0.5),
    "battery_hatch": ((0.90, 0.55, 0.15), 0.75),
}

# Scene center, roughly the middle of the assembled case
CENTER = (17.0, 47.0, -3.0)

# name -> (position offset from CENTER, view up, parallel scale or None)
VIEWS = {
    "iso_front": ((130, -110, 190), (0, -1, 0), None),
    "iso_back": ((120, -100, -200), (0, -1, 0), None),
    "side_buttons": ((250, -15, 30), (0, -1, 0), None),
    "bottom_ports": ((0, 245, 25), (0, 0, 1), None),
    "front_flat": ((0, 0, 270), (0, -1, 0), 62.0),
    "back_flat": ((0, 0, -270), (0, -1, 0), 62.0),
}

# Exploded view: per part Z offset. Front and back both lift clear
# of the board; the hatch keeps going past the back shell.
EXPLODE = {"front_shell": 55.0, "back_shell": -48.0, "battery_hatch": -84.0}

# Solo part views: opaque renders of each printed part.
# name -> list of (view name, camera offset from the part center)
PART_VIEWS = {
    "front_shell": [
        ("outside", (120, -100, 170)),
        ("inside", (-100, 120, -170)),
        ("buttons", (230, 40, 70)),
    ],
    "back_shell": [
        ("outside", (120, -100, -180)),
        ("inside", (-100, 120, 180)),
        ("hull", (210, 60, -130)),
    ],
    "battery_hatch": [
        ("outside", (70, -70, -140)),
        ("inside", (-70, 70, 140)),
    ],
}

# LilyGo's simplified board STEP uses a centered frame: SMA end at
# y = -50, buttons on +X, PCB top at z = +1. This offset maps it into
# the board frame from params.py for overlay verification.
STEP_OFFSET = (16.45, 50.0, -1.0)
STEP_STL = "lilygo_step_overlay.stl"


def render_all(stl_dir: Path, out_dir: Path, exploded: bool = True) -> list[Path]:
    import vtk

    out_dir.mkdir(parents=True, exist_ok=True)
    written = []

    def make_actor(path: Path, color, opacity, offset=(0.0, 0.0, 0.0)):
        reader = vtk.vtkSTLReader()
        reader.SetFileName(str(path))
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(reader.GetOutputPort())
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        actor.SetPosition(*offset)
        prop = actor.GetProperty()
        prop.SetColor(*color)
        prop.SetOpacity(opacity)
        prop.SetSpecular(0.25)
        prop.SetSpecularPower(20)
        return actor

    def render_scene(name: str, view, zoffsets: dict):
        ren = vtk.vtkRenderer()
        ren.SetBackground(0.96, 0.96, 0.97)
        # depth peeling for correct translucency
        ren.SetUseDepthPeeling(1)
        ren.SetMaximumNumberOfPeels(12)
        ren.SetOcclusionRatio(0.02)

        board_alpha = 0.35 if name == "board_vs_step" else 1.0
        for part, (color, opacity) in BOARD_STYLE.items():
            stl = stl_dir / f"board_{part}.stl"
            if stl.exists():
                ren.AddActor(make_actor(stl, color, opacity * board_alpha))
        if name == "board_vs_step":
            step_stl = stl_dir / STEP_STL
            if step_stl.exists():
                ren.AddActor(
                    make_actor(step_stl, (0.35, 0.5, 0.35), 1.0, STEP_OFFSET)
                )
        else:
            for part, (color, opacity) in CASE_STYLE.items():
                stl = stl_dir / f"{part}.stl"
                if stl.exists():
                    ren.AddActor(
                        make_actor(
                            stl, color, opacity,
                            (0.0, 0.0, zoffsets.get(part, 0.0)),
                        )
                    )

        rw = vtk.vtkRenderWindow()
        rw.SetOffScreenRendering(1)
        rw.SetAlphaBitPlanes(1)
        rw.SetMultiSamples(0)
        rw.SetSize(1100, 1500)
        rw.AddRenderer(ren)

        offset, up, pscale = view
        cam = ren.GetActiveCamera()
        cam.SetFocalPoint(*CENTER)
        cam.SetPosition(
            CENTER[0] + offset[0], CENTER[1] + offset[1], CENTER[2] + offset[2]
        )
        cam.SetViewUp(*up)
        if pscale:
            cam.ParallelProjectionOn()
            cam.SetParallelScale(pscale)
        ren.ResetCameraClippingRange()

        rw.Render()
        w2i = vtk.vtkWindowToImageFilter()
        w2i.SetInput(rw)
        w2i.Update()
        writer = vtk.vtkPNGWriter()
        png = out_dir / f"{name}.png"
        writer.SetFileName(str(png))
        writer.SetInputConnection(w2i.GetOutputPort())
        writer.Write()
        written.append(png)

    for name, view in VIEWS.items():
        render_scene(name, view, {})
    if exploded:
        render_scene("exploded", ((150, -125, 210), (0, -1, 0), None), EXPLODE)
    if (stl_dir / STEP_STL).exists():
        render_scene("board_vs_step", ((140, -120, 200), (0, -1, 0), None), {})
        render_scene(
            "board_vs_step_side", ((260, 0, 0), (0, -1, 0), 58.0), {}
        )
    return written


def render_parts(stl_dir: Path, out_dir: Path) -> list[Path]:
    """Opaque solo renders of each printed part."""
    import vtk

    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for part, views in PART_VIEWS.items():
        stl = stl_dir / f"{part}.stl"
        if not stl.exists():
            continue
        reader = vtk.vtkSTLReader()
        reader.SetFileName(str(stl))
        reader.Update()
        bounds = reader.GetOutput().GetBounds()
        center = (
            (bounds[0] + bounds[1]) / 2,
            (bounds[2] + bounds[3]) / 2,
            (bounds[4] + bounds[5]) / 2,
        )
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(reader.GetOutputPort())
        color, _ = CASE_STYLE[part]
        for view_name, offset in views:
            ren = vtk.vtkRenderer()
            ren.SetBackground(0.96, 0.96, 0.97)
            actor = vtk.vtkActor()
            actor.SetMapper(mapper)
            prop = actor.GetProperty()
            prop.SetColor(*color)
            prop.SetOpacity(1.0)
            prop.SetSpecular(0.3)
            prop.SetSpecularPower(25)
            ren.AddActor(actor)

            rw = vtk.vtkRenderWindow()
            rw.SetOffScreenRendering(1)
            rw.SetMultiSamples(0)
            rw.SetSize(1100, 1200)
            rw.AddRenderer(ren)

            cam = ren.GetActiveCamera()
            cam.SetFocalPoint(*center)
            cam.SetPosition(
                center[0] + offset[0], center[1] + offset[1],
                center[2] + offset[2],
            )
            cam.SetViewUp(0, -1, 0)
            ren.ResetCameraClippingRange()
            rw.Render()
            w2i = vtk.vtkWindowToImageFilter()
            w2i.SetInput(rw)
            w2i.Update()
            writer = vtk.vtkPNGWriter()
            png = out_dir / f"part_{part}_{view_name}.png"
            writer.SetFileName(str(png))
            writer.SetInputConnection(w2i.GetOutputPort())
            writer.Write()
            written.append(png)
    return written


def _smoothstep(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def animate(stl_dir: Path, out_path: Path, frames: int = 120,
            size: int = 760) -> list[Path]:
    """Turntable animation: rotate assembled, explode while turning,
    rotate exploded, close up again. Writes an MP4 (H.264, plays
    anywhere) and a GIF fallback next to it."""
    import math

    import vtk
    from PIL import Image

    ren = vtk.vtkRenderer()
    ren.SetBackground(0.97, 0.97, 0.98)
    ren.SetUseDepthPeeling(1)
    ren.SetMaximumNumberOfPeels(12)
    ren.SetOcclusionRatio(0.02)

    case_actors = {}

    def add(path: Path, color, opacity):
        reader = vtk.vtkSTLReader()
        reader.SetFileName(str(path))
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(reader.GetOutputPort())
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        prop = actor.GetProperty()
        prop.SetColor(*color)
        prop.SetOpacity(opacity)
        prop.SetSpecular(0.25)
        prop.SetSpecularPower(20)
        ren.AddActor(actor)
        return actor

    for part, (color, opacity) in BOARD_STYLE.items():
        stl = stl_dir / f"board_{part}.stl"
        if stl.exists():
            add(stl, color, opacity)
    for part, (color, opacity) in CASE_STYLE.items():
        stl = stl_dir / f"{part}.stl"
        if stl.exists():
            case_actors[part] = add(stl, color, opacity)

    rw = vtk.vtkRenderWindow()
    rw.SetOffScreenRendering(1)
    rw.SetAlphaBitPlanes(1)
    rw.SetMultiSamples(0)
    rw.SetSize(size, size)
    rw.AddRenderer(ren)

    cam = ren.GetActiveCamera()
    cam.SetFocalPoint(*CENTER)
    cam.SetViewUp(0, -1, 0)

    radius = 360.0
    rgb_frames = []
    for f in range(frames):
        t = f / frames
        angle = 2 * math.pi * 2 * t  # two revolutions total
        # explode from t 0.28..0.42, hold, implode from t 0.78..0.92
        e = _smoothstep((t - 0.28) / 0.14) - _smoothstep((t - 0.78) / 0.14)
        for part, zoff in EXPLODE.items():
            if part in case_actors:
                case_actors[part].SetPosition(0, 0, zoff * e)
        cam.SetPosition(
            CENTER[0] + radius * math.sin(angle),
            CENTER[1] - 70,
            CENTER[2] + radius * math.cos(angle),
        )
        ren.ResetCameraClippingRange()
        rw.Render()
        w2i = vtk.vtkWindowToImageFilter()
        w2i.SetInput(rw)
        w2i.Update()
        img = w2i.GetOutput()
        w, h, _ = img.GetDimensions()
        raw = img.GetPointData().GetScalars()
        buf = bytes(memoryview(raw))
        pil = Image.frombytes("RGB", (w, h), buf)
        pil = pil.transpose(Image.FLIP_TOP_BOTTOM)
        rgb_frames.append(pil)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    written = []

    mp4_path = out_path.with_suffix(".mp4")
    try:
        import imageio_ffmpeg

        w, h = rgb_frames[0].size
        writer = imageio_ffmpeg.write_frames(
            str(mp4_path), (w, h), fps=15, codec="libx264",
            output_params=["-crf", "22"],
        )
        writer.send(None)
        for pil in rgb_frames:
            writer.send(pil.tobytes())
        writer.close()
        written.append(mp4_path)
    except ImportError:
        pass

    gif_path = out_path.with_suffix(".gif")
    quantized = [pil.quantize(colors=128, dither=Image.NONE)
                 for pil in rgb_frames]
    quantized[0].save(
        str(gif_path), save_all=True, append_images=quantized[1:],
        duration=70, loop=0, optimize=True,
    )
    written.append(gif_path)
    return written
