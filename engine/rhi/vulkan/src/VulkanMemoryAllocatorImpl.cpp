// vk_mem_alloc.h is a third-party single-header library: exactly one
// translation unit must define VMA_IMPLEMENTATION before including it to
// generate the function bodies every other file only declares.
//
// Orbit builds its own code with warnings-as-errors in CI. Keep that policy for
// Orbit sources, but suppress warnings emitted by VMA's implementation itself.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4127) // conditional expression is constant
#pragma warning(disable : 4189) // initialized but unreferenced local variable
#pragma warning(disable : 4324) // structure padded due to alignment specifier
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
