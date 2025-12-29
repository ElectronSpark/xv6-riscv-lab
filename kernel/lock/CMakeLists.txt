add_library(lock OBJECT
    mutex.c
    semaphore.c
    semaphore_test.c
    rwlock.c
    rwlock_test.c
    spinlock.c
    completion.c
    rcu.c
    rcu_test.c
)
