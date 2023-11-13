set(print_help_in "${CMAKE_CURRENT_SOURCE_DIR}/print_help.h.in")
set(print_help_out "${CMAKE_CURRENT_BINARY_DIR}/print_help.h")
qwwq("${print_help_in}" "${print_help_out}")
define_file_logname_for_source("${print_help_in}" "${print_help_out}")

sm_lib(cli
    ui.c
    console_input.c
    status.c
    configure.c
    uv_configure.c
    "${print_help_out}"
    DEPS http libcitm api_client
)
add_dependencies(cli config_h)

if(WIN32)
    # additional dependencies for windows
    target_link_libraries(cli PUBLIC shlwapi)
elseif(APPLE)
else()
    target_link_libraries(cli PUBLIC systemd)
endif()

sm_test(test_status.c DEPS cli)
sm_test(test_configure.c DEPS cli)
