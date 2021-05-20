macro(add_petname asan)
    add_library("petname${asan}" petname.c)
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("petname${asan}" FALSE)
    else()
        default_compile_options("petname${asan}" TRUE)
    endif()
    define_file_logname_for_target("petname${asan}")
    target_link_libraries("petname${asan}" PUBLIC "dstr${asan}")
endmacro()

add_petname("")

if("${CMAKE_BUILD_TYPE}" STREQUAL "debug")
    # rebuild without asan
    add_petname("_noasan")
else()
    # release builds already have no asan
    add_library(petname_noasan ALIAS petname)
endif()
