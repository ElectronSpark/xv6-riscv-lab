add_library(lock OBJECT
    mutex.c
    semaphore.c
    semaphore_test.c
    rwsem.c
    rwsem_test.c
    spinlock.c
    completion.c
    rcu.c
    rcu_test.c
)
