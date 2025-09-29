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
- `--debug`, `-d` – overlay internal stats on the video (draw call count etc.)
- `--show-preview`, `-sp` – open a 1280×720 preview window while rendering
- `--color-mode`, `-cm <mode>` – color notes by `channel`, `track`, or `both`
- `--cbr` / `--vbr` – switch between constant and variable bitrate
- `--ffmpeg-path`, `-fp <path>` – specify FFmpeg executable path (default: system PATH)
- `--output-directory`, `-o <path>` – specify output directory for video files (default: executable dir)
- `--renderer`, `-rdr <backend>` – choose the rendering backend: `opengl` (default), `vulkan`, or `dx12` (Windows only)

Software codecs such as `libx264`, `libx265`, `libvpx-vp9` and hardware encoders like `h264_nvenc`, `h264_qsv`, or `h264_amf` are supported when available.

### Rendering backends
- **OpenGL** – GPU-accelerated path with optional on-screen preview (Windows & Linux)
- **Vulkan** – cross-platform GPU backend that renders headlessly for deterministic offline captures (preview window not yet supported)
- **DirectX 12** – Windows-only GPU backend (preview window not yet supported)

### Vulkan backend essentials
- Requires Vulkan 1.2+ headers and loader. When building with xmake the `vulkan-headers` and `vulkan-loader` packages are fetched automatically; otherwise install the Vulkan SDK (or provide the headers/lib) and ensure `vulkan-1` is on the link path.
- Runtime shader compilation relies on `shaderc`. xmake downloads the dependency on demand; if you compile manually, link against `shaderc_combined` (or the equivalent static/shared library).
- The renderer uploads draw calls to the GPU: rectangles, rounded gradients, borders, radial blends, and bitmap text now execute through a Vulkan graphics pipeline with instanced quads. Readback copies the final color image via `vkCmdCopyImageToBuffer` so offline captures benefit from device-side acceleration while keeping pixel parity with the other backends.
- Readback data is vertically flipped in place before piping frames to FFmpeg so Vulkan output matches the upright orientation produced by OpenGL and DirectX backends.


## Launcher
An ImGui-based launcher is included as an additional target: `MPP Video Renderer Launcher`.

- Build: `xmake build "MPP Video Renderer Launcher"`
- Run: `xmake run "MPP Video Renderer Launcher"`

## License
This project is MIT License\
Base Project([MPP Frontend](https://github.com/multiplayerpiano/mpp-frontend-v1)) is GPL-3.0 license
