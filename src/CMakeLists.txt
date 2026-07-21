# Setup the project and settings
project(raylib C)
set(PROJECT_VERSION 6.0.0)
set(API_VERSION 600)

include(GNUInstallDirs)
include(JoinPaths)

# Sets build type if not set by now
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    if(PROJECT_IS_TOP_LEVEL)
        set(default_build_type Debug)
    else()
        message(WARNING "Default build type is not set (CMAKE_BUILD_TYPE)")
    endif()

    message(STATUS "Setting build type to '${default_build_type}' as none was specified.")

    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Used as public API to be included into other projects
set(raylib_public_headers
    raylib.h
    rcamera.h
    rlgl.h
    raymath.h
    )

# Sources to be compiled
set(raylib_sources
    rcore.c
    rshapes.c
    rtext.c
    rtextures.c
    )

# Only build raudio if it's enabled
if (NOT DEFINED SUPPORT_MODULE_RAUDIO OR SUPPORT_MODULE_RAUDIO)
    list(APPEND raylib_sources raudio.c)
endif()

# Only build rmodels if it's enabled
if (NOT DEFINED SUPPORT_MODULE_RMODELS OR SUPPORT_MODULE_RMODELS)
    list(APPEND raylib_sources rmodels.c)
endif()

# <root>/cmake/GlfwImport.cmake handles the details around the inclusion of glfw
if (NOT ${PLATFORM} MATCHES "Web")
    include(GlfwImport)
endif ()

# Sets additional platform options and link libraries for each platform
# also selects the proper graphics API and version for that platform
# Produces a variable LIBS_PRIVATE that will be used later
include(LibraryConfigurations)

if (RAYLIB_USE_DAXA)
    enable_language(CXX)
    include(FetchContent)

    set(DAXA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_FSR2 OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_IMGUI OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_MEM OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_PIPELINE_MANAGER_GLSLANG ON CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_PIPELINE_MANAGER_SLANG OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_PIPELINE_MANAGER_SPIRV_VALIDATION OFF CACHE BOOL "" FORCE)
    set(DAXA_ENABLE_UTILS_TASK_GRAPH OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        daxa
        GIT_REPOSITORY https://github.com/Ipotrick/Daxa.git
        GIT_TAG        c4e7eaee588893e08a5316f543e16dcfc8f6bc21
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(daxa)

    list(APPEND raylib_sources rdaxa.cpp rdaxa.h)
endif()

if (DEFINED SUPPORT_MODULE_RAUDIO AND NOT SUPPORT_MODULE_RAUDIO)
    MESSAGE(STATUS "Audio Backend: None (-DCUSTOMIZE_BUILD=ON -DSUPPORT_MODULE_RAUDIO=OFF)")
else ()
    MESSAGE(STATUS "Audio Backend: miniaudio")
endif ()

add_library(raylib ${raylib_sources} ${raylib_public_headers})

if (NOT BUILD_SHARED_LIBS)
    MESSAGE(STATUS "Building raylib static library")
    add_library(raylib_static ALIAS raylib)
else()
    MESSAGE(STATUS "Building raylib shared library")
    target_compile_definitions(raylib
                               PRIVATE $<BUILD_INTERFACE:BUILD_LIBTYPE_SHARED>
                               INTERFACE $<INSTALL_INTERFACE:USE_LIBTYPE_SHARED>
                               )
endif()

if (${PLATFORM} MATCHES "Web")
    target_link_options(raylib PUBLIC "-sUSE_GLFW=3" -sEXPORTED_RUNTIME_METHODS=ccall)
    if(${GRAPHICS} MATCHES "GRAPHICS_API_OPENGL_ES3")
        target_link_options(raylib PUBLIC "-sMIN_WEBGL_VERSION=2")
        target_link_options(raylib PUBLIC "-sMAX_WEBGL_VERSION=2")
    endif()
endif()

if (${PLATFORM} MATCHES "Android")
    # Wrap fopen at link time so all code (including third-party libs) goes
    # through __wrap_fopen, which handles Android APK asset loading
    target_link_options(raylib PUBLIC -Wl,--wrap=fopen)
endif()

set_target_properties(raylib PROPERTIES
                      PUBLIC_HEADER "${raylib_public_headers}"
                      VERSION ${PROJECT_VERSION}
                      SOVERSION ${API_VERSION}
                      )

if (WITH_PIC OR BUILD_SHARED_LIBS)
    set_property(TARGET raylib PROPERTY POSITION_INDEPENDENT_CODE ON)
endif ()

if (BUILD_SHARED_LIBS)
    # Hide raylib's symbols by default so RLAPI can expose them
    set_property(TARGET raylib PROPERTY C_VISIBILITY_PRESET hidden)
endif ()

target_link_libraries(raylib PRIVATE $<BUILD_INTERFACE:${LIBS_PRIVATE}>)
target_link_libraries(raylib PUBLIC ${LIBS_PUBLIC})
if (RAYLIB_USE_DAXA)
    target_link_libraries(raylib PRIVATE $<BUILD_INTERFACE:daxa::daxa>)
    set_source_files_properties(rdaxa.cpp PROPERTIES LANGUAGE CXX)
    set_target_properties(raylib PROPERTIES
                          CXX_STANDARD 20
                          CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF)
endif()

# Static libraries must export their private link dependencies for installed consumers.
# LINK_ONLY keeps those dependencies out of the compile interface.
if (NOT BUILD_SHARED_LIBS)
    set(raylib_install_private_libs ${LIBS_PRIVATE})

    if (NOT glfw3_FOUND)
        list(REMOVE_ITEM raylib_install_private_libs glfw)
    endif()

    foreach(lib IN LISTS raylib_install_private_libs)
        target_link_libraries(raylib INTERFACE $<INSTALL_INTERFACE:$<LINK_ONLY:${lib}>>)
    endforeach()
endif()

# Sets some compile time definitions for the pre-processor
# If CUSTOMIZE_BUILD option is on you will not use config.h by default
# and you will be able to select more build options
include(CompileDefinitions)

# Registering include directories
target_include_directories(raylib
                           PUBLIC
                           $<INSTALL_INTERFACE:include>
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                           PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${OPENGL_INCLUDE_DIR}
                           ${OPENAL_INCLUDE_DIR}
                           )

# Copy the header files to the build directory for convenience
file(COPY ${raylib_public_headers} DESTINATION "include")
if (PLATFORM STREQUAL "Desktop")
    if (UNIX AND NOT APPLE)
        if (GLFW_BUILD_X11)
            find_package(X11 REQUIRED)

            target_compile_definitions(raylib PRIVATE _GLFW_X11)

            target_link_libraries(raylib PRIVATE
                ${X11_LIBRARIES}
            )

            message(STATUS "X11 support enabled for raylib")
        endif()
    endif()
endif()

# Includes information on how the library will be installed on the system
# when cmake --install is run
include(InstallConfigurations)

# Print the flags for the user
if (DEFINED CMAKE_BUILD_TYPE)
    message(STATUS "Generated build type: ${CMAKE_BUILD_TYPE}")
else ()
    message(STATUS "Generated config types: ${CMAKE_CONFIGURATION_TYPES}")
endif ()

message(STATUS "Compiling with the flags:")
message(STATUS "  PLATFORM=" ${PLATFORM_CPP})
message(STATUS "  GRAPHICS=" ${GRAPHICS})

# Options if you want to create an installer using CPack
include(PackConfigurations)

enable_testing()
