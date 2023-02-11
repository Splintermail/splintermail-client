#include "tools/qwerrewq/libqw.h"

// silently discards any extra bits
qw_ref_t qw_ref(uintptr_t scope_id, uintptr_t param_idx){
    return (qw_ref_t){ .scope_id = scope_id, .param_idx = param_idx };
}
