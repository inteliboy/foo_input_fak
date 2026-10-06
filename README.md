# foo_input_fak — foobar2000 components for FAK

Plays, tags and shows album art for FAK (`.fak`) lossless files in foobar2000 v2 (x64). The codec is in
[FAK-Codec](https://github.com/inteliboy/FAK-Codec); these components link its decoder. There are two components
in this repository; install **one** of them:

| | `foo_input_fak` | `foo_input_fak_adv` |
|---|---|---|
| Playback, seeking, technical info | yes | yes |
| Tags, ReplayGain, tag writing; cue sheets; album art | yes | yes |
| Encoding through the Converter (a `FAK` output format, bundled `fak.exe`) | yes | yes |
| Right-click `FAK` menu: convert, single file with cue sheet, verify | - | yes |
| Preferences > Tools > FAK (level, error recovery, output location, ...) | - | yes |
| Package size | about 1.3 MB | about 1.8 MB |

`foo_input_fak` is the small one: it plays the format and lets the Converter encode it. `foo_input_fak_adv` is
`foo_input_fak` plus the conversion menu and settings page described below, so it replaces it (it does not need
it). With both installed every `.fak` file would be claimed twice, so `foo_input_fak_adv` shows a warning at
startup; remove one of them. Everything below applies to both unless a section says "advanced only".

| Feature | How |
|---|---|
| Playback | Bit-exact decode through the Rust library (`capi/`), one chunk (~1 s) at a time, reading the file on demand: about 50 MB more in the player for a 160 MB file, whatever its length |
| Seeking | Sample-accurate: decode the containing chunk, drop the leading samples |
| Technical info | sample rate, channels, bits, length, bitrate, codec `FAK`, codec profile (level when the encoder recorded it, chunk length, FEC), format version, encoder string with its settings (`Tool`), chunk count, FEC layout, channel mask, stored PCM SHA-256. These are read-only technical fields (foobar2000 lists the non-standard ones under "Other"); they are worked out from the file each time it is opened |
| Tags | Vorbis-comment style `KEY=VALUE` tags, multi-value; `REPLAYGAIN_*` mapped to foobar2000's ReplayGain fields |
| Tag writing | Properties / ReplayGain scanner / Masstagger: the metadata block is rewritten, audio bytes are copied unchanged (the stored PCM hash stays valid) |
| Cue sheets | An embedded cue sheet (`fak encode --cuesheet`, or a `cuesheet` field written by foobar2000, e.g. when converting a disc image) shows as one playlist entry per track, with per-track titles; editing a track's tags edits the sheet. Uses the SDK's embedded-cuesheet wrapper, as foobar2000's own FLAC input does |
| CD tags | Opt-in (`fak encode --cd-tags` / `fak edit --cd-tags`). In a file that has them, they are kept current: when the cue sheet's track points change (indices moved; retitling moves nothing), the CD identifiers and AccurateRip/CTDB checksums are recomputed from the audio (`fak::cdrip`, same rule as `fak edit --cuesheet`); removing the sheet drops the `FAK_` ones. A CD tag you changed in the same edit is kept. Files without them get none |
| Album art | Front/back cover, disc, artist, icon: read, add, replace, remove; other picture types are preserved |
| Encoding | The `FAK` entry that appears in the Converter's output formats, using the bundled `fak.exe` (both components); the right-click `FAK` menu (advanced only, below). Tags are then written by the component |

Not yet supported: 32-bit/ARM64 foobar2000 builds, and streaming from non-seekable sources (the
whole compressed file is read on open; foobar2000's FLAC input reads as it plays).

## Converting from the right-click menu (`foo_input_fak_adv` only)

Select tracks, right-click, `FAK`:

- `Convert to FAK (level: X)`: uses the level set in Preferences.
- `Convert to one FAK file with a cue sheet` (two or more tracks selected): joins the tracks, in order, into
  one file with an embedded cue sheet, laid out the way foobar2000 itself lays out an embedded cue sheet
  (tags common to all tracks go in the file, the rest in the sheet), so the file shows as one playlist
  entry per track. All tracks must have the same sample rate, channel count and sample format. Cue-sheet
  positions are whole CD frames (1/75 s), as with FLAC.
- `Convert with level`: Fast, Normal, Max, Insane, Archival (Archival = Insane plus Reed-Solomon parity over the
  whole file, so any few damaged chunks (about 1% of them) can be repaired on decode, for about 1-2% extra size).
- `Verify FAK files`: decodes each selected `.fak` and checks it against the SHA-256 of the original audio.
- `FAK preferences...`

Preferences > Tools > FAK: the level slider (Archival ticks the error-recovery option), error-recovery
data (FEC) with its block size in chunks (0 = one block for the whole file, the default; usable at any level), output location (next to the source with the same
name, or a folder plus a title-formatting pattern), the name pattern of a single-file conversion, what to do
if the file exists, encoder threads, chunk length, copying album art, verifying each file after writing (the
file is read back from disk and decoded), and **CD tags** for single-file conversions. Tags (with ReplayGain) and album
art are copied from the source.

CD tags (the CD identifiers `DISCID`, `MUSICBRAINZ_DISCID`, `ACCURATERIPID`, and the `FAK_` TOC, AccurateRip
and CTDB checksums) are written for a single-file conversion only when it is an exact CD image: 44.1 kHz,
16-bit, stereo, every track boundary and the total length a whole number of 588-frame CD sectors. The result
is identical to `fak encode --cd-tags --cuesheet` on the same audio (checked). Other places CD tags come
from: `fak encode --cd-tags` and `fak edit --cd-tags` on the command line, and once a file has them they are
kept current by the component's tag writer. A file made by the Converter route below does not get them.

What is written into the file: the encoder string names both libraries' versions and the settings asked of
the encoder, for example `fak 1.1.0 (foo_input_fak 1.1.1; level=insane; fec=1/16; chunk=auto)` (the level is
not recoverable from the stream itself, which is why it is written). The command line writes the same form
(`fak 1.1.0 (level=normal; fec=none; chunk=auto)`). Lossy sources are refused; 8/16/24/32-bit integer and 32-bit float sources are supported (float streams play back as the source's exact float32 values). This is the
foobar2000 SDK's context-menu and preferences API; the Converter's own list cannot get a built-in-style
entry with sliders (see below), and this path does not apply the Converter's DSPs.

## Encoding with the Converter

On its first start the component adds a `FAK` entry to Converter setup -> Output format. It is
an ordinary custom encoder preset, so Edit changes it (add `--max` or `--insane` to the parameters
for the slower, smaller levels; see `fak help`) and Remove deletes it for good: it is added once per
profile, never re-added. Later starts only repair its encoder path if that `fak.exe` no longer
exists (component moved, reinstalled, or switched between the two components). The preset is:

- Encoder file: `fak.exe` (installed with the component, in `profile\user-components-x64\foo_input_fak\` or `...\foo_input_fak_adv\`)
- Extension: `fak`
- Parameters: `encode - %d`
- Format is: lossless; Highest BPS mode supported: 24

`fak encode -` reads the WAV foobar2000 pipes to standard input. To add it by hand (e.g. after
removing it), use Add New -> Custom with the values above.

This is the way to run the original Converter with FAK, and it keeps the Converter's own features (DSPs,
file name formatting, its output styles). Checked in foobar2000 v2.25.10: with Output style "Generate
multi-track files" the Converter writes one `.fak` with an embedded cue sheet for the selected tracks
(audio identical to joining them, cue sheet with the right tracks). Differences from the right-click
route: the encoder string carries no plugin version, there are no CD tags, and the preset says "Highest
BPS mode supported: 24" (not tested what the Converter then does with a 32-bit source; the menu route
is the one verified to keep 32-bit integer and float sources exact).

How it works: the desktop Converter ignores the SDK's `fb2k::audioEncoder` service (checked:
an encoder registered through it is never queried), and there is no API for adding output
formats. The list is the undocumented `converter.formats` blob in foobar2000's config store; its
layout (documented in `src/converter_preset.cpp`) was found by inspection of v2.25.10. The
component edits it before foo_converter reads its settings, and leaves it untouched if the blob
does not parse exactly. If the user never edited the list, the blob is absent and the Converter
uses built-in defaults; the component then writes v2.25.10's default list plus FAK.

## Building

Requirements: Rust (stable, MSVC target), Visual Studio 2022 (or its Build Tools) with the C++ workload, and the
foobar2000 SDK unpacked to `sdk\` next to this file (or anywhere, with `-Fb2kSdk <path>`); download it from
https://www.foobar2000.org/SDK (tested with SDK-2026-09-17). The SDK is not redistributed here.

```
pwsh build.ps1                    # both components
pwsh build.ps1 -Component basic   # foo_input_fak only (or: adv)
```

builds the Rust library in `capi\`, builds the `fak.exe` command-line tool from the FAK-Codec revision pinned in
`capi/Cargo.toml` and `build.ps1`, and produces `build\Release\foo_input_fak.fb2k-component` and
`foo_input_fak_adv.fb2k-component`. Install one of them by double-clicking, or via Preferences -> Components ->
Install.

Layout: `src/common/` is built into both (input, album art, cue sheets, the Converter preset), `src/basic/` and
`src/adv/` hold what is specific to each (`adv/` has the menu, the conversion engine, the settings and the
preferences page), and `capi/` is the Rust library both link.

## Format compatibility

The decoder accepts exactly one format version (v21), shown in the technical info as `fak_version`; the
format is specified in [FAK-Codec](https://github.com/inteliboy/FAK-Codec/blob/main/docs/bitstream-spec.md). Files of another version are rejected with an error
rather than misdecoded.

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or [MIT license](LICENSE-MIT) at your
option. The foobar2000 SDK has its own license and is not part of this repository.
