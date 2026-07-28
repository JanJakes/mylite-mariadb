#include <mylite/mylite.h>

#include "ownerless_platform_io.h"
#include "ownerless_probe.h"

#include <windows.h>

#include <dbghelp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ChildProcess {
    PROCESS_INFORMATION info = {};
};

void print_stack_symbol(HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) std::array<unsigned char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage.data());
    symbol->SizeOfStruct = sizeof(*symbol);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0U;
    if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
        std::fprintf(
            stderr,
            "windows-ownerless stack address=0x%llx symbol=%s+0x%llx\n",
            static_cast<unsigned long long>(address),
            symbol->Name,
            static_cast<unsigned long long>(displacement)
        );
    } else {
        std::fprintf(
            stderr,
            "windows-ownerless stack address=0x%llx\n",
            static_cast<unsigned long long>(address)
        );
    }
}

LONG WINAPI print_unhandled_exception(EXCEPTION_POINTERS *exception) {
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    std::fprintf(
        stderr,
        "windows-ownerless exception=0x%08lx address=%p\n",
        exception->ExceptionRecord->ExceptionCode,
        exception->ExceptionRecord->ExceptionAddress
    );

#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    const bool symbols_initialized = SymInitialize(process, nullptr, TRUE) != FALSE;
    print_stack_symbol(process, frame.AddrPC.Offset);
    for (unsigned depth = 1U; depth < 64U; ++depth) {
        if (StackWalk64(
                IMAGE_FILE_MACHINE_AMD64,
                process,
                thread,
                &frame,
                &context,
                nullptr,
                SymFunctionTableAccess64,
                SymGetModuleBase64,
                nullptr
            ) == FALSE ||
            frame.AddrPC.Offset == 0U) {
            break;
        }
        print_stack_symbol(process, frame.AddrPC.Offset);
    }
    if (symbols_initialized) {
        static_cast<void>(SymCleanup(process));
    }
#endif
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void print_phase(const char *phase) {
    std::fprintf(stderr, "windows-ownerless phase=%s\n", phase);
    std::fflush(stderr);
}

void test_platform_io_does_not_rewrite_cpp_streams(void) {
    std::istringstream input("x");
    char value = '\0';
    input.read(&value, 1);
    assert(input.gcount() == 1 && value == 'x');
}

std::string quote_argument(const std::string &argument) {
    return "\"" + argument + "\"";
}

std::string executable_path(void) {
    std::vector<char> path(32768U, '\0');
    const DWORD length = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    assert(length > 0U && length < path.size());
    return std::string(path.data(), length);
}

ChildProcess spawn_child(
    const char *mode,
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path,
    const std::filesystem::path &marker_path = {}
) {
    std::string command = quote_argument(executable_path()) + " " + mode + " " +
                          quote_argument(database_path.string()) + " " +
                          quote_argument(runtime_path.string());
    if (!marker_path.empty()) {
        command += " " + quote_argument(marker_path.string());
    }
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    ChildProcess child = {};
    assert(
        CreateProcessA(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &child.info
        ) != FALSE
    );
    return child;
}

DWORD wait_for_child(ChildProcess &child, DWORD timeout_ms = 120000U) {
    assert(WaitForSingleObject(child.info.hProcess, timeout_ms) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    assert(GetExitCodeProcess(child.info.hProcess, &exit_code) != FALSE);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    child.info = {};
    return exit_code;
}

mylite_open_config open_config(const std::filesystem::path &runtime_path) {
    static thread_local std::string runtime;
    runtime = runtime_path.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.busy_timeout_ms = 30000U;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = runtime.c_str();
    return config;
}

mylite_db *open_ownerless(
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path,
    bool create
) {
    mylite_db *db = nullptr;
    mylite_open_config config = open_config(runtime_path);
    const unsigned flags =
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW | (create ? MYLITE_OPEN_CREATE : 0U);
    const int result = mylite_open(database_path.string().c_str(), &db, flags, &config);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "ownerless open failed: result=%d path=%s\n",
            result,
            database_path.string().c_str()
        );
    }
    assert(result == MYLITE_OK);
    assert(db != nullptr);
    return db;
}

void exec_ok(mylite_db *db, const char *sql) {
    char *error = nullptr;
    const int result = mylite_exec(db, sql, nullptr, nullptr, &error);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "SQL failed: result=%d sql=%s error=%s\n",
            result,
            sql,
            error != nullptr ? error : ""
        );
    }
    mylite_free(error);
    assert(result == MYLITE_OK);
}

int capture_unsigned(void *ctx, int column_count, char **values, char **column_names) {
    (void)column_names;
    assert(column_count == 1);
    assert(values != nullptr && values[0] != nullptr);
    *static_cast<unsigned long long *>(ctx) = std::strtoull(values[0], nullptr, 10);
    return 0;
}

unsigned long long query_unsigned(mylite_db *db, const char *sql) {
    unsigned long long value = 0;
    assert(mylite_exec(db, sql, capture_unsigned, &value, nullptr) == MYLITE_OK);
    return value;
}

int run_update_child(
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path
) {
    print_phase("update-child-open");
    mylite_db *db = open_ownerless(database_path, runtime_path, false);
    for (unsigned iteration = 0; iteration < 25U; ++iteration) {
        exec_ok(db, "UPDATE app.platform_probe SET value = value + 1 WHERE id = 1");
    }
    assert(mylite_close(db) == MYLITE_OK);
    print_phase("update-child-complete");
    return 0;
}

int run_dead_writer_child(
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path,
    const std::filesystem::path &marker_path
) {
    print_phase("dead-writer-child-open");
    mylite_db *db = open_ownerless(database_path, runtime_path, false);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.platform_probe SET value = value + 1000 WHERE id = 1");
    {
        FILE *marker = nullptr;
        assert(fopen_s(&marker, marker_path.string().c_str(), "wb") == 0);
        assert(marker != nullptr);
        assert(std::fputs("ready", marker) >= 0);
        assert(std::fclose(marker) == 0);
    }
    print_phase("dead-writer-child-ready");
    Sleep(INFINITE);
    return 2;
}

void test_unsupported_filesystem_contract(
    const std::filesystem::path &root,
    const std::filesystem::path &runtime_path
) {
    print_phase("unsupported-filesystem");
    const std::filesystem::path unsupported_path = root / "unsupported.mylite";
    assert(_putenv_s("MYLITE_OWNERLESS_TEST_FILESYSTEM", "unsupported") == 0);
    mylite_open_config config = open_config(runtime_path);
    mylite_db *db = nullptr;
    print_phase("unsupported-filesystem-ownerless-open");
    const int unsupported_result = mylite_open(
        unsupported_path.string().c_str(),
        &db,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW,
        &config
    );
    std::fprintf(
        stderr,
        "windows-ownerless unsupported-result=%d db=%p exists=%u\n",
        unsupported_result,
        static_cast<void *>(db),
        std::filesystem::exists(unsupported_path) ? 1U : 0U
    );
    std::fflush(stderr);
    assert(unsupported_result == MYLITE_UNSUPPORTED_FILESYSTEM);
    assert(db == nullptr);
    assert(!std::filesystem::exists(unsupported_path));

    print_phase("unsupported-filesystem-ordinary-open");
    const int ordinary_result = mylite_open(
        unsupported_path.string().c_str(),
        &db,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
        &config
    );
    std::fprintf(
        stderr,
        "windows-ownerless ordinary-result=%d db=%p\n",
        ordinary_result,
        static_cast<void *>(db)
    );
    std::fflush(stderr);
    assert(ordinary_result == MYLITE_OK);
    print_phase("unsupported-filesystem-ordinary-close");
    assert(mylite_close(db) == MYLITE_OK);
    std::filesystem::remove_all(unsupported_path);
    assert(_putenv_s("MYLITE_OWNERLESS_TEST_FILESYSTEM", "") == 0);
    print_phase("unsupported-filesystem-complete");
}

void run_parent(void) {
    print_phase("capabilities");
    const unsigned long long capabilities = mylite_capabilities();
    std::fprintf(stderr, "windows-ownerless capabilities=0x%llx\n", capabilities);
    std::fflush(stderr);
    assert((capabilities & MYLITE_CAP_OWNERLESS_RW) != 0U);
    assert((capabilities & MYLITE_CAP_SHARED_READONLY) != 0U);

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("mylite-windows-ownerless-" + std::to_string(GetCurrentProcessId()));
    const std::filesystem::path runtime_path = root / "runtime";
    const std::filesystem::path database_path = root / "platform.mylite";
    const std::filesystem::path dead_writer_marker = root / "dead-writer.ready";
    std::filesystem::remove_all(root);
    assert(std::filesystem::create_directories(runtime_path));

    print_phase("filesystem");
    mylite_ownerless_filesystem_info filesystem = {};
    assert(
        mylite_ownerless_probe_filesystem(root.string().c_str(), &filesystem) ==
        MYLITE_OWNERLESS_PROBE_OK
    );
    std::fprintf(
        stderr,
        "windows-ownerless filesystem=%s kind=%u local=%u admitted=%u volume=%llu\n",
        filesystem.name,
        filesystem.kind,
        filesystem.is_local,
        filesystem.is_admitted,
        static_cast<unsigned long long>(filesystem.volume_identity)
    );
    std::fflush(stderr);
    assert(filesystem.kind == MYLITE_OWNERLESS_FILESYSTEM_NTFS);
    assert(filesystem.is_local == 1U);
    assert(filesystem.is_admitted == 1U);
    assert(filesystem.volume_identity != 0U);

    print_phase("primitives");
    mylite_ownerless_probe_result probe = {};
    assert(
        mylite_ownerless_probe_directory(root.string().c_str(), &probe) == MYLITE_OWNERLESS_PROBE_OK
    );
    std::fprintf(
        stderr,
        "windows-ownerless primitives mmap=%u locks=%u exit=%u isolation=%u grow=%u wait=%u "
        "identity=%u required=%u\n",
        probe.mmap_shared_visibility,
        probe.byte_range_locks,
        probe.lock_release_on_exit,
        probe.lock_close_isolation,
        probe.grow_remap,
        probe.wait_backend,
        probe.process_identity,
        probe.required_primitives
    );
    std::fflush(stderr);
    assert(probe.required_primitives == 1U);
    assert(probe.lock_close_isolation == 1U);

    test_unsupported_filesystem_contract(root, runtime_path);

    print_phase("parent-open");
    mylite_db *parent = open_ownerless(database_path, runtime_path, true);
    print_phase("schema");
    exec_ok(parent, "CREATE DATABASE app");
    exec_ok(
        parent,
        "CREATE TABLE app.platform_probe ("
        "id INT NOT NULL PRIMARY KEY, value BIGINT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(parent, "INSERT INTO app.platform_probe VALUES (1, 1)");

    print_phase("concurrent-updates");
    ChildProcess first = spawn_child("update", database_path, runtime_path);
    ChildProcess second = spawn_child("update", database_path, runtime_path);
    assert(wait_for_child(first) == 0U);
    assert(wait_for_child(second) == 0U);
    assert(query_unsigned(parent, "SELECT value FROM app.platform_probe WHERE id = 1") == 51U);

    print_phase("dead-writer");
    ChildProcess dead_writer =
        spawn_child("dead-writer", database_path, runtime_path, dead_writer_marker);
    const auto marker_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!std::filesystem::exists(dead_writer_marker) &&
           std::chrono::steady_clock::now() < marker_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(std::filesystem::exists(dead_writer_marker));
    assert(TerminateProcess(dead_writer.info.hProcess, 99U) != FALSE);
    assert(wait_for_child(dead_writer) == 99U);

    mylite_open_config blocked_config = open_config(runtime_path);
    mylite_db *blocked = nullptr;
    assert(
        mylite_open(
            database_path.string().c_str(),
            &blocked,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
            &blocked_config
        ) == MYLITE_BUSY
    );
    assert(blocked == nullptr);
    print_phase("parent-close");
    assert(mylite_close(parent) == MYLITE_OK);

    print_phase("recovery-open");
    mylite_db *recovery = open_ownerless(database_path, runtime_path, false);
    assert(query_unsigned(recovery, "SELECT value FROM app.platform_probe WHERE id = 1") == 51U);
    assert(mylite_close(recovery) == MYLITE_OK);

    print_phase("durable-reopen");
    mylite_db *reopened = open_ownerless(database_path, runtime_path, false);
    assert(query_unsigned(reopened, "SELECT value FROM app.platform_probe WHERE id = 1") == 51U);
    assert(mylite_close(reopened) == MYLITE_OK);
    std::filesystem::remove_all(root);
    print_phase("complete");
}

} // namespace

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(print_unhandled_exception);
    test_platform_io_does_not_rewrite_cpp_streams();
    if (argc >= 4 && std::string(argv[1]) == "update") {
        return run_update_child(argv[2], argv[3]);
    }
    if (argc >= 5 && std::string(argv[1]) == "dead-writer") {
        return run_dead_writer_child(argv[2], argv[3], argv[4]);
    }
    run_parent();
    return 0;
}
