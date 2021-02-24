if("${MYSQL_INCLUDE_DIR}" STREQUAL "")
    set(MYSQL_INCLUDE_DIR "/usr/include/mysql")
endif()

macro(add_mysql_util asan)
    add_library("mysql_util${asan}" mysql_util.c binds.c)
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("mysql_util${asan}" FALSE)
    else()
        default_compile_options("mysql_util${asan}" TRUE)
    endif()
    define_file_logname_for_target("mysql_util${asan}")
    target_include_directories("mysql_util${asan}" PUBLIC ${MYSQL_INCLUDE_DIR})
    target_link_libraries("mysql_util${asan}" PUBLIC "dstr${asan}" mysqlclient)
endmacro()

add_mysql_util("")

if("${CMAKE_BUILD_TYPE}" STREQUAL "debug")
    # rebuild without asan
    add_mysql_util("_noasan")
else()
    # release builds already have no asan
    add_library(mysql_util_noasan ALIAS mysql_util)
endif()

# Pretend `const` qualifiers are possible with bound parameters.
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options(binds.c "-Wno-discarded-qualifiers")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options(binds.c "-Wno-discarded-qualifiers")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
endif()
