#include "synchronization_report.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <stdlib.h>
#define SET_ENV(name, value) _putenv_s(name, value)
#define UNSET_ENV(name) _putenv_s(name, "")
#else
#include <stdlib.h>
#define SET_ENV(name, value) setenv(name, value, 1)
#define UNSET_ENV(name) unsetenv(name)
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
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len != NULL) {
        *out_len = rd;
    }
    return buf;
}

static int file_contains(const char *path, const char *needle)
{
    size_t len = 0;
    char *buf = read_file(path, &len);
    if (buf == NULL) {
        return 0;
    }
    int found = strstr(buf, needle) != NULL;
    free(buf);
    return found;
}

static int remove_file(const char *path)
{
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : -1;
#else
    return remove(path);
#endif
}

static int remove_dir(const char *path)
{
#ifdef _WIN32
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    return rmdir(path);
#endif
}

/* ------------------------------------------------------------------ */
/* Test 1 — Report initialization                                     */
/* ------------------------------------------------------------------ */

static void test_1_report_init_success(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");
    TEST_ASSERT(report.initialized == 1, "initialized flag should be set");
    TEST_ASSERT(report.version == SYNCHRONIZATION_REPORT_VERSION,
                "version should be 1");
    TEST_ASSERT(report.overall_status == SYNC_OVERALL_NOT_RUN,
                "overall should be not_run before run");
    TEST_ASSERT(report.total_experiments == 3, "total should be 3");
    TEST_ASSERT(report.successful_experiments == 0,
                "successful should be 0 before run");
    TEST_ASSERT(report.failed_experiments == 0, "failed should be 0 before run");
    TEST_ASSERT(report.race.status == SYNC_SUB_NOT_RUN,
                "race should be not_run before run");
    TEST_ASSERT(report.deadlock.status == SYNC_SUB_NOT_RUN,
                "deadlock should be not_run before run");
    TEST_ASSERT(report.prevention.status == SYNC_SUB_NOT_RUN,
                "prevention should be not_run before run");
    synchronization_report_destroy(&report);
    TEST_ASSERT(report.initialized == 0, "destroy should clear initialized");
}

static void test_1_init_null(void)
{
    SyncReportStatus st = synchronization_report_init(NULL);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT,
                "NULL report should be rejected");
}

static void test_1_already_initialized(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "first init should succeed");
    st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_ERR_ALREADY_INITIALIZED,
                "re-init without destroy should be rejected");
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 2 — Successful M4.1 result collection                         */
/* ------------------------------------------------------------------ */

static void test_2_m4_1_result_collection(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    TEST_ASSERT(report.race.status == SYNC_SUB_SUCCESS,
                "M4.1 should report success");
    TEST_ASSERT(report.race.race_status == RACE_EXPERIMENT_SUCCESS,
                "race experiment API should succeed");
    TEST_ASSERT(report.race.unsafe_expected_final_value == 2LL,
                "unsafe expected should be 2");
    TEST_ASSERT(report.race.unsafe_actual_final_value == 1LL,
                "unsafe deterministic demo should yield 1");
    TEST_ASSERT(report.race.unsafe_lost_updates == 1LL,
                "unsafe should lose exactly 1 update");
    TEST_ASSERT(report.race.unsafe_lost_updates ==
                    report.race.unsafe_expected_final_value -
                        report.race.unsafe_actual_final_value,
                "lost updates should equal expected minus actual");
    TEST_ASSERT(report.race.safe_expected_final_value == 2LL,
                "safe expected should be 2");
    TEST_ASSERT(report.race.safe_actual_final_value == 2LL,
                "safe should produce expected value");
    TEST_ASSERT(report.race.safe_correct == 1, "safe mode should be correct");
    TEST_ASSERT(report.race.observed_matches_expected == 1,
                "M4.1 observed should match expected");
    TEST_ASSERT(report.race.duration_seconds >= 0.0,
                "race duration should be non-negative");

    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 3 — Successful M4.2 result collection                         */
/* ------------------------------------------------------------------ */

static void test_3_m4_2_result_collection(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    TEST_ASSERT(report.deadlock.status == SYNC_SUB_SUCCESS,
                "M4.2 should report success");
    TEST_ASSERT(report.deadlock.deadlock_status == DEADLOCK_SUCCESS,
                "deadlock experiment API should succeed");
    TEST_ASSERT(report.deadlock.helper_started == 1,
                "helper process should have started");
    TEST_ASSERT(report.deadlock.deadlock_detected == 1,
                "expected deadlock should be detected");
    TEST_ASSERT(report.deadlock.watchdog_terminated == 1,
                "watchdog should have terminated helper");
    TEST_ASSERT(report.deadlock.cleanup_succeeded == 1,
                "process cleanup should succeed");
    TEST_ASSERT(report.deadlock.timeout_ms > 0, "timeout should be configured");
    TEST_ASSERT(report.deadlock.duration_seconds >= 0.0,
                "deadlock duration should be non-negative");
    TEST_ASSERT(report.deadlock.duration_seconds < 30.0,
                "should not block indefinitely");
    TEST_ASSERT(report.deadlock.observed_matches_expected == 1,
                "M4.2 observed should match expected");

    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 4 — Successful M4.3 result collection                         */
/* ------------------------------------------------------------------ */

static void test_4_m4_3_result_collection(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    TEST_ASSERT(report.prevention.status == SYNC_SUB_SUCCESS,
                "M4.3 should report success");
    TEST_ASSERT(report.prevention.prevention_status ==
                    DEADLOCK_PREVENTION_SUCCESS,
                "prevention experiment API should succeed");
    TEST_ASSERT(report.prevention.operations_attempted > 0,
                "operations should be attempted");
    TEST_ASSERT(report.prevention.operations_completed > 0,
                "operations should complete");
    TEST_ASSERT(report.prevention.operations_failed == 0,
                "no operations should fail");
    TEST_ASSERT(report.prevention.both_operations_completed == 1,
                "both opposing-direction operations should complete");
    TEST_ASSERT(report.prevention.final_state_consistent == 1,
                "final state should be consistent");
    TEST_ASSERT(report.prevention.observed_matches_expected == 1,
                "M4.3 observed should match expected");
    TEST_ASSERT(report.prevention.duration_seconds >= 0.0,
                "prevention duration should be non-negative");

    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 5 — Correct overall status                                    */
/* ------------------------------------------------------------------ */

static void test_5_correct_overall_status(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    TEST_ASSERT(report.overall_status == SYNC_OVERALL_SUCCESS,
                "overall should be success when all sub-experiments succeed");
    TEST_ASSERT(report.successful_experiments == 3,
                "all three experiments should be successful");
    TEST_ASSERT(report.failed_experiments == 0,
                "no experiments should have failed");
    TEST_ASSERT(report.total_experiments == 3, "total should be 3");

    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 6 — Individual experiment failure reporting                   */
/* ------------------------------------------------------------------ */

static void test_6_individual_failure_reporting(void)
{
    /* Point the M4.2 helper at a nonexistent path to force a launch
     * failure. deadlock_experiment.c reads DEADLOCK_HELPER_OVERRIDE. */
    SET_ENV("DEADLOCK_HELPER_OVERRIDE", "nonexistent_deadlock_helper.exe");

    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    TEST_ASSERT(report.race.status == SYNC_SUB_SUCCESS,
                "M4.1 should still succeed");
    TEST_ASSERT(report.deadlock.status == SYNC_SUB_FAILURE,
                "M4.2 should report failure on launch error");
    TEST_ASSERT(report.deadlock.helper_started == 0,
                "helper should not start on launch failure");
    TEST_ASSERT(report.deadlock.deadlock_detected == 0,
                "deadlock should not be detected on launch failure");
    TEST_ASSERT(report.prevention.status == SYNC_SUB_SUCCESS,
                "M4.3 should still succeed");

    TEST_ASSERT(report.overall_status == SYNC_OVERALL_FAILURE,
                "overall should be failure when one sub-experiment fails");
    TEST_ASSERT(report.successful_experiments == 2,
                "two experiments should be successful");
    TEST_ASSERT(report.failed_experiments == 1,
                "one experiment should have failed");

    UNSET_ENV("DEADLOCK_HELPER_OVERRIDE");
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 7 — Continuation after an independent experiment failure      */
/* ------------------------------------------------------------------ */

static void test_7_continuation_after_failure(void)
{
    SET_ENV("DEADLOCK_HELPER_OVERRIDE", "nonexistent_deadlock_helper.exe");

    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    /* M4.1 and M4.3 must have executed independently despite M4.2 failure. */
    TEST_ASSERT(report.race.status != SYNC_SUB_NOT_RUN,
                "M4.1 should have run even though M4.2 failed");
    TEST_ASSERT(report.race.status == SYNC_SUB_SUCCESS,
                "M4.1 should succeed");
    TEST_ASSERT(report.deadlock.status == SYNC_SUB_FAILURE,
                "M4.2 should fail (injected)");
    TEST_ASSERT(report.prevention.status != SYNC_SUB_NOT_RUN,
                "M4.3 should have run even though M4.2 failed");
    TEST_ASSERT(report.prevention.status == SYNC_SUB_SUCCESS,
                "M4.3 should succeed");

    TEST_ASSERT(report.overall_status == SYNC_OVERALL_FAILURE,
                "overall should be failure");
    TEST_ASSERT(report.successful_experiments == 2,
                "independent experiments should still count as successful");
    TEST_ASSERT(report.failed_experiments == 1, "one failure should count");

    UNSET_ENV("DEADLOCK_HELPER_OVERRIDE");
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 8 — Invalid result handling                                   */
/* ------------------------------------------------------------------ */

static void test_8_invalid_arguments(void)
{
    TEST_ASSERT(synchronization_report_run(NULL) ==
                    SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT,
                "run with NULL report should be rejected");
    TEST_ASSERT(synchronization_report_generate_json(NULL, "out.json") ==
                    SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT,
                "generate_json with NULL report should be rejected");
    TEST_ASSERT(synchronization_report_generate_json(NULL, NULL) ==
                    SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT,
                "generate_json with NULL path should be rejected");

    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    /* not initialized */
    TEST_ASSERT(synchronization_report_run(&report) ==
                    SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED,
                "run before init should be rejected");
    TEST_ASSERT(synchronization_report_generate_json(&report, "out.json") ==
                    SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED,
                "generate_json before init should be rejected");

    /* compute_overall guards */
    SyncOverallResult overall;
    int succ = 0, fail = 0;
    TEST_ASSERT(synchronization_report_compute_overall(NULL, &overall, &succ,
                                                       &fail) ==
                    SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT,
                "compute_overall with NULL report should be rejected");
    TEST_ASSERT(synchronization_report_compute_overall(&report, &overall,
                                                       &succ, &fail) ==
                    SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED,
                "compute_overall before init should be rejected");

    synchronization_report_destroy(&report); /* safe on uninitialized */
}

static void test_8_destroy_null_and_repeated(void)
{
    synchronization_report_destroy(NULL); /* should not crash */
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    synchronization_report_destroy(&report); /* safe on uninitialized */
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");
    synchronization_report_destroy(&report);
    synchronization_report_destroy(&report); /* double destroy safe */
    TEST_ASSERT(report.initialized == 0, "initialized should be cleared");
}

/* ------------------------------------------------------------------ */
/* Test 9 — JSON generation                                           */
/* ------------------------------------------------------------------ */

static void test_9_json_generation(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *path = "sync_report_test.json";
    remove_file(path); /* ensure clean start */
    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS,
                "JSON generation should succeed");

    TEST_ASSERT(file_contains(path, "{"), "JSON should open with brace");
    TEST_ASSERT(file_contains(path, "}"), "JSON should close with brace");
    TEST_ASSERT(file_contains(path, "\"schema_version\""),
                "JSON should contain schema_version");
    TEST_ASSERT(file_contains(path, "\"overall_status\""),
                "JSON should contain overall_status");
    TEST_ASSERT(file_contains(path, "\"total_experiments\""),
                "JSON should contain total_experiments");
    TEST_ASSERT(file_contains(path, "\"successful_experiments\""),
                "JSON should contain successful_experiments");
    TEST_ASSERT(file_contains(path, "\"failed_experiments\""),
                "JSON should contain failed_experiments");
    TEST_ASSERT(file_contains(path, "\"total_duration_seconds\""),
                "JSON should contain total_duration_seconds");
    TEST_ASSERT(file_contains(path, "\"experiments\""),
                "JSON should contain experiments");
    TEST_ASSERT(file_contains(path, "\"m4_1_race_condition\""),
                "JSON should contain m4_1_race_condition");
    TEST_ASSERT(file_contains(path, "\"m4_2_deadlock\""),
                "JSON should contain m4_2_deadlock");
    TEST_ASSERT(file_contains(path, "\"m4_3_deadlock_prevention\""),
                "JSON should contain m4_3_deadlock_prevention");

    remove_file(path);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 10 — JSON string escaping                                     */
/* ------------------------------------------------------------------ */

static void test_10_json_string_escaping(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *path = "sync_report_escape_test.json";
    remove_file(path);
    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "JSON generation should succeed");

    size_t len = 0;
    char *buf = read_file(path, &len);
    TEST_ASSERT(buf != NULL, "should be able to read generated JSON");
    if (buf != NULL) {
        /* Every string value should be wrapped in double quotes. */
        TEST_ASSERT(strstr(buf, "\"status\": \"success\"") != NULL ||
                        strstr(buf, "\"status\": \"failure\"") != NULL,
                    "status values should be quoted strings");
        TEST_ASSERT(strstr(buf, "\"overall_status\": \"success\"") != NULL,
                    "overall_status should be a quoted string");
        /* Keys should be quoted. */
        TEST_ASSERT(strstr(buf, "\"schema_version\"") != NULL,
                    "schema_version key should be quoted");
        /* No unescaped raw newline inside string values. */
        TEST_ASSERT(strstr(buf, "\"\n") == NULL ||
                        strstr(buf, "\":\n") != NULL,
                    "string values should not contain raw newlines");
        free(buf);
    }

    remove_file(path);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 11 — Correct numeric formatting                               */
/* ------------------------------------------------------------------ */

static void test_11_numeric_formatting(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *path = "sync_report_numeric_test.json";
    remove_file(path);
    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "JSON generation should succeed");

    size_t len = 0;
    char *buf = read_file(path, &len);
    TEST_ASSERT(buf != NULL, "should be able to read generated JSON");
    if (buf != NULL) {
        /* Integers should appear unquoted. */
        TEST_ASSERT(strstr(buf, "\"schema_version\": 1") != NULL,
                    "schema_version should be an unquoted integer");
        TEST_ASSERT(strstr(buf, "\"total_experiments\": 3") != NULL,
                    "total_experiments should be an unquoted integer");
        /* Durations should be numeric, not NaN/Infinity. */
        TEST_ASSERT(strstr(buf, "NaN") == NULL,
                    "JSON should not contain NaN");
        TEST_ASSERT(strstr(buf, "Infinity") == NULL,
                    "JSON should not contain Infinity");
        TEST_ASSERT(strstr(buf, "-Infinity") == NULL,
                    "JSON should not contain -Infinity");
        free(buf);
    }

    remove_file(path);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 12 — Missing output directory handling                        */
/* ------------------------------------------------------------------ */

static void test_12_missing_output_directory(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *dir = "sync_report_missing_dir";
    remove_dir(dir); /* ensure it does not exist */
    const char *path = "sync_report_missing_dir/report.json";
    remove_file(path);

    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS,
                "generation should create missing directory and succeed");
    TEST_ASSERT(file_contains(path, "\"schema_version\""),
                "generated file should contain report content");

    remove_file(path);
    remove_dir(dir);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 13 — File-writing errors                                      */
/* ------------------------------------------------------------------ */

static void test_13_file_write_error(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    /* Attempt to write into a path whose parent is a regular file. */
    const char *parent = "sync_report_file_parent.txt";
    FILE *fp = fopen(parent, "w");
    TEST_ASSERT(fp != NULL, "should be able to create parent file");
    if (fp != NULL) {
        fputs("x", fp);
        fclose(fp);
    }

    const char *path = "sync_report_file_parent.txt/report.json";
    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_ERR_FILE_WRITE_FAILED,
                "writing into a file path should report a file-write error");

    remove_file(parent);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 14 — Repeated report generation                               */
/* ------------------------------------------------------------------ */

static void test_14_repeated_report_generation(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));
    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *path = "sync_report_repeat_test.json";
    remove_file(path);

    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS,
                "first generation should succeed");
    TEST_ASSERT(file_contains(path, "\"schema_version\""),
                "first file should contain report content");

    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS,
                "second generation should succeed (overwrite)");
    TEST_ASSERT(file_contains(path, "\"schema_version\""),
                "second file should contain report content");

    remove_file(path);
    synchronization_report_destroy(&report);
}

/* ------------------------------------------------------------------ */
/* Test 15 — Cleanup and resource lifecycle                           */
/* ------------------------------------------------------------------ */

static void test_15_cleanup_and_lifecycle(void)
{
    SynchronizationReport report;
    memset(&report, 0, sizeof(report));

    /* Destroy before init is safe. */
    synchronization_report_destroy(&report);
    TEST_ASSERT(report.initialized == 0, "should not be initialized");

    SyncReportStatus st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "init should succeed");
    TEST_ASSERT(report.initialized == 1, "should be initialized after init");

    st = synchronization_report_run(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "run should succeed");

    const char *path = "sync_report_lifecycle_test.json";
    remove_file(path);
    st = synchronization_report_generate_json(&report, path);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "generation should succeed");
    remove_file(path);

    synchronization_report_destroy(&report);
    TEST_ASSERT(report.initialized == 0, "destroy should clear initialized");

    /* Use-after-destroy: init again works. */
    st = synchronization_report_init(&report);
    TEST_ASSERT(st == SYNCHRONIZATION_REPORT_SUCCESS, "re-init after destroy should succeed");
    TEST_ASSERT(report.initialized == 1, "should be initialized again");
    synchronization_report_destroy(&report);
    TEST_ASSERT(report.initialized == 0, "should be cleared again");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Test 1 — initialization */
    RUN_TEST(test_1_report_init_success);
    RUN_TEST(test_1_init_null);
    RUN_TEST(test_1_already_initialized);

    /* Tests 2–5 — happy-path experiment collection */
    RUN_TEST(test_2_m4_1_result_collection);
    RUN_TEST(test_3_m4_2_result_collection);
    RUN_TEST(test_4_m4_3_result_collection);
    RUN_TEST(test_5_correct_overall_status);

    /* Tests 6–7 — failure reporting and continuation */
    RUN_TEST(test_6_individual_failure_reporting);
    RUN_TEST(test_7_continuation_after_failure);

    /* Test 8 — invalid result handling */
    RUN_TEST(test_8_invalid_arguments);
    RUN_TEST(test_8_destroy_null_and_repeated);

    /* Tests 9–11 — JSON generation, escaping, numerics */
    RUN_TEST(test_9_json_generation);
    RUN_TEST(test_10_json_string_escaping);
    RUN_TEST(test_11_numeric_formatting);

    /* Tests 12–13 — output directory and file errors */
    RUN_TEST(test_12_missing_output_directory);
    RUN_TEST(test_13_file_write_error);

    /* Tests 14–15 — repeated generation and lifecycle */
    RUN_TEST(test_14_repeated_report_generation);
    RUN_TEST(test_15_cleanup_and_lifecycle);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
