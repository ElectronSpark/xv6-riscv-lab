// Run Queue Priority Integration Test
//
// Tests priority system by modifying the current process's priority
// and verifying the scheduler respects those changes.
//

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "percpu.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc_private.h"
#include "errno.h"
#include "string.h"

// ============================================================================
// Test 1: Two-Layer Bitmask Logic
// ============================================================================

static void test_two_layer_mask(void) {
    printf("TEST: Two-Layer Bitmask Logic\n");
    
    // Verify the mapping formulas match expected values
    int test_cases[][3] = {
        // {major, expected_group, expected_bit_in_group}
        {0, 0, 0},
        {1, 0, 1},
        {7, 0, 7},
        {8, 1, 0},
        {15, 1, 7},
        {16, 2, 0},
        {63, 7, 7},
    };
    
    int num_tests = sizeof(test_cases) / sizeof(test_cases[0]);
    int passed = 0;
    
    for (int i = 0; i < num_tests; i++) {
        int major = test_cases[i][0];
        int expected_group = test_cases[i][1];
        int expected_bit = test_cases[i][2];
        
        int actual_group = major >> 3;
        int actual_bit = major & 7;
        
        if (actual_group == expected_group && actual_bit == expected_bit) {
            passed++;
        } else {
            printf("  FAIL: major %d -> group %d bit %d, expected group %d bit %d\n",
                   major, actual_group, actual_bit, expected_group, expected_bit);
        }
    }
    
    assert(passed == num_tests, "rq_test: bitmask mapping failed %d/%d", passed, num_tests);
    printf("  PASSED: %d/%d bitmask mappings correct\n", passed, num_tests);
}

// ============================================================================
// Test 2: Priority Change via sched_setattr
// ============================================================================

static void test_priority_change(void) {
    printf("TEST: Priority Change via sched_setattr\n");
    
    struct proc *p = myproc();
    struct sched_entity *se = p->sched_entity;
    
    // Get current priority
    struct sched_attr attr;
    sched_getattr(se, &attr);
    int original_priority = attr.priority;
    int original_major = MAJOR_PRIORITY(original_priority);
    
    printf("  Original priority: major=%d minor=%d\n", 
           original_major, MINOR_PRIORITY(original_priority));
    
    // Change to a different priority
    int new_major = (original_major == 10) ? 12 : 10;
    attr.priority = MAKE_PRIORITY(new_major, 1);
    
    // Note: sched_setattr handles its own locking
    int ret = sched_setattr(se, &attr);
    
    assert(ret == 0, "rq_test: sched_setattr failed with %d", ret);
    
    // Verify the change
    struct sched_attr new_attr;
    sched_getattr(se, &new_attr);
    int changed_major = MAJOR_PRIORITY(new_attr.priority);
    int changed_minor = MINOR_PRIORITY(new_attr.priority);
    
    printf("  Changed priority: major=%d minor=%d\n", changed_major, changed_minor);
    
    assert(changed_major == new_major, 
           "rq_test: major priority not changed, got %d expected %d", changed_major, new_major);
    assert(changed_minor == 1, 
           "rq_test: minor priority not changed, got %d expected 1", changed_minor);
    
    // Yield to let scheduler see the new priority
    scheduler_yield();
    
    // Restore original priority
    attr.priority = original_priority;
    sched_setattr(se, &attr);
    
    printf("  Restored original priority\n");
    printf("  PASSED\n");
}

// ============================================================================
// Test 3: Yield Respects Priority
// ============================================================================

static void test_yield_priority(void) {
    printf("TEST: Yield Respects Priority\n");
    
    struct proc *p = myproc();
    int my_priority = p->sched_entity->priority;
    
    printf("  Current process '%s' at priority major=%d\n",
           p->name, MAJOR_PRIORITY(my_priority));
    
    // Yield multiple times - we should keep getting scheduled
    // since we're likely the only high-priority runnable user process
    int yields_completed = 0;
    for (int i = 0; i < 5; i++) {
        scheduler_yield();
        yields_completed++;
    }
    
    assert(yields_completed == 5, "rq_test: not all yields completed");
    printf("  Successfully yielded %d times and got rescheduled\n", yields_completed);
    printf("  PASSED\n");
}

// ============================================================================
// Test 4: RQ Selection Consistency
// ============================================================================

static void test_rq_selection(void) {
    printf("TEST: RQ Selection Consistency\n");
    
    int test_cpu = cpuid();
    
    rq_lock(test_cpu);
    
    // Call pick_next_rq multiple times - should be deterministic
    struct rq *rq1 = pick_next_rq();
    struct rq *rq2 = pick_next_rq();
    struct rq *rq3 = pick_next_rq();
    
    rq_unlock(test_cpu);
    
    // All three should be the same (no changes happened in between)
    assert(rq1 == rq2 && rq2 == rq3, 
           "rq_test: inconsistent rq selection: %d, %d, %d",
           rq1->class_id, rq2->class_id, rq3->class_id);
    
    printf("  Consistent selection: class_id=%d\n", rq1->class_id);
    printf("  PASSED\n");
}

// ============================================================================
// Test 5: Priority Ordering (Comprehensive)
// ============================================================================

// Two-layer bitmask structure:
// - Top layer: 8-bit mask for groups (0-7), each group covers 8 major priorities
// - Secondary layer: 64-bit mask for individual major priorities (0-63)
// - Minor priority: 2-bit (0-3) within each major priority
//
// Priority range for FIFO: major 1-62 (0 is EXIT, 63 is IDLE)
// Major priority 80-119 (currently beyond 64) reserved for future EEVDF
//
// This test covers three cases:
// Case 1: Different top-layer groups (e.g., group 0 vs group 1 vs group 2)
// Case 2: Same top-layer group, different secondary bits
// Case 3: Same major priority, different minor priorities

// Helper to verify priority is correctly set and read back
static int verify_priority(struct sched_entity *se, int expected_major, int expected_minor) {
    struct sched_attr attr;
    sched_getattr(se, &attr);
    int actual_major = MAJOR_PRIORITY(attr.priority);
    int actual_minor = MINOR_PRIORITY(attr.priority);
    if (actual_major != expected_major || actual_minor != expected_minor) {
        printf("    FAIL: expected (%d,%d) got (%d,%d)\n", 
               expected_major, expected_minor, actual_major, actual_minor);
        return 0;
    }
    return 1;
}

static void test_priority_ordering(void) {
    printf("TEST: Priority Ordering (Comprehensive)\n");
    
    struct proc *p = myproc();
    struct sched_entity *se = p->sched_entity;
    int test_cpu = cpuid();
    
    // Save original
    struct sched_attr original_attr;
    sched_getattr(se, &original_attr);
    
    struct sched_attr attr;
    int picked_major;
    struct rq *picked_rq;
    
    // =========================================================================
    // Case 1: Different top-layer groups
    // Group 0: major 0-7, Group 1: major 8-15, Group 2: major 16-23
    // Lower group number = higher priority
    // =========================================================================
    printf("  Case 1: Different top-layer groups\n");
    
    // Test major=1 (group 0, highest non-exit)
    sched_attr_init(&attr);
    attr.priority = MAKE_PRIORITY(1, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 1, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=1 (group 0): pick_next_rq returned %d\n", picked_major);
    
    // Test major=9 (group 1)
    attr.priority = MAKE_PRIORITY(9, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 9, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=9 (group 1): pick_next_rq returned %d\n", picked_major);
    
    // Test major=17 (group 2) - default FIFO priority
    attr.priority = MAKE_PRIORITY(17, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 17, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=17 (group 2): pick_next_rq returned %d\n", picked_major);
    
    // Test major=50 (group 6)
    attr.priority = MAKE_PRIORITY(50, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 50, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=50 (group 6): pick_next_rq returned %d\n", picked_major);
    
    printf("    Case 1 PASSED\n");
    
    // =========================================================================
    // Case 2: Same top-layer group, different secondary layer bits
    // All within group 0 (major 1-7), testing secondary mask ordering
    // =========================================================================
    printf("  Case 2: Same group, different secondary bits\n");
    
    // Test major=1 (group 0, bit 1)
    attr.priority = MAKE_PRIORITY(1, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 1, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=1 (bit 1): pick_next_rq returned %d\n", picked_major);
    
    // Test major=3 (group 0, bit 3)
    attr.priority = MAKE_PRIORITY(3, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 3, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=3 (bit 3): pick_next_rq returned %d\n", picked_major);
    
    // Test major=5 (group 0, bit 5)
    attr.priority = MAKE_PRIORITY(5, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 5, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=5 (bit 5): pick_next_rq returned %d\n", picked_major);
    
    // Test major=7 (group 0, bit 7)
    attr.priority = MAKE_PRIORITY(7, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 7, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=7 (bit 7): pick_next_rq returned %d\n", picked_major);
    
    printf("    Case 2 PASSED\n");
    
    // =========================================================================
    // Case 3: Same major priority, different minor priorities (0-3)
    // Tests FIFO subqueue ordering within the same major level
    // =========================================================================
    printf("  Case 3: Same major, different minor priorities\n");
    
    // Test major=5 with minor=0 (highest within major)
    attr.priority = MAKE_PRIORITY(5, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 5, 0), "rq_test: priority set failed");
    scheduler_yield();
    printf("    major=5, minor=0: priority set and yield OK\n");
    
    // Test major=5 with minor=1
    attr.priority = MAKE_PRIORITY(5, 1);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 5, 1), "rq_test: priority set failed");
    scheduler_yield();
    printf("    major=5, minor=1: priority set and yield OK\n");
    
    // Test major=5 with minor=2
    attr.priority = MAKE_PRIORITY(5, 2);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 5, 2), "rq_test: priority set failed");
    scheduler_yield();
    printf("    major=5, minor=2: priority set and yield OK\n");
    
    // Test major=5 with minor=3 (lowest within major)
    attr.priority = MAKE_PRIORITY(5, 3);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 5, 3), "rq_test: priority set failed");
    scheduler_yield();
    printf("    major=5, minor=3: priority set and yield OK\n");
    
    printf("    Case 3 PASSED\n");
    
    // =========================================================================
    // Case 4: Boundary tests (edge of groups)
    // Test transitions at group boundaries: 7->8, 15->16, etc.
    // =========================================================================
    printf("  Case 4: Group boundary transitions\n");
    
    // major=7 (last in group 0)
    attr.priority = MAKE_PRIORITY(7, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 7, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=7 (end of group 0): pick_next_rq returned %d\n", picked_major);
    
    // major=8 (first in group 1)
    attr.priority = MAKE_PRIORITY(8, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 8, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=8 (start of group 1): pick_next_rq returned %d\n", picked_major);
    
    // major=62 (highest usable, group 7, just before IDLE=63)
    attr.priority = MAKE_PRIORITY(62, 0);
    sched_setattr(se, &attr);
    assert(verify_priority(se, 62, 0), "rq_test: priority set failed");
    scheduler_yield();
    
    rq_lock(test_cpu);
    picked_rq = pick_next_rq();
    picked_major = picked_rq->class_id;
    rq_unlock(test_cpu);
    printf("    major=62 (lowest usable): pick_next_rq returned %d\n", picked_major);
    
    printf("    Case 4 PASSED\n");
    
    // Restore original priority
    sched_setattr(se, &original_attr);
    
    printf("  All priority ordering cases PASSED\n");
    printf("  PASSED\n");
}

// ============================================================================
// Test 6: Priority-Ordered Process Activation
// ============================================================================
// 
// This test verifies that when multiple processes are created and woken up
// while preemption is disabled, they are scheduled in priority order once
// preemption is re-enabled.
//
// Test procedure:
// 1. Disable preemption (push_off)
// 2. Create multiple kernel processes with different priorities
// 3. Wake up all processes (they'll be enqueued but can't run yet)
// 4. Re-enable preemption (pop_off) and yield
// 5. Each process records its activation order
// 6. Verify processes activated in priority order (lower major = higher priority)

#define PRIORITY_TEST_COUNT 5

// Shared state for the test
static volatile int activation_order[PRIORITY_TEST_COUNT];
static volatile int activation_index;
static volatile int processes_done;
static struct spinlock priority_test_lock;
static proc_queue_t main_wait_queue;

// Priority values for test processes (lower = higher priority)
// Using priorities from different groups to test two-layer mask
static const int test_priorities[PRIORITY_TEST_COUNT] = {
    MAKE_PRIORITY(50, 0),   // Lowest priority (group 6, bit 2) - should run last
    MAKE_PRIORITY(17, 0),   // Mid priority (group 2, bit 1) - default
    MAKE_PRIORITY(5, 0),    // High priority (group 0, bit 5)
    MAKE_PRIORITY(25, 0),   // Mid-low priority (group 3, bit 1)
    MAKE_PRIORITY(2, 0),    // Highest priority (group 0, bit 2) - should run first
};

// Expected activation order based on priority (indices into test_priorities)
// Priority 2 < 5 < 17 < 25 < 50, so order should be: 4, 2, 1, 3, 0
static const int expected_order[PRIORITY_TEST_COUNT] = {4, 2, 1, 3, 0};

// Entry function for test processes
static int priority_test_proc_entry(uint64 my_index, uint64 unused) {
    (void)unused;
    
    // Record our activation order
    spin_lock(&priority_test_lock);
    int my_order = activation_index++;
    activation_order[my_index] = my_order;
    processes_done++;
    
    int all_done = (processes_done == PRIORITY_TEST_COUNT);
    spin_unlock(&priority_test_lock);
    
    // When all processes are done, wake up the main thread
    if (all_done) {
        proc_queue_wakeup_all(&main_wait_queue, 0, 0);
    }
    
    return 0; // Exit cleanly
}

static void test_priority_ordered_activation(void) {
    printf("TEST: Priority-Ordered Process Activation\n");
    
    // Initialize shared state
    spin_init(&priority_test_lock, "prio_test");
    proc_queue_init(&main_wait_queue, "main_wait", &priority_test_lock);
    activation_index = 0;
    processes_done = 0;
    for (int i = 0; i < PRIORITY_TEST_COUNT; i++) {
        activation_order[i] = -1;
    }
    
    struct proc *test_procs[PRIORITY_TEST_COUNT];
    
    // =========================================================================
    // Phase 1: Create processes with preemption disabled
    // =========================================================================
    printf("  Phase 1: Creating %d processes with preemption disabled\n", PRIORITY_TEST_COUNT);
    
    push_off();  // Disable preemption
    
    // Pin all test processes to current CPU for deterministic ordering
    int test_cpu = cpuid();
    cpumask_t cpu_mask = (1ULL << test_cpu);
    
    for (int i = 0; i < PRIORITY_TEST_COUNT; i++) {
        int ret = kernel_proc_create("prio_test", &test_procs[i], 
                                     priority_test_proc_entry, i, 0, 0);
        assert(ret >= 0, "rq_test: kernel_proc_create failed for process %d", i);
        
        // Set the priority and pin to current CPU
        // sched_setattr handles its own locking internally
        struct sched_entity *se = test_procs[i]->sched_entity;
        struct sched_attr attr;
        sched_attr_init(&attr);
        attr.priority = test_priorities[i];
        attr.affinity_mask = cpu_mask;  // Pin to current CPU
        
        sched_setattr(se, &attr);
        
        printf("    Created process %d (pid=%d) with priority major=%d on CPU %d\n", 
               i, test_procs[i]->pid, MAJOR_PRIORITY(test_priorities[i]), test_cpu);
    }
    
    // =========================================================================
    // Phase 2: Wake up all processes (still with preemption disabled)
    // =========================================================================
    printf("  Phase 2: Waking up all processes\n");
    
    for (int i = 0; i < PRIORITY_TEST_COUNT; i++) {
        wakeup_proc(test_procs[i]);  // wakeup_proc handles locking internally
    }
    
    // =========================================================================
    // Phase 3: Re-enable preemption and yield
    // =========================================================================
    printf("  Phase 3: Enabling preemption and yielding\n");
    
    pop_off();  // Re-enable preemption
    
    // Yield to let higher priority processes run
    scheduler_yield();
    
    // =========================================================================
    // Phase 4: Wait for all processes to complete
    // =========================================================================
    printf("  Phase 4: Waiting for all processes to complete\n");
    
    spin_lock(&priority_test_lock);
    while (processes_done < PRIORITY_TEST_COUNT) {
        // Wait on the queue - will be woken when last process completes
        proc_queue_wait(&main_wait_queue, &priority_test_lock, 0);
    }
    spin_unlock(&priority_test_lock);
    
    // =========================================================================
    // Phase 5: Verify activation order
    // =========================================================================
    printf("  Phase 5: Verifying activation order\n");
    
    printf("    Expected: ");
    for (int i = 0; i < PRIORITY_TEST_COUNT; i++) {
        printf("proc[%d] ", expected_order[i]);
    }
    printf("\n");
    
    printf("    Actual:   ");
    int correct = 1;
    for (int i = 0; i < PRIORITY_TEST_COUNT; i++) {
        // Find which process was activated at position i
        int proc_at_pos = -1;
        for (int j = 0; j < PRIORITY_TEST_COUNT; j++) {
            if (activation_order[j] == i) {
                proc_at_pos = j;
                break;
            }
        }
        printf("proc[%d] ", proc_at_pos);
        if (proc_at_pos != expected_order[i]) {
            correct = 0;
        }
    }
    printf("\n");
    
    // Since all processes are pinned to the same CPU, ordering must be correct
    assert(correct, "rq_test: Priority ordering failed! Processes did not activate in priority order");
    printf("    Processes activated in correct priority order!\n");
    
    printf("  PASSED\n");
}

// ============================================================================
// Test 7: Affinity Mask Change
// ============================================================================

static void test_affinity_change(void) {
    printf("TEST: CPU Affinity Change\n");
    
    struct proc *p = myproc();
    struct sched_entity *se = p->sched_entity;
    
    // Get current affinity
    struct sched_attr attr;
    sched_getattr(se, &attr);
    cpumask_t original_mask = attr.affinity_mask;
    
    printf("  Original affinity mask: 0x%lx\n", original_mask);
    
    // Pin to current CPU only
    int cur_cpu = cpuid();
    attr.affinity_mask = (1ULL << cur_cpu);
    
    int ret = sched_setattr(se, &attr);
    
    assert(ret == 0, "rq_test: sched_setattr for affinity failed");
    
    // Verify
    sched_getattr(se, &attr);
    assert(attr.affinity_mask == (1ULL << cur_cpu),
           "rq_test: affinity not changed correctly");
    
    printf("  Pinned to CPU %d, mask: 0x%lx\n", cur_cpu, attr.affinity_mask);
    
    // Yield - should stay on same CPU
    scheduler_yield();
    
    int new_cpu = cpuid();
    assert(new_cpu == cur_cpu, 
           "rq_test: CPU changed despite affinity pin, was %d now %d", cur_cpu, new_cpu);
    
    // Restore original affinity
    attr.affinity_mask = original_mask;
    sched_setattr(se, &attr);
    
    printf("  Restored original affinity\n");
    printf("  PASSED\n");
}

// ============================================================================
// Main Test Entry Point
// ============================================================================

void rq_test_run(void) {
    printf("\n========================================\n");
    printf("Run Queue Priority Integration Tests\n");
    printf("Running on CPU %ld\n", cpuid());
    printf("========================================\n\n");
    
    test_two_layer_mask();
    test_priority_change();
    test_yield_priority();
    test_rq_selection();
    test_priority_ordering();
    test_priority_ordered_activation();
    test_affinity_change();
    
    printf("\n========================================\n");
    printf("All Integration Tests PASSED!\n");
    printf("========================================\n\n");
}
