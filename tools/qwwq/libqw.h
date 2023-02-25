#include "libdstr/libdstr.h"

#include <setjmp.h>

#define QWBLOCKSIZE 16384
#define QWMAXPARAMS 256
#define PTRSIZE sizeof(void*)

struct qw_engine_t;
typedef struct qw_engine_t qw_engine_t;

struct qw_origin_t;
typedef struct qw_origin_t qw_origin_t;

/* most functions require both an engine and an origin, which are separated
   so that memory associated with the config can persist through multiple
   snippets, but snippet memory can be reset after each snippet */
typedef struct {
    // the engine has the stack
    qw_engine_t *engine;
    // the origin has the backing memory
    qw_origin_t *origin;
} qw_env_t;

struct qw_comp_call_t;
typedef struct qw_comp_call_t qw_comp_call_t;

struct qw_comp_scope_t;
typedef struct qw_comp_scope_t qw_comp_scope_t;

// an instruction function takes only the engine and returns nothing
typedef void (*qw_instr_f)(qw_env_t env);

#include "tools/qwwq/types.h"
#include "tools/qwwq/instr.h"
#include "tools/qwwq/builtins.h"
#include "tools/qwwq/compile.h"
#include <tools/qwwq/generated/parse.h>
#include "tools/qwwq/scan.h"
#include "tools/qwwq/engine.h"
#include "tools/qwwq/qw.h"
