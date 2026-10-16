#include "ipbrs/backup.hpp"

#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cerr << "Usage:\n"
              << "  ipbrs-backup backup --source PATH --backup PATH "
                 "[--policy sequential|static|dynamic|stealing|adaptive] [--threads N] [--chunk N] [--min-chunk N] [--max-chunk N]\n"
              << "  ipbrs-backup recover --source PATH --backup PATH --catalog PATH "
                 "[--full] [--policy sequential|static|dynamic|stealing|adaptive] [--threads N] [--chunk N] [--min-chunk N] [--max-chunk N]\n"
              << "  ipbrs-backup verify --backup PATH --catalog PATH "
                 "[--policy sequential|static|dynamic|stealing|adaptive] [--threads N] [--chunk N]\n";
}

std::size_t parse_size(const std::string& value, const char* name) {
    if (value.empty() || value[0] == '-')
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    char* end = 0;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || errno == ERANGE || parsed == 0 ||
        parsed > (std::numeric_limits<std::size_t>::max)())
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    return static_cast<std::size_t>(parsed);
}

std::string json_escape(const std::string& input) {
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(ch)); }
        else if (ch < 0x20) out.push_back(' ');
        else out.push_back(static_cast<char>(ch));
    }
    return out;
}

void print_build_metadata() {
    std::cout << "  \"build\": {\"compiler\": \"";
#if defined(_MSC_VER)
    std::cout << "MSVC\", \"compiler_version\": \"" << _MSC_VER << "\"";
#elif defined(__clang__)
    std::cout << "Clang\", \"compiler_version\": \"" << __clang_version__ << "\"";
#elif defined(__GNUC__)
    std::cout << "GCC\", \"compiler_version\": \"" << __VERSION__ << "\"";
#else
    std::cout << "unknown\", \"compiler_version\": null";
#endif
#if defined(_OPENMP)
    std::cout << ", \"openmp_version\": " << _OPENMP;
#else
    std::cout << ", \"openmp_version\": null";
#endif
    std::cout << "},\n";
}

void parse_policy(const std::string& value, ipbrs::SchedulerConfig& scheduler) {
    if (value == "sequential") scheduler.policy = ipbrs::Policy::Sequential;
    else if (value == "static") scheduler.policy = ipbrs::Policy::StaticCostBased;
    else if (value == "dynamic") scheduler.policy = ipbrs::Policy::DynamicFixedChunk;
    else if (value == "stealing") scheduler.policy = ipbrs::Policy::CostAwareWorkStealing;
    else if (value == "adaptive") scheduler.policy = ipbrs::Policy::CostAwareAdaptiveWorkStealing;
    else throw std::invalid_argument("policy must be sequential, static, dynamic, stealing, or adaptive");
}

void print_adaptive_metrics(const ipbrs::SchedulerResult& result) {
    std::cout << "  \"estimated_cost_cv\": " << result.estimated_cost_cv << ",\n"
              << "  \"initial_adaptive_chunk\": " << result.initial_adaptive_chunk << ",\n"
              << "  \"granularity_decisions\": [";
    for (std::size_t i = 0; i < result.granularity_decisions.size(); ++i) {
        const ipbrs::GranularityDecision& decision = result.granularity_decisions[i];
        if (i) std::cout << ",";
        std::cout << "\n    {\"worker\": " << decision.worker_id
                  << ", \"chunk_size\": " << decision.chunk_size
                  << ", \"unfinished_tasks\": " << decision.unfinished_tasks
                  << ", \"stealing\": " << (decision.stealing ? "true" : "false") << "}";
    }
    if (!result.granularity_decisions.empty()) std::cout << "\n  ";
    std::cout << "],\n";
}

void print_scheduler_tasks(const ipbrs::SchedulerResult& result) {
    std::cout << "  \"tasks\": [";
    for (std::size_t i = 0; i < result.tasks.size(); ++i) {
        const ipbrs::TaskResult& task = result.tasks[i];
        if (i) std::cout << ",";
        std::cout << "\n    {\"task_id\": \"" << json_escape(task.task_id)
                  << "\", \"worker\": " << task.worker_id
                  << ", \"success\": " << (task.success ? "true" : "false")
                  << ", \"bytes\": " << task.bytes_processed
                  << ", \"duration_ns\": " << task.duration_ns
                  << ", \"error_code\": \"" << json_escape(task.error_code)
                  << "\", \"error\": \"" << json_escape(task.error_message) << "\"}";
    }
    if (!result.tasks.empty()) std::cout << "\n  ";
    std::cout << "]\n";
}

void print_worker_metrics(const ipbrs::SchedulerResult& result) {
    std::cout << "  \"workers\": [";
    for (std::size_t i = 0; i < result.workers.size(); ++i) {
        const ipbrs::WorkerMetrics& worker = result.workers[i];
        if (i) std::cout << ",";
        std::cout << "\n    {\"worker\": " << i << ", \"tasks_completed\": " << worker.tasks_completed
                  << ", \"tasks_failed\": " << worker.tasks_failed << ", \"bytes_processed\": "
                  << worker.bytes_processed << ", \"busy_ns\": " << worker.busy_ns
                  << ", \"steal_attempts\": " << worker.steal_attempts
                  << ", \"steals_succeeded\": " << worker.steals_succeeded << "}";
    }
    if (!result.workers.empty()) std::cout << "\n  ";
    std::cout << "],\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string operation(argv[1]);
    const bool is_backup = operation == "backup";
    const bool is_recovery = operation == "recover";
    const bool is_verify = operation == "verify";
    if (!is_backup && !is_recovery && !is_verify) { usage(); return 2; }

    ipbrs::BackupConfig backup_config;
    ipbrs::RecoveryConfig recovery_config;
    ipbrs::SchedulerConfig defaults;
    defaults.policy = ipbrs::Policy::Sequential;
    defaults.worker_count = 1;
    defaults.chunk_size = 1;
    backup_config.scheduler = defaults;
    recovery_config.scheduler = defaults;
    bool has_source = false, has_backup = false, has_catalog = false;

    try {
        for (int i = 2; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--full") {
                if (!is_recovery) throw std::invalid_argument("--full is only valid for recovery");
                recovery_config.full_recovery = true;
                continue;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + option);
            const std::string value(argv[++i]);
            if (option == "--source") {
                backup_config.source_root_utf8 = value;
                recovery_config.source_root_utf8 = value;
                has_source = true;
            } else if (option == "--backup") {
                backup_config.backup_root_utf8 = value;
                recovery_config.backup_root_utf8 = value;
                has_backup = true;
            } else if (option == "--catalog" && (is_recovery || is_verify)) {
                recovery_config.catalog_path_utf8 = value;
                has_catalog = true;
            } else if (option == "--threads") {
                const std::size_t count = parse_size(value, "threads");
                backup_config.scheduler.worker_count = count;
                recovery_config.scheduler.worker_count = count;
            } else if (option == "--chunk") {
                const std::size_t count = parse_size(value, "chunk");
                backup_config.scheduler.chunk_size = count;
                recovery_config.scheduler.chunk_size = count;
            } else if (option == "--min-chunk") {
                const std::size_t count = parse_size(value, "min-chunk");
                backup_config.scheduler.min_chunk_size = count;
                recovery_config.scheduler.min_chunk_size = count;
            } else if (option == "--max-chunk") {
                const std::size_t count = parse_size(value, "max-chunk");
                backup_config.scheduler.max_chunk_size = count;
                recovery_config.scheduler.max_chunk_size = count;
            } else if (option == "--policy") {
                parse_policy(value, backup_config.scheduler);
                parse_policy(value, recovery_config.scheduler);
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }
        if ((!is_verify && !has_source) || !has_backup || ((is_recovery || is_verify) && !has_catalog)) {
            usage();
            return 2;
        }

        if (is_verify) {
            const ipbrs::VerificationSummary summary =
                ipbrs::verify_backup(recovery_config.backup_root_utf8,
                                     recovery_config.catalog_path_utf8,
                                     recovery_config.scheduler);
            std::cout << "{\n";
            print_build_metadata();
            std::cout << "  \"operation\": \"verify\",\n"
                      << "  \"success\": " << (summary.success ? "true" : "false") << ",\n"
                      << "  \"policy\": \"" << ipbrs::policy_name(summary.scheduling.policy) << "\",\n"
                      << "  \"backup_id\": \"" << json_escape(summary.backup_id) << "\",\n"
                      << "  \"catalog_files\": " << summary.catalog_files << ",\n"
                      << "  \"files_verified\": " << summary.files_verified << ",\n"
                      << "  \"files_failed\": " << summary.files_failed << ",\n"
                      << "  \"files_missing\": " << summary.files_missing << ",\n"
                      << "  \"files_size_mismatch\": " << summary.files_size_mismatch << ",\n"
                      << "  \"files_timestamp_mismatch\": " << summary.files_timestamp_mismatch << ",\n"
                      << "  \"files_digest_mismatch\": " << summary.files_digest_mismatch << ",\n"
                      << "  \"files_inaccessible\": " << summary.files_inaccessible << ",\n"
                      << "  \"files_unsafe\": " << summary.files_unsafe << ",\n"
                      << "  \"elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                      << "  \"scheduler_elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                      << "  \"total_elapsed_ns\": " << summary.total_elapsed_ns << ",\n"
                      << "  \"workers_used\": " << summary.scheduling.workers_used << ",\n"
                      << "  \"error\": \"" << json_escape(summary.error_message) << "\",\n";
            print_worker_metrics(summary.scheduling);
            print_scheduler_tasks(summary.scheduling);
            std::cout << "}\n";
            return summary.success ? 0 : 1;
        }

        if (is_backup) {
            const ipbrs::BackupSummary summary = ipbrs::run_backup(backup_config);
            std::cout << "{\n";
            print_build_metadata();
            std::cout << "  \"operation\": \"backup\",\n"
                      << "  \"success\": " << (summary.success ? "true" : "false") << ",\n"
                      << "  \"policy\": \"" << ipbrs::policy_name(summary.scheduling.policy) << "\",\n"
                      << "  \"backup_id\": \"" << json_escape(summary.backup_id) << "\",\n"
                      << "  \"snapshot_path\": \"" << json_escape(summary.snapshot_path_utf8) << "\",\n"
                      << "  \"catalog_path\": \"" << json_escape(summary.catalog_path_utf8) << "\",\n"
                      << "  \"discovery_elapsed_ns\": " << summary.discovery_elapsed_ns << ",\n"
                      << "  \"discovery_workers_used\": " << summary.discovery_workers_used << ",\n"
                      << "  \"elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                      << "  \"scheduler_elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                      << "  \"total_elapsed_ns\": " << summary.total_elapsed_ns << ",\n"
                      << "  \"workers_used\": " << summary.scheduling.workers_used << ",\n"
                      << "  \"steal_attempts\": " << [&summary]() { std::size_t n=0; for(std::size_t i=0;i<summary.scheduling.workers.size();++i)n+=summary.scheduling.workers[i].steal_attempts; return n; }() << ",\n"
                      << "  \"steals_succeeded\": " << [&summary]() { std::size_t n=0; for(std::size_t i=0;i<summary.scheduling.workers.size();++i)n+=summary.scheduling.workers[i].steals_succeeded; return n; }() << ",\n"
                      << "  \"scheduling_overhead_ns\": " << summary.scheduling.scheduling_overhead_ns << ",\n"
                      << "  \"error\": \"" << json_escape(summary.error_message) << "\",\n";
            print_adaptive_metrics(summary.scheduling);
            print_worker_metrics(summary.scheduling);
            print_scheduler_tasks(summary.scheduling);
            std::cout << "}\n";
            return summary.success ? 0 : 1;
        }

        const ipbrs::RecoverySummary summary = ipbrs::run_recovery(recovery_config);
        std::cout << "{\n";
        print_build_metadata();
        std::cout << "  \"operation\": \"recover\",\n"
                  << "  \"success\": " << (summary.success ? "true" : "false") << ",\n"
                  << "  \"status\": \"" << json_escape(summary.status) << "\",\n"
                  << "  \"mode\": \"" << (recovery_config.full_recovery ? "full" : "selective") << "\",\n"
                  << "  \"backup_id\": \"" << json_escape(summary.backup_id) << "\",\n"
                  << "  \"policy\": \"" << ipbrs::policy_name(summary.scheduling.policy) << "\",\n"
                  << "  \"catalog_files\": " << summary.catalog_files << ",\n"
                  << "  \"restore_tasks\": " << summary.restore_tasks << ",\n"
                  << "  \"files_missing\": " << summary.files_missing << ",\n"
                  << "  \"files_modified\": " << summary.files_modified << ",\n"
                  << "  \"files_inaccessible\": " << summary.files_inaccessible << ",\n"
                  << "  \"files_extra\": " << summary.files_extra << ",\n"
                  << "  \"files_restored\": " << summary.files_restored << ",\n"
                  << "  \"files_skipped\": " << summary.files_skipped << ",\n"
                  << "  \"files_failed\": " << summary.files_failed << ",\n"
                  << "  \"bytes_restored\": " << summary.bytes_restored << ",\n"
                  << "  \"bytes_avoided\": " << summary.bytes_avoided << ",\n"
                  << "  \"elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                  << "  \"scheduler_elapsed_ns\": " << summary.scheduling.elapsed_ns << ",\n"
                  << "  \"total_elapsed_ns\": " << summary.total_elapsed_ns << ",\n"
                  << "  \"workers_used\": " << summary.scheduling.workers_used << ",\n"
                  << "  \"steal_attempts\": " << [&summary]() { std::size_t n=0; for(std::size_t i=0;i<summary.scheduling.workers.size();++i)n+=summary.scheduling.workers[i].steal_attempts; return n; }() << ",\n"
                  << "  \"steals_succeeded\": " << [&summary]() { std::size_t n=0; for(std::size_t i=0;i<summary.scheduling.workers.size();++i)n+=summary.scheduling.workers[i].steals_succeeded; return n; }() << ",\n"
                  << "  \"error\": \"" << json_escape(summary.error_message) << "\",\n";
        print_adaptive_metrics(summary.scheduling);
        print_worker_metrics(summary.scheduling);
        print_scheduler_tasks(summary.scheduling);
        std::cout << "}\n";
        return summary.success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        usage();
        return 2;
    }
}
