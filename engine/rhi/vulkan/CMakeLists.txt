find_package(Vulkan REQUIRED)

include(FetchContent)

# VMA is header-only, but it is not part of the Vulkan SDK. Fetch a pinned
# release so clean developer/CI machines build the Vulkan backend without
# requiring a manually installed vk_mem_alloc.h.
set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

add_library(OrbitRhiVulkan STATIC
    src/VulkanDevice.cpp
    src/VulkanQueue.cpp
    src/VulkanCommands.cpp
    src/VulkanPipeline.cpp
    src/VulkanResources.cpp
    src/VulkanSwapchain.cpp
    src/VulkanMemoryAllocatorImpl.cpp
    src/RenderDocCapture.cpp
)

add_library(Orbit::RhiVulkan ALIAS OrbitRhiVulkan)

target_include_directories(OrbitRhiVulkan
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRhiVulkan
    PUBLIC
        Orbit::RHI
    PRIVATE
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
)

target_compile_features(OrbitRhiVulkan PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRhiVulkan)
