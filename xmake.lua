-- XMake project file for OpenGL Piano Keyboard
-- Target: Windows MSVC, OpenGL piano keyboard application

-- Set C++ standard
set_languages("cxx17")

-- Set target platform and toolchain
set_arch("x64")
add_requires("vulkan-headers")
if is_plat("windows") then
    add_requires("vulkan-loader")
end

if is_plat("windows") then
    set_toolchains("msvc")
    -- Add packages for OpenGL and window management on Windows
    add_requires("glfw", "glad", "imgui[glfw_opengl3]", "stb", "shaderc")
elseif is_plat("linux") then
    -- For NixOS: Only use packages that can't be provided by system
    add_requires("glad", "imgui[glfw_opengl3]", "stb", "shaderc")
end

-- Define the target
target("MPP Video Renderer")
    set_kind("binary")
    set_default(true)
    add_rules("utils.bin2c", {extensions = {".ico", ".png"}})

    -- Add source files
    add_files("main.cpp", "opengl_renderer.cpp", "directx12_renderer.cpp", "vulkan_renderer.cpp", "piano_keyboard.cpp", "midi_video_output.cpp", "resources/window_icon_loader.cpp")
    add_files("resources/icon.png")

    -- Add header files
    add_headerfiles("*.h")

    -- Add Windows resource file
    if is_plat("windows") then
        add_files("resources/app.rc", "resources/icon.ico")
    end

    -- Add include directories
    add_includedirs(".", "midi-parser")
    on_config(function (target)
        local generated_dir = target:autogendir() .. "/rules/utils/bin2c"
        os.mkdir(generated_dir)
        target:add("includedirs", generated_dir)
    end)

    -- Add dependency on midi_parser library
    add_deps("midi_parser")

    -- System libraries for Windows
    if is_plat("windows") then
    add_links("opengl32", "gdi32", "user32", "kernel32", "shell32", "d3d12", "dxgi", "d3dcompiler", "vulkan-1")
    add_syslinks("opengl32", "gdi32", "user32", "kernel32", "shell32", "comdlg32", "d3d12", "dxgi", "d3dcompiler", "vulkan-1")
    end

    -- Add packages
    if is_plat("windows") then
    add_packages("glfw", "glad", "imgui", "stb", "shaderc", "vulkan-headers", "vulkan-loader")
    elseif is_plat("linux") then
        -- Use system GLFW and system OpenGL
        if os.getenv("NIX_PROFILES") then
            -- NixOS: Use only essential packages, use system for rest
            add_packages("glad", "stb", "shaderc", "vulkan-headers")
        else
            -- Other Linux: Use xmake packages
            add_packages("glad", "imgui", "stb", "shaderc", "vulkan-headers")
        end
        -- Link system GLFW directly
    add_links("glfw")
    end

    -- Platform specific libraries and settings
    if is_plat("linux") then
    add_links("dl", "pthread", "m", "GL", "vulkan")
        -- System X11 libraries - these should be available on most Linux systems
        add_syslinks("X11", "Xcursor", "Xrandr", "Xinerama", "Xi", "Xext")
        
        -- NixOS: Add ImGui library if available in system
        if os.getenv("NIX_PROFILES") then
            -- Get ImGui library path from environment
            local ld_library_path = os.getenv("LD_LIBRARY_PATH")
            if ld_library_path then
                for single_path in ld_library_path:gmatch("([^:]+)") do
                    if single_path:find("imgui") and os.exists(single_path) then
                        add_linkdirs(single_path)
                    end
                end
            end
            add_links("imgui")
        end
        
        -- NixOS specific paths
        local nixos_profile = os.getenv("NIX_PROFILES")
        if nixos_profile then
            -- Add Nix store paths
            add_linkdirs("/nix/store/*/lib")
            add_includedirs("/nix/store/*/include")
            
            -- Try to find headers in common Nix paths
            local nix_include_paths = {
                os.getenv("C_INCLUDE_PATH"),
                os.getenv("CPLUS_INCLUDE_PATH"),
                "/run/current-system/sw/include",
                os.getenv("HOME") .. "/.nix-profile/include"
            }
            
            for _, path in ipairs(nix_include_paths) do
                if path then
                    -- Split multiple paths by colon
                    for single_path in path:gmatch("([^:]+)") do
                        if os.exists(single_path) then
                            add_includedirs(single_path)
                        end
                    end
                end
            end
            
            -- Add library paths
            local nix_lib_paths = {
                os.getenv("LD_LIBRARY_PATH"),
                "/run/current-system/sw/lib",
                os.getenv("HOME") .. "/.nix-profile/lib"
            }
            
            for _, path in ipairs(nix_lib_paths) do
                if path then
                    -- Split multiple paths by colon
                    for single_path in path:gmatch("([^:]+)") do
                        if os.exists(single_path) then
                            add_linkdirs(single_path)
                        end
                    end
                end
            end
        end
        
        -- NixOS doesn't have standard Linux paths, so we rely only on Nix paths
        if not nixos_profile then
            -- Standard Linux paths as fallback for non-NixOS systems
            add_linkdirs("/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib")
            add_includedirs("/usr/include")
        end
    end

    -- Compiler and linker flags
    if is_plat("windows") then
        add_cxflags("/W3")  -- Warning level 3
        add_cxflags("/EHsc") -- Exception handling
        add_cxflags("/utf-8")
        add_cflags("/utf-8")
        add_defines("_CRT_SECURE_NO_WARNINGS")
        add_defines("NOMINMAX")
        add_defines("WIN32_LEAN_AND_MEAN")
    end

    -- Debug configuration
    if is_mode("debug") then
        add_defines("DEBUG")
        set_symbols("debug")
        set_optimize("none")
    end

    -- Release configuration
    if is_mode("release") then
        add_defines("NDEBUG")
        set_symbols("hidden")
        set_optimize("fastest")
        set_strip("all")
    end

-- Additional configurations
option("enable_audio")
    set_default(false)
    set_showmenu(true)
    set_description("Enable audio support")
option_end()

if has_config("enable_audio") then
    add_defines("ENABLE_AUDIO")
end

-- Custom rules
rule("check_dependencies")
    on_config(function (target)
        print("Configuring OpenGL Piano Keyboard...")
        print("Target platform: " .. get_config("plat"))
        print("Target architecture: " .. get_config("arch"))
        print("Build mode: " .. get_config("mode"))
    end)
rule_end()

-- MIDI Parser static library
target("midi_parser")
    set_kind("static")
    set_basename("midi_parser")
    
    -- Add source files from midi-parser directory
    add_files("midi-parser/midi_parser.c")
    
    -- Add header files
    add_headerfiles("midi-parser/midi_parser.h")
    
    -- Add include directories
    add_includedirs("midi-parser")
    
    -- Output directory
    set_targetdir("$(projectdir)/build/lib")
    
    -- Compiler flags
    if is_plat("windows") then
        add_cflags("/TC")  -- Compile as C
        add_cflags("/W3")  -- Warning level 3
        add_cflags("/utf-8")
        add_defines("_CRT_SECURE_NO_WARNINGS")
    else
        add_defines("_GNU_SOURCE")
        if is_plat("linux") then
            add_cxflags("-Wall", "-Wextra", {force = true})
        end
    end
    
    -- Debug configuration
    if is_mode("debug") then
        add_defines("DEBUG")
        set_symbols("debug")
        set_optimize("none")
    end
    
    -- Release configuration
    if is_mode("release") then
        add_defines("NDEBUG")
        set_symbols("hidden")
        set_optimize("fastest")
    end

-- MIDI Parser example executable
target("midi_example")
    set_kind("binary")
    set_basename("midi_example")
    
    -- Add source files
    add_files("midi-parser/example.c")
    
    -- Add dependency on midi_parser library
    add_deps("midi_parser")
    
    -- Add include directories
    add_includedirs("midi-parser")
    
    -- Output directory
    set_targetdir("$(projectdir)/build/bin")
    
    -- Compiler flags
    if is_plat("windows") then
        add_cflags("/TC")  -- Compile as C
        add_cflags("/W3")  -- Warning level 3
        add_cflags("/utf-8")
        add_defines("_CRT_SECURE_NO_WARNINGS")
    else
        add_defines("_GNU_SOURCE")
        if is_plat("linux") then
            add_cxflags("-Wall", "-Wextra", {force = true})
        end
    end
    
    -- Debug configuration
    if is_mode("debug") then
        add_defines("DEBUG")
        set_symbols("debug")
        set_optimize("none")
    end
    
    -- Release configuration
    if is_mode("release") then
        add_defines("NDEBUG")
        set_symbols("hidden")
        set_optimize("fastest")
    end

-- Apply custom rule to main target
target("MPP Video Renderer")
    add_rules("check_dependencies")

-- ImGui-based launcher
target("MPP Video Renderer Launcher")
    set_kind("binary")
    set_default(true)
    add_rules("utils.bin2c", {extensions = {".ico", ".png"}})

    add_files("launcher/launcher_main.cpp", "resources/window_icon_loader.cpp", "resources/icon.png")
    add_includedirs("launcher")
    if is_plat("windows") then
        add_files("resources/icon.ico")
    end

    on_config(function (target)
        local generated_dir = target:autogendir() .. "/rules/utils/bin2c"
        os.mkdir(generated_dir)
        target:add("includedirs", generated_dir)
    end)

    -- Add Windows resource file
    if is_plat("windows") then
        add_files("resources/launcher.rc")
    end

    if is_plat("windows") then
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_cxflags("/utf-8")
        add_links("opengl32", "gdi32", "user32", "kernel32", "shell32", "ole32")
        add_syslinks("opengl32", "gdi32", "user32", "kernel32", "shell32", "ole32", "comdlg32")
    elseif is_plat("linux") then
        add_links("dl", "pthread", "m", "GL")
        add_links("X11", "Xcursor", "Xrandr", "Xinerama", "Xi", "Xext")
    end

    add_packages("glfw", "glad", "imgui", "stb")