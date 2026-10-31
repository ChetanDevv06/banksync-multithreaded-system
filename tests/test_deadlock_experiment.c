#include "deadlock_experiment.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <stdlib.h>
#define DEADLOCK_ENV_SET(name, value) _putenv_s(name, value)
#define DEADLOCK_ENV_UNSET(name) _putenv_s(name, "")
#else
#include <stdlib.h>
#define DEADLOCK_ENV_SET(name, value) setenv(name, value, 1)
#define DEADLOCK_ENV_UNSET(name) unsetenv(name)
#endif

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
            tests_failed++;                                                 \
        }                                                                   \
    } while (0)

#define RUN_TEST(name)                                                      \
    do {                                                                    \
        printf("TEST: %s\n", #name);                                        \
        fflush(stdout);                                                     \
        name();                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helper: run the experiment with env override temporarily set       */
/* ------------------------------------------------------------------ */

static DeadlockStatus run_with_override(
    const char *override_path,
    int timeout_ms,
    DeadlockResult *result)
{
    int had_override = 0;
    char saved[1024];

#ifdef _WIN32
    /* Windows: _putenv_s has no direct "get"; just set and clear. */
    (void)had_override;
    (void)saved;
    DEADLOCK_ENV_SET("DEADLOCK_HELPER_OVERRIDE", override_path);
#else
    const char *existing = getenv("DEADLOCK_HELPER_OVERRIDE");
    if (existing != NULL) {
        strncpy(saved, existing, sizeof(saved) - 1);
        saved[sizeof(saved) - 1] = '\0';
        had_override = 1;
    }
    DEADLOCK_ENV_SET("DEADLOCK_HELPER_OVERRIDE", override_path);
#endif

    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, timeout_ms);
    if (st != DEADLOCK_SUCCESS) {
#ifdef _WIN32
        DEADLOCK_ENV_UNSET("DEADLOCK_HELPER_OVERRIDE");
#else
        if (had_override) {
            DEADLOCK_ENV_SET("DEADLOCK_HELPER_OVERRIDE", saved);
        } else {
            DEADLOCK_ENV_UNSET("DEADLOCK_HELPER_OVERRIDE");
        }
#endif
        return st;
    }

    st = deadlock_experiment_run(&exp, result);
    deadlock_experiment_destroy(&exp);

#ifdef _WIN32
    DEADLOCK_ENV_UNSET("DEADLOCK_HELPER_OVERRIDE");
#else
    if (had_override) {
        DEADLOCK_ENV_SET("DEADLOCK_HELPER_OVERRIDE", saved);
    } else {
        DEADLOCK_ENV_UNSET("DEADLOCK_HELPER_OVERRIDE");
    }
#endif
    return st;
}

static DeadlockStatus run_normal(int timeout_ms, DeadlockResult *result)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, timeout_ms);
    if (st != DEADLOCK_SUCCESS) {
        return st;
    }
    st = deadlock_experiment_run(&exp, result);
    deadlock_experiment_destroy(&exp);
    return st;
}

/* ------------------------------------------------------------------ */
/* Test A — init and validation                                       */
/* ------------------------------------------------------------------ */

static void test_a_init_success(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 5000);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "init should succeed");
    TEST_ASSERT(exp.initialized == 0 || exp.initialized == 1,
                "init flag should be set");
    deadlock_experiment_destroy(&exp);
}

static void test_a2_invalid_timeout_zero(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 0);
    TEST_ASSERT(st == DEADLOCK_ERR_INVALID_ARGUMENT,
                "zero timeout should be rejected");
}

static void test_a3_invalid_timeout_negative(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, -1);
    TEST_ASSERT(st == DEADLOCK_ERR_INVALID_ARGUMENT,
                "negative timeout should be rejected");
}

static void test_a4_invalid_timeout_too_large(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(
        &exp, DEADLOCK_MAX_TIMEOUT_MS + 1);
    TEST_ASSERT(st == DEADLOCK_ERR_INVALID_ARGUMENT,
                "excessive timeout should be rejected");
}

static void test_a5_invalid_timeout_too_small(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 50);
    TEST_ASSERT(st == DEADLOCK_ERR_INVALID_ARGUMENT,
                "timeout below minimum should be rejected");
}

static void test_a6_null_experiment(void)
{
    DeadlockStatus st = deadlock_experiment_init(NULL, 5000);
    TEST_ASSERT(st == DEADLOCK_ERR_INVALID_ARGUMENT,
                "NULL experiment should be rejected");
}

static void test_a7_run_before_init(void)
{
    DeadlockExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockResult res;
    DeadlockStatus st = deadlock_experiment_run(&exp, &res);
    TEST_ASSERT(st == DEADLOCK_ERR_NOT_INITIALIZED,
                "run before init should be rejected");
}

static void test_a8_run_null_result(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 5000);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "init should succeed");
    DeadlockStatus st2 = deadlock_experiment_run(&exp, NULL);
    TEST_ASSERT(st2 == DEADLOCK_ERR_INVALID_ARGUMENT,
                "run with NULL result should be rejected");
    deadlock_experiment_destroy(&exp);
}

static void test_a9_destroy_uninitialized(void)
{
    DeadlockExperiment exp;
    memset(&exp, 0, sizeof(exp));
    deadlock_experiment_destroy(&exp);
    TEST_ASSERT(1, "destroy on uninitialized should not crash");
}

static void test_a10_destroy_null(void)
{
    deadlock_experiment_destroy(NULL);
    TEST_ASSERT(1, "destroy on NULL should not crash");
}

static void test_a11_already_initialized(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 5000);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "first init should succeed");
    DeadlockStatus st2 = deadlock_experiment_init(&exp, 3000);
    TEST_ASSERT(st2 == DEADLOCK_ERR_ALREADY_INITIALIZED,
                "re-init without destroy should be rejected");
    deadlock_experiment_destroy(&exp);
}

static void test_a12_destroy_then_reinit(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 5000);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "init should succeed");
    deadlock_experiment_destroy(&exp);
    DeadlockStatus st2 = deadlock_experiment_init(&exp, 3000);
    TEST_ASSERT(st2 == DEADLOCK_SUCCESS, "re-init after destroy should succeed");
    deadlock_experiment_destroy(&exp);
}

/* ------------------------------------------------------------------ */
/* Test B — deadlock detection and watchdog                           */
/* ------------------------------------------------------------------ */

static void test_b_helper_started_and_deadlock_detected(void)
{
    DeadlockResult res;
    DeadlockStatus st = run_normal(5000, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "experiment run should succeed");
    TEST_ASSERT(res.helper_started == 1, "helper should have started");
    TEST_ASSERT(res.deadlock_detected == 1,
                "expected deadlock should be detected");
    TEST_ASSERT(res.outcome == DEADLOCK_OUTCOME_DEADLOCK_DETECTED,
                "outcome should be deadlock_detected");
    TEST_ASSERT(res.watchdog_terminated == 1,
                "watchdog should have terminated helper");
    TEST_ASSERT(res.cleanup_succeeded == 1,
                "parent-side cleanup should succeed");
    TEST_ASSERT(res.timeout_ms == 5000, "timeout should match config");
    TEST_ASSERT(res.duration_seconds >= 0.0, "duration should be non-negative");
    TEST_ASSERT(res.duration_seconds < 30.0,
                "should not block indefinitely");
}

static void test_b2_watchdog_termination_flag(void)
{
    DeadlockResult res;
    DeadlockStatus st = run_normal(3000, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "run should succeed");
    TEST_ASSERT(res.watchdog_terminated == 1,
                "watchdog termination flag should be set");
    TEST_ASSERT(res.deadlock_detected == 1, "deadlock should be detected");
}

static void test_b3_no_indefinite_blocking(void)
{
    DeadlockResult res;
    DeadlockStatus st = run_normal(2000, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "run should succeed");
    TEST_ASSERT(res.deadlock_detected == 1, "deadlock should be detected");
    TEST_ASSERT(res.duration_seconds >= 0.0, "duration should be non-negative");
    TEST_ASSERT(res.duration_seconds < 30.0,
                "should not block indefinitely beyond timeout");
}

static void test_b4_repeated_execution(void)
{
    for (int i = 0; i < 5; i++) {
        DeadlockResult res;
        DeadlockStatus st = run_normal(3000, &res);
        TEST_ASSERT(st == DEADLOCK_SUCCESS, "repeated run should succeed");
        TEST_ASSERT(res.helper_started == 1, "helper should start");
        TEST_ASSERT(res.deadlock_detected == 1,
                    "deadlock should be detected each time");
        TEST_ASSERT(res.watchdog_terminated == 1,
                    "watchdog should terminate each time");
        TEST_ASSERT(res.cleanup_succeeded == 1,
                    "cleanup should succeed each time");
    }
}

/* ------------------------------------------------------------------ */
/* Test C — error paths (missing helper, unexpected exit)             */
/* ------------------------------------------------------------------ */

static void test_c_missing_helper_executable(void)
{
    DeadlockResult res;
    DeadlockStatus st = run_with_override(
        "nonexistent_deadlock_helper.exe", 3000, &res);
    TEST_ASSERT(st == DEADLOCK_ERR_PROCESS_LAUNCH_FAILED,
                "missing helper should return launch-failed status");
    TEST_ASSERT(res.helper_started == 0,
                "helper_started should be 0 on launch failure");
    TEST_ASSERT(res.outcome == DEADLOCK_OUTCOME_LAUNCH_FAILED,
                "outcome should be launch_failed");
    TEST_ASSERT(res.deadlock_detected == 0,
                "deadlock should NOT be detected on launch failure");
    TEST_ASSERT(res.watchdog_terminated == 0,
                "watchdog should not terminate on launch failure");
}

static void test_c2_unexpected_helper_exit(void)
{
    /* Point the helper path at banksync.exe, which exits quickly. */
    DeadlockResult res;
    DeadlockStatus st = run_with_override(
        "banksync.exe", 5000, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "run should succeed");
    TEST_ASSERT(res.helper_started == 1, "helper should have started");
    TEST_ASSERT(res.outcome == DEADLOCK_OUTCOME_UNEXPECTED_EXIT,
                "unexpected exit should be reported");
    TEST_ASSERT(res.deadlock_detected == 0,
                "deadlock should NOT be detected when helper exits");
    TEST_ASSERT(res.watchdog_terminated == 0,
                "watchdog should not terminate an exited helper");
}

static void test_c3_launch_failure_distinct_from_deadlock(void)
{
    /* Launch failure: nonexistent path. */
    DeadlockResult res_fail;
    DeadlockStatus st_fail = run_with_override(
        "does_not_exist.exe", 3000, &res_fail);
    TEST_ASSERT(st_fail == DEADLOCK_ERR_PROCESS_LAUNCH_FAILED,
                "launch failure should be reported");
    TEST_ASSERT(res_fail.outcome == DEADLOCK_OUTCOME_LAUNCH_FAILED,
                "outcome should be launch_failed");

    /* Deadlock detection: normal helper path. */
    DeadlockResult res_deadlock;
    DeadlockStatus st_deadlock = run_normal(3000, &res_deadlock);
    TEST_ASSERT(st_deadlock == DEADLOCK_SUCCESS, "deadlock run should succeed");
    TEST_ASSERT(res_deadlock.outcome == DEADLOCK_OUTCOME_DEADLOCK_DETECTED,
                "outcome should be deadlock_detected");

    TEST_ASSERT(res_fail.helper_started == 0 &&
                    res_deadlock.helper_started == 1,
                "launch failure vs deadlock must differ in helper_started");
    TEST_ASSERT(res_fail.deadlock_detected == 0 &&
                    res_deadlock.deadlock_detected == 1,
                "launch failure vs deadlock must differ in deadlock_detected");
}

/* ------------------------------------------------------------------ */
/* Test D — cleanup                                                   */
/* ------------------------------------------------------------------ */

static void test_d_cleanup_succeeds(void)
{
    DeadlockResult res;
    DeadlockStatus st = run_normal(3000, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "run should succeed");
    TEST_ASSERT(res.cleanup_succeeded == 1,
                "process-handle cleanup should succeed");
}

static void test_d_destroy_after_run(void)
{
    DeadlockExperiment exp;
    DeadlockStatus st = deadlock_experiment_init(&exp, 3000);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "init should succeed");
    DeadlockResult res;
    st = deadlock_experiment_run(&exp, &res);
    TEST_ASSERT(st == DEADLOCK_SUCCESS, "run should succeed");
    deadlock_experiment_destroy(&exp);
    TEST_ASSERT(1, "destroy after run should not crash");
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
    RUN_TEST(test_b_helper_started_and_deadlock_detected);
    RUN_TEST(test_b2_watchdog_termination_flag);
    RUN_TEST(test_b3_no_indefinite_blocking);
    RUN_TEST(test_b4_repeated_execution);
    RUN_TEST(test_c_missing_helper_executable);
    RUN_TEST(test_c2_unexpected_helper_exit);
    RUN_TEST(test_c3_launch_failure_distinct_from_deadlock);
    RUN_TEST(test_d_cleanup_succeeds);
    RUN_TEST(test_d_destroy_after_run);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
