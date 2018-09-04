#include <stdio.h>
#include <openssl/err.h>
#include <openssl/bio.h>

int main(void){
    BIO *listener, *connection;
    // create listener
    listener = BIO_new_accept("127.0.0.1:5555");
    BIO_set_bind_mode(listener, BIO_BIND_REUSEADDR);
    // bind
    BIO_do_accept(listener);

    // this second BIO_do_accept() is where the lost memory gets allocated
    // accept
    BIO_do_accept(listener);
    connection = BIO_pop(listener);
    // close this connection
    BIO_free_all(connection);

    // if you return here valgrind says "memory still reachable"
    // accept
    BIO_do_accept(listener);
    // if you keturn here valgrind says "memory definitely lost"
    connection = BIO_pop(listener);
    // close this connection
    BIO_free_all(connection);

    // close the listener
    BIO_free_all(listener);

    // print any errors
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        printf("OpenSSL error: %s\n", buffer);
        e = ERR_get_error();
    }
    return 0;
}
