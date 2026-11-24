# Kdenlive AutoSort

A small native C++ / Qt 6 desktop helper for building a camera-ordered Kdenlive timeline.

Drop or select multiple source clips. AutoSort orders them by camera filename convention, probes each source duration with FFprobe, and writes an OpenTimelineIO `.otio` timeline. Import that timeline in Kdenlive with **File → Import Timeline → OpenTimelineIO**. The original media stays where it is; the timeline references source files directly.

## Sorting

- **GoPro:** recognizes `GOPR####` and chaptered `GP01####`, `GX01####`, `GH01####`, and `GL01####` names. Chapter files sharing a take number stay together, in chapter order.
- **DJI:** groups `DJI_####` and timestamp-prefixed DJI names, using numeric ordering.
- **Sony:** groups common `C####`, `S####`, `M4V####`, and `DSC####` names, using numeric ordering.
- Other names use case-insensitive natural numeric order (so `clip2` comes before `clip10`).

Mixed camera types are grouped GoPro, DJI, Sony, then other names. The original filenames are not changed.

## Requirements

- CMake 3.21+
- C++17 compiler
- Qt 6 Widgets
- FFprobe available on PATH (usually installed with FFmpeg)

Kdenlive's native OpenTimelineIO import is available starting with Kdenlive 25.04.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Run `build/kdenlive-autosort` (Windows: `build/Release/kdenlive-autosort.exe`).

## Use

1. Open AutoSort.
2. Drag video clips into the list or choose **Add clips…**. The list re-sorts after every addition.
3. Choose **Create Kdenlive timeline…** and save the `.otio` file.
4. In Kdenlive, use **File → Import Timeline → OpenTimelineIO** and select the saved file.

AutoSort places clips consecutively on one video track in filename order. Audio in the original video files is available when Kdenlive imports the clips. If FFprobe cannot read a clip or the media is inaccessible, AutoSort reports the filename and does not write a partial timeline.

## Scope

This repository is a companion app written in Kdenlive's native C++/Qt stack. Kdenlive does not provide a public third-party plugin API for adding custom multi-file import actions to the Project Bin. OpenTimelineIO is Kdenlive's supported interchange path for handing it a generated timeline.
