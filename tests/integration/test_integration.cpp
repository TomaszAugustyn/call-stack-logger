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
int count_indentation_depth(const std::string& line) {
    int depth = 0;
    // Find the first "|" after the timestamp (which is in brackets)
    auto bracket_end = line.find("] ");
    if (bracket_end == std::string::npos) return 0;

    std::string after_ts = line.substr(bracket_end + 2);
    size_t pos = 0;
    while (pos + 2 < after_ts.size()) {
        if (after_ts.substr(pos, 3) == "|  " || after_ts.substr(pos, 3) == "|_ ") {
            depth++;
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
