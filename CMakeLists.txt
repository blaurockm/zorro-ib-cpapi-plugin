cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

set(VCPKG_TARGET_TRIPLET "x86-windows-static")
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS ON)

project(Zorro_IB-cpapi_Plugin)

include_directories(BEFORE "../../../Zorro/include")
add_compile_options(/D_WINDLL /D_MBCS /DLL)

find_package(json-c CONFIG REQUIRED)

set(Headerdateien
    "ib-cpapi-plugin/ib-cpapi.h"
    "ib-cpapi-plugin/uthash.h"
)

source_group("Headerdateien" FILES ${Headerdateien})

set(Quelldateien
    "../../../Zorro/Source/VC++/ZorroDLL.cpp"
    "ib-cpapi-plugin/ib-cpapi.cpp"
    "ib-cpapi-plugin/utils.cpp"
)

source_group("Quelldateien" FILES ${Quelldateien})

set(ALL_FILES
    ${Headerdateien}
    ${Quelldateien}
)

add_library(IB-cpapi SHARED ${ALL_FILES})
target_link_libraries(IB-cpapi PRIVATE json-c::json-c)

