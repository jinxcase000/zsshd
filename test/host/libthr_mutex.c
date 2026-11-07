/* Host stand-in for the console's pthread mutex semantics, for
 * test/host/standdown.sh. On the console (FreeBSD libthr) a zeroed
 * pthread_mutex_t is PTHREAD_MUTEX_INITIALIZER: the first lock initialises it.
 * mbedtls_ctr_drbg_free() destroys the DRBG mutex and then zeroes the whole
 * context, so a random draw after it locks successfully and goes on into AES
 * with a zeroed key schedule. macOS pthreads refuse a zeroed mutex (EINVAL),
 * which would turn that draw into a quiet error. This swaps mbedtls's lock
 * function for one that treats an all-zero mutex the way libthr does. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/threading.h>

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_static_init = PTHREAD_MUTEX_INITIALIZER;
static int (*g_real_lock)(mbedtls_threading_mutex_t *);

static int all_zero(const void *p, size_t n) {
  const unsigned char *b = p;
  for (size_t i = 0; i < n; i++) {
    if (b[i] != 0) {
      return 0;
    }
  }
  return 1;
}

static int libthr_lock(mbedtls_threading_mutex_t *m) {
  if (m != NULL && all_zero(&m->mutex, sizeof(m->mutex))) {
    pthread_mutex_lock(&g_static_init);
    if (all_zero(&m->mutex, sizeof(m->mutex))) {
      pthread_mutex_init(&m->mutex, NULL);
    }
    pthread_mutex_unlock(&g_static_init);
  }
  return g_real_lock(m);
}

__attribute__((constructor)) static void libthr_mutex_install(void) {
  g_real_lock = mbedtls_mutex_lock;
  mbedtls_mutex_lock = libthr_lock;
}
