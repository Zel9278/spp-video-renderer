# MPP Video Renderer

```
xmake f -m release
xmake build "MPP Video Renderer"
xmake run "MPP Video Renderer" path/to/song.mid
```

## Command-line help
Run the executable with `--help` (or `-h`) to see the full reference:

```
MPP Video Renderer --help
```

Common flags:
- `--video-codec`, `-vc <codec>` – choose the FFmpeg encoder (e.g. `libx264`, `h264_nvenc`)
- `--resolution`, `-r <width>x<height>` – output resolution (defaults to 1920x1080)
- `--bitrate`, `-br <value>` – target bitrate (`40M`, `5000k`, `25mbps`, ...)
- `--audio-file`, `-af <path>` – optional audio track to mux
- `--debug`, `-d` – overlay internal stats on the video
- `--show-preview`, `-sp` – open a 1280×720 preview window while rendering
- `--color-mode`, `-cm <mode>` – color notes by `channel`, `track`, or `both`
- `--cbr` / `--vbr` – switch between constant and variable bitrate

Software codecs such as `libx264`, `libx265`, `libvpx-vp9` and hardware encoders like `h264_nvenc`, `h264_qsv`, or `h264_amf` are supported when available.

## Launcher
An ImGui-based launcher is included as an additional target: `MPP Video Renderer Launcher`.

- Build: `xmake build "MPP Video Renderer Launcher"`
- Run: `xmake run "MPP Video Renderer Launcher"`

It lets you pick MIDI/audio files, choose codecs and resolution, toggle debug/preview flags, and start renders without touching the command line. Renderer stdout/stderr is streamed into the launcher log window for easy monitoring.

## License
This project is MIT License
