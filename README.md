# Click Window Counter (Geode)

Load a `.gdr2` macro while you're in a level. The mod works out the **frame window of every click and every
release at 240 Hz** (how many frames earlier/later that input can be and still get you through) and draws a
coloured ring with the number at each one. Results are saved per level, so the rings come back whenever you
open it.

## Use
1. Open a level and pause.
2. **Frame Windows** -> pick your `.gdr2`. The level restarts in practice mode and the analysis runs fast
   (progress shown at the top). Don't click or pause while it runs.
3. When it finishes you get a summary popup and the rings appear.
4. **Play Macro** replays the macro from the start with the rings showing. (After restarting the game it asks
   for the macro file again; the rings themselves are remembered.)
5. **Markers** lists every analyzed input with a tick box: untick one to hide just that ring, or use
   "All clicks" / "All releases" to hide a whole kind. Your choices are saved.

## Rings
Clicks are a big ring with the number on its left. Releases are a smaller ring with the number on its right.
Colour = window size (same scale as the reference chart):

| Frames | Colour |
|---|---|
| 1 | red |
| 2 | orange |
| 3 | yellow |
| 4 | white |
| 5-6 | green |
| 7-8 | light blue |
| 9-12 | blue |
| 13-14 | purple |
| 15+ | pink |

A `+` after the number (e.g. `25+`) means it hit the max-offset setting without failing.

**Ring sizes** are adjustable in the mod settings: *Click ring size* (default 1.0) and *Release ring size*
(default 0.65). The ring, its outline and its number all scale together, and the rings redraw as soon as you
move the slider.

## How a window is measured
For each input the mod saves a checkpoint a few frames before it, then re-simulates with that input shifted by
+1, +2, ... and -1, -2, ... frames. A shifted input "passes" if the player survives until shortly after the
next click. Window = earliest passing offset .. latest passing offset (so early + late + 1).

- **Clicks:** the press moves (its release moves with it by default, keeping the hold length).
- **Free release** (on by default): for each click offset the mod also tries the release a few frames sooner or
  later than the shifted hold would (range is a setting, default +-4), in the order 0, -1, +1, -2, +2, ... The
  offset passes if ANY of those survives. This gives the window you really have when you may adapt the release,
  e.g. in ship: pressing a frame earlier often only works if you also let go earlier. It can never report less
  room than a fixed hold, and the release is still kept after its press and before the next press. It makes
  analysis slower (a failing offset costs up to 2 x range + 1 trials), so turn it off for a quick strict pass.
- **Releases:** the click stays fixed and only the release moves.
- An input can't change the input pattern itself: a release can't move before its press (min 1 frame hold) or
  past the next press, and a click can't move past its own release or the previous release. Those are real
  limits, not caps, so they aren't marked with `+`.

## Window tally (top left)
A panel in the top-left corner counts how many inputs have each window size, one coloured row per size, biggest
on top, in the same colours as the rings:

```
15+:   0
13-14: 0
9-12:  1
7-8:   1
5-6:   6
4:     1
3:     1
2:     0
1:     0
```

Only inputs whose ring is visible are counted, so hiding rings in the Markers list (including "All clicks" /
"All releases") changes the tally. Hide all releases to count clicks only. Size, corner and on/off are in the
settings. It's hidden while the analysis runs and appears when it finishes. If you put the conditions text in
the same corner as the tally, it sits just below it.

## Conditional windows (next-click table)
One click's timing can change how much room the next click has. With **Conditional windows** on (default), for
every frame a click can land on, the mod also measures the NEXT click's window with the first click moved there.

While you play (or watch the macro), a small text appears in the top-right corner (or top-left: see the
*Conditions text corner* setting) for the upcoming click, from 40
frames before it until 25 frames after:

```
click window 6 (you: frame 3)
if on frame 1-4, 6: next click is 3
if on frame 5: next click is 1
```

"Frame 1" is the earliest working frame of that click, and "you" is the frame you recorded. If a click has too
many different outcomes to group, it shows a compact `frame:next click` list instead. If a timing would make the
next click impossible the text says so. The text follows the ring: hiding a click's ring hides its text.
Size, corner and on/off are in the settings. Tables are saved with the results, so they come back with the rings.

Only clicks get a table (not releases), and only when another click follows. The next click is held to your
recorded timing in the click's own window; the table shows how much room it keeps.

## What the analysis options are, and how long they take
Each option makes the analysis more accurate or gives you more information, and each one costs time. The
"Roughly" column is how long a run takes compared with the plain default (clicks only, hold length fixed),
measured on a mock macro in simulated steps.

| Option | What it does | Roughly |
|---|---|---|
| **Clicks only** (baseline) | Measures each click's window. The click moves together with its release, so the hold length stays the same. | 1x |
| **+ Releases** | Also measures each release's window, with its click held still. Gives the release rings. | 3x |
| **+ Free release** | When measuring a click, also tries letting go a few frames earlier or later than the shifted hold would. A click frame counts as working if *any* of those releases survives. Finds the window you really have for holds like ship, where pressing earlier often needs an earlier release too. | 2x |
| **+ Conditional windows** ("conditions") | For every frame a click can land on, also measures how big the **next** click's window is with the first click moved there. This is what the next-click text shows (e.g. "if on frame 5: next click is 1"). It re-measures the next click once per working frame, which is why it costs the most. | 11x |
| **+ Free release + conditions** | Both of the above together. | 23x |
| **Everything** | Clicks, releases, free release and conditions all on. | 25x |

In short: **releases** adds more rings, **free release** makes click windows more accurate for holds, and
**conditions** adds the "how does this click change the next one" text. Conditions is the slow one.

On that mock, "everything" at the default analysis speed (6x) was about 15 seconds per click, so 100 clicks is
about 25 minutes. Real levels will differ. For a first test, turn **Free release** and **Conditional windows**
off and analyze a short macro; turn them on once it works.

## Settings
What to analyze (clicks / releases / both), max offset, free release + range, conditional windows, survive-after-next-click frames, max simulation length,
analysis speed, shift release, jump-only, show markers, click ring size, release ring size, show conditions text,
conditions text size, conditions text corner (default top right), show window tally, tally size, tally corner
(default top left). Analyzing clicks and releases takes about 3x as long as clicks only.

- Levels with random triggers / non-deterministic gameplay may not reproduce.
