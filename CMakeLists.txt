cmake_minimum_required(VERSION 3.21)
message("Using toolchain file ${CMAKE_TOOLCHAIN_FILE}.")

########################################################################################################################
## Define project
########################################################################################################################
if(ENABLE_SKYRIM_FLAT)
    set(TARGET_DESC "SE & AE")
elseif(ENABLE_SKYRIM_VR)
    set(TARGET_DESC "VR")
else()
    set(TARGET_DESC "NG")
endif()
project(
        MuDynamicNormalMap
        VERSION 2.1.13
        DESCRIPTION "MuDynamicNormalMap ${TARGET_DESC} by Acro."
        LANGUAGES CXX
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

include(GNUInstallDirs)

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY
)

set(headers
        extern/tinygltf/tiny_gltf.h
        include/bc7e_ispc_avx.h
        include/bc7e_ispc_avx2.h
        include/bc7e_ispc_sse2.h
        include/bc7e_ispc_sse4.h
        include/Interface/Interface.h
        include/Interface/InterfaceImpl.h
        include/Utility.hpp
        include/nifUtil.h
        include/Config.h
        include/lString.h
        include/PapyrusScripts.h
        include/ShaderManager.h
        include/ObjectNormalMapUpdater.h
        include/TaskManager.h
        include/Hook.h
        include/Store.h
        include/Common.h
        include/Geometry.h
        include/InputManager.h
        include/RGBA.h
        include/ThreadPool.h
        include/ActorVertexHasher.h
        include/NormalMapStore.h
        include/Condition.h
        include/RuntimeTuning.h
        include/SettingsMenu.h
        include/IniConfig.h
)

set(sources
        src/nifUtil.cpp
        src/Config.cpp
        src/PapyrusScripts.cpp
        src/ShaderManager.cpp
        src/ObjectNormalMapUpdater.cpp
        src/TaskManager.cpp
        src/Hook.cpp
        src/Store.cpp
        src/Geometry.cpp
        src/InputManager.cpp
        src/ActorVertexHasher.cpp
        src/NormalMapStore.cpp
        src/Condition.cpp
        src/ThreadPool.cpp
        src/SettingsMenu.cpp
        src/IniConfig.cpp
        src/Main.cpp

        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

# Pregnancy is an internal module of Mu, including its adapted lifecycle entry point.
file(GLOB_RECURSE pregnancy_sources CONFIGURE_DEPENDS pregnancy/src/*.cpp)
file(GLOB_RECURSE pregnancy_headers CONFIGURE_DEPENDS pregnancy/src/*.h)
list(APPEND sources ${pregnancy_sources})
list(APPEND headers ${pregnancy_headers})
set_source_files_properties(${pregnancy_sources} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")

source_group(
        TREE ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        ${headers}
        ${sources})

########################################################################################################################
## Configure target DLL
########################################################################################################################
find_package(CommonLibSSE REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(JsonCpp CONFIG REQUIRED)
find_package(Directxtk CONFIG REQUIRED)
find_package(DirectXTex CONFIG REQUIRED)
find_package(detours)
find_package(xxhash CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(tinygltf)
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(nlohmann_json)

execute_process(
    COMMAND python "${CMAKE_SOURCE_DIR}/include/bc7e_ispc__gen.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/include/"
    RESULT_VARIABLE MU_ISPC_RESULT
)
if(NOT MU_ISPC_RESULT EQUAL 0)
    message(FATAL_ERROR "ISPC generation failed; refusing to link previous compression objects")
endif()

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})
add_library("${PROJECT_NAME}::${PROJECT_NAME}" ALIAS "${PROJECT_NAME}")

target_compile_features(
    ${PROJECT_NAME}
    PUBLIC
        cxx_std_23
)

target_compile_definitions(
    ${PROJECT_NAME}
    PUBLIC
        SKSE_SUPPORT_XBYAK
        _UNICODE
        UNICODE
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/shader>
        $<INSTALL_INTERFACE:src>
        ${DETOURS_INCLUDE_DIRS}
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pregnancy/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern/tinygltf>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ExtraRE/include>
)

target_link_libraries(
    ${PROJECT_NAME}
    PUBLIC
        CommonLibSSE::CommonLibSSE
    PRIVATE
        magic_enum::magic_enum
        JsonCpp::JsonCpp
        lz4::lz4
        xxHash::xxhash
        TBB::tbb
        nlohmann_json::nlohmann_json
        d3d11 d3dcompiler dxgi
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows-static-md/lib/detours.lib>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows-static-md/lib/DirectXTex.lib>
)

set(PNS_DINO_ARCHIVE "" CACHE FILEPATH "Optional original DinoRMF 1.4.1 archive")
if(PNS_DINO_ARCHIVE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(dino_dir "${CMAKE_CURRENT_BINARY_DIR}/dino-compat-source")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        pregnancy/tools/prepare-dino.py pregnancy/patches/dino/PregnancyBridge.h "${PNS_DINO_ARCHIVE}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pregnancy/tools/prepare-dino.py"
        "${PNS_DINO_ARCHIVE}" "${dino_dir}" COMMAND_ERROR_IS_FATAL ANY)
    add_library(DinoRMF SHARED "${dino_dir}/src/main.cpp" "${dino_dir}/src/plugin.cpp" "${dino_dir}/src/UI.cpp")
    target_link_libraries(DinoRMF PRIVATE CommonLibSSE::CommonLibSSE d3d11 dxguid)
    target_compile_options(DinoRMF PRIVATE /bigobj /utf-8 /Zc:preprocessor)
    target_compile_definitions(DinoRMF PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/build/bc7e_avx.obj DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/build/bc7e_avx2.obj DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/build/bc7e_sse2.obj DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/build/bc7e_sse4.obj DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME})
target_sources(
    ${PROJECT_NAME}
    PRIVATE
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME}/bc7e_avx.obj>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME}/bc7e_avx2.obj>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME}/bc7e_sse2.obj>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME}/bc7e_sse4.obj>
)

target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
        include/PCH.h)
        
target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/sdl;/utf-8;/Zi;/permissive-;/Zc:preprocessor;/Zc:inline;/JMC-;/O2;/Ob3>")
target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>")

install(TARGETS ${PROJECT_NAME}
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

########################################################################################################################
## Shader Script
########################################################################################################################
set(compute_shader_sources 
   ${CMAKE_CURRENT_SOURCE_DIR}/shader/UpdateNormalMap.hlsl
   ${CMAKE_CURRENT_SOURCE_DIR}/shader/MergeTexture.hlsl
   ${CMAKE_CURRENT_SOURCE_DIR}/shader/BleedTexture.hlsl
   ${CMAKE_CURRENT_SOURCE_DIR}/shader/GenerateMips.hlsl
)
set(vertex_shader_sources 

)
set(pixel_shader_sources 

)

add_custom_target(ValidateShaders ALL 
    COMMENT "Running Shader Validation"
)
foreach(shader ${compute_shader_sources})
    get_filename_component(shader_name ${shader} NAME_WE)
    set(compiledShader "${CMAKE_CURRENT_SOURCE_DIR}/Shader/Compiled/CS_${shader_name}.cso")

    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        TARGET ValidateShaders
        COMMAND fxc.exe /T cs_5_0 /E CSMain /Fo ${compiledShader} /nologo ${shader}
        COMMAND ${CMAKE_COMMAND} -E echo "Shader valid : ${shader}"
    )
endforeach()
foreach(shader ${vertex_shader_sources})
    get_filename_component(shader_name ${shader} NAME_WE)
    set(compiledShader "${CMAKE_CURRENT_SOURCE_DIR}/Shader/Compiled/VS_${shader_name}.cso")

    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        TARGET ValidateShaders
        COMMAND fxc.exe /T vs_5_0 /E VSMain /Fo ${compiledShader} /nologo ${shader}
        COMMAND ${CMAKE_COMMAND} -E echo "Shader valid : ${shader}"
    )
endforeach()
foreach(shader ${pixel_shader_sources})
    get_filename_component(shader_name ${shader} NAME_WE)
    set(compiledShader "${CMAKE_CURRENT_SOURCE_DIR}/Shader/Compiled/PS_${shader_name}.cso")

    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        TARGET ValidateShaders
        COMMAND fxc.exe /T ps_5_0 /E PSMain /Fo ${compiledShader} /nologo ${shader}
        COMMAND ${CMAKE_COMMAND} -E echo "Shader valid : ${shader}"
    )
endforeach()

########################################################################################################################
## Automatic plugin deployment
########################################################################################################################
# Automatic deployment to FOMOD directory.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_NAME "Debug")
else()
    set(BUILD_NAME "Release")
endif()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}${TARGET_DESC}/")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}${TARGET_DESC}/")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/Plugin${BUILD_NAME}${TARGET_DESC}/")
file(GLOB_RECURSE OUTPUT_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/**/*.dll")
file(GLOB_RECURSE OUTPUT_PDBS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution/**/*.pdb")
set_property(TARGET ${PROJECT_NAME}
        APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${OUTPUT_DLLS}")
set_property(TARGET ${PROJECT_NAME}
        APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${OUTPUT_PDBS}")
