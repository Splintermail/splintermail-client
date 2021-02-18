add_library(
    crypto
    networking.c
    crypto.c
    ssl_errors.c
)
target_include_directories(crypto PUBLIC "${OPENSSL_INCLUDE_DIR}")
# TODO: refactor this so you don't need libuv to include networking code?
# it's only used for refs_t in the _keypair_t.
target_link_libraries(crypto dstr uvthread "${OPENSSL_LIBRARIES}")
default_compile_options(crypto TRUE)
define_file_logname_for_target(crypto)

# OS-specific steps
if(APPLE)
    # include frameworks for networking.c
    find_library(SECURITY NAMES Security)
    find_library(COREFOUNDATION NAMES corefoundation)
    target_link_libraries(crypto "${SECURITY}")
    target_link_libraries(crypto "${COREFOUNDATION}")
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
        target_compile_definitions(crypto "PREFERRED_CIPHERS=\"PROFILE=SYSTEM\"")
    endif()
elseif(WIN32)
    # additional dependencies for windows
    target_link_libraries(crypto INTERFACE crypt32 ws2_32 gdi32)
else()
    message(FATAL_ERROR "OS not detected")
endif()
