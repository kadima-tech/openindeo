cmake_minimum_required(VERSION 3.16)
project(openindeo VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(openindeo STATIC
    src/convert.c
    src/indeo4.c
    src/indeo5.c
    src/ivi.c
    src/tables.c
    src/transforms.c)
target_include_directories(openindeo PUBLIC include PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(openindeo PRIVATE -Wall -Wextra)
endif()

option(OPENINDEO_TOOLS "Build the oi-avi tool" ${PROJECT_IS_TOP_LEVEL})
if(OPENINDEO_TOOLS)
    add_executable(oi-avi tools/oi_avi.c)
    target_link_libraries(oi-avi PRIVATE openindeo)
endif()
