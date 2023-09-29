set(print_help_in "${CMAKE_CURRENT_SOURCE_DIR}/print_help.h.in")
set(print_help_out "${CMAKE_CURRENT_BINARY_DIR}/print_help.h")
qwwq("${print_help_in}" "${print_help_out}")
define_file_logname_for_source("${print_help_in}" "${print_help_out}")

sm_lib(cli
    ui.c
    console_input.c
    "${print_help_out}"
    DEPS http libcitm api_client
)

if(WIN32)
    # additional dependencies for windows
    target_link_libraries(cli PUBLIC shlwapi)
endif()
