#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <liburl/liburl.h>

#include "test_utils.h"

#define ASSERT(code) if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

static bool dstr_eqs(const dstr_t d, char *c){
    return strlen(c) == d.len && strncmp(c, d.data, d.len) == 0;
}

static derr_t test_error_reporting(void){
    derr_t e = E_OK;

    url_t url;
    bool ok;
    DSTR_VAR(errbuf, 512);

    dstr_t *invalid = &DSTR_LIT("https:%#asdf");
    char *full_error = "invalid url: https:%#asdf\n"
                       "                    ^";

    // gracefully handle no errbuf
    ok = parse_url_ex(invalid, &url, NULL);
    ASSERT(!ok);
    ok = parse_url_reference_ex(invalid, &url, NULL);
    ASSERT(!ok);

    // handle proper errbuf
    ok = parse_url_ex(invalid, &url, &errbuf);
    ASSERT(!ok);
    ASSERT(dstr_eqs(errbuf, full_error));
    ok = parse_url_reference_ex(invalid, &url, &errbuf);
    ASSERT(!ok);
    ASSERT(dstr_eqs(errbuf, full_error));

    // handle short errbuf
    {
        // short in the FMT_QUIET part
        DSTR_VAR(shortbuf, 15);
        memset(shortbuf.data, 'x', shortbuf.size);
        //                  0         1
        //                  0123456789012345
        char *short_error = "invalid url: h";
        ok = parse_url_ex(invalid, &url, &shortbuf);
        ASSERT(!ok);
        ASSERT(dstr_eqs(shortbuf, short_error));
        ASSERT(shortbuf.len < shortbuf.size);
        ASSERT(shortbuf.data[shortbuf.len] == '\0');
    }
    {
        // short in dstr_append_quiet part
        DSTR_VAR(shortbuf, 30);
        memset(shortbuf.data, 'x', shortbuf.size);
        //                  0         1         2          3
        //                  012345678901234567890123456 7890
        char *short_error = "invalid url: https:%#asdf\n   ";
        ok = parse_url_ex(invalid, &url, &shortbuf);
        ASSERT(!ok);
        ASSERT(dstr_eqs(shortbuf, short_error));
        ASSERT(shortbuf.len < shortbuf.size);
        ASSERT(shortbuf.data[shortbuf.len] == '\0');
    }
    {
        // short in dstr_null_terminate part
        DSTR_VAR(shortbuf, 48);
        memset(shortbuf.data, 'x', shortbuf.size);
        //                  0         1         2
        //                  012345678901234567890123456
        char *short_error = "invalid url: https:%#asdf\n"
        //                      3         4
        //                   7890123456789012345678
                            "                    ^";
        ok = parse_url_ex(invalid, &url, &shortbuf);
        ASSERT(!ok);
        ASSERT(dstr_eqs(shortbuf, short_error));
        ASSERT(shortbuf.len < shortbuf.size);
        ASSERT(shortbuf.data[shortbuf.len] == '\0');
    }
    {
        // stupid-sized buffer (checking for asan errors)
        dstr_t shortbuf = { .fixed_size = true };
        char *short_error = "";
        ok = parse_url_ex(invalid, &url, &shortbuf);
        ASSERT(!ok);
        ASSERT(dstr_eqs(shortbuf, short_error));
    }

    // check manual error case in code
    dstr_t *badport = &DSTR_LIT("https://splintermail.com:asdf");
    char *port_error = "invalid url: https://splintermail.com:asdf\n"
                       "                                      ^^^^";
    ok = parse_url_ex(badport, &url, &errbuf);
    ASSERT(!ok);
    ASSERT(dstr_eqs(errbuf, port_error));

    // errorbuf on the heap
    dstr_t heapbuf;
    PROP(&e, dstr_new(&heapbuf, 2));
    ok = parse_url_ex(badport, &url, &heapbuf);
    ASSERT(!ok);
    ok = dstr_eqs(heapbuf, port_error);
    dstr_free(&heapbuf);
    ASSERT(ok);

    return e;
}

// find a single character type in an expected url
static dstr_off_t find_matches(
    const dstr_t *url, const dstr_t exp, char c, size_t backup_pos
){
    dstr_off_t out = { .buf = url };
    for(size_t i = 0; i < exp.len; i++){
        if(exp.data[i] == c){
            if(out.len == 0){
                // the first character
                out.start = i;
            }
            out.len++;
        }else if(out.len){
            // passed the end of chars
            break;
        }
    }
    if(!out.len){
        out = (dstr_off_t){ .buf = url, .start = backup_pos, .len = 0 };
    }
    return out;
}

// parse a full expected url string
static url_t expected_url(const dstr_t *url, const dstr_t exp){
    if(url->len != exp.len){
        LOG_ERROR("bad test, url->len != exp.len\n");
        exit(2);
    }
    // scheme is present or starts at the start
    dstr_off_t scheme = find_matches(url, exp, 's', 0);
    // nothing happens before scheme's ':'
    size_t scheme_limit = 0;
    if(scheme.start + scheme.len < url->len)
        if(url->data[scheme.start + scheme.len] == ':')
            scheme_limit = scheme.start + scheme.len + 1;

    // fragment is present or ends at the end
    dstr_off_t fragment = find_matches(url, exp, 'f', url->len);

    // query is present, or ends before the fragment's '#'
    size_t backup_pos = fragment.start;
    if(backup_pos && url->data[backup_pos-1] == '#') backup_pos--;
    dstr_off_t query = find_matches(url, exp, 'q', backup_pos);

    // path is present or ends before the query's '?'
    backup_pos = query.start;
    if(backup_pos && url->data[backup_pos-1] == '?') backup_pos--;
    dstr_off_t path = find_matches(url, exp, 'p', backup_pos);

    // port is present or ends before the path
    backup_pos = path.start;
    dstr_off_t port = find_matches(url, exp, 'n', backup_pos);

    // host is present or ends before the port's ':', not before scheme's ':'
    backup_pos = port.start;
    if(backup_pos && url->data[backup_pos-1] == ':') backup_pos--;
    backup_pos = MAX(backup_pos, scheme_limit);
    dstr_off_t host = find_matches(url, exp, 'h', backup_pos);

    // pass is present or starts before the host's '@'
    backup_pos = host.start;
    if(backup_pos && url->data[backup_pos-1] == '@') backup_pos--;
    dstr_off_t pass = find_matches(url, exp, 'P', backup_pos);
    // username is present or starts before the pass's ':'
    backup_pos = pass.start;
    if(backup_pos && url->data[backup_pos-1] == ':') backup_pos--;
    backup_pos = MAX(backup_pos, scheme_limit);
    dstr_off_t user = find_matches(url, exp, 'U', backup_pos);

    return (url_t){
        .scheme = scheme,
        .user = user,
        .pass = pass,
        .host = host,
        .port = port,
        .path = path,
        .query = query,
        .fragment = fragment,
    };
}

static bool dstr_off_eq(const dstr_off_t a, const dstr_off_t b){
    if(a.buf != b.buf){
        LOG_ERROR("differently-backed dstr_off_t's in dstr_off_eq()!\n");
        LOG_ERROR("%x != %x\n", FP(a.buf), FP(b.buf));
        exit(2);
    }
    return a.len == b.len && a.start == b.start;
}

// writes one extra character at the start and one extra at the end
static dstr_t *write_offset(dstr_t *buf, const dstr_off_t off, char c){
    buf->len = 0;
    for(size_t i = 0; i < off.start; i++){
        dstr_append_quiet(buf, &DSTR_LIT(" "));
    }
    dstr_append_quiet(buf, &DSTR_LIT("|"));
    dstr_t dc;
    DSTR_WRAP(dc, &c, 1, false);
    for(size_t i = 0; i < off.len; i++){
        dstr_append_quiet(buf, &dc);
    }
    dstr_append_quiet(buf, &DSTR_LIT("|"));
    return buf;
}

static bool do_off_cmp(
    const dstr_t *url, const char *name, dstr_off_t exp, dstr_off_t got, char c
){
    if(got.buf == NULL){
        LOG_ERROR("%x was never when parsing: %x\n", FS(name), FD(url));
        return false;
    }
    if(dstr_off_eq(exp, got)) return true;

    DSTR_VAR(buf, 1024);

    LOG_ERROR("bad %x parse:\n", FS(name));
    LOG_ERROR("url was : %x\n", FD(url));
    LOG_ERROR("expected:%x\n", FD(write_offset(&buf, exp, c)));
    LOG_ERROR("but got :%x\n", FD(write_offset(&buf, got, c)));

    return false;
}

typedef bool (*parse_f)(const dstr_t *text, url_t *out, dstr_t *errbuf);

static bool do_test_case(parse_f parser, char *c_url, char *c_exp){
    dstr_t d_url, d_exp;
    DSTR_WRAP(d_url, c_url, strlen(c_url), true);
    DSTR_WRAP(d_exp, c_exp, strlen(c_exp), true);

    bool ok = true;

    url_t exp = expected_url(&d_url, d_exp);
    url_t got;
    DSTR_VAR(errbuf, 512);
    ok = parser(&d_url, &got, &errbuf);
    if(!ok){
        LOG_ERROR("failed to parse url: %x\n%x\n", FS(c_url), FD(&errbuf));
        return false;
    }

    #define OFF_CMP(name, c) do_off_cmp(&d_url, #name, exp.name, got.name, c)

    ok &= OFF_CMP(scheme, 's');
    ok &= OFF_CMP(user, 'U');
    ok &= OFF_CMP(pass, 'P');
    ok &= OFF_CMP(host, 'h');
    ok &= OFF_CMP(port, 'n');
    ok &= OFF_CMP(path, 'p');
    ok &= OFF_CMP(query, 'q');
    ok &= OFF_CMP(fragment, 'f');

    #undef OFF_CMP

    return ok;
}

static bool do_failure_case(parse_f parser, char *c_url, char *c_exp){
    dstr_t d_url;
    DSTR_WRAP(d_url, c_url, strlen(c_url), true);

    DSTR_VAR(errbuf, 512);
    url_t got;
    bool ok = parser(&d_url, &got, &errbuf);
    if(ok){
        LOG_ERROR("failed to reject invalid url: %x\n", FS(c_url));
        return false;
    }

    DSTR_VAR(exp, 512);
    FMT_QUIET(&exp, "invalid url: %x\n             %x", FS(c_url), FS(c_exp));

    if(dstr_cmp2(errbuf, exp) != 0){
        LOG_ERROR(
            "-- failure case failed, expected:\n%x\n-- but got:\n%x\n",
            FD(&exp),
            FD(&errbuf)
        );
        return false;
    }

    return true;
}

static derr_t test_url(void){
    derr_t e = E_OK;

    // all urls are also valid url_references
    #define DO_TEST_CASE(in, exp) do { \
        ok &= do_test_case(parse_url_ex, in, exp); \
        ok &= do_test_case(parse_url_reference_ex, in, exp); \
    } while(0)

    #define DO_FAILURE_CASE(in, exp) do { \
        ok &= do_failure_case(parse_url_ex, in, exp); \
        ok &= do_failure_case(parse_url_reference_ex, in, exp); \
    } while(0)

    bool ok = true;
    // hier-part:"//", all components
    DO_TEST_CASE(
        "https://user:pass:word@splintermail.com/user?query=junk#fragment",
        "sssss   UUUU PPPPPPPPP hhhhhhhhhhhhhhhhppppp qqqqqqqqqq ffffffff"
    );
    // username without password but with colon
    DO_TEST_CASE(
        "https://user:@splintermail.com/user?query=junk#fragment",
        "sssss   UUUU  hhhhhhhhhhhhhhhhppppp qqqqqqqqqq ffffffff"
    );
    // username without password or colon
    DO_TEST_CASE(
        "https://user@splintermail.com/user?query=junk#fragment",
        "sssss   UUUU hhhhhhhhhhhhhhhhppppp qqqqqqqqqq ffffffff"
    );
    // empty userinfo but with '@'
    DO_TEST_CASE(
        "https://@splintermail.com/user?query=junk#fragment",
        "sssss    hhhhhhhhhhhhhhhhppppp qqqqqqqqqq ffffffff"
    );
    // typical url with query and fragment
    DO_TEST_CASE(
        "https://splintermail.com/user?query=junk#fragment",
        "sssss   hhhhhhhhhhhhhhhhppppp qqqqqqqqqq ffffffff"
    );
    // query but no fragment
    DO_TEST_CASE(
        "https://splintermail.com/user?query=junk",
        "sssss   hhhhhhhhhhhhhhhhppppp qqqqqqqqqq"
    );
    // fragment but no query
    DO_TEST_CASE(
        "https://splintermail.com/user#fragment",
        "sssss   hhhhhhhhhhhhhhhhppppp ffffffff"
    );
    // query and fragment but no path
    DO_TEST_CASE(
        "https://splintermail.com?query=junk#fragment",
        "sssss   hhhhhhhhhhhhhhhh qqqqqqqqqq ffffffff"
    );
    // fragment but no path
    DO_TEST_CASE(
        "https://splintermail.com:443#fragment",
        "sssss   hhhhhhhhhhhhhhhh nnn ffffffff"
    );
    // a # but no fragment
    DO_TEST_CASE(
        "https://user:pass:word@splintermail.com/user?query=junk#",
        "sssss   UUUU PPPPPPPPP hhhhhhhhhhhhhhhhppppp qqqqqqqqqq "
    );
    // a ? but no query
    DO_TEST_CASE(
        "https://user:pass:word@splintermail.com/user?#",
        "sssss   UUUU PPPPPPPPP hhhhhhhhhhhhhhhhppppp  "
    );
    // ending with a ? but no query
    DO_TEST_CASE(
        "https://user:pass:word@splintermail.com/user?",
        "sssss   UUUU PPPPPPPPP hhhhhhhhhhhhhhhhppppp "
    );
    // colon but no port number
    DO_TEST_CASE(
        "https://splintermail.com:",
        "sssss   hhhhhhhhhhhhhhhh "
    );
    // failure test: invalid port number
    DO_FAILURE_CASE(
        "https://splintermail.com:12a34",
        "                         ^^^^^"
    );
    // empty hostname
    DO_TEST_CASE(
        "https://:123",
        "sssss    nnn"
    );
    // ipv4 address
    DO_TEST_CASE(
        "https://127.0.0.1:8080",
        "sssss   hhhhhhhhh nnnn"
    );
    // ipv6 address
    DO_TEST_CASE(
        "https://[::1]:8080",
        "sssss   hhhhh nnnn"
    );
    // hier-part:path-absolute
    DO_TEST_CASE(
        "https:/abc/def",
        "sssss pppppppp"
    );
    // hier-part:path-rootless
    DO_TEST_CASE(
        "https:abc/def",
        "sssss ppppppp"
    );
    // hier-part:path-empty
    DO_TEST_CASE(
        "https:",
        "sssss "
    );
    // hier-part:path-empty with fragment
    DO_TEST_CASE(
        "https:#asdf",
        "sssss  ffff"
    );

    // percent encoding
    DO_TEST_CASE(
        "https://ab%aA%3f#asdf",
        "sssss   hhhhhhhh ffff"
    );

    // failure case: invalid percent encoding
    DO_FAILURE_CASE(
        "https://ab%1g#asdf",
        "            ^"
    );

    if(!ok){
        ORIG(&e, E_VALUE, "one or more test cases failed");
    }

    return e;
}

static derr_t test_url_reference(void){
    derr_t e = E_OK;

    bool ok = true;

    // uri_reference_noqf:scheme_or_path:SLASH branch
    ok &= do_test_case(parse_url_reference_ex,
        "asdf/asdf/asdf",
        "pppppppppppppp"
    );

    // uri_reference_noqf:scheme_or_path:COLON branch
    ok &= do_test_case(parse_url_reference_ex,
        "asdf://asdf/asdf",
        "ssss   hhhhppppp"
    );

    // failure case: invalid scheme after scheme_or_path
    ok &= do_failure_case(parse_url_reference_ex,
        "a$df://asdf/asdf",
        "^^^^"
    );

    // uri_reference_noqf:SLASH:SLASH branch
    ok &= do_test_case(parse_url_reference_ex,
        "//asdf/asdf",
        "  hhhhppppp"
    );

    // uri_reference_noqf:SLASH:relpath_0 branch (nonemtpy)
    ok &= do_test_case(parse_url_reference_ex,
        "/a/b/c",
        "pppppp"
    );

    // uri_reference_noqf:SLASH:relpath_0 branch (empty)
    ok &= do_test_case(parse_url_reference_ex,
        "/",
        "p"
    );

    // uri_reference_noqf:%empty branch
    ok &= do_test_case(parse_url_reference_ex,
        "",
        ""
    );

    if(!ok){
        ORIG(&e, E_VALUE, "one or more test cases failed");
    }

    return e;
}

//

static addrspec_t expected_addrspec(const dstr_t *addrspec, const dstr_t exp){
    if(addrspec->len != exp.len){
        LOG_ERROR("bad test, addrspec->len != exp.len\n");
        exit(2);
    }
    dstr_off_t scheme = find_matches(addrspec, exp, 's', 0);
    // nothing starts before scheme's ://
    size_t scheme_limit = scheme.len ? scheme.len + 3 : 0;
    // host starts after scheme
    dstr_off_t host = find_matches(addrspec, exp, 'h', scheme_limit);
    // port is always at the end
    dstr_off_t port = find_matches(addrspec, exp, 'p', addrspec->len);

    return (addrspec_t){ .scheme = scheme, .host = host, .port = port };
}

static bool do_addrspec_test_case(char *c_spec, char *c_exp){
    dstr_t d_spec, d_exp;
    DSTR_WRAP(d_spec, c_spec, strlen(c_spec), true);
    DSTR_WRAP(d_exp, c_exp, strlen(c_exp), true);

    bool ok = true;

    addrspec_t exp = expected_addrspec(&d_spec, d_exp);
    addrspec_t got;
    DSTR_VAR(errbuf, 512);
    ok = parse_addrspec_ex(&d_spec, &got, &errbuf);
    if(!ok){
        LOG_ERROR(
            "failed to parse addrspec: %x\n%x\n", FS(c_spec), FD(&errbuf)
        );
        return false;
    }

    #define OFF_CMP(name, c) do_off_cmp(&d_spec, #name, exp.name, got.name, c)

    ok &= OFF_CMP(scheme, 's');
    ok &= OFF_CMP(host, 'h');
    ok &= OFF_CMP(port, 'n');

    #undef OFF_CMP

    return ok;
}

static derr_t test_addrspec(void){
    derr_t e = E_OK;

    bool ok = true;
    // :port
    // host
    // host:
    // host:port
    // scheme://
    // scheme://host
    // scheme://host:
    // scheme://:port
    // scheme://host:port

    ok &= do_addrspec_test_case(
        ":1993",
        " pppp"
    );

    ok &= do_addrspec_test_case(
        "localhost",
        "hhhhhhhhh"
    );

    ok &= do_addrspec_test_case(
        "[::]",
        "hhhh"
    );

    ok &= do_addrspec_test_case(
        "localhost:",
        "hhhhhhhhh "
    );
    ok &= do_addrspec_test_case(
        "[::]:",
        "hhhh "
    );

    ok &= do_addrspec_test_case(
        "localhost:1993",
        "hhhhhhhhh pppp"
    );

    ok &= do_addrspec_test_case(
        "[::]:1993",
        "hhhh pppp"
    );

    ok &= do_addrspec_test_case(
        "tls://",
        "sss   "
    );

    ok &= do_addrspec_test_case(
        "tls://:1993",
        "sss    pppp"
    );

    ok &= do_addrspec_test_case(
        "tls://localhost",
        "sss   hhhhhhhhh"
    );

    ok &= do_addrspec_test_case(
        "tls://[::]",
        "sss   hhhh"
    );

    ok &= do_addrspec_test_case(
        "tls://localhost:",
        "sss   hhhhhhhhh "
    );
    ok &= do_addrspec_test_case(
        "tls://[::]:",
        "sss   hhhh "
    );

    ok &= do_addrspec_test_case(
        "tls://localhost:1993",
        "sss   hhhhhhhhh pppp"
    );

    ok &= do_addrspec_test_case(
        "tls://[::]:1993",
        "sss   hhhh pppp"
    );

    if(!ok){
        ORIG(&e, E_VALUE, "one or more test cases failed");
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_error_reporting(), test_fail);
    PROP_GO(&e, test_url(), test_fail);
    PROP_GO(&e, test_url_reference(), test_fail);
    PROP_GO(&e, test_addrspec(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}

