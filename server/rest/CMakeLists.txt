set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(HEREBIN "${CMAKE_CURRENT_BINARY_DIR}")
set(PROJBIN "${PROJECT_BINARY_DIR}")

set(black_cmd black -l 79 "${HERE}")
set(stamp "${HEREBIN}/fmt.stamp")

sm_test(lint_black ${black_cmd} --check)
sm_test(lint_mypy mypy --strict --cache-dir "${PROJBIN}/.mypy-cache" "${HERE}")

# also add a custom command for fmt
add_custom_command(
    OUTPUT "${stamp}"
    COMMAND ${black_cmd}
    COMMENT "formatting python files" VERBATIM
)
add_custom_target(fmt_rest_api DEPENDS "${stamp}")
