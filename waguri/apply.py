#!/usr/bin/env python3
"""Register the Waguri frontend in a shallow checkout of upstream mpv."""

from pathlib import Path


def insert_once(source, anchor, replacement):
    if replacement in source:
        return source
    if source.count(anchor) != 1:
        raise SystemExit(f"Unsupported mpv checkout: could not locate {anchor!r}.")
    return source.replace(anchor, replacement, 1)


root = Path(__file__).resolve().parent.parent
meson_path = root / "meson.build"
audio_path = root / "player" / "audio.c"
if not meson_path.is_file() or not audio_path.is_file():
    raise SystemExit("Extract this archive directly into the root of the mpv checkout.")

meson = insert_once(
    meson_path.read_text(),
    "incdir_public = include_directories('./include')",
    """waguri_c_args = []
if host_machine.system() == 'linux' and get_option('libmpv')
    sources += files('waguri/spectrum.c')
    waguri_c_args = ['-DWAGURI_AUDIO_TAP']
endif

incdir_public = include_directories('./include')""",
)
meson = insert_once(
    meson,
    "libmpv = library('mpv', sources, dependencies: dependencies, gnu_symbol_visibility: 'hidden',",
    """libmpv = library('mpv', sources, dependencies: dependencies, gnu_symbol_visibility: 'hidden',
                 c_args: waguri_c_args,""",
)
frontend = """
# Waguri Linux frontend
if host_machine.system() == 'linux' and get_option('libmpv')
    subdir('waguri')
endif
"""
if frontend not in meson:
    meson += "\n" + frontend

audio = insert_once(
    audio_path.read_text(),
    '#include "audio/format.h"',
    '#include "audio/format.h"\n#ifdef WAGURI_AUDIO_TAP\n#include "waguri/spectrum.h"\n#endif',
)
audio = insert_once(
    audio,
    "        mpctx->shown_aframes += samples;",
    """#ifdef WAGURI_AUDIO_TAP
        waguri_spectrum_feed(af);
#endif
        mpctx->shown_aframes += samples;""",
)

meson_path.write_text(meson)
audio_path.write_text(audio)
