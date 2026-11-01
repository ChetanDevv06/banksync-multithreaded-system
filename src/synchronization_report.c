#include "synchronization_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Portable time helpers                                              */
/* ------------------------------------------------------------------ */

static double report_now_seconds(void)
{
#ifdef _WIN32
    return (double)clock() / (double)CLOCKS_PER_SEC;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static int write_timestamp(FILE *f)
{
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    if (utc == NULL) {
        return -1;
    }
    char buf[64];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", utc) == 0) {
        return -1;
    }
    return fprintf(f, "%s", buf) >= 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Portable output-directory creation                                 */
/* ------------------------------------------------------------------ */

static int ensure_output_directory(const char *path)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            slash = p;
        }
    }
    if (slash == NULL || slash == path) {
        return 0;
    }

    size_t dir_len = (size_t)(slash - path);
    if (dir_len == 0 || dir_len >= 1024) {
        return -1;
    }
    char dir[1024];
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

#ifdef _WIN32
    if (CreateDirectoryA(dir, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
#else
    if (mkdir(dir, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Minimal JSON writing helpers                                       */
/* ------------------------------------------------------------------ */

static int json_write_string(FILE *f, const char *s)
{
    if (fputc('"', f) == EOF) {
        return -1;
    }
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':
            if (fputs("\\\"", f) == EOF) return -1;
            break;
        case '\\':
            if (fputs("\\\\", f) == EOF) return -1;
            break;
        case '\b':
            if (fputs("\\b", f) == EOF) return -1;
            break;
        case '\f':
            if (fputs("\\f", f) == EOF) return -1;
            break;
        case '\n':
            if (fputs("\\n", f) == EOF) return -1;
            break;
        case '\r':
            if (fputs("\\r", f) == EOF) return -1;
            break;
        case '\t':
            if (fputs("\\t", f) == EOF) return -1;
            break;
        default:
            if (c < 0x20) {
                if (fprintf(f, "\\u%04x", c) < 0) return -1;
            } else {
                if (fputc((int)c, f) == EOF) return -1;
            }
            break;
        }
    }
    if (fputc('"', f) == EOF) return -1;
    return 0;
}

static int json_write_int64(FILE *f, int64_t v)
{
    return fprintf(f, "%" PRId64, v) >= 0 ? 0 : -1;
}

static int json_write_int(FILE *f, int v)
{
    return fprintf(f, "%d", v) >= 0 ? 0 : -1;
}

static int json_write_double(FILE *f, double v)
{
    return fprintf(f, "%.9g", v) >= 0 ? 0 : -1;
}

static int json_write_bool(FILE *f, int v)
{
    return fputs(v ? "true" : "false", f) == EOF ? -1 : 0;
}

static const char *sub_status_string(SyncSubResultStatus s)
{
    switch (s) {
    case SYNC_SUB_SUCCESS: return "success";
    case SYNC_SUB_FAILURE: return "failure";
    case SYNC_SUB_NOT_RUN:
    default: return "not_run";
    }
}

static const char *overall_status_string(SyncOverallResult s)
{
    switch (s) {
    case SYNC_OVERALL_SUCCESS: return "success";
    case SYNC_OVERALL_FAILURE: return "failure";
    case SYNC_OVERALL_NOT_RUN:
    default: return "not_run";
    }
}

/* ------------------------------------------------------------------ */
/* Per-experiment runners                                             */
/* ------------------------------------------------------------------ */

static SyncReportStatus run_race_experiment(SynchronizationReport *report)
{
    SyncRaceResult *r = &report->race;
    r->status = SYNC_SUB_FAILURE;
    r->race_status = RACE_EXPERIMENT_SUCCESS;
    r->unsafe_expected_final_value = 0;
    r->unsafe_actual_final_value = 0;
    r->unsafe_lost_updates = 0;
    r->safe_expected_final_value = 0;
    r->safe_actual_final_value = 0;
    r->safe_correct = 0;
    r->observed_matches_expected = 0;
    r->duration_seconds = 0.0;

    int64_t initial_value = 0LL;
    int64_t num_workers = 2LL;
    int64_t increments_per_worker = 1LL;

    /* --- Unsafe mode --- */
    RaceExperiment exp;
    memset(&exp, 0, sizeof(exp));
    RaceExperimentStatus st = race_experiment_init(
        &exp, RACE_MODE_UNSAFE, initial_value, num_workers, increments_per_worker);
    if (st != RACE_EXPERIMENT_SUCCESS) {
        r->race_status = st;
        return SYNCHRONIZATION_REPORT_ERR_RACE_EXPERIMENT_FAILED;
    }

    RaceExperimentResult ures;
    st = race_experiment_run(&exp, &ures);
    r->race_status = st;
    if (st == RACE_EXPERIMENT_SUCCESS) {
        r->unsafe_expected_final_value = ures.expected_final_value;
        r->unsafe_actual_final_value = ures.actual_final_value;
        r->unsafe_lost_updates = ures.lost_updates;
        r->duration_seconds += ures.duration_seconds;
    }
    race_experiment_destroy(&exp);

    if (st != RACE_EXPERIMENT_SUCCESS) {
        return SYNCHRONIZATION_REPORT_ERR_RACE_EXPERIMENT_FAILED;
    }

    /* --- Safe mode --- */
    st = race_experiment_init(
        &exp, RACE_MODE_SAFE, initial_value, num_workers, increments_per_worker);
    if (st != RACE_EXPERIMENT_SUCCESS) {
        r->race_status = st;
        return SYNCHRONIZATION_REPORT_ERR_RACE_EXPERIMENT_FAILED;
    }

    RaceExperimentResult sres;
    st = race_experiment_run(&exp, &sres);
    r->race_status = st;
    if (st == RACE_EXPERIMENT_SUCCESS) {
        r->safe_expected_final_value = sres.expected_final_value;
        r->safe_actual_final_value = sres.actual_final_value;
        r->safe_correct = sres.correct;
        r->duration_seconds += sres.duration_seconds;
    }
    race_experiment_destroy(&exp);

    if (st != RACE_EXPERIMENT_SUCCESS) {
        return SYNCHRONIZATION_REPORT_ERR_RACE_EXPERIMENT_FAILED;
    }

    /* Validate intended OS behavior was demonstrated. */
    int unsafe_ok = (r->unsafe_lost_updates > 0) &&
                    (r->unsafe_actual_final_value != r->unsafe_expected_final_value);
    int safe_ok = (r->safe_correct == 1) &&
                  (r->safe_actual_final_value == r->safe_expected_final_value);
    r->observed_matches_expected = (unsafe_ok && safe_ok) ? 1 : 0;
    r->status = r->observed_matches_expected ? SYNC_SUB_SUCCESS : SYNC_SUB_FAILURE;

    return SYNCHRONIZATION_REPORT_SUCCESS;
}

static SyncReportStatus run_deadlock_experiment(SynchronizationReport *report)
{
    SyncDeadlockResult *r = &report->deadlock;
    r->status = SYNC_SUB_FAILURE;
    r->deadlock_status = DEADLOCK_SUCCESS;
    r->helper_started = 0;
    r->deadlock_detected = 0;
    r->watchdog_terminated = 0;
    r->cleanup_succeeded = 0;
    r->timeout_ms = SYNCHRONIZATION_REPORT_DEADLOCK_TIMEOUT_MS;
    r->exit_code = 0;
    r->error_code = 0;
    r->observed_matches_expected = 0;
    r->duration_seconds = 0.0;

    DeadlockExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockStatus st = deadlock_experiment_init(
        &exp, SYNCHRONIZATION_REPORT_DEADLOCK_TIMEOUT_MS);
    if (st != DEADLOCK_SUCCESS) {
        r->deadlock_status = st;
        return SYNCHRONIZATION_REPORT_ERR_DEADLOCK_EXPERIMENT_FAILED;
    }

    DeadlockResult dres;
    st = deadlock_experiment_run(&exp, &dres);
    r->deadlock_status = st;
    if (st == DEADLOCK_SUCCESS) {
        r->helper_started = dres.helper_started;
        r->deadlock_detected = dres.deadlock_detected;
        r->watchdog_terminated = dres.watchdog_terminated;
        r->cleanup_succeeded = dres.cleanup_succeeded;
        r->timeout_ms = dres.timeout_ms;
        r->exit_code = dres.exit_code;
        r->error_code = dres.error_code;
        r->duration_seconds = dres.duration_seconds;
    }
    deadlock_experiment_destroy(&exp);

    if (st == DEADLOCK_SUCCESS) {
        int ok = (r->helper_started == 1) &&
                 (r->deadlock_detected == 1) &&
                 (r->watchdog_terminated == 1) &&
                 (r->cleanup_succeeded == 1);
        r->observed_matches_expected = ok ? 1 : 0;
        r->status = ok ? SYNC_SUB_SUCCESS : SYNC_SUB_FAILURE;
    }

    return SYNCHRONIZATION_REPORT_SUCCESS;
}

static SyncReportStatus run_prevention_experiment(SynchronizationReport *report)
{
    SyncPreventionResult *r = &report->prevention;
    r->status = SYNC_SUB_FAILURE;
    r->prevention_status = DEADLOCK_PREVENTION_SUCCESS;
    r->operations_attempted = 0;
    r->operations_completed = 0;
    r->operations_failed = 0;
    r->both_operations_completed = 0;
    r->final_state_consistent = 0;
    r->observed_matches_expected = 0;
    r->duration_seconds = 0.0;

    DeadlockPreventionExperiment exp;
    memset(&exp, 0, sizeof(exp));
    DeadlockPreventionStatus st = deadlock_prevention_init(
        &exp, DEADLOCK_PREVENTION_DEFAULT_TIMEOUT_MS);
    if (st != DEADLOCK_PREVENTION_SUCCESS) {
        r->prevention_status = st;
        return SYNCHRONIZATION_REPORT_ERR_PREVENTION_EXPERIMENT_FAILED;
    }

    DeadlockPreventionResult pres;
    st = deadlock_prevention_run(&exp, &pres);
    r->prevention_status = st;
    if (st == DEADLOCK_PREVENTION_SUCCESS) {
        r->operations_attempted = pres.operations_attempted;
        r->operations_completed = pres.operations_completed;
        r->operations_failed = pres.operations_failed;
        r->both_operations_completed = pres.both_operations_completed;
        r->final_state_consistent = pres.final_state_consistent;
        r->duration_seconds = pres.duration_seconds;
    }
    deadlock_prevention_destroy(&exp);

    if (st == DEADLOCK_PREVENTION_SUCCESS) {
        int ok = (r->both_operations_completed == 1) &&
                 (r->final_state_consistent == 1) &&
                 (r->operations_completed > 0);
        r->observed_matches_expected = ok ? 1 : 0;
        r->status = ok ? SYNC_SUB_SUCCESS : SYNC_SUB_FAILURE;
    }

    return SYNCHRONIZATION_REPORT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

SyncReportStatus synchronization_report_init(SynchronizationReport *report)
{
    if (report == NULL) {
        return SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT;
    }
    if (report->initialized) {
        return SYNCHRONIZATION_REPORT_ERR_ALREADY_INITIALIZED;
    }

    memset(report, 0, sizeof(*report));
    report->version = SYNCHRONIZATION_REPORT_VERSION;
    report->overall_status = SYNC_OVERALL_NOT_RUN;
    report->total_experiments = 3;
    report->successful_experiments = 0;
    report->failed_experiments = 0;
    report->total_duration_seconds = 0.0;
    report->race.status = SYNC_SUB_NOT_RUN;
    report->deadlock.status = SYNC_SUB_NOT_RUN;
    report->prevention.status = SYNC_SUB_NOT_RUN;
    report->initialized = 1;
    return SYNCHRONIZATION_REPORT_SUCCESS;
}

SyncReportStatus synchronization_report_run(SynchronizationReport *report)
{
    if (report == NULL) {
        return SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT;
    }
    if (!report->initialized) {
        return SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED;
    }

    report->race.status = SYNC_SUB_NOT_RUN;
    report->deadlock.status = SYNC_SUB_NOT_RUN;
    report->prevention.status = SYNC_SUB_NOT_RUN;

    double total_start = report_now_seconds();

    /* Each sub-experiment runs independently. A failure in one does not
     * prevent the others from executing. */
    (void)run_race_experiment(report);
    (void)run_deadlock_experiment(report);
    (void)run_prevention_experiment(report);

    double total_end = report_now_seconds();
    report->total_duration_seconds = total_end - total_start;

    SyncOverallResult overall;
    int successful = 0;
    int failed = 0;
    SyncReportStatus st = synchronization_report_compute_overall(
        report, &overall, &successful, &failed);
    if (st != SYNCHRONIZATION_REPORT_SUCCESS) {
        return st;
    }

    report->overall_status = overall;
    report->successful_experiments = successful;
    report->failed_experiments = failed;
    return SYNCHRONIZATION_REPORT_SUCCESS;
}

SyncReportStatus synchronization_report_compute_overall(
    const SynchronizationReport *report,
    SyncOverallResult *out_overall,
    int *out_successful,
    int *out_failed)
{
    if (report == NULL) {
        return SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT;
    }
    if (!report->initialized) {
        return SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED;
    }

    int successful = 0;
    int failed = 0;

    if (report->race.status == SYNC_SUB_SUCCESS) {
        successful++;
    } else if (report->race.status == SYNC_SUB_FAILURE) {
        failed++;
    }

    if (report->deadlock.status == SYNC_SUB_SUCCESS) {
        successful++;
    } else if (report->deadlock.status == SYNC_SUB_FAILURE) {
        failed++;
    }

    if (report->prevention.status == SYNC_SUB_SUCCESS) {
        successful++;
    } else if (report->prevention.status == SYNC_SUB_FAILURE) {
        failed++;
    }

    SyncOverallResult overall =
        (failed == 0 && successful == 3) ? SYNC_OVERALL_SUCCESS : SYNC_OVERALL_FAILURE;

    if (out_overall != NULL) {
        *out_overall = overall;
    }
    if (out_successful != NULL) {
        *out_successful = successful;
    }
    if (out_failed != NULL) {
        *out_failed = failed;
    }
    return SYNCHRONIZATION_REPORT_SUCCESS;
}

SyncReportStatus synchronization_report_generate_json(
    const SynchronizationReport *report,
    const char *output_path)
{
    if (report == NULL || output_path == NULL || output_path[0] == '\0') {
        return SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT;
    }
    if (!report->initialized) {
        return SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED;
    }

    if (ensure_output_directory(output_path) != 0) {
        return SYNCHRONIZATION_REPORT_ERR_FILE_WRITE_FAILED;
    }

    FILE *f = fopen(output_path, "w");
    if (f == NULL) {
        return SYNCHRONIZATION_REPORT_ERR_FILE_WRITE_FAILED;
    }

    int ok = 1;

    ok &= fprintf(f, "{\n") >= 0;
    ok &= fprintf(f, "  \"schema_version\": ") >= 0;
    ok &= json_write_int64(f, report->version) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"timestamp\": ") >= 0;
    ok &= write_timestamp(f) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"overall_status\": ") >= 0;
    ok &= json_write_string(f, overall_status_string(report->overall_status)) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"total_experiments\": ") >= 0;
    ok &= json_write_int(f, report->total_experiments) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"successful_experiments\": ") >= 0;
    ok &= json_write_int(f, report->successful_experiments) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"failed_experiments\": ") >= 0;
    ok &= json_write_int(f, report->failed_experiments) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "  \"total_duration_seconds\": ") >= 0;
    ok &= json_write_double(f, report->total_duration_seconds) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    /* --- M4.1 --- */
    ok &= fprintf(f, "  \"experiments\": {\n") >= 0;

    ok &= fprintf(f, "    \"m4_1_race_condition\": {\n") >= 0;
    ok &= fprintf(f, "      \"status\": ") >= 0;
    ok &= json_write_string(f, sub_status_string(report->race.status)) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"race_status\": ") >= 0;
    ok &= json_write_int(f, (int)report->race.race_status) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"unsafe\": {\n") >= 0;
    ok &= fprintf(f, "        \"expected_final_value\": ") >= 0;
    ok &= json_write_int64(f, report->race.unsafe_expected_final_value) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"actual_final_value\": ") >= 0;
    ok &= json_write_int64(f, report->race.unsafe_actual_final_value) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"lost_updates\": ") >= 0;
    ok &= json_write_int64(f, report->race.unsafe_lost_updates) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"observed_matches_expected\": ") >= 0;
    /* Unsafe expected behavior: a lost update was observed. */
    ok &= json_write_bool(f, report->race.unsafe_lost_updates > 0) >= 0;
    ok &= fprintf(f, "\n      },\n") >= 0;

    ok &= fprintf(f, "      \"safe\": {\n") >= 0;
    ok &= fprintf(f, "        \"expected_final_value\": ") >= 0;
    ok &= json_write_int64(f, report->race.safe_expected_final_value) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"actual_final_value\": ") >= 0;
    ok &= json_write_int64(f, report->race.safe_actual_final_value) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"correct\": ") >= 0;
    ok &= json_write_bool(f, report->race.safe_correct) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "        \"observed_matches_expected\": ") >= 0;
    ok &= json_write_bool(f, report->race.safe_correct == 1) >= 0;
    ok &= fprintf(f, "\n      },\n") >= 0;

    ok &= fprintf(f, "      \"duration_seconds\": ") >= 0;
    ok &= json_write_double(f, report->race.duration_seconds) >= 0;
    ok &= fprintf(f, ",\n") >= 0;
    ok &= fprintf(f, "      \"observed_matches_expected\": ") >= 0;
    ok &= json_write_bool(f, report->race.observed_matches_expected) >= 0;
    ok &= fprintf(f, "\n    },\n") >= 0;

    /* --- M4.2 --- */
    ok &= fprintf(f, "    \"m4_2_deadlock\": {\n") >= 0;
    ok &= fprintf(f, "      \"status\": ") >= 0;
    ok &= json_write_string(f, sub_status_string(report->deadlock.status)) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"deadlock_status\": ") >= 0;
    ok &= json_write_int(f, (int)report->deadlock.deadlock_status) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"helper_started\": ") >= 0;
    ok &= json_write_bool(f, report->deadlock.helper_started) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"deadlock_detected\": ") >= 0;
    ok &= json_write_bool(f, report->deadlock.deadlock_detected) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"watchdog_terminated\": ") >= 0;
    ok &= json_write_bool(f, report->deadlock.watchdog_terminated) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"cleanup_succeeded\": ") >= 0;
    ok &= json_write_bool(f, report->deadlock.cleanup_succeeded) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"timeout_ms\": ") >= 0;
    ok &= json_write_int(f, report->deadlock.timeout_ms) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"exit_code\": ") >= 0;
    ok &= json_write_int(f, report->deadlock.exit_code) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"error_code\": ") >= 0;
    ok &= json_write_int(f, report->deadlock.error_code) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"duration_seconds\": ") >= 0;
    ok &= json_write_double(f, report->deadlock.duration_seconds) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"observed_matches_expected\": ") >= 0;
    ok &= json_write_bool(f, report->deadlock.observed_matches_expected) >= 0;
    ok &= fprintf(f, "\n    },\n") >= 0;

    /* --- M4.3 --- */
    ok &= fprintf(f, "    \"m4_3_deadlock_prevention\": {\n") >= 0;
    ok &= fprintf(f, "      \"status\": ") >= 0;
    ok &= json_write_string(f, sub_status_string(report->prevention.status)) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"prevention_status\": ") >= 0;
    ok &= json_write_int(f, (int)report->prevention.prevention_status) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"operations_attempted\": ") >= 0;
    ok &= json_write_int(f, report->prevention.operations_attempted) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"operations_completed\": ") >= 0;
    ok &= json_write_int(f, report->prevention.operations_completed) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"operations_failed\": ") >= 0;
    ok &= json_write_int(f, report->prevention.operations_failed) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"both_operations_completed\": ") >= 0;
    ok &= json_write_bool(f, report->prevention.both_operations_completed) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"final_state_consistent\": ") >= 0;
    ok &= json_write_bool(f, report->prevention.final_state_consistent) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"duration_seconds\": ") >= 0;
    ok &= json_write_double(f, report->prevention.duration_seconds) >= 0;
    ok &= fprintf(f, ",\n") >= 0;

    ok &= fprintf(f, "      \"observed_matches_expected\": ") >= 0;
    ok &= json_write_bool(f, report->prevention.observed_matches_expected) >= 0;
    ok &= fprintf(f, "\n    }\n") >= 0;

    ok &= fprintf(f, "  }\n") >= 0;
    ok &= fprintf(f, "}\n") >= 0;

    if (fclose(f) != 0) {
        return SYNCHRONIZATION_REPORT_ERR_FILE_WRITE_FAILED;
    }

    return ok ? SYNCHRONIZATION_REPORT_SUCCESS
              : SYNCHRONIZATION_REPORT_ERR_JSON_GENERATION_FAILED;
}

void synchronization_report_destroy(SynchronizationReport *report)
{
    if (report == NULL) {
        return;
    }
    if (!report->initialized) {
        return;
    }

    report->version = 0;
    report->overall_status = SYNC_OVERALL_NOT_RUN;
    report->total_experiments = 0;
    report->successful_experiments = 0;
    report->failed_experiments = 0;
    report->total_duration_seconds = 0.0;

    memset(&report->race, 0, sizeof(report->race));
    memset(&report->deadlock, 0, sizeof(report->deadlock));
    memset(&report->prevention, 0, sizeof(report->prevention));

    report->initialized = 0;
}
