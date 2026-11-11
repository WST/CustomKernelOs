/* Гостевой тест libpthread. На хосте потоков нет: сборка только проверяет,
 * что cc принимает заголовок и -lpthread находит библиотеку. */
#include "myos.h"
#include "pthread.h"
#include "stdio.h"

static pthread_mutex_t mu;
static pthread_cond_t cv;
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int counter;
static int ready;
static int once_n;
static int local_errno[2];
static int local_key[2];

static void once_fn(void) { once_n++; }

static void *worker(void *arg) {
    int id = (int)arg;
    int i;
    errno = 40 + id;
    local_errno[id] = errno;
    pthread_setspecific(key, (void *)(200 + id));
    local_key[id] = (int)pthread_getspecific(key);
    for (i = 0; i < 200; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    if (id == 0) {
        pthread_mutex_lock(&mu);
        ready = 1;
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&mu);
    }
    return (void *)(10 + id);
}

static void *idle_fn(void *arg) {
    (void)arg;
    return 0;
}

static int fail(const char *what, int code) {
    printf("pthread: %s %d\n", what, code);
    return 1;
}

int main(void) {
    pthread_t a, b, d;
    pthread_attr_t attr;
    void *ra = 0, *rb = 0;
    int rc;
    errno = 3;
    if (pthread_key_create(&key, 0)) return fail("key", 0);
    if (pthread_setspecific(key, (void *)9) || pthread_getspecific(key) != (void *)9)
        return fail("main key", 0);
    pthread_mutex_init(&mu, 0);
    pthread_cond_init(&cv, 0);
    if (pthread_mutex_trylock(&mu)) return fail("trylock", 0);
    if (pthread_mutex_trylock(&mu) != EBUSY) return fail("relock", 0);
    pthread_mutex_unlock(&mu);
    pthread_once(&once, once_fn);
    pthread_once(&once, once_fn);
    if (once_n != 1) return fail("once", once_n);
    rc = pthread_create(&a, 0, worker, (void *)0);
    if (rc) return fail("create", rc);
    rc = pthread_create(&b, 0, worker, (void *)1);
    if (rc) return fail("create2", rc);
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    rc = pthread_join(a, &ra);
    if (rc) return fail("join", rc);
    rc = pthread_join(b, &rb);
    if (rc) return fail("join2", rc);
    if (errno != 3) return fail("errno", errno);
    if (pthread_getspecific(key) != (void *)9) return fail("key leak", 0);
    if (local_errno[0] != 40 || local_errno[1] != 41) return fail("thread errno", local_errno[0]);
    if (local_key[0] != 200 || local_key[1] != 201) return fail("thread key", local_key[0]);
    if (counter != 400) return fail("counter", counter);
    if (ra != (void *)10 || rb != (void *)11) return fail("retval", (int)ra);
    if (!pthread_equal(pthread_self(), pthread_self())) return fail("self", 0);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 65536u);
    rc = pthread_create(&d, &attr, idle_fn, 0);
    if (rc) return fail("detach create", rc);
    rc = pthread_join(d, 0);
    if (rc != EINVAL && rc != ESRCH) return fail("detach join", rc);
    printf("pthread: ok\n");
    return 0;
}
