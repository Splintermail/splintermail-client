sm_lib(crypto networking.c crypto.c ssl_errors.c cmp.c DEPS dstr NOASAN libs)

# per-variant tweaks
foreach(lib ${libs})
    target_include_directories("${lib}" PUBLIC "${OPENSSL_INCLUDE_DIR}")
    target_link_libraries("${lib}" PUBLIC "${OPENSSL_LIBRARIES}")
    # OS-specific steps
    if(APPLE)
        # include frameworks for networking.c
        find_library(SECURITY NAMES Security)
        find_library(COREFOUNDATION NAMES corefoundation)
        target_link_libraries("${lib}" PUBLIC "${SECURITY}")
        target_link_libraries("${lib}" PUBLIC "${COREFOUNDATION}")
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
            target_compile_definitions(
                "${lib}" PRIVATE "PREFERRED_CIPHERS=\"PROFILE=SYSTEM\""
            )
        endif()
    elseif(WIN32)
        # additional dependencies for windows
        target_link_libraries("${lib}" INTERFACE crypt32 ws2_32 gdi32)
    else()
        message(FATAL_ERROR "OS not detected")
    endif()
endforeach()

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
