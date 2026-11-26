Third-party components of the RIFE frame-generation backend (#248)
==================================================================

Builds with ENABLE_RIFE link the following statically and ship the RIFE
model next to the other assets (assets/models/rife-v4.6/ or
share/retrocapture/models/rife-v4.6/):

- ncnn (https://github.com/Tencent/ncnn), release 20260526, built with
  NCNN_VULKAN. BSD 3-Clause, see ncnn-LICENSE.txt (includes the notices of
  its bundled components).
- glslang (bundled with ncnn, compiles the Vulkan compute shaders at run
  time). See glslang-LICENSE.txt.
- rife-ncnn-vulkan (https://github.com/nihui/rife-ncnn-vulkan, commit
  a7532fc): the RIFE v4 graph runner, the rife.Warp layer and the compute
  shaders adapted in src/processing/rife/RifeEngine.cpp, and the ncnn
  conversion of the RIFE 4.6 model (flownet.param / flownet.bin). MIT, see
  rife-ncnn-vulkan-LICENSE.txt.
- RIFE 4.6 model weights by hzwer (https://github.com/hzwer/Practical-RIFE,
  paper "Real-Time Intermediate Flow Estimation for Video Frame
  Interpolation", ECCV 2022). MIT, see RIFE-model-LICENSE.txt.

The Vulkan loader is not linked: ncnn loads the system's (libvulkan.so.1,
vulkan-1.dll, libvulkan.dylib / MoltenVK) at run time.
