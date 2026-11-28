cmake_minimum_required(VERSION 3.16)

project(pipewire-module-sendspin
    VERSION 0.1.0
    DESCRIPTION "PipeWire module exposing a Sendspin player@v1/metadata@v1 and source@v1 client"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Everything -- sendspin-cpp and the dependencies it fetches -- ends up inside one shared object
# that the PipeWire daemon dlopen()s, so every static archive must be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Keep the library's (and IXWebSocket's, ArduinoJson's, ...) symbols out of the daemon's global
# namespace. The linker version script below is what really enforces this.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.60)
pkg_check_modules(AVAHI IMPORTED_TARGET avahi-client)
pkg_check_modules(SYSTEMD IMPORTED_TARGET libsystemd)
find_package(Threads REQUIRED)

# ==============================================================================
# sendspin-cpp
# ==============================================================================
# Only the roles this module uses are compiled in (controller@v1 drives the MPRIS controls).
# Artwork/visualizer/color cost sources and dependencies and have no use here.
set(SENDSPIN_ENABLE_PLAYER ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_METADATA ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_SOURCE ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_CONTROLLER ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_COLOR OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_ARTWORK OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENDSPIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

# Dependencies come from the git submodules under external/ (git submodule update --init
# --recursive). sendspin-cpp's own FetchContent dependencies are redirected to those checkouts so
# the build needs no network; any one that is missing is still downloaded as before.
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
foreach(dep IN ITEMS "ARDUINOJSON:ArduinoJson" "IXWEBSOCKET:IXWebSocket"
                     "MICRO_FLAC:micro-flac" "MICRO_OPUS:micro-opus" "NOISE_C:noise-c")
    string(REPLACE ":" ";" dep "${dep}")
    list(GET dep 0 dep_var)
    list(GET dep 1 dep_dir)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_${dep_var}
       AND EXISTS "${EXTERNAL_DIR}/${dep_dir}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${dep_var} "${EXTERNAL_DIR}/${dep_dir}" CACHE PATH
            "Local source for ${dep_dir}")
    endif()
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${dep_var})
        message(STATUS "Using ${dep_dir} from ${FETCHCONTENT_SOURCE_DIR_${dep_var}}")
    endif()
endforeach()

# sendspin-cpp: by default the external/sendspin-cpp submodule, else a sibling checkout next to this
# repository. Point SENDSPIN_SOURCE_DIR elsewhere, or set it to empty to fetch
# SENDSPIN_GIT_REPOSITORY at SENDSPIN_GIT_TAG.
if(EXISTS "${EXTERNAL_DIR}/sendspin-cpp/CMakeLists.txt")
    set(_sendspin_default "${EXTERNAL_DIR}/sendspin-cpp")
else()
    set(_sendspin_default "${CMAKE_CURRENT_SOURCE_DIR}/../sendspin-cpp")
endif()
set(SENDSPIN_SOURCE_DIR "${_sendspin_default}" CACHE PATH
    "Path to a sendspin-cpp source tree")
# The fork commit of Sendspin/sendspin-cpp#287 (no client source@v1_support, the source signal always
# reported, Sendspin/spec#335), the same commit as the submodule, until that is in a release.
set(SENDSPIN_GIT_REPOSITORY "https://github.com/meiser79/sendspin-cpp.git" CACHE STRING
    "sendspin-cpp repository used when SENDSPIN_SOURCE_DIR is empty")
set(SENDSPIN_GIT_TAG "96e002da70810feeb1d6243e80c82b3bf233b2fa" CACHE STRING
    "sendspin-cpp revision used with SENDSPIN_GIT_REPOSITORY")

if(SENDSPIN_SOURCE_DIR AND EXISTS "${SENDSPIN_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Using sendspin-cpp from ${SENDSPIN_SOURCE_DIR}")
    add_subdirectory("${SENDSPIN_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/sendspin" EXCLUDE_FROM_ALL)
elseif(NOT SENDSPIN_SOURCE_DIR)
    include(FetchContent)
    message(STATUS "Fetching sendspin-cpp ${SENDSPIN_GIT_TAG} from ${SENDSPIN_GIT_REPOSITORY}")
    # A shallow clone can only check out a branch or tag, not a commit hash.
    if(SENDSPIN_GIT_TAG MATCHES "^[0-9a-f]+$")
        set(_sendspin_shallow FALSE)
    else()
        set(_sendspin_shallow TRUE)
    endif()
    FetchContent_Declare(sendspin
        GIT_REPOSITORY ${SENDSPIN_GIT_REPOSITORY}
        GIT_TAG        ${SENDSPIN_GIT_TAG}
        GIT_SHALLOW    ${_sendspin_shallow})
    FetchContent_GetProperties(sendspin)
    if(NOT sendspin_POPULATED)
        FetchContent_Populate(sendspin)
        add_subdirectory(${sendspin_SOURCE_DIR} ${sendspin_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
else()
    message(FATAL_ERROR "sendspin-cpp not found at ${SENDSPIN_SOURCE_DIR} "
                        "(set SENDSPIN_SOURCE_DIR, or set it empty to fetch from git)")
endif()

# ==============================================================================
# The module
# ==============================================================================
add_library(pipewire-module-sendspin MODULE
    src/module-sendspin.cpp
    src/state_store.cpp
    src/mdns.cpp
    src/mpris.cpp
)
# PipeWire resolves "libpipewire-module-sendspin" to libpipewire-module-sendspin.so.
set_target_properties(pipewire-module-sendspin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "pipewire-module-sendspin")
target_compile_options(pipewire-module-sendspin PRIVATE -Wall -Wextra -Wno-missing-field-initializers
    -Wno-pedantic)
target_compile_definitions(pipewire-module-sendspin PRIVATE
    MODULE_VERSION="${PROJECT_VERSION}")
target_link_libraries(pipewire-module-sendspin PRIVATE
    sendspin PkgConfig::PIPEWIRE Threads::Threads)
if(AVAHI_FOUND)
    target_link_libraries(pipewire-module-sendspin PRIVATE PkgConfig::AVAHI)
    target_compile_definitions(pipewire-module-sendspin PRIVATE HAVE_AVAHI=1)
else()
    message(WARNING "avahi-client not found: the module will not advertise itself over mDNS")
endif()
if(SYSTEMD_FOUND)
    target_link_libraries(pipewire-module-sendspin PRIVATE PkgConfig::SYSTEMD)
    target_compile_definitions(pipewire-module-sendspin PRIVATE HAVE_SDBUS=1)
else()
    message(WARNING "libsystemd not found: no MPRIS media controls")
endif()
# Export exactly one symbol: the module entry point.
target_link_options(pipewire-module-sendspin PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/module.map"
    "-Wl,--no-undefined"
    "-Wl,-z,defs")
set_property(TARGET pipewire-module-sendspin APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/module.map")

pkg_get_variable(PW_MODULE_DIR libpipewire-0.3 moduledir)
if(NOT PW_MODULE_DIR)
    set(PW_MODULE_DIR "${CMAKE_INSTALL_PREFIX}/lib/pipewire-0.3")
endif()
set(PIPEWIRE_MODULE_DIR "${PW_MODULE_DIR}" CACHE PATH "Where PipeWire looks for modules")

install(TARGETS pipewire-module-sendspin LIBRARY DESTINATION "${PIPEWIRE_MODULE_DIR}"
    COMPONENT module)
# Installed as an example only: dropping it into an active pipewire.conf.d/ would load the module
# for every user on the machine (and all of them would fight over the same port).
install(FILES pipewire.conf.d/sendspin.conf
    DESTINATION "share/doc/${PROJECT_NAME}" COMPONENT module)

message(STATUS "PipeWire ${PIPEWIRE_VERSION}, module dir: ${PIPEWIRE_MODULE_DIR}")
