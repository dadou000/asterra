add_library(OrbitRhiD3D12 STATIC
    src/D3D12Device.cpp
    src/D3D12Queue.cpp
    src/D3D12Commands.cpp
    src/D3D12Swapchain.cpp
)

add_library(Orbit::RhiD3D12 ALIAS OrbitRhiD3D12)

target_include_directories(OrbitRhiD3D12
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRhiD3D12
    PUBLIC
        Orbit::RHI
    PRIVATE
        d3d12
        dxgi
        dxguid
)

target_compile_features(OrbitRhiD3D12 PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRhiD3D12)
