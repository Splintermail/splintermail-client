#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "pysm.h"

#define BUILD_STRING(dstr) \
    Py_BuildValue("s#", (dstr).data, (Py_ssize_t)(dstr).len)
#define BUILD_OPTIONAL_STRING(dstr, ok) \
    Py_BuildValue("s#", (ok) ? (dstr).data : NULL, (Py_ssize_t)(dstr).len)
#define BUILD_BYTES(dstr) \
    Py_BuildValue("y#", (dstr).data, (Py_ssize_t)(dstr).len)
#define BUILD_OPTIONAL_BYTES(dstr, ok) \
    Py_BuildValue("y#", (ok) ? (dstr).data : NULL, (Py_ssize_t)(dstr).len)

#define RETURN_BOOL(val) do { \
    if(val){ \
        Py_RETURN_TRUE; \
    }else{ \
        Py_RETURN_FALSE; \
    } \
} while(0)

REGISTER_ERROR_TYPE(E_NORAISE, "NORAISE");

// main entrypoint for python module
PyObject* PyInit_pysm(void);

static PyObject *pysm_error;

// raise a python error from a derr_t.
static void raise_derr(derr_t *e){
    if(e->type == E_NORAISE){
    }else{
        PyErr_SetString(
            pysm_error, e->msg.data ? e->msg.data : "empty e.msg"
        );
    }
    DROP_VAR(e);
}

static void py_smsql_dealloc(py_smsql_t *self){
    smsql_deinit(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int py_smsql_init(py_smsql_t *self, PyObject *args, PyObject *kwds){
    derr_t e = E_OK;

    dstr_t _sock;
    const dstr_t *sock;
    dstr_t _user;
    const dstr_t *user;
    dstr_t _pass;
    const dstr_t *pass;

    py_args_t spec = {
        pyarg_dstr_opt(&_sock, &sock, "sock", "/var/run/mysqld/mysqld.sock"),
        pyarg_nullable_dstr_opt(&_user, &user, "user", NULL),
        pyarg_nullable_dstr_opt(&_pass, &pass, "pass", NULL),
    };

    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    PROP_GO(&e, smsql_init(self, *sock, user, pass), fail);

    return 0;

fail:
    raise_derr(&e);
    return -1;
}


static PyObject *py_smsql_connect(py_smsql_t *self){
    derr_t e = E_OK;

    PROP_GO(&e, smsql_connect(self), fail);

    Py_RETURN_NONE;

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_close(py_smsql_t *self){
    smsql_close(self);
    Py_RETURN_NONE;
}


static PyObject *py_smsql_enter(py_smsql_t *self){
    derr_t e = E_OK;

    if(!self->_sql){
        PROP_GO(&e, smsql_connect(self), fail);
    }

    Py_INCREF(self);
    return (PyObject*)self;

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_exit(py_smsql_t *self, PyObject *args){
    (void)args;
    smsql_close(self);
    Py_RETURN_NONE;
}

static PyObject *py_smsql_get_uuid(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _email;
    const dstr_t *email;
    py_args_t spec = {
        pyarg_dstr(&_email, &email, "email"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    bool ok;

    PROP_GO(&e, get_uuid_for_email(&self->sql, email, &uuid, &ok), fail);

    return BUILD_OPTIONAL_BYTES(uuid, ok);

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_get_email(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    bool ok;

    PROP_GO(&e, get_email_for_uuid(&self->sql, uuid, &email, &ok), fail);

    return BUILD_OPTIONAL_STRING(email, ok);

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_list_aliases(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    link_t aliases;
    link_init(&aliases);
    PROP_GO(&e, list_aliases(&self->sql, uuid, &aliases), fail);

    // count entries
    Py_ssize_t count = 0;
    link_t *link;
    for(link = aliases.next; link != &aliases; link = link->next){
        count++;
    }

    // create the output list
    PyObject *py_list = PyList_New(count);
    if(!py_list) ORIG_GO(&e, E_NOMEM, "nomem", fail_aliases);

    count = 0;
    // populate the output list
    while((link = link_list_pop_first(&aliases))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);

        // build an output tuple of (alias, paid)
        // s# (char*, Py_ssize_t) -> creates a string
        // O (PyObject) -> increments and returns a PyObject
        PyObject *tuple = Py_BuildValue(
            "(s#, O)",
            alias->alias.data, (Py_ssize_t)alias->alias.len,
            alias->paid ? Py_True : Py_False
        );

        // always free the popped alias
        smsql_alias_free(&alias);

        // now check for errors
        if(!tuple) ORIG_GO(&e, E_NOMEM, "nomem", fail_list);

        // the SET_ITEM macro is only suitable for newly created, empty lists
        PyList_SET_ITEM(py_list, count++, tuple);
    }

    return py_list;

fail_list:
    Py_DECREF(py_list);
fail_aliases:
    while((link = link_list_pop_first(&aliases))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);
        smsql_alias_free(&alias);
    }
fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_add_random_alias(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    DSTR_VAR(alias, SMSQL_EMAIL_SIZE);
    bool ok;

    PROP_GO(&e, add_random_alias(&self->sql, uuid, &alias, &ok), fail);

    return BUILD_OPTIONAL_STRING(alias, ok);

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_add_primary_alias(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    dstr_t _alias;
    const dstr_t *alias;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
        pyarg_dstr(&_alias, &alias, "alias"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    bool ok;

    PROP_GO(&e, add_primary_alias(&self->sql, uuid, alias, &ok), fail);

    RETURN_BOOL(ok);

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *py_smsql_delete_alias(
    py_smsql_t *self, PyObject *args, PyObject *kwds
){
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    dstr_t _alias;
    const dstr_t *alias;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
        pyarg_dstr(&_alias, &alias, "alias"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    bool deleted;

    PROP_GO(&e, delete_alias(&self->sql, uuid, alias, &deleted), fail);

    RETURN_BOOL(deleted);

fail:
    raise_derr(&e);
    return NULL;
}

static PyMethodDef py_smsql_methods[] = {
    {
        .ml_name = "connect",
        .ml_meth = (PyCFunction)(void*)py_smsql_connect,
        .ml_flags = METH_NOARGS,
        .ml_doc = "Connect to server",
    },
    {
        .ml_name = "close",
        .ml_meth = (PyCFunction)(void*)py_smsql_close,
        .ml_flags = METH_NOARGS,
        .ml_doc = "Disconnect from server",
    },
    {
        .ml_name = "__enter__",
        .ml_meth = (PyCFunction)(void*)py_smsql_enter,
        .ml_flags = METH_NOARGS,
        .ml_doc = NULL,
    },
    {
        .ml_name = "__exit__",
        .ml_meth = (PyCFunction)(void*)py_smsql_exit,
        .ml_flags = METH_VARARGS,
        .ml_doc = NULL,
    },
    {
        .ml_name = "get_uuid",
        .ml_meth = (PyCFunction)(void*)py_smsql_get_uuid,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Get a uuid for an email.  Returns None if not found.",
    },
    {
        .ml_name = "get_email",
        .ml_meth = (PyCFunction)(void*)py_smsql_get_email,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Get an email for a uuid.  Returns None if not found.",
    },
    {
        .ml_name = "list_aliases",
        .ml_meth = (PyCFunction)(void*)py_smsql_list_aliases,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "List aliases for a uuid.",
    },
    {
        .ml_name = "add_random_alias",
        .ml_meth = (PyCFunction)(void*)py_smsql_add_random_alias,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Add a random alias for a uuid.  "
                  "Returns the alias, or None if max aliases was reached.",
    },
    {
        .ml_name = "add_primary_alias",
        .ml_meth = (PyCFunction)(void*)py_smsql_add_primary_alias,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Add a primary alias for a uuid.  Returns True on success.",
    },
    {
        .ml_name = "delete_alias",
        .ml_meth = (PyCFunction)(void*)py_smsql_delete_alias,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Delete an alias for a uuid.  Returns True if it happened.",
    },
    {NULL}, // sentinel
};


static PyTypeObject py_smsql_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    // this needs to be dotted to work with pickle and pydoc
    .tp_name = "pysm.SMSQL",
    .tp_doc = "python access to splintermail predefined sql statements",
    .tp_basicsize = sizeof(py_smsql_t),
    // 0 means "size is not variable"
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_dealloc = (destructor) py_smsql_dealloc,
    .tp_methods = py_smsql_methods,
    .tp_init = (initproc)py_smsql_init,
};

// helper fuctions

static PyObject *pysm_to_fsid(PyObject *self, PyObject *args, PyObject *kwds){
    (void)self;
    derr_t e = E_OK;

    dstr_t _uuid;
    const dstr_t *uuid;
    py_args_t spec = {
        pyarg_dstr(&_uuid, &uuid, "uuid"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    DSTR_VAR(fsid, SMSQL_FSID_SIZE);

    PROP_GO(&e, to_fsid(uuid, &fsid), fail);

    return BUILD_STRING(fsid);

fail:
    raise_derr(&e);
    return NULL;
}

static PyObject *pysm_to_uuid(PyObject *self, PyObject *args, PyObject *kwds){
    (void)self;
    derr_t e = E_OK;

    dstr_t _fsid;
    const dstr_t *fsid;
    py_args_t spec = {
        pyarg_dstr(&_fsid, &fsid, "fsid"),
    };
    PROP_GO(&e, pyarg_parse(args, kwds, spec), fail);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);

    PROP_GO(&e, to_uuid(fsid, &uuid), fail);

    return BUILD_BYTES(uuid);

fail:
    raise_derr(&e);
    return NULL;
}

////// old pysm

static void get_ssl_errors(char* buffer, size_t len){
    unsigned long e;
    size_t written = 0;
    while( (e = ERR_get_error()) && written < len - 1 ){
        // pop an error and write it to a string
        char temp[256];
        ERR_error_string_n(e, temp, sizeof(temp));
        // append that string to the output bufer
        int ret = snprintf(buffer + written, len - written, "%s\n", temp);
        if(ret < 0) break;
        written += (size_t)ret;
    }
    // make sure we always null terminate
    buffer[len - 1] = '\0';
}

static PyObject *pysm_hash_key(PyObject *self, PyObject *args){
    (void)self;

    int error = 0;
    // this is where we would store errors
    char errstr[1024];

    // get string with the PEM_key in it
    const char *pemcert;
    Py_ssize_t plen;
    if (!PyArg_ParseTuple(args, "s#", &pemcert, &plen))
        return NULL;

    // convert plen to int
    if(plen > INT_MAX || plen < 0){
        return NULL;
    }
    int len = (int)plen;

    // initialize
    EVP_PKEY* key = EVP_PKEY_new();
    if(!key){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 1;
        goto done;
    }

    // wrap the public key in a BIO
    BIO* bio = BIO_new_mem_buf((const void*)pemcert, len);
    if(!bio){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 2;
        goto cleanup_1;
    }

    // read the public key from the bio
    EVP_PKEY* temp;
    temp = PEM_read_bio_PUBKEY(bio, &key, NULL, NULL);
    // done with BIO
    BIO_free(bio);
    if(!temp){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 3;
        goto cleanup_1;
    }

    // now get ready to get the fingerprint of the key
    X509* x = X509_new();
    if(!x){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 4;
        goto cleanup_1;
    }

    int ret = X509_set_pubkey(x, key);
    if(ret != 1){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 5;
        goto cleanup_2;
    }

    // get the fingerprint
    unsigned char fpr[EVP_MAX_MD_SIZE];
    unsigned int fpr_len;
    const EVP_MD* type = EVP_sha256();
    ret = X509_pubkey_digest(x, type, fpr, &fpr_len);
    if(ret != 1){
        get_ssl_errors(errstr, sizeof(errstr));
        error = 6;
        goto cleanup_2;
    }

cleanup_2:
    X509_free(x);
cleanup_1:
    EVP_PKEY_free(key);
done:
    // check for errors
    if(error){
        PyErr_SetString(pysm_error, errstr);
        return NULL;
    }

    return PyBytes_FromStringAndSize((char*)fpr, fpr_len);
}

#define ARG_KWARG_FN_CAST(fn)\
    (PyCFunction)(void(*)(void))(fn)

static PyMethodDef pysm_methods[] = {
    {
        .ml_name = "hash_key",
        .ml_meth = pysm_hash_key,
        .ml_flags = METH_VARARGS,
        .ml_doc = "hash a PEM-encoded public key",
    },
    {
        .ml_name = "to_fsid",
        .ml_meth = ARG_KWARG_FN_CAST(pysm_to_fsid),
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "get an fsid for a uuid",
    },
    {
        .ml_name = "to_uuid",
        .ml_meth = ARG_KWARG_FN_CAST(pysm_to_uuid),
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "get a uuid from an fsid",
    },
    {0},  // sentinel
};

static struct PyModuleDef pysm_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "pysm",
    .m_doc = "splintermail python module",
    .m_size = -1, /* size of per-interpreter state of the module,
                     or -1 if the module keeps state in global variables. */
    .m_methods = pysm_methods,
};

PyObject* PyInit_pysm(void){
    // prep SSL
    SSL_library_init();
    SSL_load_error_strings();

    // prep mysql
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        fprintf(stderr, "failed to init mysql library\n");
        return NULL;
    }

    if (PyType_Ready(&py_smsql_type) < 0) return NULL;

    PyObject *module = PyModule_Create(&pysm_module);
    if (module == NULL){
        return NULL;
    }

    Py_INCREF((PyObject*)&py_smsql_type);
    ret = PyModule_AddObject(module, "SMSQL", (PyObject*)&py_smsql_type);
    if(ret < 0) goto fail_py_smsql;

    pysm_error = PyErr_NewException("pysm.PysmError", NULL, NULL);
    Py_INCREF(pysm_error);
    ret = PyModule_AddObject(module, "PysmError", pysm_error);
    if(ret < 0) goto fail_pysm_error;

    return module;

fail_pysm_error:
    Py_DECREF((PyObject*)&pysm_error);
fail_py_smsql:
    Py_DECREF((PyObject*)&py_smsql_type);
    Py_DECREF(module);
    return NULL;
}
