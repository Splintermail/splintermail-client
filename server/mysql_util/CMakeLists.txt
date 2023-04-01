if("${MYSQL_INCLUDE_DIR}" STREQUAL "")
    set(MYSQL_INCLUDE_DIR "/usr/include/mysql")
endif()

sm_lib(mysql_util.c binds.c DEPS dstr NOASAN libs)

foreach(lib ${libs})
    target_include_directories("${lib}" PUBLIC ${MYSQL_INCLUDE_DIR})
    target_link_libraries("${lib}" PUBLIC mysqlclient)
endforeach()

# Pretend `const` qualifiers are possible with bound parameters.
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options(binds.c "-Wno-discarded-qualifiers")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options(binds.c "-Wno-incompatible-pointer-types-discards-qualifiers")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
endif()
