# neko-surround

A GStreamer element, `nekosurround`, that turns 5.1 audio into stereo for neko's selectable audio tracks:

| `mode`            | What it does |
|-------------------|--------------|
| `plii`            | Dolby Pro Logic II matrix encode. Plays as normal stereo, and a Pro Logic II (or IIx/z) receiver decodes it back to 5.1. |
| `dolby-headphone` | Dolby Headphone virtualization: the bundled lakehp engine with the room filters read from your own `DolbyHph.dll`. |
| `stereo`          | Plain ITU downmix. Also used automatically when Dolby Headphone is unavailable (no DLL, or no filter for the sample rate). |

Input is 6 or 8 channel float (5.1 with rear or side surrounds, or 7.1), output is 2 channel float, so put `audioconvert` on both sides:

```
pulsesrc device=audio_output.monitor ! audio/x-raw,channels=6 ! audioconvert
  ! nekosurround mode=plii ! audioconvert ! opusenc ! ...
```

## Properties

| Property     | Default                          | |
|--------------|----------------------------------|-|
| `mode`       | `plii`                           | `plii`, `dolby-headphone`, `stereo`; can change while playing |
| `lfe-level`  | `0.5`                            | linear gain of the LFE channel mixed into both outputs for `plii` and `stereo` (Pro Logic II itself has no LFE; Dolby Headphone mixes LFE itself) |
| `gain`       | `0.707`                          | linear output gain; a peak limiter keeps the result from clipping |
| `dh-dll`     | `/etc/neko/dolby/DolbyHph.dll`   | your DolbyHph.dll, read when the stream starts |
| `dh-room`    | `1`                              | 1 small (DH1), 2 medium (DH2), 3 large (DH3); can change while playing |
| `dh-surround-angle` | `118`                     | azimuth of the surround speakers (the sides in 7.1), 90..165 degrees from straight ahead; 118 = the filters as made |
| `dh-rear-angle` | `0`                           | azimuth of the 7.1 rear speakers, 90..165; 0 = same as the surrounds |
| `dh-rear-auto` | `true`                         | 7.1 input with silent sides is 5.1 content: play its back channels through the surround speakers |

## Pro Logic II encoding

```
Lt = L + 0.707 C + lfe·LFE − j(0.8718 SL + 0.4899 SR)
Rt = R + 0.707 C + lfe·LFE + j(0.4899 SL + 0.8718 SR)
```

`j` is a 90° phase shift, made with two chains of four 2nd‑order allpass sections (Olli Niemitalo's design). Every channel passes through the same network, so fronts and surrounds keep identical group delay; the phase difference stays within 90 ±0.7° from about 25 Hz to 20 kHz. `tests/test_dsp.c` checks the phase response, the matrix coefficients, that surrounds come out anti-phase and that a passive decode keeps a rear source out of the center.

Keep the Opus bitrate at 128 kbit/s or higher (neko's default): low bitrates enable intensity stereo, which damages the phase information a Pro Logic II decoder relies on.

## Dolby Headphone

`lakehp/` is lakehp's engine (`lakehp.c/.h`, unchanged): a portable C99 reimplementation of the Lake Technology / Dolby Headphone engine from `DolbyHph.dll` 1.20.0276, matching the DLL to ±1 LSB. It contains no filter data. When the track starts, `src/lhp_dll.c` (a C version of lakehp's `tools/extract_filters.py`) scans your DLL for the filter of the selected room at the stream's sample rate and loads it into the engine. Nothing runs the DLL's own code, so there is no Wine, no licence-registry gate, and it works on any CPU.

- Rooms DH1, DH2 and DH3 exist at 48, 44.1, 32, 22.05, 16, 11.025 and 8 kHz.
- The engine renders 1024-frame blocks, so it adds one block of latency (21.3 ms at 48 kHz), reported in latency queries.
- A room or angle change swaps the filter in place while playing.
- About 0.5% of one x86 core for 48 kHz 5.1, 0.8% for 7.1 with the rear pair at its own angle.

### Speaker placement and 7.1

The neko copy of the engine adds the placement model and the 7.1 rear pair from the LakeHP technical breakdown (sections 4 and 5):

- The factory surround HRTFs sit at about 118°, which tends to sound "beside you". An angle moves a pair with a Woodworth head model (r = 8.75 cm): a fractional near-ear delay sets the time difference, a 2 kHz far-ear shelf sets the level difference, and three peaking filters (1, 4.5 and 11 kHz) add the "behind" cues. These are applied to the pair's direct HRTFs when the filter loads; the room tail is unchanged.
- 7.1 sides play on the engine's surround positions. The backs get a rear pair built from the same surround HRTFs with their own angle and the surround room sends. When both angles are equal, the backs are mixed into the surrounds at no extra cost, which is identical to playing them through the surrounds.
- Pro Logic II and the stereo fallback have a single surround pair, so they mix the backs into it.
- 118° gives exactly the factory filters, so 5.1 output stays bit-identical to the original port.
- If the DLL can't be read, isn't DolbyHph.dll, or has no filter for the rate, the element posts one warning and plays the stereo downmix.

`make test DLL=DolbyHph.dll` checks the element's output against the engine sample for sample (including a room change and a rear-angle change in 7.1), that the scan finds all 22 filter blobs, and that a source on one side lands in that ear (+7.6 dB with DH1). `tests/test_lakehp71.c` checks the breakdown's angle table and its 7.1 identities: factory backs = 5.1 surrounds, a rear pair at 150° = 5.1 surrounds at 150°, side + back = sum of the parts, and a surround moving back loses time and level difference as its angle grows.

## Building

```sh
make plugin                  # libgstnekosurround.so (GStreamer dev packages)
make test                    # DSP tests, element tests through GStreamer
make test DLL=DolbyHph.dll   # plus the Dolby Headphone checks
make install DESTDIR=…       # into GStreamer's plugin dir
```

See `examples/surround` for a complete neko image.
