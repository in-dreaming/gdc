cmake_minimum_required(VERSION 3.20)
project(gdc_eval C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(TMP_REF "${CMAKE_CURRENT_SOURCE_DIR}/../tmp_ref" CACHE PATH "reference source root")
set(COMP_DIR "${TMP_REF}/compression")

# ---------------- zstd ----------------
file(GLOB ZSTD_SRC
    "${COMP_DIR}/zstd/common/*.c"
    "${COMP_DIR}/zstd/compress/*.c"
    "${COMP_DIR}/zstd/decompress/*.c")
add_library(zstd_ref STATIC ${ZSTD_SRC})
target_include_directories(zstd_ref PUBLIC
    "${COMP_DIR}/zstd"
    "${COMP_DIR}/zstd/common")
target_compile_definitions(zstd_ref PRIVATE ZSTD_DISABLE_ASM=1)

# ---------------- lz4 ----------------
# NOTE: this is Unity's vendored old lz4 (r1xx API, base symbols renamed
# to UNITY_LZ4_* by unityquarantine.h).
add_library(lz4_ref STATIC
    "${COMP_DIR}/lz4/lz4.c"
    "${COMP_DIR}/lz4/lz4hc.c")
target_include_directories(lz4_ref PUBLIC "${COMP_DIR}/lz4")

# ---------------- lz3 ----------------
# lz3.cpp includes "Compression/zstd/common/..." -> expose TMP_REF as include root
# (Windows filesystem is case-insensitive, matches "compression")
add_library(lz3_ref STATIC "${COMP_DIR}/lz3/lz3.cpp")
target_include_directories(lz3_ref PUBLIC
    "${COMP_DIR}/lz3"
    "${TMP_REF}"
    "${COMP_DIR}/zstd/common")
target_link_libraries(lz3_ref PRIVATE zstd_ref)

# ---------------- oodle2 core (data compression) ----------------
# Build from source without platform asm kernels (NEWLZ_*_ASM not defined,
# C++ fallbacks are used). NOTE: decode/encode speed therefore UNDERSTATES
# shipping Oodle builds; ratios are unaffected.
file(GLOB OODLE_SRC
    "${COMP_DIR}/oodle2/core/*.cpp"
    "${COMP_DIR}/oodle2/base/*.cpp")
add_library(oodle_ref STATIC ${OODLE_SRC})
target_include_directories(oodle_ref PUBLIC
    "${COMP_DIR}/oodle2/core"
    "${COMP_DIR}/oodle2/core/public"
    "${COMP_DIR}/oodle2/base"
    "${COMP_DIR}/oodle2")
target_compile_definitions(oodle_ref PUBLIC OODLE_BUILDING_DATA=1 OODLE_BUILDING_LIB=1)
if(MSVC)
    target_compile_options(oodle_ref PRIVATE /wd4244 /wd4267 /wd4018 /wd4146 /wd4334)
endif()

# ---------------- eval tool ----------------
add_executable(gdceval
    src/main.cpp
    src/cfg.cpp
    src/codec.cpp
    src/codec_lz4.cpp
    src/codec_lz3.cpp
    src/codec_zstd.cpp
    src/codec_oodle.cpp
    src/bench.cpp
    src/gdc/rans.cpp
    src/gdc/transforms.cpp
    src/gdc/gdc1.cpp)
target_include_directories(gdceval PRIVATE src)
target_link_libraries(gdceval PRIVATE zstd_ref lz4_ref lz3_ref oodle_ref)

# ---------------- self tests ----------------
add_executable(gdctest
    tests/rans_o1_test.cpp
    src/gdc/rans.cpp
    src/gdc/gdc1.cpp)
target_link_libraries(gdctest PRIVATE zstd_ref)
if(MSVC)
    target_compile_options(gdceval PRIVATE /utf-8 /W3)
endif()
