# from https://www.zend.com/setting-up-your-php-build-environment:
# building php from source
#   git clone https://github.com/php/php-src.git && cd php-src
#   ./buildconf
#   ./configure --prefix=$PWD/install
#   make -j4 && make install
# cmake -DPHP_CONFIG=/path/to/php-src/install/bin/php-config
if("${PHP_CONFIG}" STREQUAL "")
    set(PHP_CONFIG "php-config")
endif()

if("${PHP}" STREQUAL "")
    execute_process(
        COMMAND "${PHP_CONFIG}" "--php-binary"
        OUTPUT_VARIABLE PHP
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if("${PHP_INCLUDE_DIR}" STREQUAL "")
    execute_process(
        COMMAND "php-config" "--include-dir" "--phpapi"
        OUTPUT_VARIABLE PHP_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

add_library(smphp SHARED smphp.c)
target_include_directories(
    smphp
    PRIVATE
    "${PHP_INCLUDE_DIR}"
    "${PHP_INCLUDE_DIR}/main"
    "${PHP_INCLUDE_DIR}/TSRM"
    "${PHP_INCLUDE_DIR}/Zend"
    "${PHP_INCLUDE_DIR}/ext"
    "${PHP_INCLUDE_DIR}/ext/date/lib"
)
target_link_libraries(smphp PRIVATE libsmsql_noasan badbadbad_alert_noasan)
define_file_logname_for_target(smphp)

# just smphp.so, not libsmsphp.o
set_target_properties(smphp PROPERTIES PREFIX "")

# zend doesn't handle all of the default flags we normally use
default_compile_options(smphp FALSE)
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(smphp PRIVATE "-Wno-unused-parameter")
    target_compile_options(smphp PRIVATE "-Wno-conversion")
    target_compile_options(smphp PRIVATE "-Wno-strict-prototypes")
    target_compile_options(smphp PRIVATE "-Wno-missing-prototypes")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    target_compile_options(smphp PRIVATE "-Wno-unused-parameter")
    target_compile_options(smphp PRIVATE "-Wno-conversion")
    target_compile_options(smphp PRIVATE "-Wno-strict-prototypes")
    target_compile_options(smphp PRIVATE "-Wno-missing-prototypes")
    target_compile_options(smphp PRIVATE "-Wno-used-but-marked-unused")
    target_compile_options(smphp PRIVATE "-Wno-float-equal")
    target_compile_options(smphp PRIVATE "-Wno-cast-align")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    message(FATAL_ERROR "don't compile smphp with msvc")
endif()

enable_testing()
add_test(NAME "test_smphp"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    COMMAND
        "${PYTHON_3_CMD}"
        "${PROJECT_SOURCE_DIR}/test/test_smphp.py"
        "--php-bin" "${PHP}"
)
