#include "libdstr/libdstr.h"
#include "ui_harness.h"

// pass all calls to the real thing
ui_harness_t harness = {
    .dir_r_access = dir_r_access,
    .dir_w_access = dir_w_access,
    .dir_rw_access = dir_rw_access,
    .file_r_access = file_r_access,
    .file_w_access = file_w_access,
    .file_rw_access = file_rw_access,
    .exists = exists,
    .for_each_file_in_dir = for_each_file_in_dir,
};
