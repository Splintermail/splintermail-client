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

if(DISABLE_ASAN)
    message(FATAL_ERROR "disabled asan??")
    # release builds already have no asan
    add_library(petname_noasan ALIAS petname)
else()
    # rebuild without asan
    add_petname("_noasan")
endif()
