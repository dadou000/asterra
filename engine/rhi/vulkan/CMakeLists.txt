find_package(Vulkan REQUIRED)

add_library(OrbitRhiVulkan STATIC
    src/VulkanDevice.cpp
    src/VulkanQueue.cpp
    src/VulkanCommands.cpp
    src/VulkanPipeline.cpp
    src/VulkanResources.cpp
    src/VulkanSwapchain.cpp
    src/VulkanMemoryAllocatorImpl.cpp
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
)

target_compile_features(OrbitRhiVulkan PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRhiVulkan)
