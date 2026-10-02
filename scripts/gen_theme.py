#!/usr/bin/env python3
"""Generate the theme's C++ tables from resources/themes/xen.toml.

    python3 scripts/gen_theme.py <output.cc>

The theme file is the source of the look (specs/trinket/theming.md): a palette
(a colour per ColorRole), metrics (a size per MetricRole), a set of sprites (the
imported MUI artwork), and a recipe per gadget. The sprites come from the
generated preset (resources/themes/xen-preset.toml, scripts/convert_prefs.py);
the theme may still add or override one. This turns them into the tables
theme_data.h declares -- the palette and metrics in the enum's own order, the
recipes as primitive steps, and the sprites as 0xAARRGGBB pixel arrays -- so the
engine interprets data instead of compiling the look in. A step colour may be a
literal `#rrggbb` or `$NAME`, a palette entry; a sprite step names an entry of
[sprites].

Run by the build and by scripts/check_theme.py; not part of any target build.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

import pins

TRINKET = pins.ROOT / "libs" / "hosted" / "aegir-trinket"
THEME = TRINKET / "resources" / "themes" / "xen.toml"
PRESET = TRINKET / "resources" / "themes" / "xen-preset.toml"
RESOURCES = TRINKET / "resources" / "themes" / "xen"
HEADER = TRINKET / "include" / "aegir" / "trinket" / "theme.h"

BEVEL = {"raised": 0, "sunken": 1}
MARK = {"up": 0, "down": 1, "left": 2, "right": 3}
LINE = {"top": 0, "bottom": 1, "left": 2, "right": 3}


def enum_names(header: str, name: str) -> list[str]:
    """The enumerators of `enum class NAME { ... }`, in order."""
    match = re.search(r"enum class " + name + r"\s*\{(.*?)\}", header, re.S)
    if match is None:
        raise SystemExit(f"no enum class {name} in {HEADER.name}")
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return [token.strip() for token in body.split(",") if token.strip()]


def rgb(text: str) -> int:
    text = text.lstrip("#")
    if len(text) != 6:
        raise SystemExit(f"a colour must be #rrggbb, got {text!r}")
    return int(text, 16)


def resolve(value: str, palette: dict) -> int:
    """A step colour: `$NAME` into the palette, or a literal hex."""
    if value.startswith("$"):
        name = value[1:]
        if name not in palette:
            raise SystemExit(f"no palette colour {name!r}")
        return rgb(palette[name])
    return rgb(value)


def collect_sprites(gadgets: dict) -> list[str]:
    """Every sprite the recipes name, in first-seen order. A `gadgets.X` is
    either one recipe (`bottombar`, `frame`) or a table of them (`button.normal`);
    both shapes are walked."""
    names: list[str] = []

    def scan(recipe: dict) -> None:
        for step in recipe["steps"]:
            name = step.get("sprite")
            if name is not None and name not in names:
                names.append(name)

    for group in gadgets.values():
        if "steps" in group:
            scan(group)
        else:
            for recipe in group.values():
                scan(recipe)
    return names


def parse_at(step: dict) -> tuple[int, int, int, int]:
    """The step's sub-rectangle in sixteenths (specs/trinket/chrome.md)."""
    at = step.get("at", [0, 0, 16, 16])
    if len(at) != 4:
        raise SystemExit(f"`at` must be [x0, y0, x1, y1], got {at!r}")
    x0, y0, x1, y1 = (int(v) for v in at)
    return x0, y0, x1, y1


def parse_step(step: dict, palette: dict, sprites: dict[str, int]) -> tuple:
    inset = int(step.get("inset", 0))
    if "fill" in step:
        op = ("FILL", 0, inset, 0, 1, resolve(step["fill"], palette), 0, 0)
    elif "bevel" in step:
        op = ("BEVEL", BEVEL[step["bevel"]], inset, 0, 1, 0, 0, 0)
    elif "outline" in step:
        op = ("OUTLINE", 0, inset, 0, 1, resolve(step["outline"], palette), 0, 0)
    elif "dither" in step:
        fg, bg = step["dither"]
        op = ("DITHER", 0, inset, 0, 1, resolve(fg, palette), resolve(bg, palette), 0)
    elif "mark" in step:
        num, den = step.get("size", [1, 4])
        light = step.get("light", step.get("color", "#ffffff"))
        dark = step.get("dark", step.get("color", "#000000"))
        op = ("MARK", MARK[step["mark"]], inset, num, den,
              resolve(light, palette), resolve(dark, palette), 0)
    elif "line" in step:
        # One edge, not the whole bevel (specs/trinket/chrome.md).
        if "color" not in step:
            raise SystemExit(f"a line step needs a color: {step}")
        op = ("LINE", LINE[step["line"]], inset, 0, 1,
              resolve(step["color"], palette), 0, 0)
    elif "sprite" in step:
        name = step["sprite"]
        if name not in sprites:
            raise SystemExit(f"no sprite {name!r} in [sprites]")
        op = ("SPRITE", 0, inset, 0, 1, 0, 0, sprites[name])
    else:
        raise SystemExit(f"unknown step: {step}")
    return (*op, *parse_at(step))


def emit_recipe(name: str, recipe: dict, palette: dict, sprites: dict[str, int]) -> tuple[str, str]:
    steps = recipe["steps"]
    if not steps:
        # A recipe with no steps -- Icon::NONE is one -- is a null table, not an
        # empty array: `const Step x[] = {}` is not C++, and the engine iterates
        # zero steps either way.
        return "", "{nullptr, 0}"
    lines = [f"static const Step {name}[] = {{"]
    for step in steps:
        (op, kind, inset, num, den, color, color2, sprite,
         x0, y0, x1, y1) = parse_step(step, palette, sprites)
        lines.append(f"    {{Prim::{op}, {kind}, {inset}, {num}, {den}, "
                     f"0x{color:06x}, 0x{color2:06x}, {sprite}, "
                     f"{x0}, {y0}, {x1}, {y1}}},")
    lines.append("};")
    return "\n".join(lines), f"{{{name}, {len(steps)}}}"


def emit_array(var, recipes, palette, sprites) -> str:
    blocks, items = [], []
    for i, recipe in enumerate(recipes):
        block, item = emit_recipe(f"{var}_{i}", recipe, palette, sprites)
        blocks.append(block)
        items.append(f"    {item},")
    return "\n".join(blocks) + f"\nconst Recipe {var}[] = {{\n" + "\n".join(items) + "\n};"


def emit_single(var, recipe, palette, sprites) -> str:
    block, item = emit_recipe(f"{var}_steps", recipe, palette, sprites)
    return block + f"\nconst Recipe {var} = {item};"


def emit_sprites(names: list[str], table: dict, index: dict[str, int]) -> list[str]:
    import png

    lines = []
    dims = []
    for i, name in enumerate(names):
        path = RESOURCES / table[name]
        if not path.exists():
            raise SystemExit(f"sprite {name!r} is missing: {path}")
        width, height, pixels = png.decode(path)
        dims.append((width, height))
        lines.append(f"static const uint32_t sprite_{i}[] = {{")
        for r, g, b, a in pixels:
            lines.append(f"    0x{a:02x}{r:02x}{g:02x}{b:02x},")
        lines.append("};")
    lines.append("")
    lines.append("const Sprite kSprites[] = {")
    for i, (width, height) in enumerate(dims):
        lines.append(f"    {{{width}, {height}, sprite_{i}}},  // {names[i]}")
    lines.append("};")
    lines.append(f"const unsigned kSpriteCount = {len(names)};")
    return lines


def main() -> int:
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if output is None:
        print("usage: gen_theme.py <output.cc>", file=sys.stderr)
        return 2

    theme = tomllib.loads(THEME.read_text())
    header = HEADER.read_text()
    palette = theme["palette"]
    metrics = theme["metrics"]
    fixed = set(metrics.get("fixed", []))
    g = theme["gadgets"]

    sprite_names = collect_sprites(g)
    # The sprites come from the generated preset (scripts/convert_prefs.py); the
    # theme may still add or override one by hand.
    sprite_table: dict[str, str] = {}
    if PRESET.exists():
        sprite_table.update(tomllib.loads(PRESET.read_text()).get("sprites", {}))
    sprite_table.update(theme.get("sprites", {}))
    sprite_index = {name: i for i, name in enumerate(sprite_names)}

    parts = [
        "/* Generated by scripts/gen_theme.py from",
        " * libs/hosted/aegir-trinket/resources/themes/xen.toml. Do not edit; the",
        " * theme file is the source (specs/trinket/theming.md). */",
        "",
        "#include <aegir/trinket/theme_data.h>",
        "",
        "namespace aegir::trinket {",
        "",
    ]

    parts.append("const uint32_t kPalette[] = {")
    for role in enum_names(header, "ColorRole"):
        if role not in palette:
            raise SystemExit(f"the palette has no colour for {role}")
        parts.append(f"    0x{rgb(palette[role]):06x},  // {role}")
    parts.append("};")
    parts.append("")

    metric_roles = enum_names(header, "MetricRole")
    parts.append("const int kMetrics[] = {")
    for role in metric_roles:
        if role not in metrics:
            raise SystemExit(f"the metrics have no value for {role}")
        parts.append(f"    {int(metrics[role])},  // {role}")
    parts.append("};")
    parts.append("")
    parts.append("const uint8_t kMetricFixed[] = {")
    for role in metric_roles:
        parts.append(f"    {1 if role in fixed else 0},  // {role}")
    parts.append("};")
    parts.append("")

    parts += emit_sprites(sprite_names, sprite_table, sprite_index)
    parts.append("")

    parts += [
        emit_array("kRecipeButton",
                   [g["button"][n] for n in ("normal", "hovered", "pressed",
                                             "focused", "disabled")], palette, sprite_index),
        "",
        emit_array("kRecipeCheck", [g["check"]["off"], g["check"]["on"]],
                   palette, sprite_index),
        "",
        emit_array("kRecipeRadio", [g["radio"]["off"], g["radio"]["on"]],
                   palette, sprite_index),
        "",
        emit_array("kRecipeTextbox", [g["textbox"]["normal"], g["textbox"]["focused"]],
                   palette, sprite_index),
        "",
        emit_single("kRecipeTextboxReadonly", g["textbox"]["readonly"], palette, sprite_index),
        "",
        emit_array("kRecipePanel",
                   [g["panel"][n] for n in ("flat", "raised", "sunken", "frame",
                                            "group_box")], palette, sprite_index),
        "",
        emit_single("kRecipeScrollbarTrough", g["scrollbar"]["trough"], palette, sprite_index),
        "",
        emit_single("kRecipeScrollbarThumb", g["scrollbar"]["thumb"], palette, sprite_index),
        "",
        # The arrow buttons, by orientation and press state: index pressed ? 1 : 0
        # (specs/trinket/scrollbar.md).
        emit_array("kRecipeScrollbarDecrement",
                   [g["scrollbar"][n] for n in ("decrement", "decrement_pressed")],
                   palette, sprite_index),
        "",
        emit_array("kRecipeScrollbarIncrement",
                   [g["scrollbar"][n] for n in ("increment", "increment_pressed")],
                   palette, sprite_index),
        "",
        emit_array("kRecipeScrollbarDecrementHorizontal",
                   [g["scrollbar"][n] for n in ("decrement_horizontal",
                                                "decrement_horizontal_pressed")],
                   palette, sprite_index),
        "",
        emit_array("kRecipeScrollbarIncrementHorizontal",
                   [g["scrollbar"][n] for n in ("increment_horizontal",
                                                "increment_horizontal_pressed")],
                   palette, sprite_index),
        "",
        emit_single("kRecipeSliderTrough", g["slider"]["trough"], palette, sprite_index),
        "",
        emit_single("kRecipeSliderKnob", g["slider"]["knob"], palette, sprite_index),
        "",
        emit_single("kRecipeCycleFace", g["cycle"]["face"], palette, sprite_index),
        "",
        emit_single("kRecipeCyclePressed", g["cycle"]["pressed"], palette, sprite_index),
        "",
        emit_single("kRecipeCycleDivider", g["cycle"]["divider"], palette, sprite_index),
        "",
        emit_single("kRecipeCycleMark", g["cycle"]["mark"], palette, sprite_index),
        "",
        emit_array("kRecipePopup",
                   [g["popup"][n] for n in ("popup_normal", "popup_selected",
                                            "file_normal", "file_selected",
                                            "drawer_normal", "drawer_selected")],
                   palette, sprite_index),
        "",
        emit_single("kRecipeListWell", g["list"]["well"], palette, sprite_index),
        "",
        emit_single("kRecipeListCursor", g["list"]["cursor"], palette, sprite_index),
        "",
        emit_single("kRecipeListSelected", g["list"]["selected"], palette, sprite_index),
        "",
        # The row icons, indexed by Icon (specs/trinket/listview.md).
        emit_array("kRecipeIcon",
                   [g["icon"][n] for n in ("none", "drawer", "hard_disk", "disk",
                                           "chip", "volume", "network")],
                   palette, sprite_index),
        "",
        # The window chrome (specs/trinket/chrome.md): the title bar, the bottom
        # bar, the frame, and the gadgets (kind * 2 + active).
        emit_array("kRecipeTitlebar",
                   [g["titlebar"][n] for n in ("inactive", "active")],
                   palette, sprite_index),
        "",
        emit_single("kRecipeBottombar", g["bottombar"], palette, sprite_index),
        "",
        emit_single("kRecipeWindowFrame", g["frame"], palette, sprite_index),
        "",
        emit_array("kRecipeGadget",
                   [g["gadget"][n] for n in ("close_inactive", "close_active",
                                             "zoom_inactive", "zoom_active",
                                             "depth_inactive", "depth_active")],
                   palette, sprite_index),
        "",
        # The screen bar and the menus (specs/trinket/chrome.md).
        emit_single("kRecipeScreenBar", g["screen_bar"], palette, sprite_index),
        "",
        emit_single("kRecipeMenubar", g["menubar"], palette, sprite_index),
        "",
        emit_single("kRecipeMenuWell", g["menu_well"], palette, sprite_index),
        "",
        emit_array("kRecipeMenuItem",
                   [g["menu_item"][n] for n in ("normal", "hovered")],
                   palette, sprite_index),
        "",
        emit_single("kRecipeKeycap", g["keycap"], palette, sprite_index),
        "",
        "}  // namespace aegir::trinket",
        "",
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
