cmake_minimum_required(VERSION 3.28)

# ═══════════════════════════════════════════════════════════════
#  Synapse-X Client (副机接收端)
#
#  Standalone build. On the Client machine:
#    cd client
#    cmake --preset windows-x64
#    cmake --build build_x64 --config RelWithDebInfo
#
#  Pipeline: UDP recv → LZ4 decompress → [TensorRT inference]
#
#  Directory layout:
#    include/             — public headers
#    src/                 — implementation + main
# ═══════════════════════════════════════════════════════════════

project(SynapseX_Client
    VERSION 1.0.0
    DESCRIPTION "Client receiver: UDP recv → LZ4 decompress → TensorRT inference"
    LANGUAGES C CXX
)

# ── C++17 ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Export compile commands (for clangd / IDE) ─────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Windows SDK ────────────────────────────────────────
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINIMAX)
endif()

# ── MSVC compile options ───────────────────────────────
if(MSVC)
    add_compile_options(
        /utf-8
        /W3
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
    )
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        $<$<CONFIG:Release>:NDEBUG>
    )
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Gy /Oi")
endif()

# ═══════════════════════════════════════════════════════════════
#  LZ4 (compiled directly from thirdparty source)
# ═══════════════════════════════════════════════════════════════
set(LZ4_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lz4-1.10.0/lib")
add_library(lz4_static STATIC
    "${LZ4_SRC_DIR}/lz4.c"
    "${LZ4_SRC_DIR}/lz4.h"
)
target_include_directories(lz4_static PUBLIC "${LZ4_SRC_DIR}")
if(MSVC)
    target_compile_options(lz4_static PRIVATE /W0)
endif()

# ═══════════════════════════════════════════════════════════════
#  Shared protocol headers (header-only)
# ═══════════════════════════════════════════════════════════════
add_library(SynapseX_Shared INTERFACE)
target_include_directories(SynapseX_Shared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/spdlog-1.17.0/include
)
target_compile_features(SynapseX_Shared INTERFACE cxx_std_17)
target_compile_definitions(SynapseX_Shared INTERFACE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# ═══════════════════════════════════════════════════════════════
#  CUDA 13.1 — find + compile .cu via nvcc custom command
# ═══════════════════════════════════════════════════════════════
set(SX_CUDA_FOUND FALSE)

find_package(CUDAToolkit)
if(CUDAToolkit_FOUND)
    message(STATUS "[SynapseX Client] CUDA Toolkit found via find_package:")
    message(STATUS "  Version:        ${CUDAToolkit_VERSION}")
    message(STATUS "  Include dirs:   ${CUDAToolkit_INCLUDE_DIRS}")
    message(STATUS "  Toolkit dir:    ${CUDAToolkit_TARGET_DIR}")
    set(SX_CUDA_FOUND TRUE)
    set(SX_CUDA_INCLUDE_DIRS ${CUDAToolkit_INCLUDE_DIRS})
    set(SX_CUDA_LIB_DIR     "${CUDAToolkit_TARGET_DIR}/lib/x64")
else()
    message(WARNING "[SynapseX Client] CUDA Toolkit not found. "
                    "Inference module will be disabled.")
endif()

# ═══════════════════════════════════════════════════════════════
#  TensorRT — 手动寻址或用户覆盖
# ═══════════════════════════════════════════════════════════════
set(SX_TENSORRT_FOUND FALSE)
set(TRT_BASE_PATH "$ENV{TRT_BASE_PATH}" CACHE PATH "TensorRT installation path")
if(NOT TRT_BASE_PATH)
    set(TRT_BASE_PATH "C:/Program Files/NVIDIA/TensorRT-10.16.1.11.Windows.amd64.cuda-13.2/TensorRT-10.16.1.11")
endif()

if(EXISTS "${TRT_BASE_PATH}/include/NvInfer.h")
    message(STATUS "[SynapseX Client] TensorRT found at: ${TRT_BASE_PATH}")
    set(SX_TENSORRT_FOUND TRUE)
    set(SX_TRT_INCLUDE_DIRS "${TRT_BASE_PATH}/include")

    if(EXISTS "${TRT_BASE_PATH}/lib")
        set(SX_TRT_LIB_DIR "${TRT_BASE_PATH}/lib")
    endif()

    if(SX_CUDA_FOUND)
        message(STATUS "[SynapseX Client] CUDA + TensorRT available. Enabling inference module.")
    endif()
else()
    message(WARNING "[SynapseX Client] TensorRT NOT found at ${TRT_BASE_PATH}. "
                    "Inference target will be skipped.")
endif()

# ═══════════════════════════════════════════════════════════════
#  TensorRT 推理模块 + CUDA preprocess kernel
# ═══════════════════════════════════════════════════════════════
if(SX_CUDA_FOUND AND SX_TENSORRT_FOUND)
    # Inference static lib
    add_library(SynapseX_Inference STATIC
        src/TrtInference.cpp
        src/CudaPreprocess.cpp
    )
    target_include_directories(SynapseX_Inference PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/spdlog-1.17.0/include
        ${SX_CUDA_INCLUDE_DIRS}
        ${SX_TRT_INCLUDE_DIRS}
    )
    target_compile_definitions(SynapseX_Inference PRIVATE NOMINMAX SX_HAS_CUDA=1 SX_HAS_INFERENCE=1)
    target_link_libraries(SynapseX_Inference PUBLIC
        ${SX_TRT_LIB_DIR}/nvinfer_10.lib
        ${SX_TRT_LIB_DIR}/nvinfer_plugin_10.lib
        ${SX_CUDA_LIB_DIR}/cudart.lib
        ${SX_CUDA_LIB_DIR}/cuda.lib
        ${SX_CUDA_LIB_DIR}/nvrtc.lib
    )
    set(SX_HAS_INFERENCE TRUE)
else()
    set(SX_HAS_INFERENCE FALSE)
endif()

# ═══════════════════════════════════════════════════════════════
#  Client module sources
# ═══════════════════════════════════════════════════════════════
set(CLIENT_SOURCES
    src/UdpReceiver.cpp
    src/PdController.cpp
    src/MouseDevice.cpp
    src/HttpTuner.cpp
    src/TrtInference.cpp
    src/main.cpp
)

set(CLIENT_LIBS
    SynapseX_Shared
    lz4_static
    ws2_32
)

# ── MAKCU 鼠标控制器（可选）─────────────────────────────
find_package(makcu-cpp)

# ── Main executable ────────────────────────────────────
add_executable(SynapseX_Client ${CLIENT_SOURCES})
target_include_directories(SynapseX_Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/cpp-httplib-0.47.0
)
target_compile_definitions(SynapseX_Client PRIVATE NOMINMAX)
if(SX_CUDA_FOUND)
    target_compile_definitions(SynapseX_Client PRIVATE SX_HAS_CUDA=1)
    target_include_directories(SynapseX_Client PRIVATE ${SX_CUDA_INCLUDE_DIRS})
    if(NOT SX_HAS_INFERENCE)
        target_sources(SynapseX_Client PRIVATE src/CudaPreprocess.cpp)
    endif()
    list(APPEND CLIENT_LIBS ${SX_CUDA_LIB_DIR}/cudart.lib ${SX_CUDA_LIB_DIR}/cuda.lib ${SX_CUDA_LIB_DIR}/nvrtc.lib)
endif()
if(SX_HAS_INFERENCE)
    target_compile_definitions(SynapseX_Client PRIVATE SX_HAS_INFERENCE=1)
endif()
if(makcu-cpp_FOUND)
    target_compile_definitions(SynapseX_Client PRIVATE SX_HAS_MAKCU=1)
    target_link_libraries(SynapseX_Client PRIVATE makcu::makcu-cpp)
else()
    message(WARNING "[SynapseX Client] makcu-cpp not found. Mouse control will be disabled.")
endif()
target_link_libraries(SynapseX_Client PRIVATE ${CLIENT_LIBS})

# 链接 CUDA + TensorRT 推理模块
if(SX_HAS_INFERENCE)
    target_include_directories(SynapseX_Client PRIVATE
        ${SX_TRT_INCLUDE_DIRS}
    )
    target_link_libraries(SynapseX_Client PRIVATE
        SynapseX_Inference
        ${SX_TRT_LIB_DIR}/nvinfer_10.lib
        ${SX_TRT_LIB_DIR}/nvinfer_plugin_10.lib
        ${SX_CUDA_LIB_DIR}/cudart.lib
        ${SX_CUDA_LIB_DIR}/cuda.lib
    )
endif()

# 复制 web/ 面板到输出目录
add_custom_command(TARGET SynapseX_Client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:SynapseX_Client>/web"
    COMMENT "Copying web/ to output directory"
)

# ── Summary ────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  SynapseX Client — Build Configuration")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  LZ4:           ${LZ4_SRC_DIR}")
message(STATUS "  Shared:        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include")
message(STATUS "  CUDA found:    ${SX_CUDA_FOUND}")
message(STATUS "  TensorRT found: ${SX_TENSORRT_FOUND}")
message(STATUS "  Inference:     ${SX_HAS_INFERENCE}")
message(STATUS "  MAKCU:         makcu::makcu-cpp")
message(STATUS "  HttpTuner:     cpp-httplib :9999")
if(SX_CUDA_FOUND)
    message(STATUS "  CUDA:          ${SX_CUDA_INCLUDE_DIRS}")
endif()
if(SX_TENSORRT_FOUND)
    message(STATUS "  TRT:           ${SX_TRT_INCLUDE_DIRS}")
endif()
message(STATUS "══════════════════════════════════════════════")
message(STATUS "")
