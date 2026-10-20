#include "dataset.hpp"
#include "engine.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace ipbrs;

namespace {

const char* kUsage =
    "usage: ipbrs <command> [options]\n"
    "\n"
    "commands:\n"
    "  gen      --out DIR [--files N] [--profile low|medium|high] [--seed S] [--dup R]\n"
    "           [--big-files K] [--big-mib M] [--per-dir N]\n"
    "  damage   --target DIR [--ratio R] [--types missing,modified,corrupted,truncated]\n"
    "           [--seed S] [--log FILE]\n"
    "  backup   --source DIR --backup DIR [--block-kib N] [--no-sync]\n"
    "  verify   --backup DIR\n"
    "  recover  --source DIR --backup DIR [--mode block|file|full] [--trust-mtime] [--no-verify]\n"
    "           [--no-sync]\n"
    "\n"
    "scheduling (backup, verify, recover):\n"
    "  --sched seq|static|dynamic|steal   (default steal)\n"
    "  --split none|adaptive              (default adaptive; steal+adaptive = IPBRS)\n"
    "  --threads N                        (default: hardware threads)\n"
    "  --chunk N                          (dynamic: tasks per claim, default 1)\n"
    "  --split-factor K                   (pieces per thread target, default 8)\n"
    "  --task-cost-kib N                  (fixed per-task cost, default 64)\n"
    "\n"
    "common: --json FILE writes the result record to FILE as well as stdout\n";

class Args {
public:
    Args(int argc, char** argv, const std::set<std::string>& flags) {
        for (int i = 2; i < argc; ++i) {
            std::string token = argv[i];
            if (token.size() < 3 || token.compare(0, 2, "--") != 0)
                throw std::invalid_argument("unexpected argument: " + token);
            token = token.substr(2);
            if (flags.count(token)) { flags_.insert(token); continue; }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for --" + token);
            values_[token] = argv[++i];
        }
    }
    std::string text(const std::string& key, const std::string& fallback = "") {
        used_.insert(key);
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }
    std::string required(const std::string& key) {
        const std::string value = text(key);
        if (value.empty()) throw std::invalid_argument("--" + key + " is required");
        return value;
    }
    std::uint64_t number(const std::string& key, std::uint64_t fallback) {
        const std::string value = text(key);
        if (value.empty()) return fallback;
        std::size_t used = 0;
        const unsigned long long n = std::stoull(value, &used);
        if (used != value.size()) throw std::invalid_argument("--" + key + " expects an integer");
        return n;
    }
    double real(const std::string& key, double fallback) {
        const std::string value = text(key);
        return value.empty() ? fallback : std::stod(value);
    }
    bool flag(const std::string& key) { return flags_.count(key) > 0; }
    void check_all_used() const {
        for (const auto& entry : values_)
            if (!used_.count(entry.first)) throw std::invalid_argument("unknown option --" + entry.first);
    }

private:
    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
    std::set<std::string> used_;
};

fs::path path_arg(const std::string& text) { return fs::u8path(text); }

RunConfig run_config(Args& args) {
    RunConfig run;
    run.sched.policy = parse_policy(args.text("sched", "steal"));
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    run.sched.threads = static_cast<int>(args.number("threads", hardware));
    run.sched.chunk = static_cast<std::size_t>(args.number("chunk", 1));
    run.split = parse_split(args.text("split", "adaptive"));
    run.split_factor = args.number("split-factor", 8);
    run.per_task_cost = args.number("task-cost-kib", 64) * 1024;
    if (run.sched.threads < 1 || run.sched.threads > 1024) throw std::invalid_argument("--threads must be 1..1024");
    return run;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
        if (!item.empty()) items.push_back(item);
    return items;
}

int run(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
        std::cout << kUsage;
        return argc < 2 ? 1 : 0;
    }
    const std::string command = argv[1];
    Args args(argc, argv, {"trust-mtime", "no-verify", "no-sync"});
    const std::string json_file = args.text("json");
    std::string json;
    int status = 0;

    if (command == "gen") {
        GenOptions o;
        o.out = path_arg(args.required("out"));
        o.files = args.number("files", 1000);
        o.profile = args.text("profile", "medium");
        o.seed = args.number("seed", 1);
        o.duplicate_ratio = args.real("dup", 0.0);
        o.big_files = args.number("big-files", 0);
        o.big_mib = args.number("big-mib", 256);
        o.files_per_dir = args.number("per-dir", 1000);
        o.threads = static_cast<int>(args.number("threads", std::max(1u, std::thread::hardware_concurrency())));
        args.check_all_used();
        json = generate_dataset(o);
    } else if (command == "damage") {
        DamageOptions o;
        o.target = path_arg(args.required("target"));
        o.ratio = args.real("ratio", 0.05);
        const std::string types = args.text("types");
        if (!types.empty()) o.types = split_list(types);
        o.seed = args.number("seed", 1);
        o.log = path_arg(args.text("log"));
        args.check_all_used();
        json = inject_damage(o);
    } else if (command == "backup") {
        BackupOptions o;
        o.source = path_arg(args.required("source"));
        o.backup = path_arg(args.required("backup"));
        o.block_size = static_cast<std::uint32_t>(args.number("block-kib", 1024) * 1024);
        o.sync = !args.flag("no-sync");
        o.run = run_config(args);
        args.check_all_used();
        const BackupResult r = run_backup(o);
        json = r.json;
        status = r.committed ? 0 : 2;
    } else if (command == "verify") {
        VerifyOptions o;
        o.backup = path_arg(args.required("backup"));
        o.run = run_config(args);
        args.check_all_used();
        const VerifyResult r = run_verify(o);
        json = r.json;
        status = r.bad == 0 ? 0 : 2;
    } else if (command == "recover") {
        RecoverOptions o;
        o.source = path_arg(args.required("source"));
        o.backup = path_arg(args.required("backup"));
        o.mode = parse_recovery_mode(args.text("mode", "block"));
        o.trust_mtime = args.flag("trust-mtime");
        o.post_verify = !args.flag("no-verify");
        o.sync = !args.flag("no-sync");
        o.run = run_config(args);
        args.check_all_used();
        const RecoverResult r = run_recover(o);
        json = r.json;
        status = (r.failed_files == 0 && r.verify_bad == 0) ? 0 : 2;
    } else {
        throw std::invalid_argument("unknown command: " + command);
    }

    std::cout << json << std::endl;
    if (!json_file.empty()) {
        std::ofstream out(path_arg(json_file), std::ios::binary | std::ios::app);
        if (!out) throw std::runtime_error("cannot write " + json_file);
        out << json << '\n';
    }
    return status;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "ipbrs: " << error.what() << std::endl;
        return 1;
    }
}
