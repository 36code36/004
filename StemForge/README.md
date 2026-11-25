# StemForge (FL Studio VST3)

Drag a song onto the window, pick a stem (piano, drums, bass, vocals, guitar, other).
- **Audio:** the stem is loaded into a sampler. Play it from the piano roll; each key pitches it.
  Use Start/Length to choose the part of the song, Root note to say which key plays it unpitched.
- **MIDI:** press "Make MIDI", then drag the green box into the FL Studio piano roll
  (double-click the box to open the .mid in Explorer).

## 1. Build (Windows)
Needs: Visual Studio 2022 (C++ desktop workload), CMake, Git.

    cmake -B build -G "Visual Studio 17 2022"
    cmake --build build --config Release

Copy `build\StemForge_artefacts\Release\VST3\StemForge.vst3` (a folder) to
`C:\Program Files\Common Files\VST3\`, then in FL Studio: Options > Manage plugins > Find installed plugins.
StemForge appears as an instrument.

## 2. Separation + MIDI engines (outside the plugin)
The AI models run as helper programs, so the plugin stays small and FL does not freeze.

    python -m venv C:\StemForge\venv
    C:\StemForge\venv\Scripts\pip install demucs basic-pitch

If pip complains about the Python version, use Python 3.10 or 3.11 for this venv.

On first run the plugin creates `%APPDATA%\StemForge\backend.txt`. Point it at the venv:

    separate="C:\StemForge\venv\Scripts\python.exe" -m demucs -n htdemucs_6s -o "{outdir}" "{input}"
    midi="C:\StemForge\venv\Scripts\basic-pitch.exe" "{outdir}" "{input}"

If something fails, look at `%APPDATA%\StemForge\last_log.txt`.

## Notes
- Stems are cached in `%APPDATA%\StemForge\cache`, so a song is only separated once.
  Switching the stem dropdown afterwards is instant. The 6 stems come from the htdemucs_6s model.
- FL saves the chosen stem with the project and reloads it.
- Separation of a 3-4 minute song takes about 1-5 minutes on CPU (much faster on an NVIDIA GPU).
- There is no separate "lead" stem. Synth leads end up in "other" or "vocals".
- MIDI from drums or dense chords is rough. Piano, bass and single melodies convert best.
