add_subdirectory(mysql_util)
add_subdirectory(petname)

macro(add_libsmsql asan)
    add_library("libsmsql${asan}" libsmsql.c)
    set_target_properties("libsmsql${asan}" PROPERTIES OUTPUT_NAME "smsql${asan}")
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("libsmsql${asan}" FALSE)
    else()
        default_compile_options("libsmsql${asan}" TRUE)
    endif()
    define_file_logname_for_target("libsmsql${asan}")
    target_link_libraries(
        "libsmsql${asan}"
        PUBLIC
        "mysql_util${asan}"
        "petname${asan}"
        "crypto${asan}"
        crypt
    )
endmacro()

macro(add_badbadbad_alert asan)
    add_library("badbadbad_alert${asan}" badbadbad_alert.c)
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("badbadbad_alert${asan}" FALSE)
    else()
        default_compile_options("badbadbad_alert${asan}" TRUE)
    endif()
    # badbadbad_alert needs non-strict aliasing due to sockaddr_info business
    source_compile_options(badbadbad_alert.c -fno-strict-aliasing)
    define_file_logname_for_target("badbadbad_alert${asan}")
endmacro()

add_libsmsql("")
add_badbadbad_alert("")

if(DISABLE_ASAN)
    # release builds already have no asan
    add_library(libsmsql_noasan ALIAS libsmsql)
    add_library(badbadbad_alert_noasan ALIAS badbadbad_alert)
else()
    # rebuild without asan
    add_libsmsql("_noasan")
    add_badbadbad_alert("_noasan")
endif()

add_executable(smsql smsql.c)
default_compile_options(smsql TRUE)
define_file_logname_for_target(smsql)
target_link_libraries(smsql PUBLIC libsmsql)

add_executable(olt olt.c)
default_compile_options(olt TRUE)
define_file_logname_for_target(olt)
target_link_libraries(olt PUBLIC dstr)

add_executable(badbadbad badbadbad.c)
default_compile_options(badbadbad TRUE)
define_file_logname_for_target(badbadbad)
target_link_libraries(badbadbad PUBLIC badbadbad_alert dstr)

add_executable(migmysql migmysql.c)
default_compile_options(migmysql TRUE)
define_file_logname_for_target(migmysql)
target_link_libraries(migmysql PUBLIC dstr mysql_util)

add_executable(gc_sessions gc_sessions.c)
default_compile_options(gc_sessions TRUE)
define_file_logname_for_target(gc_sessions)
target_link_libraries(gc_sessions PUBLIC libsmsql badbadbad_alert)

add_subdirectory(pysm)
add_subdirectory(smphp)
add_subdirectory(xkey)

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
target_link_libraries(from PUBLIC dstr)

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
