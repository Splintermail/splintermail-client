cmake_minimum_required(VERSION 3.1.0)
project(splintermail
    LANGUAGES C
    VERSION 0.2.0
    )

set(MAINTAINER_NAME "Splintermail Dev")
set(MAINTAINER_EMAIL "dev@splintermail.com")
set(MAINTAINER "${MAINTAINER_NAME} <${MAINTAINER_EMAIL}>")

# configure qwerrewq.json with Project version and such
# (not necessary under certain packaging situations)
if(EXISTS "${PROJECT_SOURCE_DIR}/support_files/qwerrewq.json")
    configure_file("${PROJECT_SOURCE_DIR}/support_files/qwerrewq.json"
        "${PROJECT_BINARY_DIR}/qwerrewq.json" @ONLY)
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_DEBUG")
endif()

# always include the source directory (for compiling generated .c files)
include_directories("${PROJECT_SOURCE_DIR}")
# always include the binary directory (for including generated .h files)
include_directories("${PROJECT_BINARY_DIR}")

set(OBJ_TGTS
    api_client
    common
    crypto
    ditm
    fileops
    json
    key_tool
    logger
    maildir
    networking
    opt_parse
    pop_client
    pop_server
    win_compat
    console_input
    ui
    engine
    uv_util
    loop
    tls_engine
    imap_engine
    queue
    jsw_atree
    imap_parse
    imap_expression
    imap_expression_print
    imap_write
    imap_read
    hashmap
    imap_maildir
    imap_dirmgr
    link
    imap_session
    imap_client
)

set(EXE_TGTS
    # srvcli
    splintermail
    leak
    encrypt_msg
    decrypt_msg
    sm_fetch
)

set(TEST_OBJ_TGTS
    dummy_key_tool
    dummy_ui_harness
    fake_api_server
    fake_pop_server
    ditm_thread
    test_utils
    fake_engine
    fake_imap_logic
)

set(TEST_EXE_TGTS
    test_common
    test_fileops
    test_pop_server
    test_ditm
    test_ditm_errors
    test_maildir
    test_logger
    test_pop_client
    test_crypto
    test_opt_parse
    test_networking
    test_json
    test_ui
    test_key_tool
    test_atree
    test_imap_scan
    test_imap_read
    test_imap_write
    test_hashmap
    # test_imap_maildir
    test_loop
    test_queue
    test_tls_engine
    test_imap_engine
    test_link
)

# additional TGTS if we are building server code
if("${BUILD_SERVER_CODE}")
    list(APPEND OBJ_TGTS badbadbad_alert)
    list(APPEND EXE_TGTS badbadbad)
    list(APPEND EXE_TGTS olt)
endif()


# Will add -DFILE_BASENAME="filename" for each source file depended on
# by tgt, where filename is the name of the file.  Needed for logger.h
function(define_file_basename_for_sources targetname)
    get_target_property(source_files "${targetname}" SOURCES)
    foreach(sourcefile ${source_files})
        # Get source file's current list of compile definitions.
        get_property(defs SOURCE "${sourcefile}"
            PROPERTY COMPILE_DEFINITIONS)
        # Add the FILE_BASENAME=filename compile definition to the list.
        get_filename_component(basename "${sourcefile}" NAME)
        list(APPEND defs "FILE_BASENAME=\"${basename}\"")
        # Set the updated compile definitions on the source file.
        set_property(
            SOURCE "${sourcefile}"
            PROPERTY COMPILE_DEFINITIONS ${defs})
    endforeach()
endfunction()

# python3 is needed for packaging the installers
if("${PYTHON_3_CMD}" STREQUAL "")
    set(PYTHON_3_CMD "python3")
endif()

# re2c is needed for the IMAP scanner
if("${RE2C}" STREQUAL "")
    set(RE2C "re2c")
endif()

# bison is needed for the IMAP scanner
if("${BISON}" STREQUAL "")
    set(BISON "bison")
endif()

# inlcude OpenSSL
if(APPLE OR WIN32)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

# include libuv
add_library(uv_interface INTERFACE)
# target_include_directories(libuv INTERFACE "${path/to/libuv/include}")
if("${LIBUV_LIB}" STREQUAL "")
    set(LIBUV_LIB "uv")
endif()
target_link_libraries(uv_interface INTERFACE "${LIBUV_LIB}")

# tgt object depends on external library (propagate linking and includes)
function(obj_depend_lib tgt includes libs)
    if(NOT "${includes}" STREQUAL "")
        # compile object with new includes
        target_include_directories("${tgt}_o" PUBLIC ${includes})
        # compile dependent objects with new includes
        target_include_directories("${tgt}" INTERFACE ${includes})
    endif()
    if(NOT "${libs}" STREQUAL "")
        # link dependent objects with new libraries
        target_link_libraries("${tgt}" INTERFACE ${libs})
    endif()
endfunction()

# tgt object depends on another object (propagate linking and includes)
function(obj_depend_obj tgt)
    foreach(dep ${ARGN})
        get_target_property(incl "${dep}" INTERFACE_INCLUDE_DIRECTORIES)
        if(NOT "${incl}" MATCHES ".*NOTFOUND")
            # if the INTERFACE library has an include, we need it to compile
            target_include_directories("${tgt}_o" PUBLIC ${incl})
            target_include_directories("${tgt}" INTERFACE ${incl})
        endif()
    endforeach()
    target_link_libraries("${tgt}" INTERFACE ${ARGN})
endfunction()

# for adding an interface library, which might be generated
# extra arguments become extra file dependencies
function(add_obj_tgt file tgt)
    # create the object library itself
    add_library("${tgt}_o" OBJECT "${file}")
    define_file_basename_for_sources("${tgt}_o")
    # create an interface library whose dependents will link against the object
    add_library("${tgt}" INTERFACE)
    target_sources("${tgt}" INTERFACE $<TARGET_OBJECTS:${tgt}_o> ${ARGN})
endfunction()

# generate the imap scanner
set(input "${PROJECT_SOURCE_DIR}/imap_scan.c")
set(output "${PROJECT_BINARY_DIR}/imap_scan.gen.c")
add_custom_command(
    OUTPUT "${output}"
    COMMAND "${RE2C}" -W -Werror --storable-state -o "${output}" "${input}"
    DEPENDS "${input}"
    COMMENT "generating scanner" VERBATIM
    )
add_obj_tgt("${output}" "imap_scan")

# generate the imap parser
set(input "${PROJECT_SOURCE_DIR}/imap_parse.y")
set(output "${PROJECT_BINARY_DIR}/imap_parse.tab.c")
set(bison_header "${PROJECT_BINARY_DIR}/imap_parse.tab.h")
add_custom_command(
    OUTPUT "${output}" "${bison_header}"
    COMMAND "${BISON}" -d -o "${output}" "${input}" --report=state
    DEPENDS "${input}"
    COMMENT "generating parser" VERBATIM
    )
add_obj_tgt("${output}" "imap_parse_tab")
# add a custom target just for depending on the generated header
add_custom_target(bison_header DEPENDS "${bison_header}")

# objects become OBJECT libraries, but are accessed through INTERFACE libraries
foreach(tgt ${OBJ_TGTS})
    add_obj_tgt("${tgt}.c" "${tgt}")
endforeach()

# add executables
foreach(tgt ${EXE_TGTS})
    add_executable("${tgt}" "${tgt}.c")
    define_file_basename_for_sources("${tgt}")
endforeach()

# some targets need OpenSSL headers
obj_depend_lib(networking "${OPENSSL_INCLUDE_DIR}" "${OPENSSL_LIBRARIES}")
obj_depend_lib(crypto "${OPENSSL_INCLUDE_DIR}" "${OPENSSL_LIBRARIES}")
# ui has a full test harness, so it can't use the normal obj_depend_obj
target_include_directories(ui_o PRIVATE ${OPENSSL_INCLUDE_DIR})

# object inter-dependencies make exectuable dependencies easier to express:
# objects with dummy interfaces must not be dependencies here
obj_depend_obj(common logger win_compat)
obj_depend_obj(opt_parse common)
obj_depend_obj(fileops common)
obj_depend_obj(crypto common)
obj_depend_obj(json common)
obj_depend_obj(maildir fileops)
obj_depend_obj(networking fileops)
obj_depend_obj(pop_client networking)
obj_depend_obj(pop_server networking)
obj_depend_obj(api_client networking crypto json)
obj_depend_obj(key_tool api_client crypto)
obj_depend_obj(ditm pop_client pop_server maildir json)
obj_depend_obj(uv_util common)
obj_depend_obj(queue uv_interface uv_util link common)
obj_depend_obj(engine queue)
obj_depend_obj(loop engine)
obj_depend_obj(tls_engine engine networking)
obj_depend_obj(imap_engine engine imap_read)
obj_depend_obj(jsw_atree common)
obj_depend_obj(imap_expression imap_expression_print link common)
obj_depend_obj(imap_parse_tab imap_expression imap_scan common)
obj_depend_obj(imap_parse imap_parse_tab common)
add_dependencies(imap_parse_o bison_header)
obj_depend_obj(imap_scan imap_parse common)
add_dependencies(imap_scan_o bison_header)
obj_depend_obj(imap_read imap_parse imap_scan common)
add_dependencies(imap_read_o bison_header)
obj_depend_obj(imap_write imap_expression common)
obj_depend_obj(hashmap common)
obj_depend_obj(imap_maildir uv_util link hashmap fileops)
obj_depend_obj(imap_dirmgr imap_maildir jsw_atree)
obj_depend_obj(imap_session uv_interface common)
obj_depend_obj(imap_client imap_write common)


# describe executables with dependencies
# target_link_libraries(srvcli networking)
target_link_libraries(splintermail ui console_input ditm key_tool opt_parse)
target_link_libraries(leak networking)
target_link_libraries(encrypt_msg crypto opt_parse)
target_link_libraries(decrypt_msg crypto opt_parse)
target_link_libraries(olt common fileops opt_parse)
target_link_libraries(sm_fetch loop tls_engine imap_engine imap_dirmgr imap_client imap_session hashmap)

# decide if we are building server code
if("${BUILD_SERVER_CODE}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_SERVER_CODE=yes")

    # set a default MYSQL include directory
    if("${MYSQL_INCLUDE_DIR}" STREQUAL "")
        set(MYSQL_INCLUDE_DIR "/usr/include/mysql")
    endif()
    target_include_directories(encrypt_msg PUBLIC ${MYSQL_INCLUDE_DIR})

    # set a default MYSQL include directory
    if(NOT "${MYSQL_LIBRARIES}" STREQUAL "")
        target_link_libraries(encrypt_msg ${MYSQL_LIBRARIES})
        set(MYSQL_INCLUDE_DIR "-I/usr/include/mysql")
    endif()
    target_link_libraries(encrypt_msg mysqlclient)

    # badbadbad_alert needs non-strict aliasing due to sockaddr_info business
    target_compile_options(badbadbad_alert_o PRIVATE -fno-strict-aliasing)

    obj_depend_obj(badbadbad_alert common)
    target_link_libraries(encrypt_msg badbadbad_alert)
    target_link_libraries(badbadbad badbadbad_alert)
endif()

# OS-specific steps
if(APPLE)
    # include frameworks for networking.c
    find_library(SECURITY NAMES Security)
    find_library(COREFOUNDATION NAMES corefoundation)
    target_link_libraries(networking INTERFACE "${SECURITY}")
    target_link_libraries(networking INTERFACE "${COREFOUNDATION}")
elseif(UNIX) # Linux steps
    # Fedora Linux requires special PREFERRED_CIPHERS string
    # Autodetect, unless overridden via command line
    if("${USE_FEDORA_CIPHER_STRING}" STREQUAL "")
        if(EXISTS "/etc/crypto-policies/config")
            set(USE_FEDORA_CIPHER_STRING "true")
        else()
            set(USE_FEDORA_CIPHER_STRING "false")
        endif()
    endif()
    if("${USE_FEDORA_CIPHER_STRING}")
        # Set cipher string via compile definition...
        set(newdef "PREFERRED_CIPHERS=\"PROFILE=SYSTEM\"")
        # ... when compiling networking.c
        get_property(defs SOURCE networking.c PROPERTY COMPILE_DEFINITIONS)
        list(APPEND defs "${newdef}")
        set_property(SOURCE networking.c PROPERTY COMPILE_DEFINITIONS ${defs})
        # ... and through the networking interface library
        get_property(defs TARGET networking PROPERTY INTERFACE_COMPILE_DEFINITIONS)
        list(APPEND defs "${newdef}")
        set_property(TARGET networking PROPERTY INTERFACE_COMPILE_DEFINITIONS ${defs})
    endif()
elseif(WIN32)
    # generate versioning information
    configure_file("${PROJECT_SOURCE_DIR}/support_files/windows/version.rc"
        "${PROJECT_BINARY_DIR}/version.rc")
    # add versioning to splintermail.exe
    target_sources(splintermail INTERFACE "${PROJECT_BINARY_DIR}/version.rc")
    # additional dependencies for windows
    target_link_libraries(networking INTERFACE crypt32 ws2_32 gdi32)
    target_link_libraries(crypto INTERFACE crypt32 ws2_32 gdi32)
    obj_depend_lib(ui "" shlwapi)
else()
    message(FATAL_ERROR "OS not detected")
endif()

#### define tests
if("${BUILD_TESTS}")
    enable_testing()

    # OBJECT FILES (so they aren't being recompiled over and over)
    foreach(tgt ${TEST_OBJ_TGTS})
        add_library("${tgt}_o" OBJECT "test/${tgt}.c")
        define_file_basename_for_sources("${tgt}_o")
        add_library("${tgt}" INTERFACE)
        target_sources("${tgt}" INTERFACE $<TARGET_OBJECTS:${tgt}_o>)
    endforeach()

    # object intra-dependencies, make exectuable dependencies easier to express:
    # objects with dummy interfaces must not be dependencies here
    obj_depend_obj(fake_api_server crypto networking json)
    obj_depend_obj(fake_pop_server pop_server pop_client)
    obj_depend_obj(ditm_thread ditm dummy_key_tool fileops)
    obj_depend_obj(dummy_key_tool crypto common)
    obj_depend_obj(fake_engine loop tls_engine imap_engine fake_imap_logic queue networking)
    obj_depend_obj(fake_imap_logic uv_interface)
    # some dummy tools need openssl headers to compile... but not to link
    target_include_directories(dummy_key_tool_o PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_include_directories(dummy_ui_harness_o PRIVATE ${OPENSSL_INCLUDE_DIR})

    if("${USE_VALGRIND}")
        set(valgrind_cmd valgrind --quiet
                                  --leak-check=full
                                  --show-leak-kinds=all
                                  --errors-for-leak-kinds=all
                                  --error-exitcode=255)
    endif()

    # add test executables, tests and INCLUDE the root directory
    foreach(tgt ${TEST_EXE_TGTS})
        add_executable("${tgt}" "test/${tgt}.c")
        define_file_basename_for_sources("${tgt}")
        # all exectuables use opt_parse
        target_link_libraries("${tgt}" opt_parse)
        # add test
        add_test(NAME "do_${tgt}"
            COMMAND ${valgrind_cmd} "./${tgt}" "${PROJECT_SOURCE_DIR}/test/files")
    endforeach()

    # describe test executables with dependencies
    target_link_libraries(test_common common)
    target_link_libraries(test_fileops fileops)
    target_link_libraries(test_pop_server pop_server fileops)
    target_link_libraries(test_ditm ditm_thread fake_pop_server)
    target_link_libraries(test_ditm_errors ditm_thread fake_pop_server test_utils)
    target_link_libraries(test_maildir maildir)
    target_link_libraries(test_logger logger opt_parse)
    target_link_libraries(test_pop_client pop_client fake_pop_server)
    target_link_libraries(test_crypto crypto)
    target_link_libraries(test_opt_parse opt_parse)
    target_link_libraries(test_networking networking)
    target_link_libraries(test_json json)
    target_link_libraries(test_key_tool key_tool test_utils fake_api_server)
    target_link_libraries(test_ui ui dummy_ui_harness json)
    target_link_libraries(test_atree jsw_atree)
    target_link_libraries(test_imap_scan imap_scan)
    target_link_libraries(test_imap_read imap_read)
    target_link_libraries(test_imap_write imap_write)
    target_link_libraries(test_hashmap hashmap)
    # target_link_libraries(test_imap_maildir imap_maildir)
    target_link_libraries(test_loop fake_engine networking)
    target_link_libraries(test_queue queue)
    target_link_libraries(test_tls_engine fake_engine)
    target_link_libraries(test_imap_engine fake_engine)
    target_link_libraries(test_link link common)

    if(WIN32)
        # make sure the user provided the location of Pthreads
        if("${PTHREAD_ROOT_DIR}" STREQUAL "")
            message(FATAL_ERROR "If building for windows and BUILD_TESTS=yes, "
                                "then PTHREAD_ROOT_DIR must be specified.")
        endif()

        set(PT_incl "${PTHREAD_ROOT_DIR}/include")
        set(PT_lib "${PTHREAD_ROOT_DIR}/lib/$ENV{VSCMD_ARG_TGT_ARCH}/pthreadVC2.lib")
        add_library(winpthread INTERFACE)
        target_include_directories(winpthread INTERFACE "${PT_incl}")
        target_link_libraries(winpthread INTERFACE "${PT_lib}")
        obj_depend_obj(fake_api_server winpthread)
        obj_depend_obj(fake_pop_server winpthread)
        obj_depend_obj(ditm_thread winpthread)
        target_link_libraries(test_networking winpthread)
        target_link_libraries(test_pop_server winpthread)
        target_link_libraries(test_ui winpthread)
        # not necessary with the winpthread INTERFACE library:
        # obj_depend_lib(fake_api_server "${PT_incl}" "${PT_lib}")
        # obj_depend_lib(fake_pop_server "${PT_incl}" "${PT_lib}")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
        set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} -pthread")
    endif()

endif()

# compiler-specific flags
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # flags for gcc
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wextra")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-overflow")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wconversion")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-prototypes")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-prototypes")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstrict-aliasing")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing=2")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always")
    # libuv needs non-strict aliasing
    # see discussion here: https://github.com/libuv/libuv/issues/1230
    target_compile_options(uv_util_o PRIVATE -fno-strict-aliasing)
    target_compile_options(imap_parse_tab_o PRIVATE -Wno-sign-conversion)
    target_compile_options(imap_parse_tab_o PRIVATE -Wno-conversion)
    # add some compiler hardening flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat -Wformat-security -Werror=format-security")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE -fPIC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -D_FORTIFY_SOURCE=2")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -z relro")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -z now")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -z noexecstack")
    # this just gets ignored
    # set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -z noexecheap")
    # for debug builds
    # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")

elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # flags common to "AppleClang" and "Clang"
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Weverything")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-padded")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-gnu-zero-variadic-macro-arguments")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-reserved-id-macro")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-macros")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-double-promotion")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-noreturn")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-command-line-argument")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-overlength-strings")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-shadow")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstrict-aliasing")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
    # libuv needs non-strict aliasing
    # see discussion here: https://github.com/libuv/libuv/issues/1230
    target_compile_options(uv_util_o PRIVATE -fno-strict-aliasing)
    target_compile_options(imap_parse_tab_o PRIVATE -Wno-sign-conversion)
    target_compile_options(imap_parse_tab_o PRIVATE -Wno-conversion)
    # add some compiler hardening flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat -Wformat-security -Werror=format-security")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FORTIFY_SOURCE")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
        # clang options not apparently supported on AppleClang:
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,now")
        # control flow integrity
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=cfi")
        # test for integer overflow
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=integer")
        # stack-based buffer overrun protection
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=safe-stack")
        # address sanitizer (behaves a bit like valgrind):
        # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    endif()
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
    # see https://docs.microsoft.com/en-us/cpp/error-messages/compiler-warnings
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Wall")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4221") # ANSI compliance
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4204") # ANSI compliance
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4820") # byte padded structs
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4668") # unknown macro in #if()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4996") # _CRT_SECURE warnings
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd5045") # Spectre mitigation
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4706") # setting value in if() expr
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4710") # function didn't get inlined
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4711") # function got auto-inlined
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4127") # if() tests a constant value
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4738") # float stored in mem not reg
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4738") # float stored in mem not reg
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4456") # local vars shadow prev vars
    # These "unsafe" functions are just standard C functions
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /D_CRT_SECURE_NO_WARNINGS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /O2")
    # add some compiler hardening flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /sdl")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /GS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /DYNAMICBASE")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /guard:cf")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DYNAMICBASE")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /GUARD:CF")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NXCOMPAT")
    # statically link against Visual C++ library
    # (because vc_redist distributing is insanity, and this "just works")
    foreach(flag_var CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO)
       if(${flag_var} MATCHES "/MD")
          string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
       endif(${flag_var} MATCHES "/MD")
    endforeach(flag_var)
else()
    message(WARNING "compiler not detected...")
endif()


###############################################################################
#### SOURCE INSTALL
###############################################################################

install(TARGETS splintermail DESTINATION bin)

###############################################################################
#### BINARY INSTALLERS
###############################################################################

# some useful variables
set(supp_dir "${PROJECT_SOURCE_DIR}/support_files")
set(bin_dir "${PROJECT_BINARY_DIR}")

# functions take input files and copy them into OS-specific file trees

# set preprocessor to qwerrewq
set(PP "${supp_dir}/qwerrewq")
# set preprocessor config file to qwerrewq.json
set(PPCONF "${PROJECT_BINARY_DIR}/qwerrewq.json")
# set the preprocessor dynamic value for the operating system
if(APPLE)
    SET(OS_DYN "OSX")
elseif(UNIX)
    SET(OS_DYN "LINUX")
elseif(WIN32)
    SET(OS_DYN "WINDOWS")
endif()

function(binary_copy input output)
    get_filename_component(output_dir "${output}" DIRECTORY)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${input}" "${output}"
        DEPENDS "${input}"
        COMMENT "copying ${input}" VERBATIM
        )
endfunction()

# extra arguments become extra dependencies
function(preprocessed_copy input output)
    get_filename_component(output_dir "${output}" DIRECTORY)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND "${PYTHON_3_CMD}" "${PP}" "${PPCONF}" "${input}" "${output}"
                "--" "OS=${OS_DYN}" "BUILD_DIR=${PROJECT_BINARY_DIR}"
                     "SRC_DIR=${PROJECT_SOURCE_DIR}"
        DEPENDS "${input}" "${PP}" "${PPCONF}" ${ARGN}
        COMMENT "prepocessing ${input}" VERBATIM
        )
endfunction()

if(APPLE)

    # add installer-specific files
    set(conf "${supp_dir}/splintermail.conf")
    set(preinstall_in "${supp_dir}/osx/preinstall")
    set(preinstall_out "${PROJECT_BINARY_DIR}/installer/scripts/preinstall")
    set(postinstall_in "${supp_dir}/osx/postinstall")
    set(postinstall_out "${PROJECT_BINARY_DIR}/installer/scripts/postinstall")
    set(buildinfo_in "${supp_dir}/osx/build-info.json")
    set(buildinfo_out "${PROJECT_BINARY_DIR}/installer/build-info.json")
    preprocessed_copy("${conf}" "${conf_out}")
    preprocessed_copy("${preinstall_in}" "${preinstall_out}")
    preprocessed_copy("${postinstall_in}" "${postinstall_out}" "${conf}")
    preprocessed_copy("${buildinfo_in}" "${buildinfo_out}")
    # add installer-specific files to file dependencies
    list(APPEND file_deps "${preinstall_out}")
    list(APPEND file_deps "${postinstall_out}")
    list(APPEND file_deps "${buildinfo_out}")

    # add files that are directly installed
    set(payload "${PROJECT_BINARY_DIR}/installer/payload")
    set(preuninstall_in "${supp_dir}/osx/preuninstall.sh")
    set(preuninstall_out "${payload}/usr/local/share/splintermail/preuninstall.sh")
    set(preupgrade_in "${supp_dir}/osx/preupgrade.sh")
    set(preupgrade_out "${payload}/usr/local/share/splintermail/preupgrade.sh")
    set(keygen_in "${supp_dir}/keygen.sh")
    set(keygen_out "${payload}/usr/local/share/splintermail/keygen.sh")
    set(openssl_cnf_in "${supp_dir}/openssl.cnf")
    set(openssl_cnf_out "${payload}/usr/local/share/splintermail/openssl.cnf")
    set(license_txt_in "${supp_dir}/static_link_license.txt")
    set(license_txt_out "${payload}/usr/local/share/splintermail/LICENSE.txt")
    set(bashcomp_in "${supp_dir}/completions/bash/splintermail")
    set(bashcomp_out "${payload}/usr/local/etc/bash_completion.d/splintermail")
    set(zshcomp_in "${supp_dir}/completions/zsh/_splintermail")
    set(zshcomp_out "${payload}/usr/local/etc/zsh/site-functions/_splintermail")
    set(plist_in "${supp_dir}/osx/com.splintermail.ditm.plist")
    set(plist_out "${payload}/Library/LaunchDaemons/com.splintermail.ditm.plist")
    set(man_in "${supp_dir}/splintermail.1")
    set(man_out "${payload}/usr/local/share/man/man1/splintermail.1.gz")
    preprocessed_copy("${preuninstall_in}" "${preuninstall_out}")
    preprocessed_copy("${preupgrade_in}" "${preupgrade_out}")
    preprocessed_copy("${keygen_in}" "${keygen_out}")
    preprocessed_copy("${openssl_cnf_in}" "${openssl_cnf_out}")
    preprocessed_copy("${license_txt_in}" "${license_txt_out}")
    preprocessed_copy("${bashcomp_in}" "${bashcomp_out}")
    preprocessed_copy("${zshcomp_in}" "${zshcomp_out}")
    preprocessed_copy("${plist_in}" "${plist_out}")
    preprocessed_copy("${man_in}" "${man_out}")
    list(APPEND file_deps "${preuninstall_out}")
    list(APPEND file_deps "${preupgrade_out}")
    list(APPEND file_deps "${keygen_out}")
    list(APPEND file_deps "${openssl_cnf_out}")
    list(APPEND file_deps "${license_txt_out}")
    list(APPEND file_deps "${bashcomp_out}")
    list(APPEND file_deps "${zshcomp_out}")
    list(APPEND file_deps "${plist_out}")
    list(APPEND file_deps "${man_out}")

    # add the target that gets installed
    set(splintermail_out "${payload}/usr/local/bin/splintermail")
    binary_copy("splintermail" "${splintermail_out}")
    # add this target to the list of target dependencies
    list(APPEND tgt_deps splintermail)
    # add the output file to the list of file dependencies
    list(APPEND file_deps "${splintermail_out}")

    # custom command for actually building the "raw" installer
    set(raw_installer "${PROJECT_BINARY_DIR}/installer/build/install_splintermail-${PROJECT_VERSION}.pkg")
    add_custom_command(
        OUTPUT "${raw_installer}"
        COMMAND munkipkg --quiet installer
        DEPENDS ${file_deps} ${tgt_deps}
        COMMENT "creating raw installer" VERBATIM
        )

    # files and command for actually building the "final" installer
    set(resources "${PROJECT_BINARY_DIR}/installer/resources")
    set(license_rtf_in "${supp_dir}/static_link_license.rtf")
    set(license_rtf_out "${resources}/static_link_license.rtf")
    set(dist_in "${supp_dir}/osx/dist.xml")
    set(dist_out "${PROJECT_BINARY_DIR}/installer/dist.xml")
    preprocessed_copy("${license_rtf_in}" "${license_rtf_out}")
    preprocessed_copy("${dist_in}" "${dist_out}")

    set(installer_out "${PROJECT_BINARY_DIR}/install_splintermail-${PROJECT_VERSION}.pkg")
    add_custom_command(
        OUTPUT "${installer_out}"
        COMMAND productbuild --distribution "${dist_out}"
                --resources "${resources}"
                --package-path "${PROJECT_BINARY_DIR}/installer/build"
                "${installer_out}"
        DEPENDS "${raw_installer}" "${license_rtf_out}" "${dist_out}"
        COMMENT "creating final installer" VERBATIM
        )

    # finally, create the installer target
    add_custom_target(installer DEPENDS "${installer_out}")

    # also create the uninstaller
    set(preinstall_in "${supp_dir}/osx/uninstaller-pre")
    set(preinstall_out "${PROJECT_BINARY_DIR}/uninstaller/scripts/preinstall")
    set(postinstall_in "${supp_dir}/osx/uninstaller-post")
    set(postinstall_out "${PROJECT_BINARY_DIR}/uninstaller/scripts/postinstall")
    set(buildinfo_in "${supp_dir}/osx/uninstaller-build-info.json")
    set(buildinfo_out "${PROJECT_BINARY_DIR}/uninstaller/build-info.json")
    preprocessed_copy("${preinstall_in}" "${preinstall_out}")
    preprocessed_copy("${postinstall_in}" "${postinstall_out}")
    preprocessed_copy("${buildinfo_in}" "${buildinfo_out}")

    # custom command for actually building the uninstaller
    set(uninstaller_out "uninstaller/build/uninstall_splintermail.pkg")
    add_custom_command(
        OUTPUT "${uninstaller_out}"
        COMMAND munkipkg --quiet uninstaller
        DEPENDS "${preinstall_out}" "${postinstall_out}" "${buildinfo_out}"
        COMMENT "creating uninstaller" VERBATIM
        )

    # create the uninstaller target
    add_custom_target(uninstaller DEPENDS "${uninstaller_out}")

    # we should build the uninstaller whenever we build the installer
    add_dependencies(installer uninstaller)

elseif(UNIX)

    # the linux packagers work with a tarball as a least-common-denominator
    # list all files for source tarball:
    file(GLOB src_files
        RELATIVE "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/CMakeLists.txt"
        "${PROJECT_SOURCE_DIR}/*.c"
        "${PROJECT_SOURCE_DIR}/*.h"
        "${PROJECT_SOURCE_DIR}/test/*.c"
        "${PROJECT_SOURCE_DIR}/test/*.h"
        "${PROJECT_SOURCE_DIR}/support_files/splintermail.conf"
        "${PROJECT_SOURCE_DIR}/support_files/linux/splintermail.service"
        "${PROJECT_SOURCE_DIR}/support_files/completions/bash/*"
        "${PROJECT_SOURCE_DIR}/support_files/completions/zsh/*"
        "${PROJECT_SOURCE_DIR}/support_files/keygen.sh"
        "${PROJECT_SOURCE_DIR}/support_files/openssl.cnf"
        "${PROJECT_SOURCE_DIR}/support_files/splintermail.1"
        "${PROJECT_SOURCE_DIR}/UNLICENSE"
        )

    # copy source files into a directory for tarballing
    set(src_copy_dir "${PROJECT_BINARY_DIR}/installer/splintermail-${PROJECT_VERSION}")
    foreach(file ${src_files})
        set(tempout "${src_copy_dir}/${file}")
        preprocessed_copy( "${PROJECT_SOURCE_DIR}/${file}" "${tempout}")
        list(APPEND src_copies "${tempout}")
    endforeach()


    if("${PACKAGE_FOR}" STREQUAL "DEBIAN")

        # create the tarball of the source files
        set(srctar "${PROJECT_BINARY_DIR}/installer/splintermail_${PROJECT_VERSION}.orig.tar.gz")
        add_custom_command(
            OUTPUT "${srctar}"
            COMMAND tar -czf "${srctar}" "splintermail-${PROJECT_VERSION}"
            DEPENDS ${src_copies}
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building debian-friendly source tarball" VERBATIM
            )

        # copy the required debian files
        set(debdir "${src_copy_dir}/debian")
        foreach(file "postinst" "postrm" "control" "rules" "changelog" "compat" "source/format")
            preprocessed_copy("${supp_dir}/debian/${file}" "${debdir}/${file}")
            list(APPEND debfiles "${debdir}/${file}")
        endforeach()
        preprocessed_copy("${PROJECT_SOURCE_DIR}/UNLICENSE" "${debdir}/copyright")
        list(APPEND debfiles "${debdir}/copyright")

        # copy the systemd service over
        set(svcfile_in "${supp_dir}/linux/splintermail.service")
        set(svcfile_out "${debdir}/splintermail.service")
        preprocessed_copy("${svcfile_in}" "${svcfile_out}")
        list(APPEND debfiles "${svcfile_out}")

        # run some debian commands
        execute_process(COMMAND dpkg --print-architecture OUTPUT_VARIABLE debarch)
        string(STRIP "${debarch}" debarch)
        set(debpkg "${PROJECT_BINARY_DIR}/installer/splintermail_${PROJECT_VERSION}-1_${debarch}.deb")
        add_custom_command(
            OUTPUT "${debpkg}"
            COMMAND debuild -us -uc
            DEPENDS "${srctar}" ${debfiles}
            WORKING_DIRECTORY "${src_copy_dir}"
            COMMENT "building debian package" VERBATIM
            )
        add_custom_target(installer DEPENDS "${debpkg}")

        # unpack it all
        set(unpack_stamp "${PROJECT_BINARY_DIR}/installer/stamp.unpacked")
        add_custom_command(
            OUTPUT "${unpack_stamp}"
            COMMAND rm -rf "${PROJECT_BINARY_DIR}/installer/unpacked"
            COMMAND dpkg-deb -R "${debpkg}" "${PROJECT_BINARY_DIR}/installer/unpacked"
            DEPENDS "${debpkg}"
            COMMENT "unpacking debian package for inspection" VERBATIM
            )
        add_custom_target(unpacked DEPENDS "${unpack_stamp}")

    elseif("${PACKAGE_FOR}" STREQUAL "RHEL")

        # tarball into desired location
        set(srctar "$ENV{HOME}/rpmbuild/SOURCES/splintermail-${PROJECT_VERSION}.tar.gz")
        add_custom_command(
            OUTPUT "${srctar}"
            COMMAND tar -czf "${srctar}" "splintermail-${PROJECT_VERSION}"
            DEPENDS ${src_copies}
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building source tarball" VERBATIM
            )

        # preprocess spec file
        set(specin "${supp_dir}/rpm/splintermail.spec")
        set(specout "${PROJECT_BINARY_DIR}/installer/splintermail.spec")
        preprocessed_copy("${specin}" "${specout}")

        # execute rpmbuild
        execute_process(COMMAND rpm --eval "%{_arch}" OUTPUT_VARIABLE rpmarch)
        execute_process(COMMAND rpm --eval "%{?dist}" OUTPUT_VARIABLE builderdist)
        string(STRIP "${rpmarch}" rpmarch)
        string(STRIP "${builderdist}" builderdist)
        set(rpmout "$ENV{HOME}/rpmbuild/RPMS/${rpmarch}/splintermail-${PROJECT_VERSION}-1${builderdist}.${rpmarch}.rpm")
        add_custom_command(
            OUTPUT "${rpmout}"
            COMMAND rpmbuild  -bb "${specout}"
            DEPENDS "${srctar}" "${specout}"
            COMMENT "building RPM package" VERBATIM
            )
        add_custom_target(installer DEPENDS "${rpmout}")

    elseif("${PACKAGE_FOR}" STREQUAL "ARCH")

        # create the tarball of the source files
        set(srctar "${PROJECT_BINARY_DIR}/installer/splintermail-${PROJECT_VERSION}.tar.gz")
        add_custom_command(
            OUTPUT "${srctar}"
            COMMAND tar -czf "${srctar}" "splintermail-${PROJECT_VERSION}"
            DEPENDS ${src_copies}
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building debian-friendly source tarball" VERBATIM
            )

        # preprocess PKGBUILD and splintermail.install
        set(pkgbuild_in "${supp_dir}/archlinux/PKGBUILD")
        set(pkgbuild_out "${PROJECT_BINARY_DIR}/installer/PKGBUILD")
        set(install_in "${supp_dir}/archlinux/splintermail.install")
        set(install_out "${PROJECT_BINARY_DIR}/installer/splintermail.install")
        preprocessed_copy("${pkgbuild_in}" "${pkgbuild_out}")
        preprocessed_copy("${install_in}" "${install_out}")

        # execute makepkg
        set(pkg_arch "x86_64")
        set(pkgout "${PROJECT_BINARY_DIR}/installer/splintermail-${PROJECT_VERSION}-1-${pkg_arch}.pkg.tar.xz")
        add_custom_command(
            OUTPUT "${pkgout}"
            COMMAND makepkg -f
            DEPENDS "${srctar}" "${pkgbuild_out}" "${install_out}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building Archlinux package" VERBATIM
            )
        add_custom_target(installer DEPENDS "${pkgout}")

    endif()

    # Linux install locations
    # The actual installation should pull post-qwerrewq-processed files from
    # the tarball, which means the build scripts don't depend on python3

    # files to install, common to all OS's
    # files for /usr/share/splintermail
    install(FILES "${supp_dir}/openssl.cnf"
        DESTINATION "share/splintermail")
    install(FILES "${supp_dir}/keygen.sh"
        DESTINATION "share/splintermail"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)
    # default config file
    install(FILES "${supp_dir}/splintermail.conf"
        DESTINATION "/etc")
    # bash completion file
    install(FILES "${supp_dir}/completions/bash/splintermail"
        DESTINATION "share/bash-completion/completions")
    # man page
    # set("man_in" "${supp_dir}/splintermail.1")
    # set("man_gz" "${PROJECT_BINARY_DIR}/splintermail.1.gz")
    # add_custom_command(
    #     OUTPUT "${man_gz}"
    #     COMMAND "cat ${man_in}" | gzip > "${man_gz}"
    #     DEPENDS "${man_in}"
    #     COMMENT "gzipping man page" VERBATIM
    #     )
    # add_custom_target(man ALL DEPENDS "${man_gz}")
    # install(FILES "${man_gz}" DESTINATION "share/man/man1")
    install(FILES "${supp_dir}/splintermail.1" DESTINATION "share/man/man1")

    # custom install files, for building installers
    if("${PACKAGE_FOR}" STREQUAL "DEBIAN")
        # zsh completion file
        install(FILES "${supp_dir}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/vendor-completions")
        # systemd service is handled automatically by debian packaging scripts
        # license is handled automatically as well
    elseif("${PACKAGE_FOR}" STREQUAL "RHEL")
        # zsh completion file
        install(FILES "${supp_dir}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/site-functions")
        # systemd service
        install(FILES "${supp_dir}/linux/splintermail.service"
            DESTINATION "lib/systemd/system")
        # licence handled automatically by .spec file
    elseif("${PACKAGE_FOR}" STREQUAL "ARCH")
        # zsh completion file
        install(FILES "${supp_dir}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/site-functions")
        # systemd service
        install(FILES "${supp_dir}/linux/splintermail.service"
            DESTINATION "lib/systemd/system")
        # licence
        install(FILES "UNLICENSE"
            DESTINATION "share/licenses/splintermail")
    endif()

elseif(WIN32)

    # because AntiVirus software on windows will apparently block script-based
    # install actions... we have to write executables instead of scripts...
    # ... thanks for that, antivirus industry.  Thanks.

    # keygen.exe (will be installed)
    set(keygen_src_in "${supp_dir}/windows/keygen.c")
    set(keygen_src_out "${PROJECT_BINARY_DIR}/installer/keygen.c")
    preprocessed_copy("${keygen_src_in}" "${keygen_src_out}")
    add_executable(keygen "${keygen_src_out}")
    define_file_basename_for_sources(keygen)
    target_link_libraries(keygen fileops)
    list(APPEND tgt_deps keygen)

    # rm_rf.exe (will be embedded in installer for uninstallation)
    add_executable(rm_rf "${supp_dir}/windows/rm_rf.c")
    define_file_basename_for_sources(rm_rf)
    target_link_libraries(rm_rf fileops)
    list(APPEND tgt_deps rm_rf)

    # copy files into installer directory
    set(wxs_in "${supp_dir}/windows/splintermail.wxs")
    set(wxs_out "${PROJECT_BINARY_DIR}/installer/splintermail.wxs")
    set(conf_in "${supp_dir}/splintermail.conf")
    set(conf_out "${PROJECT_BINARY_DIR}/installer/splintermail.conf")
    set(openssl_cnf_in "${supp_dir}/openssl.cnf")
    set(openssl_cnf_out "${PROJECT_BINARY_DIR}/installer/openssl.cnf")
    set(license_txt_in "${supp_dir}/static_link_license.txt")
    set(license_txt_out "${PROJECT_BINARY_DIR}/installer/static_link_license.txt")
    preprocessed_copy("${wxs_in}" "${wxs_out}")
    preprocessed_copy("${conf_in}" "${conf_out}")
    preprocessed_copy("${openssl_cnf_in}" "${openssl_cnf_out}")
    preprocessed_copy("${license_txt_in}" "${license_txt_out}")
    list(APPEND file_deps "${wxs_out}")
    list(APPEND file_deps "${conf_out}")
    list(APPEND file_deps "${openssl_cnf_out}")
    list(APPEND file_deps "${license_txt_out}")

    # "compile" msi
    set(wixobj "${PROJECT_BINARY_DIR}/installer/splintermail.wixobj")
    set(openssl_bin_def "-dOPENSSL_BIN=${OPENSSL_ROOT_DIR}/bin/openssl.exe")
    add_custom_command(
        OUTPUT "${wixobj}"
        COMMAND candle "${openssl_bin_def}" "splintermail.wxs"
                -arch "$ENV{VSCMD_ARG_TGT_ARCH}"
        DEPENDS ${file_deps}
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
        )

    # "link" msi
    set(msi_out "${PROJECT_BINARY_DIR}/installer/splintermail.msi")
    add_custom_command(
        OUTPUT "${msi_out}"
        COMMAND light -ext WixUIExtension "${wixobj}"
        BYPRODUCTS "${PROJECT_BINARY_DIR}/installer/splintermail.wixpdb"
        DEPENDS "${wixobj}" splintermail ${tgt_deps}
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
        )
    add_custom_target(msi DEPENDS "${msi_out}")

    # create bundle
      # Only run for 64-bit version (depends on msi from 32-bit version)
      # Note that nested CMake projects would be complicated due to the
      # different environments that we have to have for each build
    if("$ENV{VSCMD_ARG_TGT_ARCH}" STREQUAL "x64")
        if("${BUILD32_DIR}" STREQUAL "")
            message(FATAL_ERROR "BUILD32_DIR not specified for 64-bit build")
        endif()
        # "compile" bundle
        set(bwxs_in "${supp_dir}/windows/bundle.wxs")
        set(bwxs_out "${PROJECT_BINARY_DIR}/installer/bundle.wxs")
        set(license_rtf_in "${supp_dir}/static_link_license.rtf")
        set(license_rtf_out "${PROJECT_BINARY_DIR}/installer/static_link_license.rtf")
        preprocessed_copy("${bwxs_in}" "${bwxs_out}")
        preprocessed_copy("${license_rtf_in}" "${license_rtf_out}")
        set(bwixobj "${PROJECT_BINARY_DIR}/installer/bundle.wixobj")
        add_custom_command(
            OUTPUT "${bwixobj}"
            COMMAND candle -ext WixBalExtension "bundle.wxs"
                    "-dBUILD32_DIR=${BUILD32_DIR}"
            DEPENDS "${bwxs_out}" "${license_rtf_out}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            )
        # "link" bundle
        set(msi32 "${BUILD32_DIR}/installer/splintermail.msi")
        set(bundle_out "${PROJECT_BINARY_DIR}/installer/install_splintermail-${PROJECT_VERSION}.exe")
        add_custom_command(
            OUTPUT "${bundle_out}"
            COMMAND light -ext WixBalExtension "${bwixobj}" -o "${bundle_out}"
            BYPRODUCTS "${PROJECT_BINARY_DIR}/installer/install_splintermail-${PROJECT_VERSION}.wixpdb"
            DEPENDS "${bwixobj}" "${msi_out}" "${msi32}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            )
        add_custom_target(bundle DEPENDS "${bundle_out}")
    endif()

endif()


###############################################################################
#### GENERATED SOURCE CODE (outputs of which will be held under git)
###############################################################################

# these outputs will be held under git

if("${REGEN_SRC}")
    set(print_help_in "${PROJECT_SOURCE_DIR}/print_help.h.in")
    set(print_help_out "${PROJECT_SOURCE_DIR}/print_help.h")
    preprocessed_copy("${print_help_in}" "${print_help_out}")
    add_custom_target(gen_src_code DEPENDS "${print_help_out}")
    add_dependencies(splintermail gen_src_code)

    set(man_in "${supp_dir}/splintermail.1")
    set(man_out "${PROJECT_BINARY_DIR}/splintermail.1")
    preprocessed_copy("${man_in}" "${man_out}")
    add_custom_target(manpage DEPENDS "${man_out}")
    add_dependencies(installer manpage)
endif()

