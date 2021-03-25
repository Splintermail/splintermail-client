#ifndef SMPHP_H
# define SMPHP_H

extern zend_module_entry smphp_module_entry;
# define phpext_smphp_ptr &smphp_module_entry

# if defined(ZTS) && defined(COMPILE_DL_SMPHP)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

ZEND_BEGIN_MODULE_GLOBALS(smphp)
    zend_string *sql_sock;
    zend_long *server_id;
ZEND_END_MODULE_GLOBALS(smphp)

ZEND_EXTERN_MODULE_GLOBALS(smphp)

#define SMPHP_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(smphp, v)

#endif // SMPHP_H
