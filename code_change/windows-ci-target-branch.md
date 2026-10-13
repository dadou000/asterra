# Windows validation for the current Studio branch

Existing owner: `.github/workflows/orbit-windows.yml`.
Add `cleanup/experimental-studio` to its existing push and pull-request branch
filters so changes targeting the current Studio implementation receive the
same clean Windows builds and non-GPU CTest suite as the older development
branches. Keep the existing build matrix, executable checks and GPU exclusion;
real Vulkan dispatch still requires a capable runner. No second workflow or
engine/runtime behavior is introduced.
