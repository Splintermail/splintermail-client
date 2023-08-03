#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"
#include "ui_harness.h"

// pass all calls to the real thing
ui_harness_t harness = {
    .dir_r_access_path = dir_r_access_path,
    .dir_w_access_path = dir_w_access_path,
    .dir_rw_access_path = dir_rw_access_path,
    .file_r_access_path = file_r_access_path,
    .file_w_access_path = file_w_access_path,
    .file_rw_access_path = file_rw_access_path,
    .exists_path = exists_path,
    .for_each_file_in_dir = for_each_file_in_dir,
    .ssl_library_init = ssl_library_init,
    .ssl_library_close = ssl_library_close,
};
