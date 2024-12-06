set(raw_srcs
    types.c
    instr.c
    builtins.c
    compile.c
    plugin.c
    engine.c
    qw.c
    convert.c
)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(qw_scan_in "${HERE}/scan.c.in")
set(qw_scan_out "${GEN}/scan.c")
set(qw_parse_in "${HERE}/qw.in")
set(qw_parse_c "${GEN}/parse.c")
set(qw_parse_h "${GEN}/parse.h")

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # msvc: allow qw_handle_error() to not return without warnings
    source_compile_options("${qw_parse_c}" "/wd4702")  # unreachable code
endif()

define_file_logname_for_source("${qw_scan_in}" "${qw_scan_out}")
define_file_logname_for_source("${qw_parse_in}" "${qw_parse_c}")

re2c("gen-qw-scan" "${qw_scan_in}" "${qw_scan_out}")

gen_parser(
    "gen-qw-parse" "${qw_parse_in}" "${qw_parse_c}" "${qw_parse_h}"
)

sm_lib(
    qw
    "${raw_srcs}" "${qw_scan_out}" "${qw_parse_c}" "${qw_parse_h}"
    DEPS dstr
    NOSETNAME
)

sm_exe(qwwq main.c DEPS qw)

# Our static linking + plugin strategy is quick and easy but ends up violating
# the "one definition rule" due to duplicate instances of singletons.  The code
# checks for *val=QW_VAL_SKIP instead of val==&theskip so it should be fine.
# A real solution would involve more dynamic linking and installing libqw.so to
# the operating system before running qwwq.
sm_test(test_qw.c DEPS qw FILES ENV "ASAN_OPTIONS=detect_odr_violation=0")

set(qwbin "$<TARGET_FILE:qwwq>")
set(qwconf "${PROJECT_BINARY_DIR}/qw.conf")
configure_file("${PROJECT_SOURCE_DIR}/qw.conf" "${qwconf}" @ONLY)
if(APPLE)
    SET(qw_os "osx")
elseif(UNIX)
    SET(qw_os "linux")
elseif(WIN32)
    SET(qw_os "windows")
endif()
# "export" variables so qwwq function works from peer directories
set(qwconf "${qwconf}" PARENT_SCOPE)
set(qwbin "${qwbin}" PARENT_SCOPE)
set(qw_os "${qw_os}" PARENT_SCOPE)

# qwwq(input output MODE mode DEPS deps... TARGET tgtname)
# MODE will fail on windows
# DEPS are target-level dependencies; there's no linking of course
function(qwwq input output)
    set(options "")
    set(one_value_args "MODE" "TARGET")
    set(multi_value_args "DEPS")
    cmake_parse_arguments(
        QWWQ
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        "${ARGN}"
    )
    get_filename_component(output_dir "${output}" DIRECTORY)
    if(QWWQ_MODE)
        set(mode "--mode" "${QWWQ_MODE}")
    else()
        set(mode "")
    endif()
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND ${qwbin} ${qwconf} -i ${input} -o ${output} ${mode} os=${qw_os}
        DEPENDS qwwq ${qwconf} ${input} ${QWWQ_DEPS}
        COMMENT "prepocessing ${input}" VERBATIM
    )
    if(QWWQ_TARGET)
        add_custom_target("${QWWQ_TARGET}" DEPENDS "${output}")
    endif()
endfunction()

# usage: qw_plugin([name] src... DEPS deps...)
# if the first arg has no file extension it becomes the name, otherwise it is
# a src and the output name is derived from it automatically.
function(qw_plugin)
    set(options)
    set(one_value_args)
    set(multi_value_args "DEPS")
    cmake_parse_arguments(
        QW_PLUGIN
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        "${ARGN}"
    )

    list(GET QW_PLUGIN_UNPARSED_ARGUMENTS 0 first)
    get_filename_component(ext "${first}" EXT)
    if(ext)
        # derive the name from the first argument
        get_filename_component(name "${first}" NAME_WE)  # "without extension"
        set(srcs ${QW_PLUGIN_UNPARSED_ARGUMENTS})
    else()
        # have an explicit name
        set(name "${first}")
        list(SUBLIST QW_PLUGIN_UNPARSED_ARGUMENTS 1 -1 srcs)
    endif()

    add_library("${name}" SHARED ${srcs})
    # always link against libqw, at least
    target_link_libraries("${name}" PUBLIC qw ${QW_PLUGIN_DEPS})
    default_compile_options("${name}" ${nocov})
    define_file_logname_for_target("${name}")
    set_target_properties("${name}" PROPERTIES PREFIX "")

    # plugins won't build without generating qw code
    add_dependencies("${name}" gen-qw-scan gen-qw-parse)
endfunction()

if(BUILD_TESTS)
    qw_plugin(test_plugin.c)
    add_dependencies(test_qw test_plugin)
endif()
