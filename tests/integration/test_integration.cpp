/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Integration tests for Call Stack Logger.
 *
 * Executes the traced_test_program and parses its trace output file to verify
 * that function instrumentation, symbol resolution, and formatting work
 * correctly end-to-end.
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// TRACED_PROGRAM_PATH is defined by CMake as a compile definition
// pointing to the absolute path of the traced_test_program executable.
#ifndef TRACED_PROGRAM_PATH
    #error "TRACED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef NONINSTRUMENTED_PROGRAM_PATH
    #error "NONINSTRUMENTED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef THREADED_TRACED_PROGRAM_PATH
    #error "THREADED_TRACED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef CALLSTACK_API_PROGRAM_PATH
    #error "CALLSTACK_API_PROGRAM_PATH must be defined by CMake"
#endif

namespace {

// Read entire file contents into a string.
std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Split a string into lines.
std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Count leading indentation characters ("|  " and "|_ " patterns) in a trace line.
// Returns the nesting depth based on the tree prefix.
//
// Scans forward to the FIRST "|  " or "|_ " occurrence rather than stopping at
// the first "] ", so the helper tolerates any number of bracketed prefixes
// between the timestamp and the tree — specifically: the LOG_ELAPSED duration
// field ("[  pending ]" or a patched duration) and the LOG_ADDR field
// ("addr: [0x...]"). The tree characters are unambiguous on a valid trace line
// because none of the prefix content ever contains those exact 3-char sequences.
int count_indentation_depth(const std::string& line) {
    size_t pos = 0;
    while (pos + 2 < line.size()) {
        if (line.compare(pos, 3, "|  ") == 0 || line.compare(pos, 3, "|_ ") == 0) {
            break;
        }
        ++pos;
    }
    int depth = 0;
    while (pos + 2 < line.size()) {
        if (line.compare(pos, 3, "|  ") == 0 || line.compare(pos, 3, "|_ ") == 0) {
            ++depth;
            pos += 3;
        } else {
            break;
        }
    }
    return depth;
}

} // namespace

// Test fixture that runs the traced program and captures output.
class IntegrationTest : public ::testing::Test {
protected:
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string trace_file_path;

    void SetUp() override {
        // Create a unique temp file path for this test run
        char tmp_path[] = "/tmp/cslg_test_XXXXXX";
        int fd = mkstemp(tmp_path);
        ASSERT_GE(fd, 0) << "Failed to create temp file";
        close(fd);
        trace_file_path = tmp_path;

        // Set environment variable and run traced program.
        // Paths are quoted to handle build directories containing spaces.
        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_file_path + "\" \"" + TRACED_PROGRAM_PATH + "\"";
        int ret = system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "traced_test_program failed with exit code " << ret;

        // Read trace output
        trace_content = read_file(trace_file_path);
        trace_lines = split_lines(trace_content);
    }

    void TearDown() override {
        // Clean up temp file
        if (!trace_file_path.empty()) {
            unlink(trace_file_path.c_str());
        }
    }
};

// Verify the run separator header is present.
TEST_F(IntegrationTest, RunSeparatorPresent) {
    EXPECT_NE(trace_content.find("========================================"), std::string::npos)
            << "Run separator line not found in trace output";
    EXPECT_NE(trace_content.find("=== New trace run:"), std::string::npos)
            << "Run separator header not found in trace output";
}

// Verify main() appears in the trace.
TEST_F(IntegrationTest, MainFunctionPresent) {
    EXPECT_NE(trace_content.find("main"), std::string::npos)
            << "main() not found in trace output";
}

// Verify the free function chain is resolved.
TEST_F(IntegrationTest, FreeFunctionChainResolved) {
    EXPECT_NE(trace_content.find("func_a"), std::string::npos) << "func_a not found";
    EXPECT_NE(trace_content.find("func_b"), std::string::npos) << "func_b not found";
    EXPECT_NE(trace_content.find("func_c"), std::string::npos) << "func_c not found";
}

// Verify static method is resolved.
TEST_F(IntegrationTest, StaticMethodResolved) {
    EXPECT_NE(trace_content.find("static_method"), std::string::npos)
            << "TracedClass::static_method not found";
}

// Verify constructor is resolved.
TEST_F(IntegrationTest, ConstructorResolved) {
    EXPECT_NE(trace_content.find("TracedClass"), std::string::npos)
            << "TracedClass constructor not found";
}

// Verify template function is resolved.
TEST_F(IntegrationTest, TemplateFunctionResolved) {
    EXPECT_NE(trace_content.find("template_func"), std::string::npos)
            << "template_func not found";
}

// Verify nesting depth increases for nested calls.
TEST_F(IntegrationTest, NestingDepthCorrect) {
    // Find func_a, func_b, func_c lines and compare indentation
    int depth_a = -1, depth_b = -1, depth_c = -1;

    for (const auto& line : trace_lines) {
        if (line.find("func_a") != std::string::npos && line.find("called from") != std::string::npos) {
            depth_a = count_indentation_depth(line);
        }
        if (line.find("func_b") != std::string::npos && line.find("called from") != std::string::npos) {
            depth_b = count_indentation_depth(line);
        }
        if (line.find("func_c") != std::string::npos && line.find("called from") != std::string::npos) {
            depth_c = count_indentation_depth(line);
        }
    }

    ASSERT_GE(depth_a, 0) << "func_a not found in trace";
    ASSERT_GE(depth_b, 0) << "func_b not found in trace";
    ASSERT_GE(depth_c, 0) << "func_c not found in trace";

    EXPECT_LT(depth_a, depth_b) << "func_b should be more indented than func_a";
    EXPECT_LT(depth_b, depth_c) << "func_c should be more indented than func_b";
}

// Verify caller info format (called from: filename:line).
TEST_F(IntegrationTest, CallerInfoPresent) {
    std::regex caller_pattern(R"(\(called from: .+:\d+\))");
    bool found = false;
    for (const auto& line : trace_lines) {
        if (std::regex_search(line, caller_pattern)) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "No trace line with (called from: file:line) pattern found";
}

// Verify timestamp format in trace lines.
TEST_F(IntegrationTest, TimestampFormat) {
    std::regex ts_pattern(R"(\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    bool found = false;
    for (const auto& line : trace_lines) {
        if (std::regex_search(line, ts_pattern)) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "No trace line with expected timestamp format found";
}

// Verify CSLG_OUTPUT_FILE correctly redirected output.
TEST_F(IntegrationTest, CustomOutputPath) {
    // The trace_content was read from the custom path set via CSLG_OUTPUT_FILE.
    // If we got here with non-empty content, redirection worked.
    EXPECT_FALSE(trace_content.empty()) << "Trace output file is empty — CSLG_OUTPUT_FILE may not work";
}

// Verify std library functions are excluded from the trace.
// GCC excludes them at compile time via -finstrument-functions-exclude-file-list.
// Clang excludes them at runtime via is_std_library_symbol() in resolve_function_name().
// The traced_program calls func_with_stl() which uses std::vector and std::sort internally.
TEST_F(IntegrationTest, StdLibraryFunctionsExcluded) {
    for (const auto& line : trace_lines) {
        // Only check function entry lines (contain "|_")
        if (line.find("|_") == std::string::npos) {
            continue;
        }
        EXPECT_EQ(line.find("|_ std::"), std::string::npos)
                << "std:: function found in trace: " << line;
        EXPECT_EQ(line.find("|_ __gnu_cxx::"), std::string::npos)
                << "__gnu_cxx:: function found in trace: " << line;
        EXPECT_EQ(line.find("|_ __cxxabiv1::"), std::string::npos)
                << "__cxxabiv1:: function found in trace: " << line;
    }
}

// Verify user function that calls STL is still traced.
TEST_F(IntegrationTest, StlUsageFunctionPresent) {
    EXPECT_NE(trace_content.find("func_with_stl"), std::string::npos)
            << "func_with_stl not found — user functions using STL must still appear";
}

// Verify trace has exactly the expected number of function entries.
// The traced program calls 10 user-defined functions. Without std library filtering,
// Clang would produce hundreds of entries from template instantiations.
TEST_F(IntegrationTest, ExactTraceLineCount) {
    int entry_count = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") != std::string::npos) {
            entry_count++;
        }
    }
    // Expected: main, func_a, func_b, func_c, static_method, TracedClass (ctor),
    // instance_method, template_func, inline_func, func_with_stl = 10 entries.
    EXPECT_EQ(entry_count, 10)
            << "Expected exactly 10 function entries; got " << entry_count
            << ". If count is much higher, std library functions may be leaking through.";
}

// Verify that a program compiled WITHOUT instrumentation flags produces no trace output.
// This tests the behavior of DISABLE_INSTRUMENTATION=ON and validates that the split
// compilation approach (library without -finstrument-functions) doesn't accidentally
// produce trace entries when user code is also not instrumented.
TEST(DisableInstrumentationTest, NoTraceOutputWithoutInstrumentation) {
    // Create a unique temp file
    char tmp_path[] = "/tmp/cslg_noinstr_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "Failed to create temp file";
    close(fd);

    // Run the non-instrumented program
    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path)
                    + "\" \"" + NONINSTRUMENTED_PROGRAM_PATH + "\"";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "noninstrumented_test_program failed with exit code " << ret;

    // Read trace output — should contain no function entries
    std::ifstream ifs(tmp_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Count function entry lines
    int entry_count = 0;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("(called from:") != std::string::npos) {
            entry_count++;
        }
    }

    EXPECT_EQ(entry_count, 0)
            << "Non-instrumented program should produce 0 function entries, got "
            << entry_count << ". DISABLE_INSTRUMENTATION may be broken.";

    unlink(tmp_path);
}

// ============================================================================
// Multi-threaded integration tests — exercise per-thread trace files.
// The threaded traced program spawns 4 worker threads, each producing its own
// <base>_tid_<tid> file. Main produces <base> (no _tid_ suffix).
// ============================================================================

namespace {

// Remove a directory and all its contents (one level deep — we don't nest).
void remove_dir_tree(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        unlink(full.c_str());
    }
    closedir(d);
    rmdir(dir.c_str());
}

// List regular files in a directory (non-recursive).
std::vector<std::string> list_files_in(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return files;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        files.push_back(name);
    }
    closedir(d);
    return files;
}

// Extract the numeric TID suffix from a filename matching "<base>_tid_<n>".
// Returns -1 if the pattern doesn't match.
long parse_tid_suffix(const std::string& filename, const std::string& base_name) {
    const std::string marker = base_name + "_tid_";
    if (filename.compare(0, marker.size(), marker) != 0) return -1;
    const std::string tail = filename.substr(marker.size());
    if (tail.empty()) return -1;
    for (char c : tail) {
        if (c < '0' || c > '9') return -1;
    }
    return std::stol(tail);
}

} // namespace

class ThreadedIntegrationTest : public ::testing::Test {
protected:
    std::string tmpdir;
    std::string base_filename = "thread_trace.out";
    std::string main_trace_path;
    long main_tid_from_stdout = -1;

    // filename -> full file content (per file)
    std::map<std::string, std::string> contents;
    // filename -> vector of lines
    std::map<std::string, std::vector<std::string>> lines_by_file;
    // All worker filenames (everything matching <base>_tid_<n>)
    std::vector<std::string> worker_filenames;

    void SetUp() override {
        // Unique temp directory per test (mkdtemp) so we don't collide with prior runs.
        char tmpl[] = "/tmp/cslg_threaded_XXXXXX";
        char* d = mkdtemp(tmpl);
        ASSERT_NE(d, nullptr) << "mkdtemp failed";
        tmpdir = d;
        main_trace_path = tmpdir + "/" + base_filename;

        // Run the threaded program; capture stdout (which prints MAIN_TID=<n>).
        const std::string stdout_path = tmpdir + "/stdout.txt";
        std::string cmd = "CSLG_OUTPUT_FILE=\"" + main_trace_path
                        + "\" \"" + THREADED_TRACED_PROGRAM_PATH + "\" > \""
                        + stdout_path + "\"";
        int ret = system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "threaded_traced_test_program failed, exit=" << ret;

        // Parse MAIN_TID=<n> from stdout.
        std::ifstream sifs(stdout_path);
        std::string sline;
        while (std::getline(sifs, sline)) {
            const std::string marker = "MAIN_TID=";
            if (sline.compare(0, marker.size(), marker) == 0) {
                main_tid_from_stdout = std::stol(sline.substr(marker.size()));
                break;
            }
        }
        sifs.close();
        ASSERT_GT(main_tid_from_stdout, 0)
                << "Could not parse MAIN_TID from program stdout";

        // Enumerate trace files in the tmpdir and read them all.
        for (const std::string& name : list_files_in(tmpdir)) {
            if (name == base_filename) {
                // main trace file
            } else if (parse_tid_suffix(name, base_filename) >= 0) {
                worker_filenames.push_back(name);
            } else {
                continue;  // skip stdout.txt, etc.
            }
            std::ifstream ifs(tmpdir + "/" + name);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
            contents[name] = content;
            std::istringstream iss(content);
            std::vector<std::string> v;
            std::string line;
            while (std::getline(iss, line)) v.push_back(line);
            lines_by_file[name] = std::move(v);
        }
    }

    void TearDown() override {
        if (!tmpdir.empty()) {
            remove_dir_tree(tmpdir);
        }
    }
};

TEST_F(ThreadedIntegrationTest, MainFileExists) {
    ASSERT_TRUE(contents.count(base_filename) > 0)
            << "Main trace file '" << base_filename << "' not found in " << tmpdir;
    EXPECT_FALSE(contents[base_filename].empty()) << "Main trace file is empty";
}

TEST_F(ThreadedIntegrationTest, WorkerFilesExist) {
    // Program spawns 4 worker threads; expect 4 worker files.
    EXPECT_EQ(worker_filenames.size(), 4u)
            << "Expected 4 worker files, got " << worker_filenames.size();
}

TEST_F(ThreadedIntegrationTest, WorkerFilenamesContainDistinctTids) {
    std::vector<long> tids;
    for (const auto& name : worker_filenames) {
        long tid = parse_tid_suffix(name, base_filename);
        ASSERT_GT(tid, 0) << "Worker filename TID suffix invalid: " << name;
        tids.push_back(tid);
    }
    std::sort(tids.begin(), tids.end());
    EXPECT_TRUE(std::unique(tids.begin(), tids.end()) == tids.end())
            << "Duplicate TIDs in worker filenames";
}

TEST_F(ThreadedIntegrationTest, EachFileHasRunSeparator) {
    // Main file + all worker files must contain the run-separator header with
    // the "thread ID: <n>" suffix.
    std::regex header_pattern(R"(=== New trace run: .+, thread ID: \d+ ===)");
    ASSERT_TRUE(contents.count(base_filename) > 0);
    EXPECT_TRUE(std::regex_search(contents[base_filename], header_pattern))
            << "Main file missing header: " << base_filename;
    for (const auto& name : worker_filenames) {
        EXPECT_TRUE(std::regex_search(contents[name], header_pattern))
                << "Worker file missing header: " << name;
    }
}

TEST_F(ThreadedIntegrationTest, MainHeaderContainsMainTid) {
    // Main file's header must contain "thread ID: <main_tid_from_stdout>".
    ASSERT_TRUE(contents.count(base_filename) > 0);
    std::string expected = "thread ID: " + std::to_string(main_tid_from_stdout);
    EXPECT_NE(contents[base_filename].find(expected), std::string::npos)
            << "Main file header does not contain expected main TID. "
            << "Expected substring: '" << expected << "'";
}

TEST_F(ThreadedIntegrationTest, WorkerHeaderTidMatchesFilename) {
    // Each worker file's _tid_<n> filename suffix must match the "thread ID: <n>"
    // value in its own header — proves filename and in-file identification agree.
    for (const auto& name : worker_filenames) {
        long file_tid = parse_tid_suffix(name, base_filename);
        std::string expected = "thread ID: " + std::to_string(file_tid);
        EXPECT_NE(contents[name].find(expected), std::string::npos)
                << "Worker file " << name << " header TID doesn't match filename ("
                << file_tid << "). Expected substring: '" << expected << "'";
    }
}

TEST_F(ThreadedIntegrationTest, EachFileHasValidTimestampFormat) {
    // Same timestamp regex as single-threaded test, applied to every file.
    std::regex ts_pattern(R"(\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    // Main may have the header but no function entries if its tracing only covers
    // main_only_post_join(); we check for the timestamp on the entry line instead.
    auto assert_has_ts = [&](const std::string& file_name) {
        bool found = false;
        for (const auto& line : lines_by_file[file_name]) {
            if (std::regex_search(line, ts_pattern)) { found = true; break; }
        }
        EXPECT_TRUE(found) << "File " << file_name << " has no line with expected timestamp format";
    };
    assert_has_ts(base_filename);
    for (const auto& name : worker_filenames) assert_has_ts(name);
}

TEST_F(ThreadedIntegrationTest, MainFilePostJoinCalls) {
    // main_only_post_join() is called only from main after all workers joined.
    // It must appear in the main file.
    ASSERT_TRUE(contents.count(base_filename) > 0);
    EXPECT_NE(contents[base_filename].find("main_only_post_join"), std::string::npos)
            << "Main file missing main_only_post_join";
}

TEST_F(ThreadedIntegrationTest, WorkerFilesDoNotHaveMainPostJoinContent) {
    // main_only_post_join is main-only; must NOT appear in any worker file.
    for (const auto& name : worker_filenames) {
        EXPECT_EQ(contents[name].find("main_only_post_join"), std::string::npos)
                << "Worker file " << name << " unexpectedly contains main_only_post_join";
    }
}

TEST_F(ThreadedIntegrationTest, WorkerFilesContainWorkerCallChain) {
    // Each worker file should contain the worker_top / worker_mid / worker_leaf chain.
    for (const auto& name : worker_filenames) {
        EXPECT_NE(contents[name].find("worker_top"), std::string::npos)
                << "Worker file " << name << " missing worker_top";
        EXPECT_NE(contents[name].find("worker_mid"), std::string::npos)
                << "Worker file " << name << " missing worker_mid";
        EXPECT_NE(contents[name].find("worker_leaf"), std::string::npos)
                << "Worker file " << name << " missing worker_leaf";
    }
}

TEST_F(ThreadedIntegrationTest, NoStdPollutionInWorkerFiles) {
    // Verify the runtime std library filter works per-file (critical: std::thread
    // internals on Clang would explode every worker trace without this).
    for (const auto& name : worker_filenames) {
        for (const auto& line : lines_by_file[name]) {
            if (line.find("|_") == std::string::npos) continue;
            EXPECT_EQ(line.find("|_ std::"), std::string::npos)
                    << "std:: leaked in worker " << name << ": " << line;
            EXPECT_EQ(line.find("|_ __gnu_cxx::"), std::string::npos)
                    << "__gnu_cxx:: leaked in worker " << name << ": " << line;
            EXPECT_EQ(line.find("|_ __cxxabiv1::"), std::string::npos)
                    << "__cxxabiv1:: leaked in worker " << name << ": " << line;
        }
    }
}

TEST_F(ThreadedIntegrationTest, AllWorkerFilesHaveEqualUserFunctionCount) {
    // Every worker runs the same call chain (worker_top → worker_mid → worker_leaf),
    // so every worker file should have the same number of function entries.
    std::vector<int> counts;
    for (const auto& name : worker_filenames) {
        int c = 0;
        for (const auto& line : lines_by_file[name]) {
            if (line.find("(called from:") != std::string::npos) c++;
        }
        counts.push_back(c);
    }
    ASSERT_FALSE(counts.empty());
    for (size_t i = 1; i < counts.size(); ++i) {
        EXPECT_EQ(counts[i], counts[0])
                << "Worker file " << worker_filenames[i] << " has "
                << counts[i] << " entries, but " << worker_filenames[0]
                << " has " << counts[0];
    }
}

// ============================================================================
// Graceful-degradation test — CSLG_OUTPUT_FILE points to an unwritable path.
// The program must still run to completion and emit a stderr warning.
// ============================================================================

TEST(BadOutputPathTest, OpenFailureIsNonFatalAndWarns) {
    // Use a path inside a non-existent directory — open() returns ENOENT.
    // The traced program must run normally and emit a stderr warning rather
    // than crashing or blocking indefinitely.
    char stderr_tmpl[] = "/tmp/cslg_bad_stderr_XXXXXX";
    int fd = mkstemp(stderr_tmpl);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    const std::string bad_path = "/nonexistent_cslg_dir/trace.out";
    std::string cmd = "CSLG_OUTPUT_FILE=\"" + bad_path + "\" \""
                    + TRACED_PROGRAM_PATH + "\" 2> \"" + stderr_tmpl + "\"";
    int ret = system(cmd.c_str());
    EXPECT_EQ(ret, 0) << "traced_test_program exited non-zero with bad path: " << ret;

    std::ifstream ifs(stderr_tmpl);
    std::string err_content((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    ifs.close();
    unlink(stderr_tmpl);

    // Expect the warning emitted by open_this_thread_file() on open() failure.
    EXPECT_NE(err_content.find("[call-stack-logger] WARNING"), std::string::npos)
            << "Expected stderr warning on open failure. stderr was:\n" << err_content;
    EXPECT_NE(err_content.find(bad_path), std::string::npos)
            << "Expected the attempted path in the warning. stderr was:\n" << err_content;

    // No trace file should have been created at the bad path (the parent
    // directory doesn't exist, so nothing to clean up).
}

// ============================================================================
// Public API test — exercises instrumentation::get_call_stack()
// ============================================================================

// Run the callstack API program, capture stdout, and verify the emitted stack
// contains the expected ancestor functions.
TEST(CallStackApiTest, GetCallStackResolvesAncestors) {
    char stdout_tmpl[] = "/tmp/cslg_cs_stdout_XXXXXX";
    int fd = mkstemp(stdout_tmpl);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = std::string("\"") + CALLSTACK_API_PROGRAM_PATH
                    + "\" > \"" + stdout_tmpl + "\"";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "callstack_api_program failed, exit=" << ret;

    std::ifstream ifs(stdout_tmpl);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();
    unlink(stdout_tmpl);

    // The program prints each frame as "FRAME: <name>". Verify the three
    // ancestor functions are present in order of appearance in the stack
    // (get_call_stack() returns innermost → outermost).
    auto pos_leaf = content.find("FRAME: print_stack_from_leaf");
    auto pos_mid  = content.find("FRAME: callstack_mid");
    auto pos_top  = content.find("FRAME: callstack_top");
    auto pos_main = content.find("FRAME: main");

    EXPECT_NE(pos_leaf, std::string::npos) << "print_stack_from_leaf not in stack. Output:\n" << content;
    EXPECT_NE(pos_mid,  std::string::npos) << "callstack_mid not in stack";
    EXPECT_NE(pos_top,  std::string::npos) << "callstack_top not in stack";
    EXPECT_NE(pos_main, std::string::npos) << "main not in stack";

    // Ordering (innermost first):
    if (pos_leaf != std::string::npos && pos_mid != std::string::npos) {
        EXPECT_LT(pos_leaf, pos_mid) << "Expected leaf to appear before mid";
    }
    if (pos_mid != std::string::npos && pos_top != std::string::npos) {
        EXPECT_LT(pos_mid, pos_top) << "Expected mid to appear before top";
    }
    if (pos_top != std::string::npos && pos_main != std::string::npos) {
        EXPECT_LT(pos_top, pos_main) << "Expected top to appear before main";
    }

    // Verify per-frame caller info is correct (regression guard for the bug where
    // bfdResolver::resolve's hard-coded 6-frame unwind made every frame report the
    // SAME caller, regardless of which frame was being described).
    //
    // Each "FRAME: <fn> | CALLER: <file>:<line>" line should reference the source
    // file (callstack_api_program.cpp) for at least the inner ancestors, and the
    // caller_filename should differ across frames — at minimum, NOT all frames
    // should map to the exact same line number.
    std::regex caller_pattern(R"(CALLER: ([^:]+):(\d+|\?))");
    std::vector<std::string> caller_locs;
    auto begin = std::sregex_iterator(content.begin(), content.end(), caller_pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        caller_locs.push_back((*it)[1].str() + ":" + (*it)[2].str());
    }
    ASSERT_GE(caller_locs.size(), 3u) << "expected at least 3 caller entries, got " << caller_locs.size();

    // At least one of the inner-frame callers should reference the test program's
    // own source file. (We don't assert on every frame because the outermost main
    // frame's caller is in libc and we don't always have line info there.)
    bool found_program_source = false;
    for (const auto& loc : caller_locs) {
        if (loc.find("callstack_api_program.cpp") != std::string::npos) {
            found_program_source = true;
            break;
        }
    }
    EXPECT_TRUE(found_program_source)
            << "Expected at least one CALLER to reference callstack_api_program.cpp. "
               "If every frame reports the same fixed caller location, this is the "
               "old hard-coded-unwind bug. Output:\n" << content;

    // Distinct caller locations across frames — guards specifically against "every
    // frame has the same caller" pathology. Only check among frames that resolved
    // to a numeric line (skip the libc/unresolved tail).
    std::set<std::string> distinct_locs;
    for (const auto& loc : caller_locs) {
        if (loc.find(":?") == std::string::npos) {
            distinct_locs.insert(loc);
        }
    }
    EXPECT_GT(distinct_locs.size(), 1u)
            << "All resolved caller locations are identical (" << caller_locs.size()
            << " entries, " << distinct_locs.size() << " distinct) — likely the "
               "hard-coded-unwind bug has regressed. Output:\n" << content;
}

// ============================================================================
// Build-flag tests — exercise the LOG_ADDR and LOG_NOT_DEMANGLED CMake options
// via library variants built with each macro defined.
// ============================================================================

namespace {

// Run a traced program under CSLG_OUTPUT_FILE=<tmp>, return file contents.
std::string run_and_capture_trace(const char* program_path) {
    char tmp[] = "/tmp/cslg_flag_test_XXXXXX";
    int fd = mkstemp(tmp);
    EXPECT_GE(fd, 0) << "mkstemp failed";
    if (fd < 0) return {};
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp)
                    + "\" \"" + program_path + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    EXPECT_EQ(ret, 0) << program_path << " failed, exit=" << ret;

    std::ifstream ifs(tmp);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();
    unlink(tmp);
    return content;
}

} // namespace

// Behavioral test: with -DLOG_ADDR=ON, every traced line must include the
// "addr: [0x...]" prefix between the timestamp and the function name.
TEST(LogAddrFlagTest, AddressPrefixAppearsInTrace) {
    std::string content = run_and_capture_trace(TRACED_PROGRAM_LOG_ADDR_PATH);
    ASSERT_FALSE(content.empty()) << "trace file is empty";

    // Pattern: "[<timestamp>] addr: [0x<hex>] ..." — the address should be a
    // non-zero hex value of pointer width (16 chars on x86_64).
    std::regex addr_pattern(R"(\] addr: \[0x[0-9a-f]+\] )");

    int total_entries = 0;
    int with_address = 0;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++total_entries;
        if (std::regex_search(line, addr_pattern)) ++with_address;
    }

    ASSERT_GT(total_entries, 0) << "no function entries in trace";
    EXPECT_EQ(with_address, total_entries)
            << "Expected every entry to have 'addr: [0x...]' prefix when "
            << "LOG_ADDR is defined. Got " << with_address << "/" << total_entries
            << ". Sample line:\n" << line;
}

// Negative test: the default-built (no LOG_ADDR) traced_test_program must NOT
// include the address prefix. This guards against the macro accidentally
// becoming always-on. Skipped when the build was configured with
// -DLOG_ADDR=ON, in which case the "default" callstacklogger ALSO has
// LOG_ADDR PUBLIC and the negative assertion would false-fail.
TEST(LogAddrFlagTest, NoAddressPrefixWithoutFlag) {
#ifdef CSLG_DEFAULT_HAS_LOG_ADDR
    GTEST_SKIP() << "Skipped: configured with -DLOG_ADDR=ON, so the default "
                    "callstacklogger target also defines LOG_ADDR.";
#else
    std::string content = run_and_capture_trace(TRACED_PROGRAM_PATH);
    ASSERT_FALSE(content.empty()) << "trace file is empty";

    std::regex addr_pattern(R"(\] addr: \[0x)");
    EXPECT_FALSE(std::regex_search(content, addr_pattern))
            << "Default build should not include 'addr: [0x' — LOG_ADDR macro "
               "may be leaking into the default callstacklogger target.";
#endif
}

// Smoke test: with -DLOG_NOT_DEMANGLED=ON, the program still compiles, runs,
// and produces normal trace output. The flag's actual differential behavior
// (logging frames where dladdr returns dli_sname == nullptr) is hard to trigger
// deterministically — so this test only verifies the macro wires through and
// doesn't break the trace pipeline.
TEST(LogNotDemangledFlagTest, ProducesNormalTraceOutput) {
    std::string content = run_and_capture_trace(TRACED_PROGRAM_LOG_NOT_DEMANGLED_PATH);
    ASSERT_FALSE(content.empty()) << "trace file is empty";

    // Standard sanity checks — same as the default build's expectations.
    EXPECT_NE(content.find("=== New trace run:"), std::string::npos)
            << "missing run separator";
    EXPECT_NE(content.find("(called from:"), std::string::npos)
            << "no function entries";
    EXPECT_NE(content.find("main"), std::string::npos)
            << "main not in trace";
}
