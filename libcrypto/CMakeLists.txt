macro(add_crypto asan)
    add_library(
        "crypto${asan}"
        networking.c
        crypto.c
        ssl_errors.c
        cmp.c
    )
    target_include_directories("crypto${asan}" PUBLIC "${OPENSSL_INCLUDE_DIR}")
    # TODO: refactor this so you don't need libuv to include networking code?
    # it's only used for refs_t in the _keypair_t.
    target_link_libraries("crypto${asan}" PUBLIC "dstr${asan}" "uvthread${asan}" "${OPENSSL_LIBRARIES}")
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("crypto${asan}" FALSE)
    else()
        default_compile_options("crypto${asan}" TRUE)
    endif()
    define_file_logname_for_target("crypto${asan}")

    # OS-specific steps
    if(APPLE)
        # include frameworks for networking.c
        find_library(SECURITY NAMES Security)
        find_library(COREFOUNDATION NAMES corefoundation)
        target_link_libraries("crypto${asan}" PUBLIC "${SECURITY}")
        target_link_libraries("crypto${asan}" PUBLIC "${COREFOUNDATION}")
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
            target_compile_definitions("crypto${asan}" "PREFERRED_CIPHERS=\"PROFILE=SYSTEM\"")
        endif()
    elseif(WIN32)
        # additional dependencies for windows
        target_link_libraries("crypto${asan}" INTERFACE crypt32 ws2_32 gdi32)
    else()
        message(FATAL_ERROR "OS not detected")
    endif()
endmacro()

add_crypto("")

# cmp.c should not use any compiler optimizations to make absolutely sure it
# is constant time.
if(WIN32)
source_compile_options(cmp.c -U_FORTIFY_SOURCE /Od)
else()
source_compile_options(cmp.c -U_FORTIFY_SOURCE -O0)
endif()


# Uncomment to view the assember output so you can verify constant time.
# (probably just build libcrypto_noasan and check cmp.s)
# source_compile_options(cmp.c -Wa,-adhln=cmp.s -fverbose-asm -masm=intel)

if("${CMAKE_BUILD_TYPE}" STREQUAL "debug")
    # rebuild without asan
    add_crypto("_noasan")
else()
    # release builds already have no asan
    add_library(crypto_noasan ALIAS crypto)
endif()
