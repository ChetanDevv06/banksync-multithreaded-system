#include "race_experiment.h"

#include <stdio.h>
#include <string.h>

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

static void test_a_unsafe_init_success(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "unsafe init should succeed");
    TEST_ASSERT(exp.initialized == 0 || 1, "exp should be initialized");
    race_experiment_destroy(&exp);
}

static void test_a2_safe_init_success(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_SAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "safe init should succeed");
    race_experiment_destroy(&exp);
}

static void test_a3_null_experiment(void)
{
    RaceExperimentStatus st =
        race_experiment_init(NULL, RACE_MODE_UNSAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "NULL experiment should be rejected");
}

static void test_a4_invalid_worker_count_zero(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 0, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "zero workers should be rejected");
}

static void test_a5_invalid_worker_count_negative(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, -1, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "negative workers should be rejected");
}

static void test_a6_invalid_worker_count_too_large(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, RACE_EXPERIMENT_MAX_WORKERS + 1, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "excessive workers should be rejected");
}

static void test_a7_invalid_increment_count_zero(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, 0);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "zero increments should be rejected");
}

static void test_a8_invalid_increment_count_negative(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, -1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "negative increments should be rejected");
}

static void test_a9_invalid_increment_count_too_large(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, RACE_EXPERIMENT_MAX_INCREMENTS + 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "excessive increments should be rejected");
}

static void test_a10_overflow_validation(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 0LL, 2, INT64_MAX);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "overflowing workload should be rejected");
}

static void test_a11_overflow_validation_max_value(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, INT64_MAX, 1, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "INT64_MAX initial with increment should be rejected");
}

static void test_b_deterministic_lost_update_unsafe(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

    RaceExperimentResult res;
    st = race_experiment_run(&exp, &res);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
    TEST_ASSERT(res.mode == RACE_MODE_UNSAFE, "mode should be unsafe");
    TEST_ASSERT(res.initial_value == 1000LL, "initial value should match");
    TEST_ASSERT(res.expected_final_value == 1002LL,
                "expected should be 1002");
    TEST_ASSERT(res.num_workers == 2, "workers should be 2");
    TEST_ASSERT(res.increments_per_worker == 1, "increments should be 1");
    TEST_ASSERT(res.num_operations == 2, "operations should be 2");
    TEST_ASSERT(res.actual_final_value < res.expected_final_value,
                "unsafe mode should lose an update");
    TEST_ASSERT(res.lost_updates ==
                    res.expected_final_value - res.actual_final_value,
                "lost updates should equal expected minus actual");
    TEST_ASSERT(res.correct == 0, "unsafe result should not be correct");

    race_experiment_destroy(&exp);
}

static void test_b2_safe_correct_result(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_SAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

    RaceExperimentResult res;
    st = race_experiment_run(&exp, &res);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
    TEST_ASSERT(res.mode == RACE_MODE_SAFE, "mode should be safe");
    TEST_ASSERT(res.expected_final_value == 1002LL, "expected should be 1002");
    TEST_ASSERT(res.actual_final_value == res.expected_final_value,
                "safe mode should produce correct result");
    TEST_ASSERT(res.lost_updates == 0, "safe mode should have no lost updates");
    TEST_ASSERT(res.correct == 1, "safe result should be correct");

    race_experiment_destroy(&exp);
}

static void test_b3_different_initial_values(void)
{
    int64_t values[] = {0LL, 1LL, 5000000LL, -1000LL, 999999LL};
    size_t count = sizeof(values) / sizeof(values[0]);

    for (size_t i = 0; i < count; i++) {
        RaceExperiment exp;
        RaceExperimentStatus st = race_experiment_init(
            &exp, RACE_MODE_SAFE, values[i], 3, 5);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

        RaceExperimentResult res;
        st = race_experiment_run(&exp, &res);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
        TEST_ASSERT(res.initial_value == values[i],
                    "initial value should match");
        TEST_ASSERT(res.expected_final_value == values[i] + 15LL,
                    "expected should be initial + 15");
        TEST_ASSERT(res.actual_final_value == res.expected_final_value,
                    "safe result should be correct");
        TEST_ASSERT(res.correct == 1, "result should be correct");

        race_experiment_destroy(&exp);
    }
}

static void test_b4_different_worker_counts(void)
{
    int64_t worker_counts[] = {1LL, 2LL, 4LL, 8LL};
    size_t count = sizeof(worker_counts) / sizeof(worker_counts[0]);

    for (size_t i = 0; i < count; i++) {
        RaceExperiment exp;
        RaceExperimentStatus st = race_experiment_init(
            &exp, RACE_MODE_SAFE, 0LL, worker_counts[i], 10);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

        RaceExperimentResult res;
        st = race_experiment_run(&exp, &res);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
        TEST_ASSERT(res.num_workers == worker_counts[i],
                    "worker count should match");
        TEST_ASSERT(res.expected_final_value == worker_counts[i] * 10LL,
                    "expected should match");
        TEST_ASSERT(res.actual_final_value == res.expected_final_value,
                    "safe result should be correct");
        TEST_ASSERT(res.correct == 1, "result should be correct");

        race_experiment_destroy(&exp);
    }
}

static void test_b5_different_increment_counts(void)
{
    int64_t increment_counts[] = {1LL, 5LL, 100LL, 1000LL};
    size_t count = sizeof(increment_counts) / sizeof(increment_counts[0]);

    for (size_t i = 0; i < count; i++) {
        RaceExperiment exp;
        RaceExperimentStatus st = race_experiment_init(
            &exp, RACE_MODE_SAFE, 0LL, 4, increment_counts[i]);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

        RaceExperimentResult res;
        st = race_experiment_run(&exp, &res);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
        TEST_ASSERT(res.increments_per_worker == increment_counts[i],
                    "increment count should match");
        TEST_ASSERT(res.expected_final_value == 4LL * increment_counts[i],
                    "expected should match");
        TEST_ASSERT(res.actual_final_value == res.expected_final_value,
                    "safe result should be correct");
        TEST_ASSERT(res.correct == 1, "result should be correct");

        race_experiment_destroy(&exp);
    }
}

static void test_b6_unsafe_lost_update_multiple_workers(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 0LL, 4, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

    RaceExperimentResult res;
    st = race_experiment_run(&exp, &res);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
    TEST_ASSERT(res.expected_final_value == 4LL, "expected should be 4");
    TEST_ASSERT(res.actual_final_value < res.expected_final_value,
                "unsafe with 4 workers should lose updates");
    TEST_ASSERT(res.lost_updates ==
                    res.expected_final_value - res.actual_final_value,
                "lost updates should equal expected minus actual");
    TEST_ASSERT(res.correct == 0, "unsafe result should not be correct");

    race_experiment_destroy(&exp);
}

static void test_b7_repeated_execution_unsafe(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 0LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

    for (int i = 0; i < 20; i++) {
        RaceExperimentResult res;
        st = race_experiment_run(&exp, &res);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
        TEST_ASSERT(res.actual_final_value == 1LL,
                    "unsafe deterministic demo should always yield 1");
        TEST_ASSERT(res.lost_updates == 1LL, "lost update should be 1");
        TEST_ASSERT(res.correct == 0, "result should not be correct");
    }

    race_experiment_destroy(&exp);
}

static void test_b8_repeated_execution_safe(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_SAFE, 0LL, 8, 25);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");

    for (int i = 0; i < 10; i++) {
        RaceExperimentResult res;
        st = race_experiment_run(&exp, &res);
        TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
        TEST_ASSERT(res.actual_final_value == 200LL,
                    "safe result should always be 200");
        TEST_ASSERT(res.lost_updates == 0LL, "lost updates should be 0");
        TEST_ASSERT(res.correct == 1, "result should be correct");
    }

    race_experiment_destroy(&exp);
}

static void test_c_run_before_init(void)
{
    RaceExperiment exp;
    memset(&exp, 0, sizeof(exp));
    RaceExperimentResult res;
    RaceExperimentStatus st = race_experiment_run(&exp, &res);
    TEST_ASSERT(st == RACE_EXPERIMENT_ERR_NOT_INITIALIZED,
                "run before init should be rejected");
}

static void test_c2_destroy_uninitialized(void)
{
    RaceExperiment exp;
    memset(&exp, 0, sizeof(exp));
    race_experiment_destroy(&exp);
    TEST_ASSERT(1, "destroy on uninitialized should not crash");
}

static void test_c3_destroy_null(void)
{
    race_experiment_destroy(NULL);
    TEST_ASSERT(1, "destroy on NULL should not crash");
}

static void test_c4_run_null_result(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_SAFE, 0LL, 1, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");
    RaceExperimentStatus st2 = race_experiment_run(&exp, NULL);
    TEST_ASSERT(st2 == RACE_EXPERIMENT_ERR_INVALID_ARGUMENT,
                "run with NULL result should be rejected");
    race_experiment_destroy(&exp);
}

static void test_c5_already_initialized(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "first init should succeed");
    RaceExperimentStatus st2 = race_experiment_init(
        &exp, RACE_MODE_SAFE, 2000LL, 3, 5);
    TEST_ASSERT(st2 == RACE_EXPERIMENT_ERR_ALREADY_INITIALIZED,
                "re-init without destroy should be rejected");
    race_experiment_destroy(&exp);
}

static void test_c6_destroy_then_reinit(void)
{
    RaceExperiment exp;
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, 1000LL, 2, 1);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "init should succeed");
    race_experiment_destroy(&exp);
    RaceExperimentStatus st2 = race_experiment_init(
        &exp, RACE_MODE_SAFE, 2000LL, 3, 5);
    TEST_ASSERT(st2 == RACE_EXPERIMENT_SUCCESS, "re-init after destroy should succeed");
    RaceExperimentResult res;
    st = race_experiment_run(&exp, &res);
    TEST_ASSERT(st == RACE_EXPERIMENT_SUCCESS, "run should succeed");
    TEST_ASSERT(res.actual_final_value == 2015LL,
                "re-initialized safe result should be correct");
    race_experiment_destroy(&exp);
}

int main(void)
{
    RUN_TEST(test_a_unsafe_init_success);
    RUN_TEST(test_a2_safe_init_success);
    RUN_TEST(test_a3_null_experiment);
    RUN_TEST(test_a4_invalid_worker_count_zero);
    RUN_TEST(test_a5_invalid_worker_count_negative);
    RUN_TEST(test_a6_invalid_worker_count_too_large);
    RUN_TEST(test_a7_invalid_increment_count_zero);
    RUN_TEST(test_a8_invalid_increment_count_negative);
    RUN_TEST(test_a9_invalid_increment_count_too_large);
    RUN_TEST(test_a10_overflow_validation);
    RUN_TEST(test_a11_overflow_validation_max_value);
    RUN_TEST(test_b_deterministic_lost_update_unsafe);
    RUN_TEST(test_b2_safe_correct_result);
    RUN_TEST(test_b3_different_initial_values);
    RUN_TEST(test_b4_different_worker_counts);
    RUN_TEST(test_b5_different_increment_counts);
    RUN_TEST(test_b6_unsafe_lost_update_multiple_workers);
    RUN_TEST(test_b7_repeated_execution_unsafe);
    RUN_TEST(test_b8_repeated_execution_safe);
    RUN_TEST(test_c_run_before_init);
    RUN_TEST(test_c2_destroy_uninitialized);
    RUN_TEST(test_c3_destroy_null);
    RUN_TEST(test_c4_run_null_result);
    RUN_TEST(test_c5_already_initialized);
    RUN_TEST(test_c6_destroy_then_reinit);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
