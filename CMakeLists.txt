cmake_minimum_required(VERSION 3.14)
project(pdf_translator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

set(LLAMA_DIR "/home/m/llama.cpp")

# تضمين ملفات الرأس
include_directories(
    ${LLAMA_DIR}
    ${LLAMA_DIR}/include
    ${LLAMA_DIR}/ggml/include
)

# البحث التلقائي عن ملفات llama المبنية حديثاً
find_library(LLAMA_LIB NAMES llama PATHS "${LLAMA_DIR}/build/src" "${LLAMA_DIR}/build/bin" NO_DEFAULT_PATH)
find_library(GGML_LIB NAMES ggml PATHS "${LLAMA_DIR}/build/ggml/src" "${LLAMA_DIR}/build/bin" "${LLAMA_DIR}/build/ggml" NO_DEFAULT_PATH)

if(NOT LLAMA_LIB OR NOT GGML_LIB)
    message(FATAL_ERROR "لم يتم العثور على مكتبات llama. تأكد من بناء llama.cpp أولاً عن طريق: cd ~/llama.cpp && cmake -B build -DGGML_CUDA=ON && cmake --build build --config Release")
endif()

message(STATUS "تم العثور على LLAMA_LIB: ${LLAMA_LIB}")
message(STATUS "تم العثور على GGML_LIB: ${GGML_LIB}")

# البحث عن libzip
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBZIP QUIET libzip)
endif()
if(NOT LIBZIP_FOUND)
    find_library(LIBZIP_LIBRARIES NAMES zip)
    find_path(LIBZIP_INCLUDE_DIRS zip.h)
endif()
if(NOT LIBZIP_LIBRARIES OR NOT LIBZIP_INCLUDE_DIRS)
    message(FATAL_ERROR "لم يتم العثور على libzip")
endif()
include_directories(${LIBZIP_INCLUDE_DIRS})

add_executable(pdf_translator main.cpp)

target_link_libraries(pdf_translator
    ${LLAMA_LIB}
    ${GGML_LIB}
    ${LIBZIP_LIBRARIES}
    pthread
    dl
    stdc++fs
)

target_compile_options(pdf_translator PRIVATE -O2 -march=native -DNDEBUG)
