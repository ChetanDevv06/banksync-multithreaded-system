#include "deadlock_prevention.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
            fflush(stdout);                                                 \
            tests_failed++;                                                 \
        }                                                                   \
    } while (0)

#define RUN_TEST(name)                                                      \
    do {                                                                    \
        printf("TEST: %s\n", #name);                                        \
        fflush(stdout);                                                     \
        name();                                                             \
    } while (0)

static DeadlockPreventionStatus run_experiment(
    int timeout_ms,
    DeadlockPreventionResult *result)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(&exp, timeout_ms);
    if (st != DEADLOCK_PREVENTION_SUCCESS) {
        return st;
    }
    st = deadlock_prevention_run(&exp, result);
    deadlock_prevention_destroy(&exp);
    return st;
}

/* ------------------------------------------------------------------ */
/* Test A — init and validation                                       */
/* ------------------------------------------------------------------ */

static void test_a_init_success(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    TEST_ASSERT(exp.initialized == 1, "init flag should be set");
    TEST_ASSERT(exp.resource_a.id == DEADLOCK_PREVENTION_RESOURCE_A_ID,
                "resource A should have stable ID");
    TEST_ASSERT(exp.resource_b.id == DEADLOCK_PREVENTION_RESOURCE_B_ID,
                "resource B should have stable ID");
    TEST_ASSERT(exp.resource_a.mutex_initialized == 1,
                "resource A mutex should be initialized");
    TEST_ASSERT(exp.resource_b.mutex_initialized == 1,
                "resource B mutex should be initialized");
    TEST_ASSERT(exp.resource_a.balance == 1000000LL,
                "resource A should start with initial balance");
    TEST_ASSERT(exp.resource_b.balance == 1000000LL,
                "resource B should start with initial balance");
    deadlock_prevention_destroy(&exp);
}

static void test_a2_invalid_timeout_zero(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(&exp, 0);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "zero timeout should be rejected");
}

static void test_a3_invalid_timeout_negative(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(&exp, -1);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "negative timeout should be rejected");
}

static void test_a4_invalid_timeout_too_large(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(
        &exp, DEADLOCK_PREVENTION_MAX_TIMEOUT_MS + 1);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "excessive timeout should be rejected");
}

static void test_a5_invalid_timeout_too_small(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(&exp, 50);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "timeout below minimum should be rejected");
}

static void test_a6_null_experiment(void)
{
    DeadlockPreventionStatus st =
        deadlock_prevention_init(NULL, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "NULL experiment should be rejected");
}

static void test_a7_run_before_init(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionResult res;
    DeadlockPreventionStatus st = deadlock_prevention_run(&exp, &res);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_ERR_NOT_INITIALIZED,
                "run before init should be rejected");
}

static void test_a8_run_null_result(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    DeadlockPreventionStatus st2 = deadlock_prevention_run(&exp, NULL);
    TEST_ASSERT(st2 == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "run with NULL result should be rejected");
    deadlock_prevention_destroy(&exp);
}

static void test_a9_destroy_uninitialized(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    memset(&exp, 0, sizeof(exp));
    deadlock_prevention_destroy(&exp);
    TEST_ASSERT(1, "destroy on uninitialized should not crash");
}

static void test_a10_destroy_null(void)
{
    deadlock_prevention_destroy(NULL);
    TEST_ASSERT(1, "destroy on NULL should not crash");
}

static void test_a11_already_initialized(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "first init should succeed");
    DeadlockPreventionStatus st2 = deadlock_prevention_init(&exp, 3000);
    TEST_ASSERT(st2 == DEADLOCK_PREVENTION_ERR_ALREADY_INITIALIZED,
                "re-init without destroy should be rejected");
    deadlock_prevention_destroy(&exp);
}

static void test_a12_destroy_then_reinit(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    deadlock_prevention_destroy(&exp);
    DeadlockPreventionStatus st2 =
        deadlock_prevention_init(&exp, 3000);
    TEST_ASSERT(st2 == DEADLOCK_PREVENTION_SUCCESS,
                "re-init after destroy should succeed");
    deadlock_prevention_destroy(&exp);
}

/* ------------------------------------------------------------------ */
/* Test B — resource IDs and ordering                                 */
/* ------------------------------------------------------------------ */

static void test_b_resource_ids_and_ordering(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    TEST_ASSERT(exp.resource_a.id == DEADLOCK_PREVENTION_RESOURCE_A_ID,
                "resource A ID should be the stable lower ID");
    TEST_ASSERT(exp.resource_b.id == DEADLOCK_PREVENTION_RESOURCE_B_ID,
                "resource B ID should be the stable higher ID");
    TEST_ASSERT(exp.resource_a.id < exp.resource_b.id,
                "resource A ID should be less than resource B ID");
    deadlock_prevention_destroy(&exp);
}

/* ------------------------------------------------------------------ */
/* Test C — safe ordered locking                                      */
/* ------------------------------------------------------------------ */

static void test_c_both_operations_complete(void)
{
    DeadlockPreventionResult res;
    DeadlockPreventionStatus st = run_experiment(5000, &res);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "experiment run should succeed");
    TEST_ASSERT(res.mode == DEADLOCK_PREVENTION_MODE_SAFE_ORDERED,
                "mode should be safe ordered");
    TEST_ASSERT(res.operations_attempted > 0, "operations should be attempted");
    TEST_ASSERT(res.operations_completed > 0, "operations should complete");
    TEST_ASSERT(res.operations_failed == 0, "no operations should fail");
    TEST_ASSERT(res.both_operations_completed == 1,
                "both opposing-direction operations should complete");
    TEST_ASSERT(res.lower_id_acquired_first == 1,
                "lower-ID resource should be acquired first");
    TEST_ASSERT(res.final_state_consistent == 1,
                "final state should be consistent");
    TEST_ASSERT(res.total_balance_preserved == 1,
                "total balance should be preserved");
    TEST_ASSERT(res.duration_seconds >= 0.0, "duration should be non-negative");
    TEST_ASSERT(res.duration_seconds < 30.0,
                "should not block indefinitely");
}

static void test_c_ordering_rule_followed(void)
{
    DeadlockPreventionResult res;
    DeadlockPreventionStatus st = run_experiment(5000, &res);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "run should succeed");
    TEST_ASSERT(res.lower_id_acquired_first == 1,
                "ascending-ID lock order should be followed");
}

static void test_c_repeated_concurrent_execution(void)
{
    for (int i = 0; i < 5; i++) {
        DeadlockPreventionResult res;
        DeadlockPreventionStatus st = run_experiment(5000, &res);
        TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS,
                    "repeated run should succeed");
        TEST_ASSERT(res.both_operations_completed == 1,
                    "both operations should complete each run");
        TEST_ASSERT(res.final_state_consistent == 1,
                    "final state should be consistent each run");
        TEST_ASSERT(res.total_balance_preserved == 1,
                    "total balance should be preserved each run");
        TEST_ASSERT(res.lower_id_acquired_first == 1,
                    "lock ordering should hold each run");
        TEST_ASSERT(res.operations_failed == 0,
                    "no failures each run");
    }
}

/* ------------------------------------------------------------------ */
/* Test D — same-resource handling                                    */
/* ------------------------------------------------------------------ */

static void test_d_same_resource_transfer(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");

    /* Transferring from a resource to itself must not deadlock. */
    st = deadlock_prevention_transfer_safe(
        &exp, &exp.resource_a, &exp.resource_a, 100);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS,
                "same-resource transfer should succeed");

    /* Balance should be unchanged after a self-transfer. */
    TEST_ASSERT(exp.resource_a.balance == 1000000LL,
                "self-transfer should not change balance");

    deadlock_prevention_destroy(&exp);
}

/* ------------------------------------------------------------------ */
/* Test E — invalid arguments                                         */
/* ------------------------------------------------------------------ */

static void test_e_invalid_result_pointer(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    DeadlockPreventionStatus st2 = deadlock_prevention_run(&exp, NULL);
    TEST_ASSERT(st2 == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "run with NULL result should be rejected");
    deadlock_prevention_destroy(&exp);
}

static void test_e_invalid_transfer_args(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");

    DeadlockPreventionStatus st2 = deadlock_prevention_transfer_safe(
        NULL, &exp.resource_a, &exp.resource_b, 100);
    TEST_ASSERT(st2 == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "NULL experiment should be rejected");

    DeadlockPreventionStatus st3 = deadlock_prevention_transfer_safe(
        &exp, NULL, &exp.resource_b, 100);
    TEST_ASSERT(st3 == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "NULL from resource should be rejected");

    DeadlockPreventionStatus st4 = deadlock_prevention_transfer_safe(
        &exp, &exp.resource_a, NULL, 100);
    TEST_ASSERT(st4 == DEADLOCK_PREVENTION_ERR_INVALID_ARGUMENT,
                "NULL to resource should be rejected");

    deadlock_prevention_destroy(&exp);
}

/* ------------------------------------------------------------------ */
/* Test F — destruction and cleanup                                   */
/* ------------------------------------------------------------------ */

static void test_f_destroy_after_run(void)
{
    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st =
        deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
    DeadlockPreventionResult res;
    st = deadlock_prevention_run(&exp, &res);
    TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "run should succeed");
    deadlock_prevention_destroy(&exp);
    TEST_ASSERT(1, "destroy after run should not crash");
}

static void test_f_repeated_init_destroy_cycle(void)
{
    for (int i = 0; i < 3; i++) {
        DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
        DeadlockPreventionStatus st =
            deadlock_prevention_init(&exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
        TEST_ASSERT(st == DEADLOCK_PREVENTION_SUCCESS, "init should succeed");
        deadlock_prevention_destroy(&exp);
        TEST_ASSERT(exp.initialized == 0,
                    "initialized flag should be cleared after destroy");
    }
}

int main(void)
{
    RUN_TEST(test_a_init_success);
    RUN_TEST(test_a2_invalid_timeout_zero);
    RUN_TEST(test_a3_invalid_timeout_negative);
    RUN_TEST(test_a4_invalid_timeout_too_large);
    RUN_TEST(test_a5_invalid_timeout_too_small);
    RUN_TEST(test_a6_null_experiment);
    RUN_TEST(test_a7_run_before_init);
    RUN_TEST(test_a8_run_null_result);
    RUN_TEST(test_a9_destroy_uninitialized);
    RUN_TEST(test_a10_destroy_null);
    RUN_TEST(test_a11_already_initialized);
    RUN_TEST(test_a12_destroy_then_reinit);
    RUN_TEST(test_b_resource_ids_and_ordering);
    RUN_TEST(test_c_both_operations_complete);
    RUN_TEST(test_c_ordering_rule_followed);
    RUN_TEST(test_c_repeated_concurrent_execution);
    RUN_TEST(test_d_same_resource_transfer);
    RUN_TEST(test_e_invalid_result_pointer);
    RUN_TEST(test_e_invalid_transfer_args);
    RUN_TEST(test_f_destroy_after_run);
    RUN_TEST(test_f_repeated_init_destroy_cycle);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
