-- XMake project file for OpenGL Piano Keyboard
-- Target: Windows MSVC, OpenGL piano keyboard application

-- Set C++ standard
set_languages("cxx17")

-- Set target platform and toolchain
set_arch("x64")
if is_plat("windows") then
    set_toolchains("msvc")
end

-- Add packages for OpenGL and window management
add_requires("glfw", "glad", "imgui[glfw_opengl3]", "stb")

-- Define the target
target("piano_keyboard_video_renderer")
    set_kind("binary")
    set_default(true)

    -- Add source files
    add_files("main.cpp", "opengl_renderer.cpp", "piano_keyboard.cpp", "keyboard_mapping.cpp", "midi_video_output.cpp")

    -- Add header files
    add_headerfiles("*.h")

    -- Add include directories
    add_includedirs(".", "midi-parser")

    -- Add dependency on midi_parser library
    add_deps("midi_parser")

    -- System libraries for Windows
    if is_plat("windows") then
        add_links("opengl32", "gdi32", "user32", "kernel32", "shell32")
        add_syslinks("opengl32", "gdi32", "user32", "kernel32", "shell32")
    end

    -- Add packages
    add_packages("glfw", "glad", "imgui", "stb")

    -- Math library (for some platforms)
    if not is_plat("windows") then
        add_links("m")
    end

    -- Compiler and linker flags
    if is_plat("windows") then
        add_cxflags("/W3")  -- Warning level 3
        add_cxflags("/EHsc") -- Exception handling
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
    set_targetdir("$(builddir)/lib")
    
    -- Compiler flags
    if is_plat("windows") then
        add_cflags("/TC")  -- Compile as C
        add_cflags("/W3")  -- Warning level 3
        add_defines("_CRT_SECURE_NO_WARNINGS")
    else
        add_cflags("-std=c99", "-Wall", "-Wextra")
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
    set_targetdir("$(builddir)/bin")
    
    -- Compiler flags
    if is_plat("windows") then
        add_cflags("/TC")  -- Compile as C
        add_cflags("/W3")  -- Warning level 3
        add_defines("_CRT_SECURE_NO_WARNINGS")
    else
        add_cflags("-std=c99", "-Wall", "-Wextra")
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
target("piano_keyboard_video_renderer")
    add_rules("check_dependencies")