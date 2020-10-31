# add a mysql_interface library
add_library(mysql_helper mysql_helper.c)
if("${MYSQL_INCLUDE_DIR}" STREQUAL "")
    set(MYSQL_INCLUDE_DIR "/usr/include/mysql")
endif()
target_include_directories(mysql_helper PUBLIC ${MYSQL_INCLUDE_DIR})
target_link_libraries(mysql_helper PUBLIC mysqlclient)


add_obj_tgt(badbadbad_alert.c badbadbad_alert)
# badbadbad_alert needs non-strict aliasing due to sockaddr_info business
target_compile_options(badbadbad_alert_o PRIVATE -fno-strict-aliasing)

add_executable(olt olt.c)
default_compile_options(olt TRUE)
define_file_logname_for_target(olt)
target_link_libraries(olt dstr)

add_executable(badbadbad badbadbad.c)
default_compile_options(badbadbad TRUE)
define_file_logname_for_target(badbadbad)
target_link_libraries(badbadbad badbadbad_alert dstr)

add_executable(keysync keysync.c)
default_compile_options(keysync TRUE)
define_file_logname_for_target(keysync)
target_link_libraries(keysync dstr uv_util uv_interface mysql_helper)

find_package(Python3 COMPONENTS Development)
Python3_add_library(pysm MODULE pysm.c)
default_compile_options(pysm FALSE)
target_include_directories(pysm PRIVATE "${OPENSSL_INCLUDE_DIR}")
target_link_libraries(pysm PRIVATE "${OPENSSL_LIBRARIES}")

#############

# generate the From scanner
set(from_scan_in "${CMAKE_CURRENT_SOURCE_DIR}/from.c")
set(from_scan_out "${PROJECT_BINARY_DIR}/from.gen.c")
add_custom_command(
    OUTPUT "${from_scan_out}"
    COMMAND "${RE2C}" -W -Werror -o "${from_scan_out}" "${from_scan_in}"
    DEPENDS "${from_scan_in}"
    COMMENT "generating From scanner" VERBATIM
    )

# generate the From parser
set(from_parse_in "${CMAKE_CURRENT_SOURCE_DIR}/from.y")
set(from_parse_out "${PROJECT_BINARY_DIR}/from.tab.c")
set(bison_header "${PROJECT_BINARY_DIR}/from.tab.h")
add_custom_command(
    OUTPUT "${from_parse_out}" "${bison_header}"
    COMMAND "${BISON}" -d -o "${from_parse_out}" "${from_parse_in}" --report=state
    DEPENDS "${from_parse_in}"
    COMMENT "generating From parser" VERBATIM
    )

define_file_logname_for_source("${from_scan_in}" "${from_scan_out}")
define_file_logname_for_source("${from_parse_in}" "${from_parse_out}")

add_executable(from "${from_scan_out}" "${from_parse_out}")
default_compile_options(from TRUE)
target_link_libraries(from dstr)

# Work around missing prototype and poor enum handling with bison 3.6:
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options("${from_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${from_parse_out}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options("${from_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${from_parse_out}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
endif()

if("${BUILD_TESTS}")
    enable_testing()
    add_test(
        NAME "test_from"
        COMMAND "${PROJECT_SOURCE_DIR}/test/test_from.sh" "$<TARGET_FILE:from>"
    )
endif()
