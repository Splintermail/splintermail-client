if(NOT WIN32)
    cmake_minimum_required(VERSION 3.1.0)
else()
    # Windows + cmake pukes if you can't use target_link_options (3.13.0), and
    # also CMP0091 is really nice (3.15.0).  Plus windows users would have to
    # install CMake manually, so there's no old package managers to support.
    cmake_minimum_required(VERSION 3.15.0)

    # statically link against Visual C++ library
    # (because vc_redist distributing is insanity, and this "just works")
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Strip the /MDd, /MD, and /W3 flags, which we always override.
    foreach(flag_var CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO)
        if(${flag_var} MATCHES "/MDd")
            string(REGEX REPLACE "/MDd" "" ${flag_var} "${${flag_var}}")
        endif()
        if(${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "" ${flag_var} "${${flag_var}}")
        endif()
        if(${flag_var} MATCHES "/W3")
            string(REGEX REPLACE "/W3" "" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
endif()

project(
    splintermail
    LANGUAGES C
    VERSION 0.4.0
)

set(MAINTAINER_NAME "Splintermail Dev")
set(MAINTAINER_EMAIL "dev@splintermail.com")
set(MAINTAINER "${MAINTAINER_NAME} <${MAINTAINER_EMAIL}>")

set(supp_dir "${PROJECT_SOURCE_DIR}/support_files")
set(bin_dir "${PROJECT_BINARY_DIR}")

string(TOLOWER "${CMAKE_BUILD_TYPE}" lowerbuild)
if("${lowerbuild}" STREQUAL "debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_DEBUG")
endif()

# python3 is needed for packaging the installers
if("${PYTHON_3_CMD}" STREQUAL "")
    set(PYTHON_3_CMD "python3")
endif()

if("${RE2C}" STREQUAL "")
    set(RE2C "re2c")
endif()

if("${BISON}" STREQUAL "")
    set(BISON "bison")
endif()

if("${REGEN_SRC}" STREQUAL "")
    set(REGEN_SRC "true")
endif()

if(APPLE)
    if("${MUNKIPKG}" STREQUAL "")
        set(MUNKIPKG "munkipkg")
    endif()
endif()

if("${DISABLE_ASAN}" STREQUAL "")
    if(NOT "${lowerbuild}" STREQUAL "debug")
        set(DISABLE_ASAN TRUE)
    endif()
endif()

# include OpenSSL
if("${BUILD_STATIC}")
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
if(NOT "${OPENSSL_ROOT_DIR}" STREQUAL "")
    # prepare to validate that find_package respected OPENSSL_ROOT_DIR
    # otherwise sometimes random builds laying around will get picked up
    get_filename_component(requested_ssl "${OPENSSL_ROOT_DIR}" ABSOLUTE)
endif()
find_package(OpenSSL REQUIRED)
if(NOT "${requested_ssl}" STREQUAL "")
    get_filename_component(chosen_ssl "${OPENSSL_INCLUDE_DIR}" ABSOLUTE)
    string(FIND "${chosen_ssl}" "${requested_ssl}" relpath_idx)
    if(NOT "${relpath_idx}" EQUAL "0")
        message("for source file: ${final_sourcefile}")
        message("chosen OPENSSL_ROOT is: ${chosen_ssl}")
        message("requested OPENSSL_ROOT is: ${requested_ssl}")
        message(WARNING "find_package did not respect OPENSSL_ROOT_DIR")
    endif()
endif()

# set preprocessor to qwerrewq
set(PP "${supp_dir}/qwerrewq")
# set preprocessor config file to qwerrewq.json
configure_file("${supp_dir}/qwerrewq.json" "${bin_dir}/qwerrewq.json" @ONLY)
set(PPCONF "${bin_dir}/qwerrewq.json")
# set the preprocessor dynamic value for the operating system
if(APPLE)
    SET(OS_DYN "OSX")
elseif(UNIX)
    SET(OS_DYN "LINUX")
elseif(WIN32)
    SET(OS_DYN "WINDOWS")
endif()

# extra arguments become extra dependencies
function(preprocessed_copy input output)
    get_filename_component(output_dir "${output}" DIRECTORY)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND "${PYTHON_3_CMD}" "${PP}" "${PPCONF}" "${input}" "${output}"
                "--" "OS=${OS_DYN}"
        DEPENDS "${input}" "${PP}" "${PPCONF}" ${ARGN}
        COMMENT "prepocessing ${input}" VERBATIM
        )
endfunction()

function(default_compile_options tgt allow_asan)
    # compiler-specific flags
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        # flags for gcc
        target_compile_options("${tgt}" PRIVATE "-Werror")
        target_compile_options("${tgt}" PRIVATE "-Wall")
        target_compile_options("${tgt}" PRIVATE "-Wextra")
        target_compile_options("${tgt}" PRIVATE "-Wswitch-enum")
        target_compile_options("${tgt}" PRIVATE "-Wstrict-overflow")
        target_compile_options("${tgt}" PRIVATE "-Wconversion")
        target_compile_options("${tgt}" PRIVATE "-Wstrict-prototypes")
        target_compile_options("${tgt}" PRIVATE "-Wmissing-prototypes")
        target_compile_options("${tgt}" PRIVATE "-fstrict-aliasing")
        target_compile_options("${tgt}" PRIVATE "-Wstrict-aliasing=2")
        target_compile_options("${tgt}" PRIVATE "-Wno-missing-field-initializers")
        target_compile_options("${tgt}" PRIVATE "-Wvla")
        target_compile_options("${tgt}" PRIVATE "-fdiagnostics-color=always")
        # add some compiler hardening flags
        target_compile_options("${tgt}" PRIVATE "-Wformat" "-Wformat-security" "-Werror=format-security")
        target_compile_options("${tgt}" PRIVATE "-fstack-protector-strong")
        target_compile_options("${tgt}" PRIVATE "-fPIE" "-fPIC")
        target_compile_options("${tgt}" PRIVATE "-O2" "-D_FORTIFY_SOURCE=2")
        target_link_libraries("${tgt}" PRIVATE "-Wl,-z,relro")
        target_link_libraries("${tgt}" PRIVATE "-Wl,-z,now")
        target_link_libraries("${tgt}" PRIVATE "-Wl,-z,noexecstack")
        # this just gets ignored
        # target_link_libraries("${tgt}" PRIVATE "-z noexecheap")
        if(NOT DISABLE_ASAN AND ("${allow_asan}"))
            target_compile_options("${tgt}" PRIVATE "-fsanitize=address" "-fno-omit-frame-pointer")
            target_link_libraries("${tgt}" PRIVATE "-fsanitize=address" "-fno-omit-frame-pointer")
        endif()
        # executables only:
        get_target_property(tgt_type "${tgt}" TYPE)
        if("${tgt_type}" STREQUAL "EXECUTABLE")
            target_link_libraries("${tgt}" PRIVATE "-pie")
        endif()

    elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
        # flags common to "AppleClang" and "Clang"
        target_compile_options("${tgt}" PRIVATE "-Werror")
        target_compile_options("${tgt}" PRIVATE "-fdiagnostics-color=always")
        target_compile_options("${tgt}" PRIVATE "-Weverything")
        target_compile_options("${tgt}" PRIVATE "-Wno-padded")
        target_compile_options("${tgt}" PRIVATE "-Wno-gnu-zero-variadic-macro-arguments")
        target_compile_options("${tgt}" PRIVATE "-Wno-reserved-id-macro")
        target_compile_options("${tgt}" PRIVATE "-Wno-unused-macros")
        target_compile_options("${tgt}" PRIVATE "-Wno-double-promotion")
        target_compile_options("${tgt}" PRIVATE "-Wno-missing-noreturn")
        target_compile_options("${tgt}" PRIVATE "-Wno-missing-field-initializers")
        target_compile_options("${tgt}" PRIVATE "-Wno-unused-command-line-argument")
        target_compile_options("${tgt}" PRIVATE "-Wno-overlength-strings")
        target_compile_options("${tgt}" PRIVATE "-Wno-shadow")
        target_compile_options("${tgt}" PRIVATE "-Wno-disabled-macro-expansion")
        target_compile_options("${tgt}" PRIVATE "-Wno-covered-switch-default")
        target_compile_options("${tgt}" PRIVATE "-Wno-unreachable-code-break")
        target_compile_options("${tgt}" PRIVATE "-Wvla")
        target_compile_options("${tgt}" PRIVATE "-fstrict-aliasing")
        target_compile_options("${tgt}" PRIVATE "-O2")
        # add some compiler hardening flags
        target_compile_options("${tgt}" PRIVATE "-Wformat" "-Wformat-security" "-Werror=format-security")
        target_compile_options("${tgt}" PRIVATE "-D_FORTIFY_SOURCE")
        # target_compile_options("${tgt}" PRIVATE "-fstack-protector-strong")
        target_compile_options("${tgt}" PRIVATE "-fPIE" "-fPIC" "-Wno-unused-command-line-argument")
        if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
            # clang options not apparently supported on AppleClang:
            target_link_libraries("${tgt}" PRIVATE "-Wl,-z,relro")
            target_link_libraries("${tgt}" PRIVATE "-Wl,-z,now")
            target_compile_options("${tgt}" PRIVATE "-fvisibility=default")
            if(NOT DISABLE_ASAN AND ("${allow_asan}"))
                target_compile_options("${tgt}" PRIVATE "-fsanitize=address" "-fno-omit-frame-pointer")
                target_link_libraries("${tgt}" PRIVATE "-fsanitize=address" "-fno-omit-frame-pointer")
                # control flow integrity
                target_compile_options("${tgt}" PRIVATE "-fsanitize=cfi")
                target_compile_options("${tgt}" PRIVATE "-flto")
                target_link_libraries("${tgt}" PRIVATE "-flto")
                # test for integer overflow
                target_compile_options("${tgt}" PRIVATE "-fsanitize=integer")
                # stack-based buffer overrun protection (not compatible with fsanitize=address)
                # target_compile_options("${tgt}" PRIVATE "-fsanitize=safe-stack")
            endif()
        else()
            # Apple-only.  Not sure where it thinks it's cross-compiling from.
            # This should only fire for local source builds.
            if(NOT "${BUILD_STATIC}")
                target_compile_options("${tgt}" PRIVATE "-Wno-poison-system-directories")
            endif()
        endif()
        # executables only:
        get_target_property(tgt_type "${tgt}" TYPE)
        if("${}" STREQUAL "EXECUTABLE")
            target_link_libraries("${tgt}" PRIVATE "-pie")
        endif()

    elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # flags for MVSC (VS 2017)
        # see https://docs.microsoft.com/en-us/cpp/error-messages/compiler-warnings
        target_compile_options("${tgt}" PRIVATE "/W4") # show all the warnings
        target_compile_options("${tgt}" PRIVATE "/Wall") # no, really, show all of them
        target_compile_options("${tgt}" PRIVATE "/WX") # warnings are errors
        target_compile_options("${tgt}" PRIVATE "/wd4214") # ANSI compliance: non-int bitfields
        target_compile_options("${tgt}" PRIVATE "/wd4221") # ANSI compliance
        target_compile_options("${tgt}" PRIVATE "/wd4204") # ANSI compliance
        target_compile_options("${tgt}" PRIVATE "/wd4820") # byte padded structs
        target_compile_options("${tgt}" PRIVATE "/wd4668") # unknown macro in #if()
        target_compile_options("${tgt}" PRIVATE "/wd4996") # _CRT_SECURE warnings
        target_compile_options("${tgt}" PRIVATE "/wd5045") # Spectre mitigation
        target_compile_options("${tgt}" PRIVATE "/wd4706") # setting value in if() expr
        target_compile_options("${tgt}" PRIVATE "/wd4710") # function didn't get inlined
        target_compile_options("${tgt}" PRIVATE "/wd4711") # function got auto-inlined
        target_compile_options("${tgt}" PRIVATE "/wd4127") # if() tests a constant value
        target_compile_options("${tgt}" PRIVATE "/wd4738") # float stored in mem not reg
        target_compile_options("${tgt}" PRIVATE "/wd4738") # float stored in mem not reg
        target_compile_options("${tgt}" PRIVATE "/wd4456") # local vars shadow prev vars
        target_compile_options("${tgt}" PRIVATE "/wd5031") # bad #pragma warning(push)
        target_compile_options("${tgt}" PRIVATE "/wd5032") # bad #pragma warning(pop)
        # These "unsafe" functions are just standard C functions
        target_compile_options("${tgt}" PRIVATE "/D_CRT_SECURE_NO_WARNINGS")
        # add some compiler hardening flags
        target_compile_options("${tgt}" PRIVATE "/sdl")
        target_compile_options("${tgt}" PRIVATE "/GS")
        target_compile_options("${tgt}" PRIVATE "/DYNAMICBASE")
        target_compile_options("${tgt}" PRIVATE "/guard:cf")
        target_link_options("${tgt}" PRIVATE "/DYNAMICBASE")
        target_link_options("${tgt}" PRIVATE "/GUARD:CF")
        target_link_options("${tgt}" PRIVATE "/NXCOMPAT")

    else()
        message(FATAL_ERROR "compiler not detected...")
    endif()
endfunction()


function(source_compile_options src)
    get_property(opts SOURCE "${src}" PROPERTY COMPILE_OPTIONS)
    list(APPEND opts ${ARGN})
    set_property(SOURCE "${src}" PROPERTY COMPILE_OPTIONS ${opts})
endfunction()


# Add -DFILE_BASENAME="logname" to the compile options for a source file.
function(define_file_logname_for_source orig_sourcefile final_sourcefile)
    # detect if it has been set already
    get_property(defs SOURCE "${final_sourcefile}"
        PROPERTY COMPILE_DEFINITIONS)
    foreach(def ${defs})
        string(FIND "${def}" "FILE_BASENAME=" idx)
        if("${idx}" EQUAL "0")
            return()
        endif()
    endforeach()

    get_filename_component(projpath "${PROJECT_SOURCE_DIR}" ABSOLUTE)
    string(LENGTH "${projpath}" projpath_len)
    math(EXPR projpath_rm_len "${projpath_len} + 1")

    # Ensure that the projpath can be removed from the orig_sourcefile path
    get_filename_component(srcpath "${orig_sourcefile}" ABSOLUTE)
    string(FIND "${srcpath}" "${projpath}" relpath_idx)
    if(NOT "${relpath_idx}" EQUAL "0")
        message("for source file: ${final_sourcefile}")
        message("srcpath is: ${srcpath}")
        message("projpath is: ${projpath}")
        message( FATAL_ERROR "srcpath does not start with projpath!")
    endif()
    # Remove the projpath and a trailing '/' from the sourcefile path
    string(SUBSTRING "${srcpath}" "${projpath_rm_len}" -1 logname)
    list(APPEND defs "FILE_BASENAME=\"${logname}\"")
    # Set the updated compile definitions on the source file.
    set_property(
        SOURCE "${final_sourcefile}"
        PROPERTY COMPILE_DEFINITIONS ${defs})
endfunction()

# Will add -DFILE_BASENAME="logname" for each source file depended on
# by tgt, where filename is the name of the file.  Needed for logger.h
function(define_file_logname_for_target targetname)
    get_target_property(source_files "${targetname}" SOURCES)
    foreach(sourcefile ${source_files})
        define_file_logname_for_source("${sourcefile}" "${sourcefile}")
    endforeach()
endfunction()

# always include the source directory (for compiling generated .c files)
include_directories("${PROJECT_SOURCE_DIR}")
# always include the binary directory (for including generated .h files)
include_directories("${PROJECT_BINARY_DIR}")

add_subdirectory(tools)

add_subdirectory(libdstr)
add_subdirectory(libuvthread)
add_subdirectory(libcrypto)
add_subdirectory(libditm)
add_subdirectory(libparsing)
add_subdirectory(liburl)
add_subdirectory(libimap)
add_subdirectory(libimaildir)
add_subdirectory(libengine)
add_subdirectory(libcitm)

add_library(api_client api_client.c)
define_file_logname_for_target(api_client)
default_compile_options(api_client TRUE)
target_link_libraries(api_client PUBLIC crypto)

add_library(console_input console_input.c)
define_file_logname_for_target(console_input)
default_compile_options(console_input TRUE)

add_library(ui_harness ui_harness.c)
define_file_logname_for_target(ui_harness)
default_compile_options(ui_harness TRUE)

set(print_help_in "${PROJECT_SOURCE_DIR}/print_help.h.in")
set(print_help_out "${PROJECT_BINARY_DIR}/print_help.h")
preprocessed_copy("${print_help_in}" "${print_help_out}")
define_file_logname_for_source("${print_help_in}" "${print_help_out}")

add_library(ui ui.c "${print_help_out}")
define_file_logname_for_target(ui)
default_compile_options(ui TRUE)
target_link_libraries(ui PUBLIC ui_harness api_client crypto dstr)

add_executable(splintermail splintermail.c)
default_compile_options(splintermail TRUE)
define_file_logname_for_target(splintermail)

add_executable(encrypt_msg encrypt_msg.c)
default_compile_options(encrypt_msg TRUE)
define_file_logname_for_target(encrypt_msg)

add_executable(decrypt_msg decrypt_msg.c)
default_compile_options(decrypt_msg TRUE)
define_file_logname_for_target(decrypt_msg)

add_executable(inject_local_msg inject_local_msg.c)
default_compile_options(inject_local_msg TRUE)
define_file_logname_for_target(inject_local_msg)

target_link_libraries(splintermail PRIVATE ui console_input ditm libcitm)
target_link_libraries(encrypt_msg PRIVATE crypto)
target_link_libraries(decrypt_msg PRIVATE crypto)
target_link_libraries(inject_local_msg PRIVATE imaildir crypto)

# decide if we are building server code
if(BUILD_SERVER_CODE)
    add_subdirectory(server)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_SERVER_CODE=yes")
    target_link_libraries(encrypt_msg PRIVATE badbadbad_alert mysql_util libsmsql)
endif()

# OS-specific steps
if(APPLE)
    # nothing top-level yet
elseif(UNIX) # Linux steps
    # nothing top-level yet
elseif(WIN32)
    # generate versioning information
    configure_file("${supp_dir}/windows/version.rc"
        "${PROJECT_BINARY_DIR}/version.rc")
    # add versioning to splintermail.exe
    target_sources(splintermail INTERFACE "${PROJECT_BINARY_DIR}/version.rc")
    # additional dependencies for windows
    target_link_libraries(ui PUBLIC shlwapi)
else()
    message(FATAL_ERROR "OS not detected")
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()


###############################################################################
#### SOURCE INSTALL
###############################################################################

install(TARGETS splintermail DESTINATION bin)

###############################################################################
#### BINARY INSTALLERS
###############################################################################

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
    set(plist_in "${supp_dir}/osx/com.splintermail.citm.plist")
    set(plist_out "${payload}/Library/LaunchDaemons/com.splintermail.citm.plist")
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
        COMMAND "${MUNKIPKG}" --quiet installer
        DEPENDS ${file_deps} ${tgt_deps}
        COMMENT "creating raw installer" VERBATIM
        )
    add_custom_target(raw_installer DEPENDS "${raw_installer}")

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
    add_custom_target(installer DEPENDS raw_installer "${installer_out}")

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
        COMMAND "${MUNKIPKG}" --quiet uninstaller
        DEPENDS "${preinstall_out}" "${postinstall_out}" "${buildinfo_out}"
        COMMENT "creating uninstaller" VERBATIM
        )

    # create the uninstaller target
    add_custom_target(uninstaller DEPENDS "${uninstaller_out}")

    # we should build the uninstaller whenever we build the installer
    add_dependencies(installer uninstaller)

elseif(UNIX)
    # linux source install
    set(supp_build "${PROJECT_BINARY_DIR}/support_files")
    preprocessed_copy("${supp_dir}/openssl.cnf" "${supp_build}/openssl.cnf")
    preprocessed_copy("${supp_dir}/keygen.sh" "${supp_build}/keygen.sh")
    preprocessed_copy("${supp_dir}/splintermail.conf" "${supp_build}/splintermail.conf")
    preprocessed_copy("${supp_dir}/completions/bash/splintermail" "${supp_build}/completions/bash/splintermail")
    preprocessed_copy("${supp_dir}/completions/zsh/_splintermail" "${supp_build}/completions/zsh/_splintermail")
    preprocessed_copy("${supp_dir}/linux/splintermail.service" "${supp_build}/linux/splintermail.service")
    preprocessed_copy("${supp_dir}/splintermail.1" "${supp_build}/splintermail.1")
    preprocessed_copy("${supp_dir}/linux/splintermail.service" "${supp_build}/linux/splintermail.service")
    add_custom_target(pp_openssl_cnf ALL DEPENDS "${supp_build}/openssl.cnf")
    add_custom_target(pp_keygen_sh ALL DEPENDS "${supp_build}/keygen.sh")
    add_custom_target(pp_splintermail_conf ALL DEPENDS "${supp_build}/splintermail.conf")
    add_custom_target(pp_completions_bash ALL DEPENDS "${supp_build}/completions/bash/splintermail")
    add_custom_target(pp_completions_zsh ALL DEPENDS "${supp_build}/completions/zsh/_splintermail")
    add_custom_target(pp_man ALL DEPENDS "${supp_build}/splintermail.1")
    add_custom_target(pp_splintermail_service ALL DEPENDS "${supp_build}/linux/splintermail.service")

    install(FILES "${supp_build}/openssl.cnf"
        DESTINATION "share/splintermail")

    install(FILES "${supp_build}/keygen.sh"
        DESTINATION "share/splintermail"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

    # bash completion file
    install(FILES "${supp_build}/completions/bash/splintermail"
        DESTINATION "share/bash-completion/completions")

    # zsh completion file
    if("${PACKAGE_FOR}" STREQUAL "DEBIAN")
        install(FILES "${supp_build}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/vendor-completions")
    elseif("${PACKAGE_FOR}" STREQUAL "RHEL")
        install(FILES "${supp_build}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/site-functions")
    elseif("${PACKAGE_FOR}" STREQUAL "ARCH")
        install(FILES "${supp_build}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/site-functions")
    elseif(EXISTS "/usr/share/zsh/vendor-completions")
        # autodetect: debian-like
        install(FILES "${supp_build}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/vendor-completions")
    else()
        # autodetect: non-debian-like
        install(FILES "${supp_build}/completions/zsh/_splintermail"
            DESTINATION "share/zsh/site-functions")
    endif()

    # man page
    set("man_in" "${supp_build}/splintermail.1")
    set("man_gz" "${supp_build}/splintermail.1.gz")
    add_custom_command(
        OUTPUT "${man_gz}"
        COMMAND cat "${man_in}"
                | gzip > "${man_gz}.temp"
                && mv "${man_gz}.temp" "${man_gz}"
        DEPENDS "${man_in}"
        COMMENT "gzipping man page" VERBATIM
        )
    add_custom_target(man ALL DEPENDS "${man_gz}")
    install(FILES "${man_gz}" DESTINATION "share/man/man1")

    if(NOT "${PACKAGE_FOR}" STREQUAL "")
        # default config file, only installed when we are building packages
        install(FILES "${supp_build}/splintermail.conf" DESTINATION "/etc")
    endif()

    # custom install files, for building installers
    if("${PACKAGE_FOR}" STREQUAL "DEBIAN")
        # systemd service is handled automatically by debian packaging scripts
        # license is handled automatically as well
    elseif("${PACKAGE_FOR}" STREQUAL "RHEL")
        # systemd service
        install(FILES "${supp_build}/linux/splintermail.service"
            DESTINATION "lib/systemd/system")
        # licence handled automatically by .spec file
    elseif("${PACKAGE_FOR}" STREQUAL "ARCH")
        # systemd service
        install(FILES "${supp_build}/linux/splintermail.service"
            DESTINATION "lib/systemd/system")
        # licence
        install(FILES "UNLICENSE"
            DESTINATION "share/licenses/splintermail")
    endif()

    if("${PACKAGE_FOR}" STREQUAL "DEBIAN")
        # copy the required debian files
        set(debdir "${PROJECT_BINARY_DIR}/installer/debian")
        foreach(file "postinst" "postrm" "control" "rules" "changelog" "compat" "source/format")
            preprocessed_copy("${supp_dir}/debian/${file}" "${debdir}/${file}")
            list(APPEND debfiles "${debdir}/${file}")
        endforeach()
        preprocessed_copy("${PROJECT_SOURCE_DIR}/UNLICENSE" "${debdir}/copyright")
        list(APPEND debfiles "${debdir}/copyright")
        preprocessed_copy("${supp_dir}/debian/makefile" "${PROJECT_BINARY_DIR}/installer/makefile")
        list(APPEND debfiles "${PROJECT_BINARY_DIR}/installer/makefile")

        # copy the systemd service over
        set(svcfile_in "${supp_dir}/linux/splintermail.service")
        set(svcfile_out "${debdir}/splintermail.service")
        preprocessed_copy("${svcfile_in}" "${svcfile_out}")
        list(APPEND debfiles "${svcfile_out}")

        # run some debian commands
        execute_process(COMMAND dpkg --print-architecture OUTPUT_VARIABLE debarch)
        string(STRIP "${debarch}" debarch)
        set(debpkg "${PROJECT_BINARY_DIR}/splintermail_${PROJECT_VERSION}-1_${debarch}.deb")
        add_custom_command(
            OUTPUT "${debpkg}"
            COMMAND debuild -i -us -uc -b
            DEPENDS ${debfiles}
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building debian package" VERBATIM
            )
        add_custom_command(OUTPUT pkgname COMMAND echo "${debpkg}" > pkgname)
        add_custom_target(installer DEPENDS "${debpkg}" pkgname)

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
        # preprocess spec file
        set(specin "${supp_dir}/rpm/splintermail.spec")
        set(specout "${PROJECT_BINARY_DIR}/installer/splintermail.spec")
        preprocessed_copy("${specin}" "${specout}")

        # execute rpmbuild
        execute_process(COMMAND rpm --eval "%{_arch}" OUTPUT_VARIABLE rpmarch)
        execute_process(COMMAND rpm --eval "%{?dist}" OUTPUT_VARIABLE builderdist)
        string(STRIP "${rpmarch}" rpmarch)
        string(STRIP "${builderdist}" builderdist)
        set(rpmname "splintermail-${PROJECT_VERSION}-1${builderdist}.${rpmarch}.rpm")
        set(rpmout "$ENV{HOME}/rpmbuild/RPMS/${rpmarch}/${rpmname}")
        set(rpmfinal "${PROJECT_BINARY_DIR}/installer/${rpmname}")
        add_custom_command(
            OUTPUT "${rpmfinal}"
            # this will fail on errors
            COMMAND rpmlint --info "${specout}"
            # this will fail on warnings
            COMMAND sh -c "! (rpmlint '${specout}' 2>&1 | grep -q ': W:')"
            COMMAND rpmbuild  -bb "${specout}"
            COMMAND mv "${rpmout}" "${rpmfinal}"
            DEPENDS "${specout}"
            COMMENT "building RPM package" VERBATIM
            )
        add_custom_command(OUTPUT pkgname COMMAND echo "${rpmfinal}" > pkgname)
        add_custom_target(installer DEPENDS "${rpmfinal}" pkgname)

    elseif("${PACKAGE_FOR}" STREQUAL "ARCH")
        # copy the PKGBUILD with the dummy build system
        set(pkgbuild_in "${supp_dir}/archlinux/PKGBUILD")
        set(pkgbuild_out "${PROJECT_BINARY_DIR}/installer/PKGBUILD")
        set(install_in "${supp_dir}/archlinux/splintermail.install")
        set(install_out "${PROJECT_BINARY_DIR}/installer/splintermail.install")
        preprocessed_copy("${pkgbuild_in}" "${pkgbuild_out}")
        preprocessed_copy("${install_in}" "${install_out}")

        # execute makepkg
        set(pkg_arch "x86_64")
        set(pkgout "${PROJECT_BINARY_DIR}/installer/splintermail-${PROJECT_VERSION}-1-${pkg_arch}.pkg.tar.zst")
        add_custom_command(
            OUTPUT "${pkgout}"
            COMMAND makepkg -f
            DEPENDS "${pkgbuild_out}" "${install_out}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/installer"
            COMMENT "building Archlinux package" VERBATIM
            )
        add_custom_command(OUTPUT pkgname COMMAND echo "${pkgout}" > pkgname)
        add_custom_target(installer DEPENDS "${pkgout}" pkgname)

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
    default_compile_options(keygen FALSE)
    define_file_logname_for_source("${keygen_src_in}" "${keygen_src_out}")
    target_link_libraries(keygen PRIVATE dstr)
    list(APPEND tgt_deps keygen)

    # rm_rf.exe (will be embedded in installer for uninstallation)
    add_executable(rm_rf "${supp_dir}/windows/rm_rf.c")
    default_compile_options(rm_rf FALSE)
    define_file_logname_for_target(rm_rf)
    target_link_libraries(rm_rf PRIVATE dstr)
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
        add_custom_target(installer DEPENDS "${bundle_out}")
    endif()

endif()
