# MPP Video Renderer Launcher

This ImGui-based launcher lets you configure every option required by the `MPP Video Renderer` executable from a friendly GUI. You can pick MIDI and optional audio files, select codecs, tweak resolution, and monitor the renderer output without touching the command line.

## Highlights

- Browse and select the renderer executable, the target MIDI file, and an optional audio track (uses the native Windows dialog, or zenity/qarma/kdialog on Linux when available).
- Choose from the bundled list of software and hardware video codecs, and pick the rendering backend (OpenGL, Vulkan, or DirectX 12 on Windows).
- Configure resolution, toggle the debug overlay, and enable the live preview window.
- Watch standard output and error streams from the renderer in real time.
- Scroll or auto-scroll through the log window to keep track of progress.

## Build

The launcher is registered as an additional xmake target. Build it with:

```powershell
xmake build "MPP Video Renderer Launcher"
```

> By default the build runs in Release mode. If you prefer a Debug build, run `xmake f -m debug` before building.

## Run

```powershell
xmake run "MPP Video Renderer Launcher"
```

You can also start `build/windows/x64/release/MPP Video Renderer Launcher.exe` directly.

## Usage

1. **MPP Video Renderer executable** – Normally defaults to `MPP Video Renderer.exe` next to the launcher. Point it elsewhere if needed.
2. **MIDI file** – Select the `.mid` or `.midi` file you want to render.
3. **Audio file (optional)** – Provide an external audio track to mux. Leave blank to render video only.
4. **Renderer backend, codec, resolution** – Select the GPU backend (OpenGL, Vulkan, DirectX 12 on Windows) and video codec, then enter the desired width and height. These map directly to the renderer’s command-line options.
5. **Debug overlay / Preview** – Toggle the check boxes to pass `--debug` and `--show-preview` to the renderer.
6. Review the settings and click **Start rendering**. The renderer launches and its output appears live in the log pane.

Disable **Auto-scroll** in the log pane if you want to browse earlier output without snapping back to the latest line.

## Notes

- On Windows the **Browse** buttons use the native file dialog. On other platforms, enter paths manually for now.
- On Linux the launcher attempts to use `zenity`, `qarma`, or `kdialog` for file selection; if none are installed, the text boxes remain available for manual entry.
- The preview window toggle is only available with the OpenGL backend (Vulkan/DirectX 12 currently render headless).
- The **Start rendering** button is disabled while a render is running and becomes available again after completion or failure.
- The resulting video still depends on the behaviour of `MPP Video Renderer`; this launcher only orchestrates parameters and process execution.
