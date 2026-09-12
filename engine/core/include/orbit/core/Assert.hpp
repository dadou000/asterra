#pragma once

#include <cassert>

#if defined(NDEBUG)
    #define ORBIT_ASSERT(condition) ((void)0)
#else
    #define ORBIT_ASSERT(condition) assert(condition)
#endif
