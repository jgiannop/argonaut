# Writing a theme for Argonaut

A theme is one small file of colours. Put it in the themes folder and it
appears in **Preferences → Appearance**, in **View → Theme** and in the
command palette (Ctrl+Shift+P, type the theme's name).

The quickest start: choose the theme closest to what you want, then
**Preferences → Save This Theme as a File…**. Argonaut writes the file with
every colour spelled out, switches to it and shows it to you. Open it in a
text editor, change a colour, save: the window follows at once.

## Where themes live

| Folder | Whose |
|---|---|
| `~/.local/share/argonaut/themes` | Yours. **Preferences → Open Themes Folder** opens it. |
| `/usr/share/argonaut/themes` | The ones that come with Argonaut (Harbor, Parchment): examples to copy. |

A file of yours with the same file name as one that came with Argonaut is
used instead of it. The file's name without `.json` is the theme's key, so
keep it to letters, digits, spaces, `.`, `-` and `_`.

## The file

```json
{
    "name": "Harbor",
    "dark": true,
    "window": "#17212b",
    "sidebar": "#1d2a36",
    "text": "#dfe7ee",
    "accent": "#4fb3bf"
}
```

| Key | Meaning |
|---|---|
| `window` | Required. The background of the file area and its header. |
| `text` | Required. The text colour. |
| `accent` | Required. The colour of selections, links and the focus. |
| `sidebar` | The background of the sidebar and its header. Left out: a shade of `window`. |
| `name` | The name shown. Left out: the file's name. |
| `dark` | `true` for a dark theme, `false` for a light one. Left out: told from how dark `window` is. |

Those are enough: every other colour is worked out from them, the same way
as in the built-in themes, so a theme of four colours looks complete. Any of
the others can be said outright when you want it different:

| Key | What it colours |
|---|---|
| `dim` | Secondary text (sizes, dates, hints). |
| `selection` | The background of selected files. |
| `sidebarSelection` | The selected row in the sidebar. |
| `hover`, `pressed` | An item or button under the pointer, and while pressed. |
| `pathBar` | The pill behind the path bar and the search field. |
| `button`, `buttonHover` | The round window buttons. |
| `popup` | Menus and the floating bar. |
| `border` | Thin lines between areas. |
| `outline` | The 1-pixel edge of the window. |
| `scrollbar`, `scrollbarHover` | The scrollbar's handle. |

A colour is `"#rrggbb"`. For the keys in the second table a translucent
colour often looks best, written `"#aarrggbb"` (alpha first):
`"#40b5651d"` is the accent at a quarter strength.

## Good to know

- A file that can't be read as a theme (a missing required colour, a typing
  mistake in the JSON) simply isn't offered. While you are editing the theme
  in use and the file is briefly wrong, the last good colours stay.
- If the file of the theme in use is removed, Argonaut goes back to
  **Follow System**.
- Check your theme with both light and dark content in mind: `text` on
  `window` and on `sidebar` should be easy to read, and `accent` should
  stand out on both.
- Extensions that draw their own pictures (the System Dashboard) are told
  the theme's colours, so they follow your theme too.
- A theme is only colours. It can't run anything, so a theme file from
  someone else is safe to try.
