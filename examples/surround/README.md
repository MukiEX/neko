# Neko - Surround Example

Firefox and VLC on a 5.1 virtual output, offered to viewers as two stereo audio tracks. Each viewer picks one under **Settings → Audio Track** (the choice is remembered in their browser):

- **Pro Logic II** (default) – stereo that a Pro Logic II receiver decodes back to 5.1; sounds like normal stereo everywhere else.
- **Dolby Headphone** – virtual 5.1 for headphones: the lakehp engine (a C port of the Lake/Dolby Headphone engine) with the room filters read from your own `DolbyHph.dll`.

```
VLC (5.1) ─┐                                  ┌─ nekosurround mode=plii ────────────┐
           ├─► audio_output (5.1 null sink) ──┤                                     ├─► Opus ─► viewer's choice
Firefox ───┘   stereo stays on front L/R      └─ nekosurround mode=dolby-headphone ─┘
                                                  (lakehp + filters from DolbyHph.dll)
```

A track's pipeline only runs while somebody listens to it.

## Build

The neko images must be built from this branch (selectable audio tracks are new), then this image on top. From the repository root:

```sh
./build -y                 # ghcr.io/m1k1o/neko/base:latest
./build -y -a firefox      # ghcr.io/m1k1o/neko/firefox:latest
docker build -f examples/surround/Dockerfile -t neko-surround .
```

The image build also runs the DSP and element tests. Nothing runs the DLL itself (no Wine), so the image works on amd64 and arm64, and it contains no Dolby data.

## Run

```sh
cd examples/surround
mkdir -p media
cp /path/to/DolbyHph.dll .
docker compose up -d
```

`DolbyHph.dll` is required: `docker compose up` refuses to start without it. Put media for VLC into `media/`.

Firefox and VLC both run by default. To run only one, set `NEKO_SURROUND_APPS`:

```sh
NEKO_SURROUND_APPS=vlc docker compose up -d        # VLC only
NEKO_SURROUND_APPS=firefox docker compose up -d    # Firefox only
docker compose up -d                               # both
```

Or set it permanently in `docker-compose.yaml` (or a `.env` file next to it). Changing it takes a `docker compose up -d`, which recreates the container.

## Dolby Headphone DLL

Use `DolbyHph.dll` version 1.20.0276 (Lake Technology, "Dolby Headphone Engine"). When the Dolby Headphone track starts, the element reads the DLL, picks the filter for the room and the sample rate (DH1/DH2/DH3 exist at 48, 44.1, 32, 22.05, 16, 11.025 and 8 kHz), and renders with lakehp, about 0.5% of one CPU core. Change the room with `dh-room` in `neko.yaml`.

If the DLL can't be read or has no matching filter, the track plays a plain stereo downmix and neko's log says why (`Dolby Headphone unavailable`).

## Files

| File | |
|------|-|
| `default.pa` | `audio_output` becomes a 5.1 sink |
| `daemon.conf` | keeps stereo apps on front L/R instead of copying them to the surrounds |
| `neko.yaml` | the two audio tracks (room, LFE level, gain are set here) |
| `supervisord.conf` | VLC |
| `run-app.sh` | starts Firefox / VLC only when listed in `NEKO_SURROUND_APPS` |
