#include <stdio.h>
#include <string.h>
#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>

#include <libdstr/common.h>
#include <libdstr/logger.h>
#include <ditm.h>
#include <networking.h>
#include <libdstr/fileops.h>

#include "ditm_thread.h"
#include "fake_pop_server.h"
#include "test_utils.h"
#include "dummy_key_tool.h"

#include <libdstr/win_compat.h>

// path to where the test files can be found
const char* g_test_files;

DSTR_STATIC(multi_line_end, "\r\n.\r\n");
DSTR_STATIC(line_end, "\r\n");

#define EXPECT_RESPONSE(namestr, exp){ \
    int result; \
    /* read from the socket */ \
    recv->len = 0; \
    size_t count = 0; \
    while(count == 0){ \
        PROP(&e, connection_read(conn, recv, NULL) ); \
        count = dstr_count(recv, &line_end); \
    } \
    /* compare to expected string */ \
    result = dstr_cmp(&exp, recv); \
    if(result != 0){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FD(&exp), FD(recv)); \
        ORIG(&e, E_VALUE, "incorrect " namestr " response" ); \
    } \
}

#define EXPECT_MULTI_RESPONSE(namestr, exp_1, exp_body){ \
    int result; \
    /* read from the socket */ \
    recv->len = 0; \
    size_t count = 0; \
    while(count == 0){ \
        PROP(&e, connection_read(conn, recv, NULL) ); \
        count = dstr_count(recv, &multi_line_end); \
        if(recv->len < 2048) \
        PFMT("count = %x, recv:\n%x\n--------\n", FU(count), FD(recv)); \
    } \
    /* pop3 decode */ \
    buffer->len = 0; \
    bool found_end; \
    PROP(&e, pop3_decode(recv, buffer, &found_end) ); \
    /* get substrings for first line and body */ \
    LIST_PRESET(dstr_t, line_end_pattern, DSTR_LIT("\r\n")); \
    char* pos = dstr_find(buffer, &line_end_pattern, NULL, NULL); \
    if(!pos) ORIG(&e, E_VALUE, "first line not found");\
    pos += line_end_pattern.data[0].len; \
    dstr_t first = dstr_sub(buffer, 0, (uintptr_t)pos - (uintptr_t)buffer->data); \
    dstr_t body = dstr_sub(buffer, (uintptr_t)pos - (uintptr_t)buffer->data, 0); \
    /* compare to expected first line */ \
    result = dstr_cmp(&exp_1, &first); \
    if(result != 0){ \
        TRACE(&e, "during %x:\n" \
                 "expected: %x" \
                 "but got:  %x", FS(namestr), FD(&exp_1), FD(&first)); \
        ORIG(&e, E_VALUE, "incorrect response" ); \
    } \
    /* compare to expected body */ \
    result = dstr_cmp(&exp_body, &body); \
    if(result != 0){ \
        TRACE(&e, "during %x:\n" \
                 "expected: %x" \
                 "but got:  %x", FS(namestr), FD(&exp_body), FD(&body)); \
        TRACE(&e, "length %x vs %x\n", FU(exp_body.len), FU(body.len)); \
        ORIG(&e, E_VALUE, "incorrect response" ); \
    } \
}

static derr_t list_of_tests_1(LIST(dstr_t)* ans, dstr_t* recv, dstr_t* buffer,
                              connection_t* conn){
    derr_t e = E_OK;
    size_t len_sum = 0;
    for(size_t i = 0; i < ans->len; i++){
        len_sum += ans->data[i].len;
    }
    // greeting from server
    {
        DSTR_STATIC(expect, "+OK DITM ready.\r\n");
        EXPECT_RESPONSE("greeting", expect);
    }
    // loginhook
    {
        // USER command
        DSTR_VAR(temp, 64);
        PROP(&e, FMT(&temp, "USER %x\r\n", FS(g_username)) );
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("user", expect);
        // PASS command
        temp.len = 0;
        PROP(&e, FMT(&temp, "PASS %x\r\n", FS(g_password)) );
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect2, "+OK Logged in.\r\n");
        EXPECT_RESPONSE("login", expect2);
    }
    // stathook
    {
        DSTR_STATIC(temp, "STAT\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(expect, 32);
        PROP(&e, FMT(&expect, "+OK %x %x\r\n", FU(ans->len), FU(len_sum)) );
        EXPECT_RESPONSE("stat", expect);
    }
    // listhook
    {
        DSTR_STATIC(temp, "LIST\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(exp_1, 64);
        PROP(&e, FMT(&exp_1, "+OK %x messages (%x octets)\r\n",
                          FU(ans->len), FU(len_sum)) );
        DSTR_VAR(expect, 256);
        for(size_t i = 0; i < ans->len; i++){
            PROP(&e, FMT(&expect, "%x %x\r\n", FU(i + 1), FU(ans->data[i].len)) );
        }
        EXPECT_MULTI_RESPONSE("list", exp_1, expect);
    }
    // listhook with index
    {
        DSTR_STATIC(temp, "LIST 1\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(expect, 32);
        PROP(&e, FMT(&expect, "+OK 1 %x\r\n", FU(ans->data[0].len)) );
        EXPECT_RESPONSE("list 1", expect);
    }
    // retrhook
    for(size_t i = 0; i < ans->len; i++){
        size_t index = i + 1;
        DSTR_VAR(temp, 32);
        PROP(&e, FMT(&temp, "RETR %x\r\n", FU(index)) );
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(exp_1, 32);
        PROP(&e, FMT(&exp_1, "+OK %x octets\r\n", FU(ans->data[i].len)) );
        DSTR_VAR(tag, 32);
        PROP(&e, FMT(&tag, "retr %x", FU(index)) );
        EXPECT_MULTI_RESPONSE(tag.data, exp_1, ans->data[i]);
    }
    // delehook
    {
        DSTR_STATIC(temp, "DELE 1\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK 1 deleted\r\n");
        EXPECT_RESPONSE("dele 1", expect);
    }
    {
        DSTR_STATIC(temp, "DELE 1\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "-ERR no such message\r\n");
        EXPECT_RESPONSE("dele 1b", expect);
    }
    // listhook, make sure 1 was deleted
    {
        DSTR_STATIC(temp, "LIST\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(exp_1, 64);
        PROP(&e, FMT(&exp_1, "+OK %x messages (%x octets)\r\n",
                           FU(ans->len - 1), FU(len_sum - ans->data[0].len)) );
        DSTR_VAR(expect, 256);
        for(size_t i = 1; i < ans->len; i++){
            PROP(&e, FMT(&expect, "%x %x\r\n", FU(i + 1), FU(ans->data[i].len)) );
        }
        EXPECT_MULTI_RESPONSE("list after delete", exp_1, expect);
    }
    // rsethook
    {
        DSTR_STATIC(temp, "RSET\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("rset", expect);
    }
    // tophook
    {
        DSTR_STATIC(temp, "TOP 1 2\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(exp_1, "+OK\r\n");
        dstr_t expect = dstr_sub(&ans->data[0], 0, 313);
        EXPECT_MULTI_RESPONSE("top", exp_1, expect);
    }
    // tophook, but with the really long message
    {
        DSTR_STATIC(temp, "TOP 2 600\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(exp_1, "+OK\r\n");
        dstr_t expect = dstr_sub(&ans->data[1], 0, 111788);
        EXPECT_MULTI_RESPONSE("top 2", exp_1, expect);
    }
    // uidlhook
    {
        DSTR_STATIC(temp, "UIDL\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(exp_1, "+OK\r\n");
        DSTR_STATIC(expect, "1 basic\r\n"
                            "2 long\r\n"
                            "3 dots\r\n"
                            "4 basic_enc\r\n");
        EXPECT_MULTI_RESPONSE("uidl", exp_1, expect);
    }
    // uidlhook with index
    {
        DSTR_STATIC(temp, "UIDL 1\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK 1 basic\r\n");
        EXPECT_RESPONSE("udil 1", expect);
    }
    // delehook, this time will be used to test the quithook
    {
        DSTR_STATIC(temp, "DELE 2\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK 2 deleted\r\n");
        EXPECT_RESPONSE("dele 2", expect);
    }
    // quithook
    {
        DSTR_STATIC(temp, "QUIT\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK Goodbye, my love.\r\n");
        EXPECT_RESPONSE("quit", expect);
    }
    return e;
}

static derr_t list_of_tests_2(LIST(dstr_t)* ans, dstr_t* recv,
                              connection_t* conn){
    derr_t e = E_OK;
    size_t len_sum = 0;
    for(size_t i = 0; i < ans->len; i++){
        len_sum += ans->data[i].len;
    }
    // greeting from server
    {
        DSTR_STATIC(expect, "+OK DITM ready.\r\n");
        EXPECT_RESPONSE("greeting", expect);
    }
    // loginhook
    {
        // USER command
        DSTR_VAR(temp, 64);
        PROP(&e, FMT(&temp, "USER %x\r\n", FS(g_username)) );
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("user", expect);
        // PASS command
        temp.len = 0;
        PROP(&e, FMT(&temp, "PASS %x\r\n", FS(g_password)) );
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect2, "+OK Logged in.\r\n");
        EXPECT_RESPONSE("login", expect2);
    }
    // stathook, to check that one file was deleted
    {
        DSTR_STATIC(temp, "STAT\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_VAR(expect, 32);
        PROP(&e, FMT(&expect, "+OK %x %x\r\n",
                           FU(ans->len - 1), FU(len_sum - ans->data[1].len)) );
        EXPECT_RESPONSE("stat", expect);
    }
    // now just quit
    {
        DSTR_STATIC(temp, "QUIT\r\n");
        PROP(&e, connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK Goodbye, my love.\r\n");
        EXPECT_RESPONSE("quit", expect);
    }
    return e;
}

static derr_t test_ditm(void){
    derr_t e = E_OK;
    derr_t e2;

    PROP(&e, ditm_thread_start(fps_pop_port) );

    // also start the fake pop_server
    const char* files[] = {"fps/mail_basic",
                           "fps/mail_long",
                           "fps/mail_dots",
                           "fps/mail_basic.enc"};
    const char* uids[] = {"basic",
                          "long",
                          "dots",
                          "basic_enc"};
    const char* ansfiles[] = {"fps/mail_basic_mangled",
                              "fps/mail_long_mangled",
                              "fps/mail_dots_mangled",
                              "fps/mail_basic"};
    size_t nfiles = sizeof(files) / sizeof(*files);
    fake_pop_server_t fps;
    PROP_GO(&e, fake_pop_server_new(&fps, files, nfiles, uids), cu_ditm);
    // start fake pop_server and wait until its ready
    PROP_GO(&e, fake_pop_server_start(&fps), cu_ditm);

    // load up the files that we should expect to receive from DITM
    LIST_VAR(dstr_t, ans, sizeof(files) / sizeof(*files));
    for(size_t i = 0; i < nfiles; i++){
        dstr_t temp;
        PROP_GO(&e, dstr_new(&temp, 256), cu_ans);
        DSTR_VAR(path, 4096);
        PROP_GO(&e, FMT(&path, "%x/%x", FS(g_test_files), FS(ansfiles[i])), cu_ans);
        e2 = dstr_read_file(path.data, &temp);
        if(!e2.type) e2 = LIST_APPEND(dstr_t, &ans, temp);
        if(e2.type){
            dstr_free(&temp);
            PROP_GO(&e, e2, cu_ans);
        }
    }

    // set up a couple of dynamically allocated buffers
    dstr_t recv;
    PROP_GO(&e, dstr_new(&recv, 4096), cu_fps);
    dstr_t buffer;
    PROP_GO(&e, dstr_new(&buffer, 4096), cu_recv);

    // prepare ssl context
    ssl_context_t ctx;
    PROP_GO(&e, ssl_context_new_client(&ctx), cu_buffer);

    // now connect to the server
    ditm_thread_start_test();
    fps_start_test();
    connection_t conn;
    PROP_GO(&e, connection_new_ssl(&conn, &ctx, "127.0.0.1", 1996), cu_ctx);
    LOG_INFO("test client connected\n");

    // run the list of tests for the first connection
    PROP_GO(&e, list_of_tests_1(&ans, &recv, &buffer, &conn), cu_conn);
    PROP_GO(&e, ditm_thread_end_test(), cu_conn);
    PROP_GO(&e, fps_end_test(), cu_conn);

    // get another connection to the server
    connection_close(&conn);
    ditm_thread_start_test();
    fps_start_test();
    PROP_GO(&e, connection_new_ssl(&conn, &ctx, "127.0.0.1", 1996), cu_ctx);
    LOG_INFO("test_client connected\n");

    // run another list of tests to make sure the quithook ran nicely
    PROP_GO(&e, list_of_tests_2(&ans, &recv, &conn), cu_conn);
    PROP_GO(&e, ditm_thread_end_test(), cu_conn);
    PROP_GO(&e, fps_end_test(), cu_conn);

    // done
    ditm_thread_done();
    fps_done();

cu_conn:
    connection_close(&conn);
cu_ctx:
    ssl_context_free(&ctx);
cu_buffer:
    dstr_free(&buffer);
cu_recv:
    dstr_free(&recv);
cu_fps:
    fps_done();
    MERGE_CMD(&e, fake_pop_server_join(), "fake pop server");
    fake_pop_server_free(&fps);
cu_ans:
    for(size_t i = 0; i < ans.len; i++){
        dstr_free(&ans.data[i]);
    }
    ans.len = 0;
cu_ditm:
    ditm_thread_done();
    MERGE_CMD(&e, ditm_thread_join(), "ditm thread");
    return e;
}

static void sig_handler(int signum){
    if(signum == SIGINT){
        DROP_CMD( rm_rf(ditm_path) );
        DROP_CMD( rm_rf("test_ignore_dir") );
        exit(1);
    }
}

static derr_t do_test_mangle(const char* infile, const char* outfile,
                             derr_t (*mangle_hook)(int, int, size_t*)){
    derr_t e = E_OK;
    // open the relevant files
    int infd = open(infile, O_RDONLY);
    if(infd < 0){
        TRACE(&e, "%x: %x\n", FS(infile), FE(&errno));
        ORIG(&e, E_OS, "unable to open file for test");
    }
    int outfd = open(outfile, O_RDONLY);
    if(outfd < 0){
        TRACE(&e, "%x: %x\n", FS(outfile), FE(&errno));
        ORIG_GO(&e, E_OS, "unable to open file for test", cu1);
    }
    // open a temp file
    int temp = open("do_test_mangle.temp", O_CREAT | O_RDWR | O_TRUNC, 0660);
    if(temp < 0)
        ORIG_GO(&e, E_OS, "unable to create file for test", cu2);
    // allocate memory for the "answer"
    dstr_t answer;
    PROP_GO(&e, dstr_new(&answer, 4096), cu3);
    // allocate memory for the test result
    dstr_t result;
    PROP_GO(&e, dstr_new(&result, 4096), cu4);

    // ok, now we can actually run a test
    size_t outlen;
    size_t amnt_read;
    PROP_GO(&e, mangle_hook(infd, temp, &outlen), cu5);
    // reset temp file descriptor for reading
    off_t oret = lseek(temp, 0, SEEK_SET);
    if(oret == (off_t) -1){
        TRACE(&e, "%x: %x\n", FS("lseek"), FE(&errno));
        ORIG_GO(&e, E_OS, "lseek failed", cu5);
    }
    // read the test result
    while(true){
        PROP_GO(&e, dstr_read(temp, &result, 4096, &amnt_read), cu5);
        if(!amnt_read) break;
    }
    // read the answer
    while(true){
        PROP_GO(&e, dstr_read(outfd, &answer, 4096, &amnt_read), cu5);
        if(!amnt_read) break;
    }
    // compare result to the answer
    if(outlen != answer.len){
        ORIG_GO(&e, E_VALUE, "wrong outlen", cu5);
    }
    if(dstr_cmp(&answer, &result) != 0){
        ORIG_GO(&e, E_VALUE, "result does not match answer", cu5);
    }

cu5:
    dstr_free(&result);
cu4:
    dstr_free(&answer);
cu3:
    close(temp);
    remove("do_test_mangle.temp");
cu2:
    close(outfd);
cu1:
    close(infd);
    return e;
}

static derr_t test_mangle_unencrypted(void){
    derr_t e = E_OK;
    // run tests on test_mangle_corrupted
    DSTR_VAR(inpath, 4096);
    DSTR_VAR(outpath, 4096);
    PROP(&e, FMT(&inpath, "%x/mangle_unencrypted/in_short", FS(g_test_files)) );
    PROP(&e, FMT(&outpath, "%x/mangle_unencrypted/out_short", FS(g_test_files)) );
    PROP(&e, do_test_mangle(inpath.data, outpath.data, ditm_mangle_unencrypted) );

    inpath.len = 0;
    outpath.len = 0;
    PROP(&e, FMT(&inpath, "%x/mangle_unencrypted/in_long", FS(g_test_files)) );
    PROP(&e, FMT(&outpath, "%x/mangle_unencrypted/out_long", FS(g_test_files)) );
    PROP(&e, do_test_mangle(inpath.data, outpath.data, ditm_mangle_unencrypted) );

    inpath.len = 0;
    outpath.len = 0;
    PROP(&e, FMT(&inpath, "%x/mangle_unencrypted/in_nosubj", FS(g_test_files)) );
    PROP(&e, FMT(&outpath, "%x/mangle_unencrypted/out_nosubj", FS(g_test_files)) );
    PROP(&e, do_test_mangle(inpath.data, outpath.data, ditm_mangle_unencrypted) );

    return e;
}

static derr_t test_mangle_corrupted(void){
    derr_t e = E_OK;
    time_t epoch_orig = time(NULL);
    time_t epoch = epoch_orig;
    // try to start in the first part of a second
    // (ok, it's horrible but it's portable)
    while(epoch == epoch_orig){
        epoch = time(NULL);
    }
    // get a date string
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_OS, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_OS, "error formatting time string");
    }
    DSTR_STATIC(input,
        "-----BEGIN SPLINTERMAIL MESSAGE-----\r\n"
        "There never was a race of people who so completely gave the lie to\r\n"
        "history as these giants, or whom all the chroniclers have so\r\n"
        "cruelly libelled.  Instead of roaring and ravaging about the world,\r\n"
        "constantly catering for their cannibal larders, and perpetually\r\n"
        "going to market in an unlawful manner, they are the meekest people\r\n"
        "in any man's acquaintance:  rather inclining to milk and vegetable\r\n"
        "diet, and bearing anything for a quiet life.  So decidedly are\r\n"
        "amiability and mildness their characteristics, that I confess I\r\n"
        "look upon that youth who distinguished himself by the slaughter of\r\n"
        "these inoffensive persons, as a false-hearted brigand, who,\r\n"
        "pretending to philanthropic motives, was secretly influenced only\r\n"
        "by the wealth stored up within their castles, and the hope of\r\n"
        "plunder.\r\n"
        "-----END SPLINTERMAIL MESSAGE-----\r\n");
    DSTR_VAR(output, 2048);
    PROP(&e, FMT(&output,
        "From: DITM <ditm@localhost>\r\n"
        "To: Local User <email_user@localhost>\r\n"
        "Date: %x\r\n"
        "Subject: DITM failed to decrypt message\r\n"
        "\r\n"
        "The following message appears to be corrupted"
        " and cannot be decrypted:\r\n"
        "\r\n"
        "-----BEGIN SPLINTERMAIL MESSAGE-----\r\n"
        "There never was a race of people who so completely gave the lie to\r\n"
        "history as these giants, or whom all the chroniclers have so\r\n"
        "cruelly libelled.  Instead of roaring and ravaging about the world,\r\n"
        "constantly catering for their cannibal larders, and perpetually\r\n"
        "going to market in an unlawful manner, they are the meekest people\r\n"
        "in any man's acquaintance:  rather inclining to milk and vegetable\r\n"
        "diet, and bearing anything for a quiet life.  So decidedly are\r\n"
        "amiability and mildness their characteristics, that I confess I\r\n"
        "look upon that youth who distinguished himself by the slaughter of\r\n"
        "these inoffensive persons, as a false-hearted brigand, who,\r\n"
        "pretending to philanthropic motives, was secretly influenced only\r\n"
        "by the wealth stored up within their castles, and the hope of\r\n"
        "plunder.\r\n"
        "-----END SPLINTERMAIL MESSAGE-----\r\n", FS(d)) );

    // write the temp files for the mangle test
    const char* infile = "do_test_mangle_corrupted.in";
    const char* outfile = "do_test_mangle_corrupted.out";
    PROP(&e, dstr_write_file(infile, &input) );
    PROP_GO(&e, dstr_write_file(outfile, &output), cu1);

    // do the test
    PROP_GO(&e, do_test_mangle(infile, outfile, ditm_mangle_corrupted), cu2);
cu2:
    remove(outfile);
cu1:
    remove(infile);
    return e;
}

static derr_t test_ignore_list(void){
    derr_t e = E_OK;
    // first make a throwaway directory
    int ret = mkdir("test_ignore_dir", 0770);
    if(ret != 0){
        ORIG(&e, E_OS, "unable to make ignore directory");
    }
    // allocate/load up the test list
    ignore_list_t il;
    DSTR_VAR(path, 4096);
    PROP_GO(&e, FMT(&path, "%x/ignore_list", FS(g_test_files)), cu1);
    PROP_GO(&e, ignore_list_load(&il, &path), cu1);

    DSTR_STATIC(pos_queries, "ghosty");
    DSTR_STATIC(neg_queries, "MCGHOSTFAE");

    // should be ignored
    for(size_t i = 0; i < pos_queries.len; i++){
        dstr_t sub = dstr_sub(&pos_queries, i, i+1);
        if(ignore_list_should_ignore(&il, &sub) != true){
            ORIG_GO(&e, E_VALUE, "should_ignore() incorrect result", cu2);
        }
    }

    // should not be ignored
    for(size_t i = 0; i < neg_queries.len; i++){
        dstr_t sub = dstr_sub(&neg_queries, i, i+1);
        if(ignore_list_should_ignore(&il, &sub) != false){
            ORIG_GO(&e, E_VALUE, "should_ignore() incorrect result", cu2);
        }
    }

    // ignore_add
    for(size_t i = 0; i < neg_queries.len; i++){
        dstr_t sub = dstr_sub(&neg_queries, i, i+1);
        PROP_GO(&e, ignore_list_add(&il, &sub), cu2);
    }

    // write the resulting list
    DSTR_STATIC(outdir, "test_ignore_dir");
    PROP_GO(&e, ignore_list_write(&il, &outdir), cu2);

    // read the result into memory
    DSTR_VAR(result, 4096);
    PROP_GO(&e, dstr_fread_file("test_ignore_dir/ignore.json", &result), cu2);

    // read the answer into memory
    path.len = 0;
    PROP_GO(&e, FMT(&path, "%x/ignore_list/out", FS(g_test_files)), cu2);
    DSTR_VAR(answer, 4096);
    PROP_GO(&e, dstr_fread_file(path.data, &answer), cu2);

    if(dstr_cmp(&result, &answer) != 0){
        ORIG_GO(&e, E_VALUE, "ignore list result did not match answer", cu2);
    }

cu2:
    ignore_list_free(&il);
cu1:
    DROP_CMD( rm_rf("test_ignore_dir") );
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    signal(SIGINT, sig_handler);

    PROP_GO(&e, ssl_library_init(), test_fail);

    if(!dir_rw_access(ditm_path, true)){
        ORIG_GO(&e, E_FS, "unable to create ditm_dir", test_fail);
    }
    PROP_GO(&e, test_ditm(), test_fail);
    PROP_GO(&e, rm_rf(ditm_path), test_fail);

    PROP_GO(&e, test_mangle_unencrypted(), test_fail);
    PROP_GO(&e, test_mangle_corrupted(), test_fail);
    PROP_GO(&e, test_ignore_list(), test_fail);


    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    DROP_CMD( rm_rf(ditm_path) );
    DROP_CMD( rm_rf("test_ignore_dir") );
    ssl_library_close();
    return 1;
}
