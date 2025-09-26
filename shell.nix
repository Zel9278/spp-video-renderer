{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    # Build tools
    xmake
    gcc
    binutils
    pkg-config
    
    # OpenGL and windowing libraries
    glfw
    imgui
    libGL
    libGLU
    libglvnd.dev
    mesa
    
    # X11 libraries
    xorg.libX11
    xorg.libXcursor
    xorg.libXrandr
    xorg.libXinerama
    xorg.libXi
    xorg.libXext
    
    # Other dependencies
    cmake
    git
  ];
  
  shellHook = ''
    echo "Development environment for spp-video-renderer"
    echo "All dependencies are now available"
    
    # Set up environment variables for xmake
    export PKG_CONFIG_PATH="${pkgs.glfw}/lib/pkgconfig:${pkgs.libGL}/lib/pkgconfig:${pkgs.mesa}/lib/pkgconfig:$PKG_CONFIG_PATH"
    export LD_LIBRARY_PATH="${pkgs.glfw}/lib:${pkgs.imgui.lib}/lib:${pkgs.libGL}/lib:${pkgs.mesa}/lib:${pkgs.xorg.libX11}/lib:$LD_LIBRARY_PATH"
    export C_INCLUDE_PATH="${pkgs.glfw}/include:${pkgs.imgui}/include:${pkgs.libGL}/include:${pkgs.libglvnd.dev}/include:${pkgs.mesa}/include:${pkgs.xorg.libX11}/include:$C_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="$C_INCLUDE_PATH:$CPLUS_INCLUDE_PATH"
    
    # Debug: Show where GL headers are located
    echo "GL headers should be in: ${pkgs.libglvnd.dev}/include"
    echo "Mesa headers should be in: ${pkgs.mesa}/include"
    echo "ImGui lib should be in: ${pkgs.imgui.lib}/lib"
  '';
}
