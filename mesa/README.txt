Mesa3D for Windows (Software OpenGL Renderer)
=============================================
Provides OpenGL 3.3+ on CPU via llvmpipe.
Required only when "Software (CPU)" renderer is selected in the launcher.

DLLs needed:
  - opengl32.dll        (Mesa OpenGL wrapper)
  - mesa_driver_llvmpipe.dll (software rasterizer)
  - LLVM-C.dll          (LLVM JIT backend)

To download, run: download_mesa.bat
Or download manually from:
  https://github.com/pal1000/mesa-dist-win/releases/tag/26.1.3
  (extract x64/opengl32.dll, x64/mesa_driver_llvmpipe.dll, x64/LLVM-C.dll here)
