Veyra 2.0.2 AMD lmxxf NR experimental portable

The MIT lmxxf runtime DLL is included.
Neural assets/weights are NOT redistributed here.

To enable AMD NR:
1. Find an existing working dlss5-on-amd package on your PC.
2. Open its folder: DLSS5-AMD\native-game-tiled-assets
3. Copy ALL CONTENTS of native-game-tiled-assets into this folder:
   runtime\experimental\amd-lmxxf
   This folder should then contain HIP\SHA256SUMS, .hsaco modules, weights,
   noise.f32 and codec HLSL files.
4. Start Veyra-AMD-NR.exe.
5. In the NR runtime selector choose: AMD RDNA4 · lmxxf.
6. First test: SDR, one NR layer, strength around 1.0.

This build intentionally does not bundle NVIDIA neural weights.
