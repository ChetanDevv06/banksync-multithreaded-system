#ifndef SYNCHRONIZATION_REPORT_H
#define SYNCHRONIZATION_REPORT_H

#include <stdint.h>
#include <stddef.h>

#include "race_experiment.h"
#include "deadlock_experiment.h"
#include "deadlock_prevention.h"

#define SYNCHRONIZATION_REPORT_VERSION 1
#define SYNCHRONIZATION_REPORT_MAX_OUTPUT_PATH 1024

/* Use the documented default timeout for the M4.2 deadlock watchdog. */
#define SYNCHRONIZATION_REPORT_DEADLOCK_TIMEOUT_MS DEADLOCK_DEFAULT_TIMEOUT_MS

typedef enum SyncReportStatus {
    SYNCHRONIZATION_REPORT_SUCCESS = 0,
    SYNCHRONIZATION_REPORT_ERR_INVALID_ARGUMENT = -1,
    SYNCHRONIZATION_REPORT_ERR_NOT_INITIALIZED = -2,
    SYNCHRONIZATION_REPORT_ERR_ALREADY_INITIALIZED = -3,
    SYNCHRONIZATION_REPORT_ERR_ALLOC_FAILED = -4,
    SYNCHRONIZATION_REPORT_ERR_RACE_EXPERIMENT_FAILED = -5,
    SYNCHRONIZATION_REPORT_ERR_DEADLOCK_EXPERIMENT_FAILED = -6,
    SYNCHRONIZATION_REPORT_ERR_PREVENTION_EXPERIMENT_FAILED = -7,
    SYNCHRONIZATION_REPORT_ERR_JSON_GENERATION_FAILED = -8,
    SYNCHRONIZATION_REPORT_ERR_FILE_WRITE_FAILED = -9,
} SyncReportStatus;

typedef enum SyncOverallResult {
    SYNC_OVERALL_NOT_RUN = 0,
    SYNC_OVERALL_SUCCESS = 1,
    SYNC_OVERALL_FAILURE = 2,
} SyncOverallResult;

typedef enum SyncSubResultStatus {
    SYNC_SUB_NOT_RUN = 0,
    SYNC_SUB_SUCCESS = 1,
    SYNC_SUB_FAILURE = 2,
} SyncSubResultStatus;

/* M4.1 — Race condition result summary */
typedef struct SyncRaceResult {
    SyncSubResultStatus status;
    RaceExperimentStatus race_status;
    int64_t unsafe_expected_final_value;
    int64_t unsafe_actual_final_value;
    int64_t unsafe_lost_updates;
    int64_t safe_expected_final_value;
    int64_t safe_actual_final_value;
    int safe_correct;
    int observed_matches_expected;
    double duration_seconds;
} SyncRaceResult;

/* M4.2 — Deadlock result summary */
typedef struct SyncDeadlockResult {
    SyncSubResultStatus status;
    DeadlockStatus deadlock_status;
    int helper_started;
    int deadlock_detected;
    int watchdog_terminated;
    int cleanup_succeeded;
    int timeout_ms;
    int exit_code;
    int error_code;
    int observed_matches_expected;
    double duration_seconds;
} SyncDeadlockResult;

/* M4.3 — Deadlock prevention result summary */
typedef struct SyncPreventionResult {
    SyncSubResultStatus status;
    DeadlockPreventionStatus prevention_status;
    int operations_attempted;
    int operations_completed;
    int operations_failed;
    int both_operations_completed;
    int final_state_consistent;
    int observed_matches_expected;
    double duration_seconds;
} SyncPreventionResult;

/* Consolidated synchronization experiment report */
typedef struct SynchronizationReport {
    int version;
    SyncOverallResult overall_status;
    int total_experiments;
    int successful_experiments;
    int failed_experiments;
    double total_duration_seconds;
    int initialized;

    SyncRaceResult race;
    SyncDeadlockResult deadlock;
    SyncPreventionResult prevention;
} SynchronizationReport;

/* Initialize the report structure to a clean, not-run state. */
SyncReportStatus synchronization_report_init(SynchronizationReport *report);

/*
 * Execute M4.1, M4.2 and M4.3 in sequence, collect results, validate
 * expected outcomes and compute the overall result. A failure in one
 * experiment does not prevent subsequent independent experiments from
 * running. The intentional deadlock is never executed in the parent
 * process; it runs inside the M4.2 helper process only.
 */
SyncReportStatus synchronization_report_run(SynchronizationReport *report);

/*
 * Re-derive the overall status and pass/fail counts from the per-experiment
 * sub-results. Useful for testing failure/continuation logic without
 * re-executing the experiments.
 */
SyncReportStatus synchronization_report_compute_overall(
    const SynchronizationReport *report,
    SyncOverallResult *out_overall,
    int *out_successful,
    int *out_failed);

/* Generate a structured JSON report at output_path. Creates the output
 * directory if needed. Reports file-creation/write errors. */
SyncReportStatus synchronization_report_generate_json(
    const SynchronizationReport *report,
    const char *output_path);

/* Release any resources held by the report. Safe to call on NULL or
 * already-destroyed reports. */
void synchronization_report_destroy(SynchronizationReport *report);

#endif
