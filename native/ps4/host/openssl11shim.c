#define _GNU_SOURCE
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/x509.h>

#ifdef ERR_put_error
#undef ERR_put_error
#endif
#ifdef CRYPTO_num_locks
#undef CRYPTO_num_locks
#endif
#ifdef CRYPTO_set_locking_callback
#undef CRYPTO_set_locking_callback
#endif
#ifdef CRYPTO_add_lock
#undef CRYPTO_add_lock
#endif
#ifdef EVP_MD_size
#undef EVP_MD_size
#endif
#ifdef EVP_CIPHER_CTX_cleanup
#undef EVP_CIPHER_CTX_cleanup
#endif
#ifdef EVP_CIPHER_CTX_init
#undef EVP_CIPHER_CTX_init
#endif
#ifdef HMAC_CTX_cleanup
#undef HMAC_CTX_cleanup
#endif
#ifdef HMAC_CTX_init
#undef HMAC_CTX_init
#endif
#ifdef OPENSSL_add_all_algorithms_conf
#undef OPENSSL_add_all_algorithms_conf
#endif
#ifdef SSL_get_peer_certificate
#undef SSL_get_peer_certificate
#endif
#ifdef SSL_library_init
#undef SSL_library_init
#endif
#ifdef SSL_load_error_strings
#undef SSL_load_error_strings
#endif
#ifdef SSL_state
#undef SSL_state
#endif
#ifdef ERR_load_crypto_strings
#undef ERR_load_crypto_strings
#endif

void ERR_put_error(int lib, int func, int reason, const char *file, int line) {
    (void)func;
    ERR_new();
    ERR_set_debug(file, line, NULL);
    ERR_set_error(lib, reason, NULL);
}

int EVP_MD_size(const EVP_MD *md) { return EVP_MD_get_size(md); }
int EVP_CIPHER_CTX_cleanup(EVP_CIPHER_CTX *ctx) { return EVP_CIPHER_CTX_reset(ctx); }
void EVP_CIPHER_CTX_init(EVP_CIPHER_CTX *ctx) { (void)EVP_CIPHER_CTX_reset(ctx); }
void HMAC_CTX_cleanup(HMAC_CTX *ctx) { (void)HMAC_CTX_reset(ctx); }
void HMAC_CTX_init(HMAC_CTX *ctx) { (void)HMAC_CTX_reset(ctx); }

void ERR_load_crypto_strings(void) { (void)OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL); }
void OPENSSL_add_all_algorithms_conf(void) {
    (void)OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS | OPENSSL_INIT_LOAD_CONFIG, NULL);
}
int SSL_library_init(void) { return OPENSSL_init_ssl(0, NULL) ? 1 : 0; }
void SSL_load_error_strings(void) {
    (void)OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}
X509 *SSL_get_peer_certificate(const SSL *ssl) { return SSL_get1_peer_certificate(ssl); }
int SSL_state(const SSL *ssl) { return (int)SSL_get_state(ssl); }

/* OpenSSL 1.0.x thread-lock callbacks. 1.1+ handles locking internally. */
int CRYPTO_num_locks(void) { return 1; }
void CRYPTO_set_locking_callback(void (*func)(int, int, const char *, int)) { (void)func; }
int CRYPTO_add_lock(int *pointer, int amount, int type, const char *file, int line) {
    (void)type; (void)file; (void)line;
    *pointer += amount;
    return *pointer;
}

#ifdef EVP_MD_CTX_create
#undef EVP_MD_CTX_create
#endif
#ifdef EVP_MD_CTX_destroy
#undef EVP_MD_CTX_destroy
#endif
#ifdef RSA_PKCS1_SSLeay
#undef RSA_PKCS1_SSLeay
#endif
#ifdef SSLeay
#undef SSLeay
#endif
#ifdef SSLv23_method
#undef SSLv23_method
#endif
#ifdef sk_new_null
#undef sk_new_null
#endif
#ifdef sk_free
#undef sk_free
#endif
#ifdef sk_num
#undef sk_num
#endif
#ifdef sk_value
#undef sk_value
#endif
#ifdef sk_push
#undef sk_push
#endif
#ifdef sk_pop
#undef sk_pop
#endif
#ifdef sk_pop_free
#undef sk_pop_free
#endif

/* Additional OpenSSL 1.0/1.1 aliases removed in OpenSSL 3. */
EVP_MD_CTX *EVP_MD_CTX_create(void) { return EVP_MD_CTX_new(); }
void EVP_MD_CTX_destroy(EVP_MD_CTX *ctx) { EVP_MD_CTX_free(ctx); }
const RSA_METHOD *RSA_PKCS1_SSLeay(void) { return RSA_PKCS1_OpenSSL(); }
unsigned long SSLeay(void) { return OpenSSL_version_num(); }
const SSL_METHOD *SSLv23_method(void) { return TLS_method(); }

OPENSSL_STACK *sk_new_null(void) { return OPENSSL_sk_new_null(); }
void sk_free(OPENSSL_STACK *st) { OPENSSL_sk_free(st); }
int sk_num(const OPENSSL_STACK *st) { return OPENSSL_sk_num(st); }
void *sk_value(const OPENSSL_STACK *st, int i) { return OPENSSL_sk_value(st, i); }
int sk_push(OPENSSL_STACK *st, const void *data) { return OPENSSL_sk_push(st, data); }
void *sk_pop(OPENSSL_STACK *st) { return OPENSSL_sk_pop(st); }
void sk_pop_free(OPENSSL_STACK *st, void (*func)(void *)) { OPENSSL_sk_pop_free(st, func); }
