# Testing: the acceptance runner and its rect cues

Aegir's acceptance is outside-in. A guest cannot see its own screen, so
`scripts/run_target.py` drives QEMU over QMP, reads the console for cues, and
reads the screen back from screendumps. The steps are data (`scripts/targets.py`'s
`QmpStep`), not logic: each names a console line to wait for (`trigger`) and the
checks and inputs to run when it arrives.

## The step

A step fires on every console line its `trigger` regex matches, up to `times`
(0: every match). When it fires the runner, in order:

1. dumps the named QEMU devices (`dumps`) and checks the PPMs -- the dimensions
   (`expect`), the driver's band pattern at its posts (`bands`), sampled pixels
   (`pixels`), and that a region holds ink (`dark`);
2. sends pointer events (`events`), or clicks a reported rectangle (`clicks`);
3. types `press`, one key per character.

The screens are read before the input, because the input is what moves the guest
on to its next cue.

## Rect cues

A pinned screen coordinate is a fact about one layout. A widget whose position a
font or metric decides -- a title bar whose height is the font's line, a tab whose
width is its title's -- moves when the font changes, and every such change was a
re-baseline of the acceptance.

So a guest may report where a widget is, and the acceptance may click it by name.
The guest writes a line:

```
  rect <name> <x> <y> <width> <height>
```

in screen pixels (`aegir-trinket`'s `report_rect`, `Widget::screen_rect`). A step
names one with `clicks = ((name, rx, ry), ...)`, and the runner lands at the
fraction `(rx, ry)` of that rectangle -- the center at `(0.5, 0.5)` -- converting
the pixel to the tablet's axis the way the console maps it back
(`pixel * 32767 / span`). The click therefore follows the layout: a font that
moves the widget moves the click with it.

A widget whose parts the pointer hit-tests reports them itself
(`Widget::report_parts`): a tab group its tabs (`<prefix>.tab.<n>`), a radio group
its members, a scrollbar its arrows and trough, a slider its trough, a cycle its
cell and text, a list its rows (`<prefix>.row.<n>`), all one-based. These are the
parts a fraction of the whole widget cannot name once the font decides their
size -- a list row's height is the font's line, so "the third row" is a
rectangle, not a fraction.

`Window::report_rects` reports the window's own chrome -- content, frame, and
each gadget -- because the title bar's height is the font's too.

A cue must be reported *before* the cue line that opens the step which needs it:
a step's input is sent the moment its trigger arrives, so a rectangle printed
after the trigger is not in hand yet. The demo reports its widgets at the top of
`on_poll`, and a popup's rectangle as the popup opens, before the cue that
announces it.

## What stays pinned

Not every sample is layout. A `pixels` or `dark` check names the ink itself --
a title bar's colour, a grid's text -- and is re-baselined deliberately when the
look changes. The rect cues move the *input* to where the layout put the widget;
the checks still read what the widget drew.
