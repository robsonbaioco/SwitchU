# Making a theme

A SwitchU theme is a folder with a `theme.json` in it. Nothing else is
required: no installer, no archive, no entry in a catalogue. Drop the folder on
the card, restart the menu, and it is in the list.

Everything on this page was read out of the code that parses these files
(`projects/menu/src/core/ThemePreset.cpp`), and the defaults quoted are the ones
that apply when a key is absent.

- [Where a theme lives](#where-a-theme-lives)
- [The smallest theme that works](#the-smallest-theme-that-works)
- [Identity](#identity)
- [Colours](#colours)
- [The background](#the-background)
- [Fonts and icons](#fonts-and-icons)
- [Sound](#sound)
- [The cover](#the-cover)
- [Testing it on the console](#testing-it-on-the-console)
- [Publishing it](#publishing-it)
- [Every key, in one manifest](#every-key-in-one-manifest)

`docs/example-theme/` is a working skeleton: copy that folder to the card and it
applies as it is, without any asset files.

## Where a theme lives

```
sdmc:/config/SwitchU/themes/<your-folder>/theme.json
```

Every directory there with a readable `theme.json` is listed under **Themes →
Installed**. A directory without one is skipped silently -- a name alone is not
a theme, and inventing a manifest for it would put a broken entry in front of
somebody. Directories ending in `.installing`, `.installing.part` or `.previous`
are the theme installer's working copies and are ignored.

All paths inside the manifest are relative to the theme's own folder.

## The smallest theme that works

```json
{
  "id": "my-theme",
  "name": "My Theme",
  "version": "1.0.0",
  "theme": { "mode": "dark" }
}
```

That is a legal theme. It takes the menu's dark palette unchanged, so it looks
like the default -- everything below is how you make it look like yours.

## Identity

| Key | Aliases | Notes |
| --- | --- | --- |
| `id` | `slug`, `themeId`, `theme_id` | Unique. Gets a `package:` prefix unless it already contains a `:`. Falls back to the folder name. |
| `name` | `title`, `displayName` | Shown in the list. Falls back to the folder name. |
| `author` | `creator`, `by`, `artist`, `designer` | |
| `version` | `themeVersion` | Used by the catalogue to offer updates. |
| `mode` | inside `theme`, or `variant` | `dark` or `light`. Picks the base palette your colours are applied over. Default `dark`. |

## Colours

**Hue, saturation and lightness all run from 0 to 1.** Not 0-360 for the hue.
This is the single most common way to end up with a grey theme: `"accent": [210,
0.8, 0.55]` is not a blue.

```json
"colors": {
  "accent":           [0.55, 0.80, 0.55],
  "cursor":           [0.55, 0.90, 0.62],
  "background":       [0.62, 0.35, 0.10],
  "backgroundAccent": [0.62, 0.40, 0.17],
  "shapes":           [0.55, 0.55, 0.50]
}
```

| Triplet | Aliases | What it paints |
| --- | --- | --- |
| `accent` | `primaryAccent` | The theme's main colour. |
| `cursor` | `cursorAccent` | The selection cursor and its glow. Follows `accent` when absent. |
| `background` | `bg` | The wall behind everything. |
| `backgroundAccent` | `bgAccent` | The second background tone the gradient runs to. |
| `shapes` | `shape`, `shapeColor`, `floatingShapes` | The drifting shapes. |

Each triplet can also be written as three separate keys -- `accentH`,
`accent_s`, `accentL` and so on, in either camelCase or snake_case.

Colours are read from the top level of the manifest, from a `"colors"` object,
and from inside `"theme"` (and `"theme": {"colors": ...}`). Later ones win, so
pick one place and stay there.

## The background

```json
"background": {
  "layout": "floating",
  "shape": "hexagons",
  "count": 26,
  "size": [18, 62],
  "speed": [5, 22],
  "wobble": 14,
  "rotationSpeed": 0.4,
  "roundness": 0.25,
  "opacity": 0.9,
  "symmetry": "none"
}
```

| Key | Values | Default |
| --- | --- | --- |
| `layout` (`pattern`, `style`) | `floating`, `grid` | `floating` |
| `shape` (`shapes`, `shapeSet`) | `circle`, `triangle`, `square`, `diamond`, `hexagon`, `mixed` (plurals accepted) | `mixed` |
| `count` (`shapeCount`) | how many shapes drift | `30` |
| `size` (`sizeRange`) | `[min, max]` in pixels | `[14, 54]` |
| `speed` (`speedRange`) | `[min, max]` | `[6, 28]` |
| `spacing` (`gap`) | `[x, y]`, grid layout | `[88, 88]` |
| `columns` / `rows` (or a `grid` object) | grid layout | `14` / `8` |
| `wobble` (`drift`) | sideways sway | `16` |
| `rotationSpeed` (`spin`) | | `0.5` |
| `orientation` (`angle`) | degrees; setting it locks rotation | unset |
| `roundness` (`cornerRoundness`) | 0-1 | `0` |
| `opacity` (`shapeOpacity`) | 0-1 | `1` |
| `symmetry` (`mirror`) | `none`, `mirrorX`, `mirrorY`, `quad` | `none` |

A wallpaper image goes in the same object:

```json
"image": { "path": "bg.jpg", "opacity": 0.85, "fit": "cover" }
```

> **`opacity` is not optional here.** It defaults to `0`, and the image is only
> drawn when it is above zero. A theme that sets a path and no opacity shows no
> wallpaper at all, and the log will say the file loaded perfectly well.

`fit` is `cover` (fills, crops) or `contain` (fits, letterboxes). JPEG and PNG
are what the decoder reads. For an animated wallpaper, list the frames instead:

```json
"image": { "frames": ["f/01.jpg", "f/02.jpg", "f/03.jpg"], "fps": 12, "opacity": 1 }
```

Frames are decoded once when the theme is applied and then only swapped, so the
cost is memory rather than per-frame work -- keep the sequence short and the
resolution sane. An animated theme starts at its own clip speed rather than the
menu's background-speed slider, until somebody moves that slider.

## Fonts and icons

```json
"font":  { "regular": "fonts/Display.ttf", "small": "fonts/Text.ttf" },
"icons": "icons/"
```

`"font": "fonts/One.ttf"` as a plain string uses the same face for both sizes.
`icons` is a directory that replaces the menu's own icon set; as an object it is
`{ "path": "icons/" }`.

## Sound

The simplest choice is the shared set:

```json
"audio": { "preset": "wiiu" }
```

To ship your own, put the files in the theme folder and they are picked up
automatically -- the presence of `sounds/sfx`, `sounds/music`, `sfx/` or
`music/` is enough to switch the theme to its own sound set. The effects are
looked up by name:

```
sfx/navigation.wav      sfx/show_modal.wav      sfx/toggle_on.wav
sfx/activation.wav      sfx/hide_modal.wav      sfx/toggle_off.wav
sfx/tab_transition.wav  sfx/launch_game.wav     sfx/slider_up.wav
sfx/confirm.wav         sfx/volume.wav          sfx/slider_down.wav
```

Menu music is listed explicitly:

```json
"audio": { "bundled": true, "music": ["music/theme.mp3"] }
```

## The cover

```json
"preview": { "cover": "preview/cover.jpg" }
```

1280x720 is the right size. Without a cover the theme is still listed, just
without a picture. If you name the file `preview/cover.png`,
`preview/cover.jpg`, `preview/screenshot.png`, `preview/screenshot.jpg` or
`cover.png`, the menu finds it with no manifest entry at all.

## Testing it on the console

1. Copy the folder to `sdmc:/config/SwitchU/themes/`.
2. Restart the menu. Closing and reopening a game does it; so does rebooting.
3. **Themes → Installed**, pick it, apply.
4. Read `sdmc:/config/SwitchU/menu.log`. Applying a theme narrates itself:

```
[theme-apply] background image: path=bg.jpg exists=1 reloaded=1 loaded=1
[theme-apply] regular font: path=fonts/Display.ttf exists=0 reloaded=0 loaded=0
```

`exists=0` is a path that is not there -- almost always a leading slash, a
capital letter, or a file that never left the PC. `exists=1 loaded=0` is a file
the decoder refused. From 2.5.6 the menu's log survives its own restarts, so the
line is still there after you have gone back and forth a few times.

A manifest that does not parse makes the theme vanish from the list rather than
appear broken. If your folder is not listed at all, the JSON is invalid --
check it in any validator.

## Publishing it

SwitchU reads two catalogues, and both serve packages as a zip with `theme.json`
at the root. The installer unpacks into `<slug>.installing`, validates the
manifest it finds, keeps the previous version in `<slug>.previous` until the new
one is in place, and cleans both up when it is done.

- `https://raw.githubusercontent.com/PoloNX/SwitchU-Themes/main/index.json`
- `https://themes.nclabs.dev/index.json` -- the site's source is in
  `web/themes-site/` in this repository.

## Every key, in one manifest

Nothing here is required. This is the whole surface in one place.

```json
{
  "id": "example-theme",
  "name": "Example Theme",
  "author": "your name here",
  "version": "1.0.0",

  "theme": { "mode": "dark" },

  "colors": {
    "accent":           [0.55, 0.80, 0.55],
    "cursor":           [0.55, 0.90, 0.62],
    "background":       [0.62, 0.35, 0.10],
    "backgroundAccent": [0.62, 0.40, 0.17],
    "shapes":           [0.55, 0.55, 0.50]
  },

  "background": {
    "layout": "floating",
    "shape": "hexagons",
    "count": 26,
    "size": [18, 62],
    "speed": [5, 22],
    "spacing": [88, 88],
    "columns": 14,
    "rows": 8,
    "wobble": 14,
    "rotationSpeed": 0.4,
    "orientation": 0,
    "roundness": 0.25,
    "opacity": 0.9,
    "symmetry": "none",
    "image": { "path": "bg.jpg", "opacity": 0.85, "fit": "cover" }
  },

  "font":  { "regular": "fonts/Display.ttf", "small": "fonts/Text.ttf" },
  "icons": "icons/",

  "audio": { "preset": "wiiu", "music": ["music/theme.mp3"] },

  "preview": { "cover": "preview/cover.jpg" }
}
```
