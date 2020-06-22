#include <limits.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

// main entrypoint for module
PyObject* PyInit_pysm(void);

static PyObject *PysmError;

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
        PyErr_SetString(PysmError, errstr);
        return NULL;
    }

    return PyBytes_FromStringAndSize((char*)fpr, fpr_len);
}

static PyMethodDef PysmMethods[] = {
    {
        "hash_key",
        pysm_hash_key,
        METH_VARARGS,
        "hash a PEM-encoded public key",
    },
    {NULL, NULL, 0, NULL},  // sentinel
};

static struct PyModuleDef pysmmodule = {
    PyModuleDef_HEAD_INIT,
    "pysm", /* name of module */
    NULL,   /* module documentation, may be NULL */
    -1,     /* size of per-interpreter state of the module,
               or -1 if the module keeps state in global variables. */
    PysmMethods
};

PyObject* PyInit_pysm(void){
    // prep SSL
    SSL_library_init();
    SSL_load_error_strings();

    PyObject *m;

    m = PyModule_Create(&pysmmodule);
    if (m == NULL)
    return NULL;

    PysmError = PyErr_NewException("pysm.error", NULL, NULL);
    Py_INCREF(PysmError);
    PyModule_AddObject(m, "error", PysmError);
    return m;
}

