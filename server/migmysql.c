#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>

#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"

typedef struct {
    const dstr_t *path;
    unsigned int tgt;
    const dstr_t *sock;
    const dstr_t *user;
    const dstr_t *pass;
    bool override;
} config_t;


// container for some migrations on file
typedef struct {
    dstr_t file;
    string_builder_t path;
    dstr_t content;
    bool up;
    // is the migration an executable (which should generate sql)?
    bool exe;
    unsigned int id;
    jsw_anode_t node;
} migration_t;
DEF_CONTAINER_OF(migration_t, node, jsw_anode_t)

// copies file and references path
static derr_t migration_new(
    const string_builder_t *path,
    const dstr_t *file,
    unsigned int id,
    bool up,
    bool exe,
    migration_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    migration_t *migration = malloc(sizeof(*migration));
    if(!migration) ORIG(&e, E_NOMEM, "nomem");
    *migration = (migration_t){ .up = up, .exe = exe, .id = id };

    PROP_GO(&e, dstr_copy(file, &migration->file), fail);

    migration->path = sb_append(path, FD(&migration->file));

    PROP_GO(&e, dstr_new(&migration->content, 256), fail_file);

    // actually read the migration from file
    string_builder_t fullpath = sb_append(path, FD(file));
    PROP_GO(&e, dstr_read_path(&fullpath, &migration->content), fail_content);

    *out = migration;
    return e;

fail_content:
    dstr_free(&migration->content);
fail_file:
    dstr_free(&migration->file);
fail:
    free(migration);
    return e;
}

static void migration_free(migration_t *migration){
    dstr_free(&migration->content);
    dstr_free(&migration->file);
    free(migration);
}

// for jsw_atree_t
static const void *migration_get_uint(const jsw_anode_t *node){
    migration_t *migration = CONTAINER_OF(node, migration_t, node);
    return &migration->id;
}


// container for in-db states and their undo functions
typedef struct {
    unsigned int id;
    dstr_t name;
    bool exe;
    dstr_t undo;
    jsw_anode_t node;
} state_t;
DEF_CONTAINER_OF(state_t, node, jsw_anode_t)

// copies name and undo
static derr_t state_new(
    unsigned int id,
    const dstr_t *name,
    bool exe,
    const dstr_t *undo,
    state_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    state_t *state = malloc(sizeof(*state));
    if(!state) ORIG(&e, E_NOMEM, "nomem");
    *state = (state_t){ .id = id, .exe = exe, };

    PROP_GO(&e, dstr_copy(name, &state->name), fail);
    PROP_GO(&e, dstr_copy(undo, &state->undo), fail_name);

    *out = state;
    return e;

fail_name:
    dstr_free(&state->name);
fail:
    free(state);
    return e;
}

static void state_free(state_t *state){
    dstr_free(&state->undo);
    dstr_free(&state->name);
    free(state);
}

// for jsw_atree_t
static const void *state_get_uint(const jsw_anode_t *node){
    state_t *state = CONTAINER_OF(node, state_t, node);
    return &state->id;
}

///////// migration file handling

static bool endswith(const dstr_t *str, char *pattern){
    dstr_t check;
    DSTR_WRAP(check, pattern, strlen(pattern), true);
    if(str->len < check.len) return false;
    dstr_t sub = dstr_sub(str, str->len - check.len, str->len);
    return dstr_cmp(&check, &sub) == 0;
}

static unsigned int get_id(const dstr_t *file){
    derr_t e = E_OK;

    LIST_VAR(dstr_t, split, 2);
    PROP_GO(&e, dstr_split_soft(file, &DSTR_LIT("-"), &split), fail);

    unsigned int out = 0;
    PROP_GO(&e, dstr_tou(&split.data[0], &out, 10), fail);

    return out;

fail:
    DROP_VAR(&e);
    return 0;
}

static dstr_t get_name(const dstr_t *file){
    derr_t e = E_OK;

    LIST_VAR(dstr_t, split, 2);
    PROP_GO(&e, dstr_split_soft(file, &DSTR_LIT("-"), &split), fail);

    if(split.len < 2) return (dstr_t){0};
    dstr_t post_hyphen = split.data[1];

    size_t len = post_hyphen.len;

    // figure out what the end is
    if(endswith(file, ".sql")){
        // exclue the .dn.sql or .up.sql
        if(len < 7) return (dstr_t){0};
        len -= 7;
    }else{
        // exclue the .dn or .up
        if(len < 3) return (dstr_t){0};
        len -= 3;
    }

    return dstr_sub(&post_hyphen, 0, len);

fail:
    DROP_VAR(&e);
    return (dstr_t){0};
}

typedef struct {
    jsw_atree_t *ups;
    jsw_atree_t *dns;
    derr_t e;
    bool *ok;
} get_sql_data_t;

static derr_t sql_file_hook(
    const string_builder_t *base, const dstr_t *file, bool isdir, void *data
){
    derr_t e = E_OK;
    get_sql_data_t *gsd = data;

    if(isdir) return e;

    if(
        !endswith(file, ".sql")
        && !endswith(file, ".up")
        && !endswith(file, ".dn")
    ) return e;

    bool up;
    bool exe;
    if(endswith(file, ".up.sql")){
        up = true;
        exe = false;
    }else if(endswith(file, ".dn.sql")){
        up = false;
        exe = false;
    }else if(endswith(file, ".up")){
        up = true;
        exe = true;
    }else if(endswith(file, ".dn")){
        up = false;
        exe = true;
    }else{
        TRACE(&gsd->e, "invalid migration file name: %x\n", FD(file));
        *gsd->ok = false;
        return e;
    }

    unsigned int id = get_id(file);
    if(!id){
        TRACE(&gsd->e, "invalid migration file name: %x\n", FD(file));
        *gsd->ok = false;
        return e;
    }

    const dstr_t name = get_name(file);
    if(!name.len){
        TRACE(&gsd->e, "invalid sql file name: %x\n", FD(file));
        *gsd->ok = false;
        return e;
    }

    jsw_atree_t *tree = up ? gsd->ups : gsd->dns;

    // check for duplicate ids
    jsw_anode_t *node = jsw_afind(tree, &id, NULL);
    if(node){
        migration_t *old = CONTAINER_OF(node, migration_t, node);
        TRACE(&gsd->e,
            "duplicated sql file ids: %x and %x\n", FD(file), FD(&old->file)
        );
        *gsd->ok = false;
        return e;
    }

    // check for duplicate names
    jsw_atrav_t trav;
    for(node = jsw_atfirst(&trav, tree); node; node = jsw_atnext(&trav)){
        migration_t *old = CONTAINER_OF(node, migration_t, node);
        dstr_t old_name = get_name(&old->file);
        if(dstr_cmp(&old_name, &name) == 0){
            TRACE(&gsd->e,
                "duplicated sql file names: %x and %x\n",
                FD(file),
                FD(&old->file)
            );
            *gsd->ok = false;
            return e;
        }
    }

    // finally, create a migration
    migration_t *migration;
    PROP(&e, migration_new(base, file, id, up, exe, &migration) );

    jsw_ainsert(tree, &migration->node);

    return e;
}

static derr_t check_ups_vs_dns(get_sql_data_t *gsd){
    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node;
    for(node = jsw_atfirst(&trav, gsd->ups); node; node = jsw_atnext(&trav)){
        migration_t *up = CONTAINER_OF(node, migration_t, node);
        // find matching dn
        node = jsw_afind(gsd->dns, &up->id, NULL);
        if(!node){
            TRACE(&gsd->e, "unmatched up migration: %x\n", FD(&up->file));
            *gsd->ok = false;
            continue;
        }
        migration_t *dn = CONTAINER_OF(node, migration_t, node);

        // match names
        dstr_t up_name = get_name(&up->file);
        dstr_t dn_name = get_name(&dn->file);
        if(dstr_cmp(&up_name, &dn_name) != 0){
            TRACE(&gsd->e,
                "mismatch name in sql file pair: %x and %x\n",
                FD(&up->file),
                FD(&dn->file)
            );
            *gsd->ok = false;
        }
    }

    // reverse check
    for(node = jsw_atfirst(&trav, gsd->dns); node; node = jsw_atnext(&trav)){
        migration_t *dn = CONTAINER_OF(node, migration_t, node);
        node = jsw_afind(gsd->ups, &dn->id, NULL);
        if(!node){
            TRACE(&gsd->e, "unmatched dn migration: %x\n", FD(&dn->file));
            *gsd->ok = false;
        }
    }

    return e;
}

static derr_t get_sql_files(
    const dstr_t *path, jsw_atree_t *ups, jsw_atree_t *dns
){
    derr_t e = E_OK;

    bool ok = true;

    get_sql_data_t gsd = { .ups = ups, .dns = dns, .e = E_OK, .ok = &ok };

    string_builder_t sb = SB(FD(path));
    PROP_GO(&e, for_each_file_in_dir2(&sb, sql_file_hook, &gsd), fail);

    PROP_GO(&e, check_ups_vs_dns(&gsd), fail);

    if(!ok){
        TRACE_ORIG(&gsd.e, E_PARAM, "invalid file names");
        return gsd.e;
    }

    return e;

fail:
    DROP_VAR(&gsd.e);
    return e;
}


static void limit_migrations(jsw_atree_t *tree, unsigned int tgt){
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, tree);
    while(node){
        migration_t *migration = CONTAINER_OF(node, migration_t, node);
        if(migration->id <= tgt){
            break;
        }
        node = jsw_pop_atprev(&trav);
        migration_free(migration);
    }
}


/////////// sql logic


static derr_t mig_bootstrap(MYSQL *sql){
    derr_t e = E_OK;

    // ensure migmysql table exists
    DSTR_STATIC(script,
        "CREATE DATABASE IF NOT EXISTS splintermail;\n"
        "use splintermail;\n"
        "CREATE TABLE IF NOT EXISTS migmysql ("
        "    id INT PRIMARY KEY NOT NULL,"
        "    name VARCHAR(256) NOT NULL,"
        "    exe BIT(1) NOT NULL,"
        "    `undo` BLOB NOT NULL"
        ");\n"
    );
    PROP(&e, sql_exec_multi(sql, &script) );

    return e;
}


static derr_t mig_override(MYSQL *sql, const dstr_t *path){
    derr_t e = E_OK;

    dstr_t base = dstr_basename(path);
    dstr_t name = get_name(&base);
    unsigned int id = get_id(&base);
    bool exe = !endswith(path, ".sql");

    if(!id || !name.len){
        ORIG(&e, E_PARAM, "filename seems invalid");
    }

    // read content of file
    dstr_t content;
    PROP(&e, dstr_new(&content, 256) );
    PROP_GO(&e, dstr_read_path(&SB(FD(path)), &content), cu);

    DSTR_STATIC(query,
        "INSERT INTO migmysql (id, name, exe, `undo`) VALUES (?,?,?,?)"
    );
    PROP_GO(&e,
        sql_norow_query(sql,
            &query,
            NULL,
            uint_bind_in(&id),
            string_bind_in(&name),
            bool_bind_in(&exe),
            blob_bind_in(&content)
        ),
    cu);

cu:
    dstr_free(&content);

    return e;
}


static derr_t get_mig_states(MYSQL *sql, jsw_atree_t *states){
    derr_t e = E_OK;

    PROP(&e,
        sql_query(sql, &DSTR_LIT("SELECT id, name, exe, `undo` FROM migmysql"))
    );

    // get result
    MYSQL_RES* res;
    PROP(&e, sql_use_result(sql, &res) );

    // loop through results
    MYSQL_ROW row;
    while((row = mysql_fetch_row(res))){
        dstr_t idstr, name, exestr, undo;
        PROP_GO(&e,
            sql_read_row(res, &row, &idstr, &name, &exestr, &undo),
        fail_loop);

        unsigned int id;
        PROP_GO(&e, dstr_tou(&idstr, &id, 10), fail_loop);

        bool exe;
        PROP_GO(&e, sql_read_bit_dstr(&exestr, &exe), fail_loop);

        state_t *state;
        PROP(&e, state_new(id, &name, exe, &undo, &state) );

        jsw_ainsert(states, &state->node);
    }

fail_loop:
    while(row) row = mysql_fetch_row(res);

    mysql_free_result(res);

    return e;
}


static derr_t urandom_bytes(dstr_t *out, size_t count){
    derr_t e = E_OK;

    int fd;
    PROP(&e, dopen("/dev/urandom", O_RDONLY, 0, &fd) );

    size_t amnt_read;
    PROP_GO(&e, dstr_read(fd, out, count, &amnt_read), cu);

    if(amnt_read != count)
        ORIG_GO(&e, E_OS, "wrong number of bytes", cu);

cu:
    close(fd);

    return e;
}


static derr_t write_content_file(const char *tempname, const dstr_t *content){
    derr_t e = E_OK;

    int fd;
    // make it executable
    PROP(&e, dopen(tempname, O_WRONLY | O_CREAT, 0700, &fd) );
    PROP_GO(&e, dstr_write(fd, content), cu);
    PROP_GO(&e, dfsync(fd), cu);

cu:
    close(fd);

    return e;
}


static derr_t exec_content_child(
    const config_t *config, const dstr_t tempname
){
    derr_t e = E_OK;

    // pack settings into the environment
    dstr_t sock = {0};
    dstr_t user = {0};
    dstr_t pass = {0};

    dstr_t empty = DSTR_LIT("");
    PROP(&e,
        FMT(&sock, "SQL_SOCK=%x", FD(config->sock ? config->sock : &empty))
    );
    PROP(&e,
        FMT(&user, "SQL_USER=%x", FD(config->user ? config->user : &empty))
    );
    PROP(&e,
        FMT(&pass, "SQL_PASS=%x", FD(config->pass ? config->pass : &empty))
    );

    DSTR_STATIC(dbname, "SQL_DB=splintermail");

    // provide a convenience SQL_CMD for shell scripts
    dstr_t cmd = {0};
    PROP(&e, FMT(&cmd, "SQL_CMD=mysql") );
    if(config->sock){
        PROP(&e, FMT(&cmd, " --socket \"$SQL_SOCK\"") );
    }
    if(config->user){
        PROP(&e, FMT(&cmd, " --user \"$SQL_USER\"") );
    }
    if(config->pass){
        PROP(&e, FMT(&cmd, " --password \"$SQL_PASS\"") );
    }
    PROP(&e, FMT(&cmd, " \"$SQL_DB\"") );

    char **env;
    PROP(&e, make_env(&env, NULL, sock, user, pass, dbname, cmd) );

    // run the executable (no args)
    e = _dexec(tempname, env, NULL, 0);

    return e;
}

// return a pointer to the usable content, either content or exe_output
// you must free exe_output after calling these, even on failure.
static derr_t maybe_exec_content(
    const config_t *config,
    bool exe,
    const dstr_t *content,
    dstr_t *exe_output,
    const dstr_t **out
){
    derr_t e = E_OK;

    if(!exe){
        *out = content;
        return e;
    }

    *out = NULL;

    // choose a default filename
    DSTR_VAR(bin, 8);
    PROP(&e, urandom_bytes(&bin, 8) );
    DSTR_VAR(tempname, 64);
    PROP(&e, FMT(&tempname, "/tmp/migmysql-%x", FX(&bin)) );

    // write the tempfile
    PROP_GO(&e, write_content_file(tempname.data, content), cu_file);

    pid_t pid;
    bool child;
    int outfd;
    PROP_GO(&e, dfork_ex(&pid, &child, &devnull, &outfd, NULL), cu_file);
    if(child)
        RUN_CHILD_PROC(exec_content_child(config, tempname), 3);

    PROP_GO(&e, dstr_read_all(outfd, exe_output), cu_child);

cu_child:
    if(is_error(e)){
        kill(pid, SIGKILL);
    }
    int exit_code;
    PROP_GO(&e, dwait(pid, &exit_code, NULL), cu_file);
    if(!is_error(e) && exit_code){
        TRACE(&e, "executable exited with code %x\n", FI(exit_code));
        ORIG_GO(&e, E_VALUE, "script failed", cu_file);
    }

cu_file:
    DROP_CMD( dremove(tempname.data) );

    if(!is_error(e)){
        *out = exe_output;
    }

    return e;
}


static derr_t migmysql_apply_one(
    const config_t *config, MYSQL *sql, migration_t *up, migration_t *dn
){
    derr_t e = E_OK;

    dstr_t name = get_name(&up->file);
    LOG_INFO("-- \x1b[32mapplying %x:\x1b[m\n", FD(&name));

    dstr_t exe_output = {0};
    const dstr_t *content = NULL;
    PROP_GO(&e,
        maybe_exec_content(
            config, up->exe, &up->content, &exe_output, &content
        ),
    cu);

    LOG_INFO("%x", FD(content));

    // run the migration
    PROP_GO(&e, sql_exec_multi(sql, content), cu);

    // then, add an undo entry to the migmysql table via a prepared statement
    DSTR_STATIC(query,
        "INSERT INTO migmysql (id, name, exe, `undo`) VALUES (?,?,?,?)"
    );
    PROP_GO(&e,
        sql_norow_query(sql,
            &query,
            NULL,
            uint_bind_in(&dn->id),
            string_bind_in(&name),
            bool_bind_in(&dn->exe),
            blob_bind_in(&dn->content)
        ),
    cu);

cu:
    dstr_free(&exe_output);

    return e;
}

static derr_t migmysql_undo_one(
    const config_t *config, MYSQL *sql, state_t *state
){
    derr_t e = E_OK;

    LOG_INFO("-- \x1b[32mreverting %x:\x1b[m\n", FD(&state->name));

    dstr_t exe_output = {0};
    const dstr_t *content;
    PROP(&e,
        maybe_exec_content(
            config, state->exe, &state->undo, &exe_output, &content
        )
    );

    LOG_INFO("%x", FD(content));

    // apply the undo operation
    PROP_GO(&e, sql_exec_multi(sql, content), cu);

    // erase the state
    PROP_GO(&e,
        sql_norow_query(sql,
            &DSTR_LIT("DELETE FROM migmysql where id = ?"),
            NULL,
            uint_bind_in(&state->id)
        ),
    cu);

cu:
    dstr_free(&exe_output);

    return e;
}

static derr_t migmysql_apply(
    const config_t *config,
    MYSQL *sql,
    jsw_atree_t *ups,
    jsw_atree_t *dns,
    jsw_atree_t *states
){
    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node;

    // undo too-high migrations in reverse order
    for(node = jsw_atlast(&trav, states); node; node = jsw_atprev(&trav)){
        state_t *state = CONTAINER_OF(node, state_t, node);
        if(jsw_afind(ups, &state->id, NULL))
            continue;
        PROP(&e, migmysql_undo_one(config, sql, state) );
    }

    // apply yet-undone migration scripts
    for(node = jsw_atfirst(&trav, ups); node; node = jsw_atnext(&trav)){
        migration_t *up = CONTAINER_OF(node, migration_t, node);
        if(jsw_afind(states, &up->id, NULL))
            continue;
        node = jsw_afind(dns, &up->id, NULL);
        if(!node) ORIG(&e, E_INTERNAL, "missing dn migration");
        migration_t *dn = CONTAINER_OF(node, migration_t, node);
        PROP(&e, migmysql_apply_one(config, sql, up, dn) );
    }

    return e;
}


static derr_t migmysql(const config_t *config){
    derr_t e = E_OK;

    jsw_anode_t *node;

    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(&e, E_SQL, "unable to init mysql object", cu_sql_lib);
    }

    PROP_GO(&e,
        sql_connect_unix_ex(
            &sql, config->user, config->pass, config->sock, NULL
        ),
    cu_sql);

    PROP_GO(&e, mig_bootstrap(&sql), cu_sql);

    // handle override mode
    if(config->override){
        PROP_GO(&e, mig_override(&sql, config->path), cu_sql);
        goto cu_sql;
    }

    jsw_atree_t ups, dns;
    jsw_ainit(&ups, jsw_cmp_uint, migration_get_uint);
    jsw_ainit(&dns, jsw_cmp_uint, migration_get_uint);

    PROP_GO(&e, get_sql_files(config->path, &ups, &dns), cu_migrations);

    // ignore anything higher than the target migration level
    limit_migrations(&ups, config->tgt);
    limit_migrations(&dns, config->tgt);

    jsw_atree_t states;
    jsw_ainit(&states, jsw_cmp_uint, state_get_uint);

    PROP_GO(&e, get_mig_states(&sql, &states), cu_states);

    PROP_GO(&e, migmysql_apply(config, &sql, &ups, &dns, &states), cu_states);

cu_states:
    while((node = jsw_apop(&states))){
        state_t *state = CONTAINER_OF(node, state_t, node);
        state_free(state);
    }

cu_migrations:
    while((node = jsw_apop(&ups))){
        migration_t *migration = CONTAINER_OF(node, migration_t, node);
        migration_free(migration);
    }
    while((node = jsw_apop(&dns))){
        migration_t *migration = CONTAINER_OF(node, migration_t, node);
        migration_free(migration);
    }

cu_sql:
    mysql_close(&sql);

cu_sql_lib:
    mysql_library_end();

    return e;
}

static void print_help(FILE *f){
    fprintf(f,
        "migmysql: database migrations that embed nicely within git\n"
        "\n"
        "usage: migmysql PATH [TARGET_LEVEL] [OPTIONS]\n"
        "usage: migmysql --override FILE.dn.sql [OPTIONS]\n"
        "\n"
        "where OPTIONS are one of:\n"
        "  -h --help\n"
        "  -d --debug\n"
        "  -s --socket PATH     default: /var/run/mysqld/mysqld.sock\n"
        "     --user\n"
        "     --pass\n"
        "     --host            (not yet supported)\n"
        "     --port            (not yet supported)\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    // specify command line options
    opt_spec_t o_help = {'h', "help", false, OPT_RETURN_INIT};
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t o_sock = {'s', "socket", true, OPT_RETURN_INIT};
    opt_spec_t o_user = {'\0', "user", true, OPT_RETURN_INIT};
    opt_spec_t o_pass = {'\0', "pass", true, OPT_RETURN_INIT};
    opt_spec_t o_host = {'\0', "host", true, OPT_RETURN_INIT};
    opt_spec_t o_port = {'\0', "port", true, OPT_RETURN_INIT};
    opt_spec_t o_ovr = {'\0', "override", false, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {
        &o_help,
        &o_debug,
        &o_sock,
        &o_user,
        &o_pass,
        &o_host,
        &o_port,
        &o_ovr,
    };

    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    e = opt_parse(argc, argv, spec, speclen, &newargc);
    if(is_error(e)){
        logger_add_fileptr(LOG_LVL_ERROR, stderr);
        DUMP(e);
        DROP_VAR(&e);
        return 2;
    }

    // print help?
    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    if(newargc != 2 && (o_ovr.found || newargc != 3)){
        print_help(stderr);
        return 1;
    }

    // --debug
    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    // PATH
    dstr_t path;
    DSTR_WRAP(path, argv[1], strlen(argv[1]), true);

    // validate PATH
    if(o_ovr.found){
        bool ok;
        PROP_GO(&e, is_file(path.data, &ok), fail);
        if(!ok){
            LOG_ERROR("--override provided but %x is not a file\n", FD(&path));
            return 1;
        }
        dstr_t base = dstr_basename(&path);
        dstr_t name = get_name(&base);
        unsigned int id = get_id(&base);
        if(!endswith(&base, ".dn.sql") || !name.len || !id){
            LOG_ERROR("--override file (%x) must end in .dn.sql\n", FD(&base));
            LOG_ERROR("should match NUM-NAME.dn.sql\n", FD(&base));
            return 1;
        }
    }else{
        bool ok;
        PROP_GO(&e, is_dir(path.data, &ok), fail);
        if(!ok){
            LOG_ERROR("%x is not a directory\n", FD(&path));
            return 1;
        }
    }

    // TARGET
    unsigned int tgt = UINT_MAX;
    if(newargc == 3){
        dstr_t d_arg;
        DSTR_WRAP(d_arg, argv[2], strlen(argv[2]), true);
        IF_PROP(&e, dstr_tou(&d_arg, &tgt, 10) ){
            DROP_VAR(&e);
            fprintf(stderr, "invalid target number: %s\n", argv[2]);
            exit(1);
        }
    }

    // --host
    if(o_host.found){
        LOG_ERROR("--host not supported\n");
        exit(1);
    }

    // --port
    if(o_port.found){
        LOG_ERROR("--host not supported\n");
        exit(1);
    }

    // --sock, --user, and --pass
    config_t config = {
        .path = &path,
        .tgt = tgt,
        .sock = o_sock.found ? &o_sock.val : NULL,
        .user = o_user.found ? &o_user.val : NULL,
        .pass = o_pass.found ? &o_pass.val : NULL,
        .override = o_ovr.found,
    };

    PROP_GO(&e, migmysql(&config), fail);

    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
