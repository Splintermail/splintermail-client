#ifndef POP_SERVER_H
#define POP_SERVER_H

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"

typedef struct {
    // int arguments might be -1, meaning "no argument given"
    // unsigned int arguments will never be -1
    // no index will ever be passed as 0, as per POP standard
    // of course, the "lines" argument of tophook might be zero
    derr_t (*login)(void* arg, const dstr_t* username, const dstr_t* password,
                    bool* login_ok);
    derr_t (*stat)(void* arg);
    derr_t (*list)(void* arg, int index);
    derr_t (*retr)(void* arg, unsigned int index);
    derr_t (*dele)(void* arg, unsigned int index);
    derr_t (*rset)(void* arg);
    derr_t (*top)(void* arg, unsigned int index, unsigned int lines);
    derr_t (*uidl)(void* arg, int index);
    derr_t (*quit)(void* arg, bool* update_ok);
} pop_server_hooks_t;

typedef enum {
    POP_SERVER_STATE_AUTH,
    POP_SERVER_STATE_TRANS,
} pop_server_state_t;

typedef struct {
    connection_t conn;
    pop_server_hooks_t hooks;
} pop_server_t;

derr_t pop_server_loop(pop_server_t* ps, void* arg);
derr_t pop_server_send_dstr(pop_server_t* ps, const dstr_t* buffer);
derr_t pop_server_parse_tokens(pop_server_t* ps, const LIST(dstr_t)* tokens);

#endif //POP_SERVER_H
