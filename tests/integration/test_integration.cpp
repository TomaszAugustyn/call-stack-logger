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

#include "eventFormat.h"
#include "prettyTime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <link.h> // ElfW, for the ELF section-name check of the fixture files
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
#ifndef CALLSTACK_API_PROGRAM_INSTRUMENTED_PATH
    #error "CALLSTACK_API_PROGRAM_INSTRUMENTED_PATH must be defined by CMake"
#endif
#ifndef STRIPPED_CALLER_PROGRAM_PATH
    #error "STRIPPED_CALLER_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef DLOPEN_CTOR_PROGRAM_PATH
    #error "DLOPEN_CTOR_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef OVERFLOW_DEPTH_PROGRAM_PATH
    #error "OVERFLOW_DEPTH_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef FILTERED_OVERFLOW_PROGRAM_PATH
    #error "FILTERED_OVERFLOW_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef EXCEPTION_TRACED_PROGRAM_PATH
    #error "EXCEPTION_TRACED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef GLOBAL_DTOR_PROGRAM_PATH
    #error "GLOBAL_DTOR_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef CANCEL_TRACED_PROGRAM_PATH
    #error "CANCEL_TRACED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef CANCEL_PROGRAM_LOG_ELAPSED_PATH
    #error "CANCEL_PROGRAM_LOG_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef CHDIR_TRACED_PROGRAM_PATH
    #error "CHDIR_TRACED_PROGRAM_PATH must be defined by CMake"
#endif
#ifndef CRASH_PROGRAM_LOG_ELAPSED_PATH
    #error "CRASH_PROGRAM_LOG_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_ELAPSED_PATH
    #error "TRACED_PROGRAM_LOG_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef THREADED_TRACED_PROGRAM_LOG_ELAPSED_PATH
    #error "THREADED_TRACED_PROGRAM_LOG_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH
    #error "TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_EXCEPTIONS_PATH
    #error "TRACED_PROGRAM_LOG_EXCEPTIONS_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH
    #error "TRACED_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_EXCEPTIONS_ALL_PATH
    #error "TRACED_PROGRAM_LOG_EXCEPTIONS_ALL_PATH must be defined by CMake"
#endif
#ifndef UNCAUGHT_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH
    #error "UNCAUGHT_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH must be defined by CMake"
#endif
#ifndef TRACED_PROGRAM_LOG_EXCEPTIONS_INLINED_PATH
    #error "TRACED_PROGRAM_LOG_EXCEPTIONS_INLINED_PATH must be defined by CMake"
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

// True when the three characters at `pos` are one tree element: an inner "|  "
// or the final glyph "|_ ", whose '|' LOG_EXCEPTIONS may have patched to '!'
// (left by an exception) or '~' (skipped by a non-local jump).
bool is_tree_element(const std::string& line, size_t pos) {
    if (pos + 2 >= line.size()) return false;
    if (line.compare(pos, 3, "|  ") == 0) return true;
    if (line.compare(pos, 3, "!! ") == 0) return true; // an exception event line's glyph
    const char glyph = line[pos];
    return (glyph == '|' || glyph == '!' || glyph == '~') && line[pos + 1] == '_' && line[pos + 2] == ' ';
}

// Count leading indentation characters ("|  " and "|_ " patterns) in a trace line.
// Returns the nesting depth based on the tree prefix.
//
// Scans forward to the FIRST tree element rather than stopping at the first
// "] ", so the helper tolerates any number of bracketed prefixes between the
// timestamp and the tree — specifically: the LOG_ELAPSED duration field
// ("[  pending ]" or a patched duration) and the LOG_ADDR field
// ("addr: [0x...]"). The tree characters are unambiguous on a valid trace line
// because none of the prefix content ever contains those exact 3-char sequences.
int count_indentation_depth(const std::string& line) {
    size_t pos = 0;
    while (pos + 2 < line.size() && !is_tree_element(line, pos)) {
        ++pos;
    }
    int depth = 0;
    while (is_tree_element(line, pos)) {
        ++depth;
        pos += 3;
    }
    return depth;
}

// The character of the line's tree glyph: '|' for a frame that returned normally,
// '!' or '~' once LOG_EXCEPTIONS marked it (see utils::FRAME_END_* in format.h),
// '\0' for a line without a glyph (depth 0).
char tree_glyph_of(const std::string& line) {
    size_t pos = 0;
    while (pos + 2 < line.size() && !is_tree_element(line, pos)) {
        ++pos;
    }
    while (is_tree_element(line, pos)) {
        if (line[pos + 1] == '_' || line[pos + 1] == '!') return line[pos];
        pos += 3;
    }
    return '\0';
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

// Verify recursion: three nested frames of the SAME function must each get
// their own entry, one indentation level deeper than the previous. Guards the
// LIFO frame-resolution stack under same-callee stacking — the case a
// callee-address-based self-heal could never disambiguate.
TEST_F(IntegrationTest, RecursionProducesOneEntryPerLevel) {
    std::vector<int> depths;
    for (const auto& line : trace_lines) {
        if (line.find("recursive_countdown") != std::string::npos
            && line.find("(called from:") != std::string::npos) {
            depths.push_back(count_indentation_depth(line));
        }
    }
    ASSERT_EQ(depths.size(), 3u)
            << "Expected exactly 3 recursive_countdown entries (n=3), got " << depths.size();
    for (std::size_t i = 1; i < depths.size(); ++i) {
        EXPECT_EQ(depths[i], depths[i - 1] + 1)
                << "Recursive frame " << i << " should be exactly one level deeper "
                << "than its parent (got depth " << depths[i] << " after " << depths[i - 1] << ")";
    }
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

// Verify internal-linkage callees are traced and named. None of these has a
// dynamic symbol (dladdr() yields dli_sname == nullptr), so the resolver must
// fall through to the symbol table / DWARF lookup instead of dropping the frame.
// The lambda's operator() is spelled differently by the two compilers —
// "main::{lambda(int)#1}::operator()(int) const" on GCC,
// "main::$_0::operator()(int) const" on Clang — so the check accepts both.
TEST_F(IntegrationTest, InternalLinkageFunctionsResolved) {
    EXPECT_NE(trace_content.find("|_ file_static_func(int)"), std::string::npos)
            << "static function missing from trace:\n" << trace_content;
    EXPECT_NE(trace_content.find("|_ (anonymous namespace)::anon_ns_func(int)"), std::string::npos)
            << "anonymous-namespace function missing from trace:\n" << trace_content;
    std::regex lambda_pattern(
            R"(\|_ main::(\{lambda\(int\)#1\}|\$_[0-9]+)::operator\(\)\(int\) const  \(called from:)");
    EXPECT_TRUE(std::regex_search(trace_content, lambda_pattern))
            << "lambda operator() missing from trace:\n" << trace_content;
}

// Verify trace has exactly the expected number of function entries.
// The traced program produces 16 user-defined function entries. Without std
// library filtering, Clang would produce hundreds of entries from template
// instantiations.
TEST_F(IntegrationTest, ExactTraceLineCount) {
    int entry_count = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") != std::string::npos) {
            entry_count++;
        }
    }
    // Expected: main, func_a, func_b, func_c, static_method, TracedClass (ctor),
    // instance_method, template_func, inline_func, func_with_stl,
    // recursive_countdown x3, file_static_func, anon_ns_func, the lambda's
    // operator() = 16 entries.
    EXPECT_EQ(entry_count, 16)
            << "Expected exactly 16 function entries; got " << entry_count
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

// O_NOFOLLOW hardening: when CSLG_OUTPUT_FILE is a symlink, open() must refuse
// to follow it (ELOOP). The program still runs to completion, emits the
// documented warning, and the symlink's target stays untouched.
TEST(BadOutputPathTest, SymlinkOutputPathIsRejectedAndWarns) {
    char dir_tmpl[] = "/tmp/cslg_symlink_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string target = dir + "/target.out";
    const std::string link = dir + "/link.out";
    { std::ofstream ofs(target); } // create the (empty) symlink target
    ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0) << "symlink() failed";

    char stderr_tmpl[] = "/tmp/cslg_symlink_stderr_XXXXXX";
    int fd = mkstemp(stderr_tmpl);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + link + "\" \"" + TRACED_PROGRAM_PATH
                    + "\" > /dev/null 2> \"" + stderr_tmpl + "\"";
    int ret = system(cmd.c_str());
    EXPECT_EQ(ret, 0) << "traced_test_program exited non-zero with symlinked path: " << ret;

    std::string err_content = read_file(stderr_tmpl);
    unlink(stderr_tmpl);

    EXPECT_NE(err_content.find("[call-stack-logger] WARNING"), std::string::npos)
            << "Expected stderr warning for a symlinked output path. stderr was:\n" << err_content;
    EXPECT_NE(err_content.find(link), std::string::npos)
            << "Expected the attempted path in the warning. stderr was:\n" << err_content;
    // The target must not have been written through the symlink.
    EXPECT_TRUE(read_file(target).empty())
            << "Symlink target received data — O_NOFOLLOW protection has regressed.";

    remove_dir_tree(dir);
}

// ============================================================================
// Stripped-caller regression test — the callback's caller address lives in a
// shared library with no .symtab and no debug info (stripped post-build), in a
// file-local function not covered by .dynsym. dladdr() then yields no symbol
// name and the symbolizer finds neither a symbol nor a line.
// resolve_filename_and_line() once looped forever on exactly this input
// (holding the resolver's mutex), hanging the traced program. The program runs
// under `timeout`, so a regression shows up as exit code 124 instead of hanging
// the test suite. (The symbolizer searches only local separate-debug locations
// — next to the file, .debug/, /usr/lib/debug — and never contacts a
// debuginfod server.)
// ============================================================================

TEST(StrippedCallerTest, CallerInStrippedLibraryDoesNotHang) {
    char tmp_path[] = "/tmp/cslg_stripped_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path)
                    + "\" timeout 10 \"" + STRIPPED_CALLER_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    // timeout(1) exits 124 when it had to kill the program — the historical
    // infinite-loop failure mode of resolve_filename_and_line().
    EXPECT_EQ(ret, 0) << "cslg_stripped_caller_program did not finish cleanly (raw status="
                      << ret << "; status 124<<8 means killed by timeout — the "
                         "caller-resolution hang has regressed)";

    std::string content = read_file(tmp_path);
    unlink(tmp_path);

    // The callback must still be traced; its caller degrades gracefully (no
    // symbol, no line info in the stripped .so: "<unknown function>:???").
    EXPECT_NE(content.find("traced_callback"), std::string::npos)
            << "traced_callback entry missing from trace. Trace:\n" << content;
}

// ============================================================================
// dlopen()/chdir() regression test — the plugin is loaded by a RELATIVE path
// and first entered after the host chdir()ed to "/". dladdr() reports that
// relative string as dli_fname; the resolver used to open that string
// at first sight of a plugin address, i.e. from the new working directory —
// opening nothing (every plugin frame became "<could not open object file>")
// or, with a same-named file there, an unrelated object whose symbol names
// and lines were then reported. The object file must instead be found via
// its mapping's absolute path in /proc/self/maps.
// ============================================================================

TEST(DlopenPluginTest, RelativeDlopenPathResolvesAfterChdir) {
    char tmp_path[] = "/tmp/cslg_dlopen_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                    + DLOPEN_TRACED_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "dlopen_traced_program failed, exit=" << ret;

    std::string content = read_file(tmp_path);
    unlink(tmp_path);

    EXPECT_EQ(content.find("<could not open"), std::string::npos)
            << "plugin object file not found after chdir. Trace:\n" << content;
    EXPECT_NE(content.find("dlopen_plugin_entry"), std::string::npos)
            << "dlopen_plugin_entry missing from trace. Trace:\n" << content;

    // The helper's name AND its caller location must both come from the real
    // plugin's symbol table / DWARF, not from whatever "./<file>" names now.
    bool helper_found = false;
    for (const std::string& line : split_lines(content)) {
        if (line.find("dlopen_plugin_helper") == std::string::npos) {
            continue;
        }
        helper_found = true;
        EXPECT_NE(line.find("dlopen_plugin.cpp:"), std::string::npos)
                << "helper's caller not attributed to the plugin source: " << line;
    }
    EXPECT_TRUE(helper_found) << "dlopen_plugin_helper missing from trace. Trace:\n" << content;
}

// ============================================================================
// Deep-recursion test — call depth beyond the per-thread frame stack's initial
// capacity (INITIAL_FRAME_CAPACITY = 2048 in trace.cpp). The stack must grow
// on demand so every frame keeps its own record; the driver recurses 3000 deep
// and then calls a marker function, which must trace at the same depth as the
// first recursion frame (both are direct children of main).
// ============================================================================

TEST(OverflowDepthTest, DepthBeyondMaxStaysConsistent) {
    char tmp_path[] = "/tmp/cslg_overflow_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                    + OVERFLOW_DEPTH_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "overflow_depth_program failed, exit=" << ret;

    std::string content = read_file(tmp_path);
    unlink(tmp_path);
    std::vector<std::string> lines = split_lines(content);

    // Every enter writes exactly one timestamped line, even past the overflow
    // limit (deep lines are clamped by format()'s buffer and lose their tail,
    // but the timestamp prefix always survives). Expected:
    // main + 3000 recursion frames + post_overflow_marker = 3002.
    std::regex ts_prefix(R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    int entry_lines = 0;
    for (const auto& line : lines) {
        if (std::regex_search(line, ts_prefix)) ++entry_lines;
    }
    EXPECT_EQ(entry_lines, 3002)
            << "Expected one trace line per enter (main + 3000 recursion + marker).";

    // Depth proof: the marker (traced AFTER all 3000 exits, ~950 of which popped
    // records that only exist because the frame stack grew) must sit at the
    // same depth as the first recursion frame — both are direct children of main.
    int first_recursion_depth = -1;
    int marker_depth = -1;
    for (const auto& line : lines) {
        if (first_recursion_depth < 0 && line.find("deep_recursion") != std::string::npos) {
            first_recursion_depth = count_indentation_depth(line);
        }
        if (line.find("post_overflow_marker") != std::string::npos) {
            marker_depth = count_indentation_depth(line);
        }
    }
    ASSERT_GE(first_recursion_depth, 0) << "first deep_recursion line not found";
    ASSERT_GE(marker_depth, 0) << "post_overflow_marker line not found";
    EXPECT_EQ(marker_depth, first_recursion_depth)
            << "post_overflow_marker traced at depth " << marker_depth
            << " but the first deep_recursion frame was at depth "
            << first_recursion_depth
            << " — deep recursion desynchronized the depth accounting.";
}

// ============================================================================
// Deep recursion with an UNLOGGED frame at every level. Each level calls into a
// stripped, instrumented shared library whose file-local helper has no symbol
// left for dladdr() or the symbolizer, so its enter hook fires but the resolver drops the
// frame and no line is written. Beyond the frame stack's initial capacity, a
// third of the frames are therefore "entered but not logged" — the regression
// this pins: a fixed-capacity stack that assumed every deep frame
// was logged decremented the depth once per filtered frame, pushed it
// thousands of levels negative, and rendered every later line on the thread
// at depth 0. With per-frame records at any depth, the markers must trace at
// their true depths: marker_a and marker_b level with the first recursion
// frame, marker_b_child one level deeper.
// ============================================================================

TEST(FilteredOverflowTest, UnloggedFramesBeyondInitialCapacityKeepDepthExact) {
    char tmp_path[] = "/tmp/cslg_filtered_overflow_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                    + FILTERED_OVERFLOW_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "filtered_overflow_program failed, exit=" << ret;

    std::string content = read_file(tmp_path);
    unlink(tmp_path);
    std::vector<std::string> lines = split_lines(content);

    // Premise check: the helper really is unlogged. Every OTHER enter writes
    // exactly one timestamped line: main + 3000 recursion frames + 2999 calls
    // into the library (logged by their dynamic symbol; the recursion's base
    // level returns before calling it) + three markers = 6003.
    EXPECT_EQ(content.find("hidden_helper"), std::string::npos)
            << "hidden_helper was logged — the test premise (an unlogged frame at "
               "every recursion level) no longer holds.";
    std::regex ts_prefix(R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    int entry_lines = 0;
    for (const auto& line : lines) {
        if (std::regex_search(line, ts_prefix)) ++entry_lines;
    }
    EXPECT_EQ(entry_lines, 6003)
            << "Expected one trace line per logged enter (main + 3000 recursion + 2999 "
               "library calls + 3 markers).";

    int first_recursion_depth = -1;
    int marker_a_depth = -1;
    int marker_b_depth = -1;
    int marker_b_child_depth = -1;
    for (const auto& line : lines) {
        if (first_recursion_depth < 0 && line.find("deep_recursion") != std::string::npos) {
            first_recursion_depth = count_indentation_depth(line);
        } else if (line.find("marker_a") != std::string::npos) {
            marker_a_depth = count_indentation_depth(line);
        } else if (line.find("marker_b_child") != std::string::npos) {
            marker_b_child_depth = count_indentation_depth(line);
        } else if (line.find("marker_b") != std::string::npos) {
            marker_b_depth = count_indentation_depth(line);
        }
    }
    ASSERT_GE(first_recursion_depth, 0) << "first deep_recursion line not found";
    ASSERT_GE(marker_a_depth, 0) << "marker_a line not found";
    ASSERT_GE(marker_b_depth, 0) << "marker_b line not found";
    ASSERT_GE(marker_b_child_depth, 0) << "marker_b_child line not found";
    EXPECT_EQ(marker_a_depth, first_recursion_depth)
            << "marker_a traced at depth " << marker_a_depth
            << " but the first deep_recursion frame was at depth " << first_recursion_depth
            << " — unlogged deep frames drifted the depth accounting.";
    EXPECT_EQ(marker_b_depth, first_recursion_depth)
            << "marker_b traced at depth " << marker_b_depth
            << " but the first deep_recursion frame was at depth " << first_recursion_depth;
    EXPECT_EQ(marker_b_child_depth, first_recursion_depth + 1)
            << "marker_b_child traced at depth " << marker_b_child_depth
            << " but should be one level below marker_b (depth " << marker_b_depth << ")";
}

// ============================================================================
// Exception-unwind pairing test, both compilers. GCC's -finstrument-functions
// emits __cyg_profile_func_exit on the exceptional path too (as a cleanup), so
// a throw through instrumented frames leaves the depth counter intact by
// itself. Clang runs no exit hook for the unwound frames; the frame
// reconciliation (include/frameReconcile.h, on in every build) reclaims their
// stale records at the next hook, so the marker after the catch sits at its
// true depth there too.
// ============================================================================

TEST(ExceptionUnwindTest, DepthConsistentAfterCatch) {
    char tmp_path[] = "/tmp/cslg_exception_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                    + EXCEPTION_TRACED_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "exception_traced_program failed, exit=" << ret;

    std::string content = read_file(tmp_path);
    unlink(tmp_path);
    std::vector<std::string> lines = split_lines(content);

    // The frames unwound by the throw were still ENTERED normally.
    EXPECT_NE(content.find("exception_thrower"), std::string::npos)
            << "exception_thrower entry missing. Trace:\n" << content;
    EXPECT_NE(content.find("exception_mid"), std::string::npos)
            << "exception_mid entry missing";

    // Pairing proof: after the catch, the marker must trace at the same depth
    // as exception_catcher (both are direct children of main). GCC ran the exit
    // hooks of the two unwound frames; Clang ran none, and their stale records
    // would shift the marker two levels deeper if the reconciliation had not
    // reclaimed them at the marker's enter.
    int catcher_depth = -1;
    int marker_depth = -1;
    for (const auto& line : lines) {
        if (line.find("exception_catcher") != std::string::npos) {
            catcher_depth = count_indentation_depth(line);
        }
        if (line.find("post_catch_marker") != std::string::npos) {
            marker_depth = count_indentation_depth(line);
        }
    }
    ASSERT_GE(catcher_depth, 0) << "exception_catcher line not found";
    ASSERT_GE(marker_depth, 0) << "post_catch_marker line not found";
    EXPECT_EQ(marker_depth, catcher_depth)
            << "post_catch_marker depth " << marker_depth << " != exception_catcher depth " << catcher_depth
            << " — the frames the exception unwound were not reclaimed. Trace:\n" << content;
}

// ============================================================================
// longjmp() over instrumented frames: their exit hooks never run, on either
// compiler. The reconciliation reclaims the jumped-over records at the next
// hook, so the calls that follow a jump sit at their true depth (README,
// "Compiler-specific instrumentation"; longjmp_traced_program.cpp).
// ============================================================================

namespace {

// Depth of the first line whose function name contains `function`, or -1.
int first_depth_of(const std::vector<std::string>& lines, const std::string& function) {
    for (const auto& line : lines) {
        if (line.find(function) != std::string::npos) {
            return count_indentation_depth(line);
        }
    }
    return -1;
}

} // namespace

TEST(LongjmpTest, CallsAfterAJumpSitAtTheirTrueDepth) {
    char tmp_path[] = "/tmp/cslg_longjmp_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                    + LONGJMP_TRACED_PROGRAM_PATH + "\" > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "longjmp_traced_program failed, exit=" << ret;

    std::string content = read_file(tmp_path);
    unlink(tmp_path);
    std::vector<std::string> lines = split_lines(content);

    // main -> jump_outer(1) -> jump_mid(2) -> jump_leaf(3) jumps back into
    // jump_outer, which then calls post_jump_marker: a child of jump_outer, not
    // of the two frames the jump skipped.
    EXPECT_EQ(first_depth_of(lines, "jump_outer"), 1) << content;
    EXPECT_EQ(first_depth_of(lines, "jump_mid"), 2) << content;
    EXPECT_EQ(first_depth_of(lines, "jump_leaf"), 3) << content;
    EXPECT_EQ(first_depth_of(lines, "post_jump_marker"), 2)
            << "the call after the jump must sit under jump_outer:\n" << content;

    // A retry loop: jump_loop(1) calls jump_loop_leaf(2) from one call site three
    // times, each jumping back out; every re-entry and the marker after the loop
    // sit at their true depth.
    EXPECT_EQ(first_depth_of(lines, "jump_loop()"), 1) << content;
    int leaf_lines = 0;
    for (const auto& line : lines) {
        if (line.find("jump_loop_leaf") != std::string::npos) {
            ++leaf_lines;
            EXPECT_EQ(count_indentation_depth(line), 2) << line;
        }
    }
    EXPECT_EQ(leaf_lines, 3) << content;
    EXPECT_EQ(first_depth_of(lines, "loop_marker"), 2) << "the call after the loop sits under jump_loop:\n"
                                                        << content;
    EXPECT_EQ(first_depth_of(lines, "final_marker"), 1) << content;
}

// ============================================================================
// Public API test — exercises instrumentation::get_call_stack()
// ============================================================================

// Run the callstack API program, capture stdout, and verify the emitted stack
// contains the expected ancestor functions. Also pins the lazy-open behavior:
// a non-instrumented program that only uses the on-demand API must NOT create
// any trace file (trace files are opened lazily on the first traced call, and
// this program never triggers a hook).
TEST(CallStackApiTest, GetCallStackResolvesAncestors) {
    char dir_tmpl[] = "/tmp/cslg_cs_api_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string trace_path = dir + "/trace.out";
    const std::string stdout_path = dir + "/stdout.txt";

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_path + "\" \""
                    + CALLSTACK_API_PROGRAM_PATH + "\" > \"" + stdout_path + "\"";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "callstack_api_program failed, exit=" << ret;

    std::string content = read_file(stdout_path);

    // No hooks ever fired, so no trace file may exist — a regression here means
    // the library went back to eagerly opening the main thread's file, spamming
    // API-only consumers with stray trace.out files.
    struct stat st;
    EXPECT_NE(stat(trace_path.c_str(), &st), 0)
            << "API-only (non-instrumented) program created a trace file — "
               "the lazy main-thread open has regressed.";

    remove_dir_tree(dir);

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

    // Verify per-frame caller info is correct (regression guard for the historical
    // bug where get_call_stack() went through a resolver path that walked a fixed
    // number of stack frames, so every frame reported the SAME caller regardless
    // of which frame was being described).
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

// get_call_stack() must work from worker threads too — the two workers in the
// API program capture their stacks CONCURRENTLY, contending on s_resolver_mutex
// from threads that never ran trace_begin(). Each worker's chain must resolve
// with the innermost frame first, independently of the other worker.
TEST(CallStackApiTest, GetCallStackResolvesFromConcurrentWorkerThreads) {
    char dir_tmpl[] = "/tmp/cslg_cs_worker_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string stdout_path = dir + "/stdout.txt";

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + dir + "/trace.out\" \""
                    + CALLSTACK_API_PROGRAM_PATH + "\" > \"" + stdout_path + "\"";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "callstack_api_program failed, exit=" << ret;

    std::string content = read_file(stdout_path);
    remove_dir_tree(dir);

    for (const std::string prefix : { "W1_", "W2_" }) {
        const auto pos_leaf = content.find(prefix + "FRAME: worker_stack_leaf");
        const auto pos_top = content.find(prefix + "FRAME: worker_stack_top");
        EXPECT_NE(pos_leaf, std::string::npos)
                << prefix << "worker did not resolve worker_stack_leaf. Output:\n" << content;
        EXPECT_NE(pos_top, std::string::npos)
                << prefix << "worker did not resolve worker_stack_top. Output:\n" << content;
        if (pos_leaf != std::string::npos && pos_top != std::string::npos) {
            EXPECT_LT(pos_leaf, pos_top)
                    << prefix << "worker's stack is not innermost-first. Output:\n" << content;
        }
    }
}

// The on-demand API and the per-call hooks must coexist in one process: the
// same program source compiled WITH -finstrument-functions calls
// get_call_stack() (which takes s_resolver_mutex outside any hook) while the hooks
// fire around it. Verifies both outputs: the printed stack (API worked) and
// the trace file (hooks worked). Uses a temp DIRECTORY because the program's
// worker threads produce their own <trace>_tid_<n> files that need cleanup.
TEST(CallStackApiTest, WorksFromInstrumentedProgram) {
    char dir_tmpl[] = "/tmp/cslg_cs_instr_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string trace_path = dir + "/trace.out";
    const std::string stdout_path = dir + "/stdout.txt";

    // timeout(1): a regression of the re-entrancy guard on the public API
    // deadlocks on s_resolver_mutex under Clang (hook firing inside the resolver via
    // user-TU COMDAT template instantiations) — fail fast with status 124<<8
    // instead of hanging the suite.
    std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_path + "\" timeout 10 \""
                    + CALLSTACK_API_PROGRAM_INSTRUMENTED_PATH + "\" > \"" + stdout_path + "\"";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "callstack_api_program_instrumented failed, exit=" << ret
                      << " (raw status 124<<8 means killed by timeout — the public-API "
                         "re-entrancy guard deadlock has regressed)";

    std::string out = read_file(stdout_path);
    std::string trace = read_file(trace_path);
    remove_dir_tree(dir);

    // The on-demand API resolved the ancestor chain...
    EXPECT_NE(out.find("FRAME: print_stack_from_leaf"), std::string::npos)
            << "API output missing print_stack_from_leaf. Output:\n" << out;
    EXPECT_NE(out.find("FRAME: callstack_mid"), std::string::npos)
            << "API output missing callstack_mid";
    EXPECT_NE(out.find("FRAME: callstack_top"), std::string::npos)
            << "API output missing callstack_top";

    // ...while the hooks traced the same functions into the trace file.
    EXPECT_NE(trace.find("callstack_top"), std::string::npos)
            << "Trace file missing callstack_top — hooks did not run. Trace:\n" << trace;
    EXPECT_NE(trace.find("callstack_mid"), std::string::npos)
            << "Trace file missing callstack_mid — hooks did not run";

    // The library's own value type must not leak into the trace. ResolvedFrame's
    // special members are emitted in the user's (instrumented) TU, so without their
    // explicit NO_INSTRUMENT declarations in types.h every frame the program
    // destroyed produced a "ResolvedFrame::~ResolvedFrame()" line on both compilers.
    EXPECT_EQ(trace.find("ResolvedFrame::~ResolvedFrame"), std::string::npos)
            << "ResolvedFrame destructor was instrumented in the user's TU. Trace:\n" << trace;
    EXPECT_EQ(trace.find("ResolvedFrame::ResolvedFrame"), std::string::npos)
            << "ResolvedFrame constructor was instrumented in the user's TU. Trace:\n" << trace;
    EXPECT_EQ(trace.find("ResolvedFrame::operator="), std::string::npos)
            << "ResolvedFrame assignment was instrumented in the user's TU. Trace:\n" << trace;
}

// ============================================================================
// Build-flag tests — exercise the LOG_ADDR CMake option via a library variant
// built with the macro defined.
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

// ============================================================================
// LOG_ELAPSED build-flag tests
//
// Each line produced under -DLOG_ELAPSED=ON carries a 12-byte fixed-width
// duration field "[  1.234us]" / "[ 123.456ms]" / etc. between the timestamp
// and whatever follows (addr / tree / name / caller). On function exit the
// logger pwrites the formatted elapsed duration on top of the "[  pending ]"
// placeholder written at enter. These tests drive a dedicated program
// (log_elapsed_traced_program.cpp) that has a predictable nested call chain
// (elapsed_outer -> elapsed_middle -> elapsed_inner) with a usleep(10000)
// sentinel at the deepest frame.
// ============================================================================

namespace {

// Regex for any valid duration field: "[ 1.234ns]" / "[ 123.456us]" / ...
// Trailing ']' is preceded by 'n' / 'u' / 'm' / ' ' (space in the s-unit case).
// Saturation sentinel "[  >999.9s ]" matches via the ".9s " tail.
const std::regex& duration_field_regex() {
    // Formats we emit: "[ 123.456ns]", "[ 123.456us]", "[ 123.456ms]",
    // "[ 123.456s ]", or the saturation "[  >999.9s ]".
    static const std::regex r(R"(\[[ >0-9.]{8}(ns|us|ms|s )\])");
    return r;
}

// Extract the duration (in nanoseconds) from the 12-byte field occurring
// AFTER the timestamp on a LOG_ELAPSED trace line. Returns -1 if no match.
// Used for monotonicity and sentinel assertions.
//
// Parses whole and fractional parts as integers (via std::stoll) rather than
// std::stod so this helper is locale-independent: the trace file uses '.' as
// the decimal separator regardless of LC_NUMERIC (see format_duration_12chars
// in durationFormat.h), and std::stod would misparse that as an integer on
// hosts where LC_NUMERIC expects ','. Integer conversion has no such coupling.
long long parse_duration_ns(const std::string& line) {
    std::smatch m;
    if (!std::regex_search(line, m, duration_field_regex())) return -1;
    const std::string field = m.str();
    if (field.size() != 12) return -1;
    if (field.substr(1, 10).find('>') != std::string::npos) {
        return std::numeric_limits<long long>::max();
    }
    // Field layout produced by format_duration_12chars:
    //   [ <4-char whole> . <3-char frac> <2-char unit> ]
    //   positions: 0='[', 1..4=whole, 5='.', 6..8=frac, 9..10=unit, 11=']'
    try {
        const std::string whole_str = field.substr(1, 4);
        const std::string frac_str = field.substr(6, 3);
        const std::string unit = field.substr(9, 2);
        const long long whole = std::stoll(whole_str);   // tolerates leading spaces
        const long long frac = std::stoll(frac_str);
        long long scale_whole = 0;
        long long scale_frac = 0;
        if (unit == "ns") { scale_whole = 1; scale_frac = 0; }
        else if (unit == "us") { scale_whole = 1'000; scale_frac = 1; }
        else if (unit == "ms") { scale_whole = 1'000'000; scale_frac = 1'000; }
        else if (unit == "s ") { scale_whole = 1'000'000'000; scale_frac = 1'000'000; }
        else return -1;
        return whole * scale_whole + frac * scale_frac;
    } catch (...) {
        return -1;
    }
}

// Find the single line that matches a function-name substring (restricted to
// lines that also contain "(called from:" so we ignore the run-separator).
// Returns "" if not found or if multiple matches (ambiguous).
std::string find_unique_line(const std::vector<std::string>& lines, const std::string& name) {
    std::string found;
    int matches = 0;
    for (const auto& line : lines) {
        if (line.find(name) != std::string::npos &&
            line.find("(called from:") != std::string::npos) {
            found = line;
            ++matches;
        }
    }
    return (matches == 1) ? found : "";
}

} // namespace

// Test fixture that runs the LOG_ELAPSED-enabled program and captures output.
class LogElapsedFlagTest : public ::testing::Test {
protected:
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string trace_file_path;

    void SetUp() override {
        char tmp_path[] = "/tmp/cslg_elapsed_test_XXXXXX";
        int fd = mkstemp(tmp_path);
        ASSERT_GE(fd, 0) << "mkstemp failed";
        close(fd);
        trace_file_path = tmp_path;

        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_file_path + "\" \""
                        + TRACED_PROGRAM_LOG_ELAPSED_PATH + "\" > /dev/null 2>&1";
        int ret = system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "traced_test_program_log_elapsed failed, exit=" << ret;

        trace_content = read_file(trace_file_path);
        trace_lines = split_lines(trace_content);
    }

    void TearDown() override {
        if (!trace_file_path.empty()) {
            unlink(trace_file_path.c_str());
        }
    }
};

// Every function-entry line must carry a valid 12-byte duration field.
TEST_F(LogElapsedFlagTest, EveryLineHasDurationField) {
    int total_entries = 0;
    int with_duration = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++total_entries;
        if (std::regex_search(line, duration_field_regex())) ++with_duration;
    }
    ASSERT_GT(total_entries, 0) << "no function entries in trace";
    EXPECT_EQ(with_duration, total_entries)
            << "Expected every entry to carry a duration field. Got "
            << with_duration << "/" << total_entries << ". Trace:\n" << trace_content;
}

// After a clean exit, no "[  pending ]" placeholders must remain on disk.
TEST_F(LogElapsedFlagTest, NoPendingLeftovers) {
    EXPECT_EQ(trace_content.find("[  pending ]"), std::string::npos)
            << "Found un-patched placeholder after clean exit. Exit-side pwrite "
               "probably didn't run for some frame.\nTrace:\n" << trace_content;
}

// The nested call chain "elapsed_outer -> elapsed_middle -> elapsed_inner"
// must show parent durations >= child durations (up to a small jitter margin).
TEST_F(LogElapsedFlagTest, DurationsAreMonotonicallySensible) {
    std::string l_outer = find_unique_line(trace_lines, "elapsed_outer");
    std::string l_middle = find_unique_line(trace_lines, "elapsed_middle");
    std::string l_inner = find_unique_line(trace_lines, "elapsed_inner");

    ASSERT_FALSE(l_outer.empty()) << "elapsed_outer line missing or not unique";
    ASSERT_FALSE(l_middle.empty()) << "elapsed_middle line missing or not unique";
    ASSERT_FALSE(l_inner.empty()) << "elapsed_inner line missing or not unique";

    long long d_outer = parse_duration_ns(l_outer);
    long long d_middle = parse_duration_ns(l_middle);
    long long d_inner = parse_duration_ns(l_inner);

    ASSERT_GE(d_outer, 0) << "failed to parse elapsed_outer duration: " << l_outer;
    ASSERT_GE(d_middle, 0) << "failed to parse elapsed_middle duration: " << l_middle;
    ASSERT_GE(d_inner, 0) << "failed to parse elapsed_inner duration: " << l_inner;

    // Parent >= child. The 3-decimal rounding of the displayed format can
    // produce ties when durations are very close — hence GE, not GT.
    EXPECT_GE(d_outer, d_middle)
            << "elapsed_outer (" << d_outer << "ns) < elapsed_middle ("
            << d_middle << "ns) — parent should be >= child";
    EXPECT_GE(d_middle, d_inner)
            << "elapsed_middle (" << d_middle << "ns) < elapsed_inner ("
            << d_inner << "ns) — parent should be >= child";
}

// elapsed_inner's reported duration must be >= 10 ms (the usleep sentinel),
// and not unreasonably large (< 5 s catches any catastrophic clock/offset
// corruption, with generous room for CI scheduling jitter).
TEST_F(LogElapsedFlagTest, UsleepSentinelMatches) {
    std::string line = find_unique_line(trace_lines, "elapsed_inner");
    ASSERT_FALSE(line.empty()) << "elapsed_inner line missing or not unique";

    long long ns = parse_duration_ns(line);
    ASSERT_GE(ns, 0) << "failed to parse duration from: " << line;

    constexpr long long TEN_MS_NS = 10'000'000LL;
    constexpr long long FIVE_SEC_NS = 5'000'000'000LL;
    EXPECT_GE(ns, TEN_MS_NS)
            << "elapsed_inner should sleep >= 10 ms, got " << ns
            << " ns. Line:\n" << line;
    EXPECT_LT(ns, FIVE_SEC_NS)
            << "elapsed_inner exceeded 5 s — clock corruption? Got " << ns
            << " ns. Line:\n" << line;
}

// Every LOG_ELAPSED line must parse cleanly — timestamp, duration, optional
// addr, tree, name, caller — and pwrite must not have scribbled bytes
// outside the 12-byte field. Checks the overall line shape via a single
// regex covering the three common layouts (with / without addr / with tree).
TEST_F(LogElapsedFlagTest, NoFormatCorruption) {
    // Pattern: "[<ts>] [<dur>] (tree)? <name> (called from: <file>:<line>)"
    // Tree and name are hard to constrain tightly in regex; we validate
    // the fixed-layout prefix (timestamp + duration) and the trailing
    // "(called from: ...)" and trust the middle.
    std::regex full(R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[[ >0-9.]{8}(ns|us|ms|s )\] .*\(called from: .+:.+\)$)");

    int entries = 0;
    int matched = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++entries;
        if (std::regex_match(line, full)) ++matched;
        else {
            ADD_FAILURE() << "Malformed LOG_ELAPSED line (pwrite may have corrupted "
                             "byte boundaries):\n" << line;
        }
    }
    ASSERT_GT(entries, 0) << "no function entries in trace";
    EXPECT_EQ(matched, entries);
}

// Negative test: the DEFAULT (non-LOG_ELAPSED) traced_test_program must NOT
// include a duration field. Guards against the macro accidentally becoming
// always-on. Skipped when the build was configured with -DLOG_ELAPSED=ON,
// in which case the default callstacklogger target also defines LOG_ELAPSED.
//
// Lives in its own suite (not LogElapsedFlagTest) because gtest forbids
// mixing TEST_F (fixture-bound) and TEST (free) in the same suite.
TEST(LogElapsedDefaultBuildTest, NoDurationFieldWithoutFlag) {
#ifdef CSLG_DEFAULT_HAS_LOG_ELAPSED
    GTEST_SKIP() << "Skipped: configured with -DLOG_ELAPSED=ON, so the default "
                    "callstacklogger target also defines LOG_ELAPSED.";
#else
    std::string content = run_and_capture_trace(TRACED_PROGRAM_PATH);
    ASSERT_FALSE(content.empty()) << "trace file is empty";

    EXPECT_FALSE(std::regex_search(content, duration_field_regex()))
            << "Default build unexpectedly carries a duration field — LOG_ELAPSED "
               "macro may be leaking into the default callstacklogger target.";
    EXPECT_EQ(content.find("[  pending ]"), std::string::npos)
            << "Default build unexpectedly has a pending placeholder.";
#endif
}

// Negative test: the DEFAULT build (no LOG_EXCEPTIONS) neither traces exception
// events nor marks the lines of the frames an exception left — the interposers
// and the in-place marks exist only with the option. The exception driver
// throws and catches through instrumented frames; its trace must show plain
// call lines only. Guards against the option accidentally becoming always-on,
// like the LOG_ADDR / LOG_ELAPSED negative tests. Skipped when the build was
// configured with -DLOG_EXCEPTIONS=ON.
TEST(LogExceptionsDefaultBuildTest, NoEventLinesOrMarksWithoutFlag) {
#ifdef CSLG_DEFAULT_HAS_LOG_EXCEPTIONS
    GTEST_SKIP() << "Skipped: configured with -DLOG_EXCEPTIONS=ON, so the default "
                    "callstacklogger target also defines LOG_EXCEPTIONS.";
#else
    std::string content = run_and_capture_trace(EXCEPTION_TRACED_PROGRAM_PATH);
    ASSERT_FALSE(content.empty()) << "trace file is empty";
    ASSERT_NE(content.find("exception_thrower"), std::string::npos)
            << "the driver's throw did not run. Trace:\n" << content;
    EXPECT_EQ(content.find("!! "), std::string::npos)
            << "Default build unexpectedly carries an exception event line — LOG_EXCEPTIONS "
               "may be leaking into the default callstacklogger target.\n" << content;
    for (const auto& line : split_lines(content)) {
        const char glyph = tree_glyph_of(line);
        EXPECT_TRUE(glyph == '\0' || glyph == '|')
                << "Default build unexpectedly marked a frame's line:\n" << line;
    }
#endif
}

// Multi-run append: a second run appends past the first run's content, so the
// per-thread byte cursor is seeded from lseek(SEEK_END) on a NON-empty file.
// Every placeholder in BOTH runs must still be patched at the correct offset —
// a mis-seeded cursor would leave pending placeholders or splice duration
// bytes into the first run's lines.
TEST(LogElapsedMultiRunAppendTest, SecondRunPatchesCorrectlyAfterAppend) {
    char tmp_path[] = "/tmp/cslg_elapsed_append_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT_GE(fd, 0) << "mkstemp failed";
    close(fd);

    const std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(tmp_path) + "\" \""
                          + TRACED_PROGRAM_LOG_ELAPSED_PATH + "\" > /dev/null 2>&1";
    ASSERT_EQ(system(cmd.c_str()), 0) << "first run failed";
    ASSERT_EQ(system(cmd.c_str()), 0) << "second run failed";

    std::string content = read_file(tmp_path);
    unlink(tmp_path);

    // Two run-separator headers — the file really contains both runs.
    const std::size_t first = content.find("=== New trace run:");
    ASSERT_NE(first, std::string::npos) << "no run separator found";
    EXPECT_NE(content.find("=== New trace run:", first + 1), std::string::npos)
            << "second run's separator missing — append did not happen";

    // No placeholder anywhere: both runs' exits patched their own lines.
    EXPECT_EQ(content.find("[  pending ]"), std::string::npos)
            << "Un-patched placeholder after multi-run append — the cursor "
               "seeding from the appended file's EOF has regressed.\nTrace:\n" << content;

    // Every entry line in both runs carries a well-formed duration field.
    int entries = 0;
    int with_duration = 0;
    for (const auto& line : split_lines(content)) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++entries;
        if (std::regex_search(line, duration_field_regex())) ++with_duration;
    }
    ASSERT_GT(entries, 0) << "no function entries in trace";
    EXPECT_EQ(with_duration, entries)
            << "Some lines lost their duration field after append. Trace:\n" << content;
}

// LOG_ELAPSED + multi-threading: each thread owns an independent cursor and
// patch_fd for its own file. After a clean run, every per-thread file (main +
// 4 workers) must be fully patched — a cross-thread mix-up of offsets or fds
// would leave pending placeholders or corrupt another thread's file.
TEST(LogElapsedThreadedTest, PerThreadFilesPatchIndependently) {
    char dir_tmpl[] = "/tmp/cslg_elapsed_threaded_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string tmpdir = d;
    const std::string base = "thread_trace.out";

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + tmpdir + "/" + base + "\" \""
                    + THREADED_TRACED_PROGRAM_LOG_ELAPSED_PATH + "\" > /dev/null 2>&1";
    ASSERT_EQ(system(cmd.c_str()), 0) << "threaded LOG_ELAPSED program failed";

    int worker_files = 0;
    for (const std::string& name : list_files_in(tmpdir)) {
        const bool is_main = (name == base);
        const bool is_worker = parse_tid_suffix(name, base) >= 0;
        if (!is_main && !is_worker) continue;
        if (is_worker) ++worker_files;

        const std::string content = read_file(tmpdir + "/" + name);
        EXPECT_EQ(content.find("[  pending ]"), std::string::npos)
                << "Un-patched placeholder in " << name << ":\n" << content;

        int entries = 0;
        int with_duration = 0;
        for (const auto& line : split_lines(content)) {
            if (line.find("(called from:") == std::string::npos) continue;
            ++entries;
            if (std::regex_search(line, duration_field_regex())) ++with_duration;
        }
        if (is_worker) {
            EXPECT_GT(entries, 0) << "worker file " << name << " has no entries";
        }
        EXPECT_EQ(with_duration, entries)
                << "File " << name << " has entries without a duration field:\n" << content;
    }
    EXPECT_EQ(worker_files, 4) << "expected 4 worker trace files";

    remove_dir_tree(tmpdir);
}

// ============================================================================
// Combined LOG_ELAPSED + LOG_ADDR — proves LOG_ELAPSED's
// fixed byte-offset math is independent of layout changes downstream of the
// timestamp (LOG_ADDR prepends "addr: [0x...]" before the tree). If the
// placeholder offset were derived from the line tail rather than from
// PRETTY_TIME_LENGTH, this combination would silently corrupt the addr column.
// ============================================================================

class LogElapsedCombinedFlagsTest : public ::testing::Test {
protected:
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string trace_file_path;

    void SetUp() override {
        char tmp_path[] = "/tmp/cslg_combo_test_XXXXXX";
        int fd = mkstemp(tmp_path);
        ASSERT_GE(fd, 0) << "mkstemp failed";
        close(fd);
        trace_file_path = tmp_path;

        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_file_path + "\" \""
                        + TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH
                        + "\" > /dev/null 2>&1";
        int ret = system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "combined traced program failed, exit=" << ret;

        trace_content = read_file(trace_file_path);
        trace_lines = split_lines(trace_content);
    }

    void TearDown() override {
        if (!trace_file_path.empty()) {
            unlink(trace_file_path.c_str());
        }
    }
};

// On every entry, the duration field must sit BETWEEN the timestamp and the
// "addr: [0x...]" column — not after it, not before it. Proves layout ordering.
TEST_F(LogElapsedCombinedFlagsTest, EveryLineHasDurationThenAddr) {
    // "[<ts>] [<dur>] addr: [0x<hex>] ..." — the duration field comes right
    // after the closing ']' of the timestamp.
    std::regex pattern(R"(\] \[[ >0-9.]{8}(ns|us|ms|s )\] addr: \[0x[0-9a-f]+\] )");
    int total_entries = 0;
    int matched = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++total_entries;
        if (std::regex_search(line, pattern)) ++matched;
        else {
            ADD_FAILURE() << "Combined-flag line missing the expected "
                             "<dur> then addr ordering:\n" << line;
        }
    }
    ASSERT_GT(total_entries, 0) << "no function entries in trace";
    EXPECT_EQ(matched, total_entries);
}

// The tree alignment column (where '|_' lives) must be at the same byte offset
// for every line at a given depth — proving the fixed-width 13-byte LOG_ELAPSED
// prefix preserves alignment under the combined LOG_ADDR layout.
TEST_F(LogElapsedCombinedFlagsTest, TreeColumnsStillAlign) {
    // Group lines by indentation depth and assert all lines at the same depth
    // share the same byte position for "|_". depth 0 = top-level frame which
    // has no '|_' — so we look only at depth >= 1 lines.
    std::map<int, std::vector<std::size_t>> positions_by_depth;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") == std::string::npos) continue;
        int depth = count_indentation_depth(line);
        if (depth < 1) continue;
        std::size_t pos = line.find("|_ ");
        if (pos == std::string::npos) continue;
        positions_by_depth[depth].push_back(pos);
    }

    ASSERT_FALSE(positions_by_depth.empty())
            << "no depth-1+ lines found — test assumes nested calls in the program";

    for (const auto& [depth, positions] : positions_by_depth) {
        ASSERT_FALSE(positions.empty());
        const std::size_t first = positions.front();
        for (std::size_t p : positions) {
            EXPECT_EQ(p, first)
                    << "Tree column at depth " << depth
                    << " not aligned: '|_' found at offset " << p
                    << " but expected " << first
                    << ". LOG_ELAPSED's 13-byte prefix may be drifting under "
                       "the combined LOG_ADDR layout.";
        }
    }
}

// No "[  pending ]" placeholders remain after a clean exit, even with all
// three flags on. This is the same invariant as in LogElapsedFlagTest, but
// re-checking it under the combined layout proves that addr-column patching
// hasn't accidentally shifted any per-frame offset.
TEST_F(LogElapsedCombinedFlagsTest, NoPendingLeftovers) {
    EXPECT_EQ(trace_content.find("[  pending ]"), std::string::npos)
            << "Pending placeholder remains under combined flags. Trace:\n"
            << trace_content;
}

// On every entry, the addr column must contain a plausible non-zero pointer
// value. If pwrite() ever stomped past its 12-byte field, the addr column —
// which sits 13 bytes after the placeholder start — would be the first thing
// to corrupt.
TEST_F(LogElapsedCombinedFlagsTest, PwriteDidNotCorruptAddr) {
    std::regex addr_pat(R"(addr: \[0x([0-9a-f]+)\])");
    int total_entries = 0;
    int valid_addrs = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") == std::string::npos) continue;
        ++total_entries;
        std::smatch m;
        if (!std::regex_search(line, m, addr_pat)) {
            ADD_FAILURE() << "addr column missing or malformed:\n" << line;
            continue;
        }
        // hex value must parse and be non-zero (zero would imply stomped bytes).
        const std::string hex = m[1].str();
        try {
            unsigned long long val = std::stoull(hex, nullptr, 16);
            if (val != 0) ++valid_addrs;
            else ADD_FAILURE() << "addr is zero — possible byte corruption:\n" << line;
        } catch (...) {
            ADD_FAILURE() << "addr hex unparseable — corruption:\n" << line;
        }
    }
    ASSERT_GT(total_entries, 0) << "no function entries in trace";
    EXPECT_EQ(valid_addrs, total_entries);
}

// ============================================================================
// LOG_ELAPSED crash diagnostics — README's flagship "[  pending ]" behavior.
//
// crash_traced_program_log_elapsed calls crash_completed_work() (returns
// normally), then descends crash_outer -> crash_middle -> crash_leaf and
// abort()s at the leaf. Exit hooks never run for the frames active at the
// crash, so their placeholders must survive on disk, while the completed
// frame's line must carry a real patched duration — the exact mixed picture
// a post-crash diagnosis relies on.
// ============================================================================

class LogElapsedCrashTest : public ::testing::Test {
protected:
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string trace_file_path;
    int run_status = -1;

    void SetUp() override {
        char tmp_path[] = "/tmp/cslg_elapsed_crash_XXXXXX";
        int fd = mkstemp(tmp_path);
        ASSERT_GE(fd, 0) << "mkstemp failed";
        close(fd);
        trace_file_path = tmp_path;

        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_file_path + "\" \""
                        + CRASH_PROGRAM_LOG_ELAPSED_PATH + "\" > /dev/null 2>&1";
        // Expected to terminate via SIGABRT (shell reports 128+6=134); asserted
        // non-zero in the test body rather than exactly 134 so sanitizer
        // runtimes that re-raise the abort differently don't cause false
        // failures.
        run_status = system(cmd.c_str());

        trace_content = read_file(trace_file_path);
        trace_lines = split_lines(trace_content);
    }

    void TearDown() override {
        if (!trace_file_path.empty()) {
            unlink(trace_file_path.c_str());
        }
    }
};

// Every frame still on the stack when abort() fired must keep its
// "[  pending ]" placeholder — their exit hooks never ran. Line-buffered mode
// guarantees the enter lines already reached the kernel before the crash.
TEST_F(LogElapsedCrashTest, FramesActiveAtCrashKeepPendingPlaceholders) {
    ASSERT_NE(run_status, 0)
            << "crash program exited cleanly — abort() did not fire, nothing to test";

    for (const char* fn : { " main ", "crash_outer", "crash_middle", "crash_leaf" }) {
        std::string line = find_unique_line(trace_lines, fn);
        ASSERT_FALSE(line.empty())
                << fn << " line missing or not unique. Trace:\n" << trace_content;
        EXPECT_NE(line.find("[  pending ]"), std::string::npos)
                << fn << " should have kept its placeholder (its exit hook never ran):\n"
                << line;
    }
}

// A frame that completed BEFORE the crash must have its duration patched in:
// the trace is a mix of patched and pending lines, and the pending tail is
// what identifies the frames active at the crash site.
TEST_F(LogElapsedCrashTest, FrameCompletedBeforeCrashIsPatched) {
    ASSERT_NE(run_status, 0)
            << "crash program exited cleanly — abort() did not fire, nothing to test";

    std::string line = find_unique_line(trace_lines, "crash_completed_work");
    ASSERT_FALSE(line.empty())
            << "crash_completed_work line missing or not unique. Trace:\n" << trace_content;
    EXPECT_EQ(line.find("[  pending ]"), std::string::npos)
            << "completed frame kept its placeholder:\n" << line;
    ASSERT_TRUE(std::regex_search(line, duration_field_regex()))
            << "completed frame has no valid duration field:\n" << line;
    // The function body usleep()s 1 ms, and usleep sleeps AT LEAST the
    // requested time — the patched duration must reflect that.
    EXPECT_GE(parse_duration_ns(line), 1'000'000LL)
            << "patched duration below the 1 ms usleep lower bound:\n" << line;
}

// ============================================================================
// Exit-window safety — instrumented global destructor at process exit.
//
// global_dtor_traced_program has a global object whose destructor calls an
// instrumented function during exit() processing. Constructed before
// trace_begin and destroyed after trace_shutdown (exit handlers are LIFO),
// its hooks fire in both disabled windows and must be silent no-ops. The
// assertions here pin "no crash, destructor completed, normal tracing
// unaffected"; under the ASan/TSan jobs this also proves no exit-time
// use-after-free hides in the shutdown sequence.
// ============================================================================

TEST(GlobalDtorTest, InstrumentedGlobalDestructorAtExitIsSafe) {
    char dir_tmpl[] = "/tmp/cslg_global_dtor_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string trace_path = dir + "/trace.out";
    const std::string stdout_path = dir + "/stdout.txt";

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_path + "\" \""
                    + GLOBAL_DTOR_PROGRAM_PATH + "\" > \"" + stdout_path + "\" 2>&1";
    int ret = system(cmd.c_str());
    EXPECT_EQ(ret, 0) << "global_dtor_traced_program did not exit cleanly (status "
                      << ret << ") — an instrumented global destructor broke the exit sequence";

    const std::string out = read_file(stdout_path);
    EXPECT_NE(out.find("GLOBAL_DTOR_RAN"), std::string::npos)
            << "global destructor did not run to completion. Stdout:\n" << out;

    const std::string trace = read_file(trace_path);
    EXPECT_NE(trace.find("traced_marker_in_main"), std::string::npos)
            << "normal tracing during main() is missing. Trace:\n" << trace;

    unlink(trace_path.c_str());
    unlink(stdout_path.c_str());
    rmdir(dir.c_str());
}

// ============================================================================
// dlopen() of an instrumented plugin while another thread traces. The plugin's
// static initializer fires the enter hook on the dlopen() thread, with glibc's
// loader lock held, and the hook takes s_resolver_mutex. The resolver used to call
// dladdr() — which takes the loader lock — with s_resolver_mutex held, so the two
// threads deadlocked whenever the tracing thread was on its cold path. The
// driver keeps one thread on the cold path (thousands of never-seen callees)
// while another dlopen()s the plugin in a loop; it runs under `timeout`, so a
// regression shows up as exit code 124 instead of hanging the suite.
// ============================================================================

TEST(DlopenConstructorTest, InstrumentedPluginConstructorDoesNotDeadlock) {
    char dir_tmpl[] = "/tmp/cslg_dlopen_ctor_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;

    std::string cmd = "CSLG_OUTPUT_FILE=\"" + dir + "/trace.out\" timeout 30 \""
                    + DLOPEN_CTOR_PROGRAM_PATH + "\" > \"" + dir + "/stdout.txt\" 2>&1";
    const int status = system(cmd.c_str());
    const std::string out = read_file(dir + "/stdout.txt");
    remove_dir_tree(dir);

    EXPECT_EQ(status, 0) << "dlopen_ctor_traced_program did not finish (raw status " << status
                         << "; 124<<8 means killed by timeout — the dladdr/dlopen lock-order "
                            "deadlock has regressed). Output:\n" << out;
    EXPECT_NE(out.find("DLOPEN_CTOR_DONE"), std::string::npos) << "Output:\n" << out;
}

// ============================================================================
// pthread_cancel through instrumented code. The hooks contain cancellation
// points (fwrite, the lazy open, pwrite with LOG_ELAPSED) and both compilers
// emit the hook call as non-throwing, so a cancellation request that landed
// inside a hook made glibc's forced unwind escape a frame without an unwind
// entry and the whole process died ("FATAL: exception not rethrown" or
// std::terminate). The hooks now disable cancellation for their duration.
// The LOG_ELAPSED variant is covered too: its exit hook pwrite()s and its thread-exit
// destructor close()s the patch descriptor — both cancellation points of their own
// (the destructor runs before glibc marks the thread as exiting, so a pending request
// would otherwise be honored inside a noexcept destructor). Two runs per variant pin
// both halves of the fix:
//   - "testcancel": the worker loop has its own cancellation point, so the
//     request is honored there — the thread ends with PTHREAD_CANCELED.
//   - default: the loop has no cancellation point of its own; instrumentation
//     must not add one, so the request is never honored and the program falls
//     back to a cooperative stop (JOIN_TIMEOUT) — but it must never abort.
// ============================================================================

namespace {

std::string run_cancel_program(const char* program, const std::string& arg, int& exit_status) {
    char dir_tmpl[] = "/tmp/cslg_cancel_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    if (d == nullptr) {
        exit_status = -1;
        return {};
    }
    const std::string dir = d;
    const std::string stdout_path = dir + "/stdout.txt";
    std::string cmd = "CSLG_OUTPUT_FILE=\"" + dir + "/trace.out\" timeout 20 \"" + program + "\" "
                    + arg + " > \"" + stdout_path + "\" 2>&1";
    exit_status = system(cmd.c_str());
    std::string out = read_file(stdout_path);
    remove_dir_tree(dir);
    return out;
}

void expect_cancellation_honored(const char* program) {
    int status = -1;
    const std::string out = run_cancel_program(program, "testcancel", status);
    EXPECT_EQ(status, 0) << program << " did not exit cleanly (raw status " << status
                         << "; 134 means SIGABRT — a cancellation escaped the tracer). Output:\n" << out;
    EXPECT_NE(out.find("JOINED: PTHREAD_CANCELED"), std::string::npos)
            << "worker was not cancelled at its pthread_testcancel(). Output:\n" << out;
}

void expect_no_tracer_cancellation_points(const char* program) {
    int status = -1;
    const std::string out = run_cancel_program(program, "", status);
    EXPECT_EQ(status, 0) << program << " did not exit cleanly (raw status " << status
                         << "; 134 means SIGABRT — a cancellation escaped the tracer). Output:\n" << out;
    EXPECT_NE(out.find("JOIN_TIMEOUT"), std::string::npos)
            << "a cancellation was honored inside the tracer (the hooks acted as cancellation "
               "points). Output:\n" << out;
}

} // namespace

TEST(PthreadCancelTest, CancellationIsHonoredAtTheProgramsOwnCancellationPoint) {
    expect_cancellation_honored(CANCEL_TRACED_PROGRAM_PATH);
}

TEST(PthreadCancelTest, HooksAddNoCancellationPoints) {
    expect_no_tracer_cancellation_points(CANCEL_TRACED_PROGRAM_PATH);
}

TEST(PthreadCancelTest, LogElapsedCancellationIsHonoredAtTheProgramsOwnCancellationPoint) {
    expect_cancellation_honored(CANCEL_PROGRAM_LOG_ELAPSED_PATH);
}

TEST(PthreadCancelTest, LogElapsedHooksAndThreadExitAddNoCancellationPoints) {
    expect_no_tracer_cancellation_points(CANCEL_PROGRAM_LOG_ELAPSED_PATH);
}

// ============================================================================
// A relative CSLG_OUTPUT_FILE must be anchored to the working directory at program
// start. The driver chdir()s into "sub" after the main thread's lazy open but
// before the worker thread's, so without the startup-time resolution in
// trace_begin() the worker file would land in "sub" while main's stays in the
// startup directory — the exact scatter this test guards against.
TEST(ChdirTest, RelativeOutputPathIsAnchoredToStartupDirectory) {
    char dir_tmpl[] = "/tmp/cslg_chdir_XXXXXX";
    char* d = mkdtemp(dir_tmpl);
    ASSERT_NE(d, nullptr) << "mkdtemp failed";
    const std::string dir = d;
    const std::string sub_dir = dir + "/sub";
    const std::string base_name = "rel_trace.out";
    const std::string stdout_path = dir + "/stdout.txt";

    // Start the program IN the temp directory with a purely relative output path.
    std::string cmd = "cd \"" + dir + "\" && CSLG_OUTPUT_FILE=\"" + base_name + "\" \""
                    + CHDIR_TRACED_PROGRAM_PATH + "\" > \"" + stdout_path + "\" 2>&1";
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0) << "chdir_traced_program failed, exit=" << ret << "\n"
                      << read_file(stdout_path);
    ASSERT_NE(read_file(stdout_path).find("CHDIR_OK"), std::string::npos)
            << "driver never switched directory. Stdout:\n" << read_file(stdout_path);

    // Main file and exactly one worker file, both in the startup directory.
    std::vector<std::string> worker_files;
    bool main_found = false;
    for (const std::string& name : list_files_in(dir)) {
        if (name == base_name) {
            main_found = true;
        } else if (parse_tid_suffix(name, base_name) >= 0) {
            worker_files.push_back(name);
        }
    }
    EXPECT_TRUE(main_found) << "main trace file missing from startup directory " << dir;
    ASSERT_EQ(worker_files.size(), 1u)
            << "expected exactly one worker file in the startup directory " << dir;

    const std::string main_trace = read_file(dir + "/" + base_name);
    EXPECT_NE(main_trace.find("chdir_main_post_join"), std::string::npos)
            << "main file lacks main-thread content:\n" << main_trace;
    const std::string worker_trace = read_file(dir + "/" + worker_files[0]);
    EXPECT_NE(worker_trace.find("chdir_worker_leaf"), std::string::npos)
            << "worker file lacks the worker chain:\n" << worker_trace;

    // The directory the program chdir()ed into must hold no trace file at all.
    for (const std::string& name : list_files_in(sub_dir)) {
        EXPECT_TRUE(name != base_name && parse_tid_suffix(name, base_name) < 0)
                << "trace file '" << name << "' was written to the chdir() target "
                << sub_dir << " instead of the startup directory";
    }

    remove_dir_tree(sub_dir);
    remove_dir_tree(dir);
}

// ============================================================================
// LOG_EXCEPTIONS — frame-stack reconciliation after non-local exits, and the
// markers on the lines of frames that did not return normally.
//
// exceptions_traced_program.cpp makes instrumented frames leave without their
// exit hook in every way the tracer knows (exceptions caught above them, which
// run no exit hooks on Clang; longjmp over them, which runs none on either
// compiler) and calls a marker from a known depth after each scenario. With
// the reconciliation rules (include/frameReconcile.h) every marker must sit at
// its true depth on BOTH compilers — a stale record would shift it deeper.
// ============================================================================

namespace {

// Runs one of the LOG_EXCEPTIONS variants of the driver in a fresh directory
// (it spawns worker threads, so more trace files appear), captures its stdout
// and parses the "<TAG>=<line>" pairs the driver prints for its throw and
// catch sites. `worker_content` / `worker_lines` are the file of the thread
// that catches an exception of its own (scenario L); the other workers' files
// are reached through worker_lines_with().
class LogExceptionsRunner : public ::testing::Test {
protected:
    std::string dir;
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string worker_content;
    std::vector<std::string> worker_lines;
    // Every worker file's lines, by file name.
    std::map<std::string, std::vector<std::string>> worker_files;
    std::map<std::string, int> site_lines;

    void run_driver(const char* program_path) {
        char dir_tmpl[] = "/tmp/cslg_exceptions_XXXXXX";
        char* d = mkdtemp(dir_tmpl);
        ASSERT_NE(d, nullptr) << "mkdtemp failed";
        dir = d;
        const std::string trace_path = dir + "/trace.out";
        const std::string stdout_path = dir + "/stdout.txt";

        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_path + "\" \"" + program_path
                        + "\" > \"" + stdout_path + "\" 2>&1";
        int ret = system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "exceptions driver failed, exit=" << ret << "\n" << read_file(stdout_path);

        trace_content = read_file(trace_path);
        trace_lines = split_lines(trace_content);

        for (const auto& name : list_files_in(dir)) {
            if (name.rfind("trace.out_tid_", 0) == 0) {
                const std::string content = read_file(dir + "/" + name);
                worker_files[name] = split_lines(content);
                if (content.find("exc_thread_body") != std::string::npos) {
                    worker_content = content;
                    worker_lines = worker_files[name];
                }
            }
        }

        for (const auto& line : split_lines(read_file(stdout_path))) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string tag = line.substr(0, eq);
            if (tag.empty() || tag.find(' ') != std::string::npos) continue;
            try {
                site_lines[tag] = std::stoi(line.substr(eq + 1));
            } catch (...) {
            }
        }
    }

    void TearDown() override {
        if (!dir.empty()) {
            remove_dir_tree(dir);
        }
    }

    // The lines of the worker file that mentions `needle` (empty when none does).
    const std::vector<std::string>& worker_lines_with(const std::string& needle) const {
        static const std::vector<std::string> none;
        for (const auto& [name, lines] : worker_files) {
            for (const auto& line : lines) {
                if (line.find(needle) != std::string::npos) return lines;
            }
        }
        return none;
    }

    // Depth of the unique entry line naming `function` (-1 when missing or ambiguous).
    static int depth_of(const std::vector<std::string>& lines, const std::string& function) {
        const std::string line = find_unique_line(lines, function);
        return line.empty() ? -1 : count_indentation_depth(line);
    }

    // Depths of every entry line naming `function`, in file order.
    static std::vector<int> depths_of(const std::vector<std::string>& lines, const std::string& function) {
        std::vector<int> depths;
        for (const auto& line : lines) {
            if (line.find(function) != std::string::npos && line.find("(called from:") != std::string::npos) {
                depths.push_back(count_indentation_depth(line));
            }
        }
        return depths;
    }

    // Every entry line naming `function`, in file order.
    static std::vector<std::string> lines_of(const std::vector<std::string>& lines, const std::string& function) {
        std::vector<std::string> found;
        for (const auto& line : lines) {
            if (line.find(function) != std::string::npos && line.find("(called from:") != std::string::npos) {
                found.push_back(line);
            }
        }
        return found;
    }

    // Tree glyph of the unique entry line naming `function` ('?' when missing).
    static char glyph_of(const std::vector<std::string>& lines, const std::string& function) {
        const std::string line = find_unique_line(lines, function);
        return line.empty() ? '?' : tree_glyph_of(line);
    }

    // The 12-byte field after the timestamp (empty when the line is too short).
    static std::string field_of(const std::string& line) {
        const size_t start = utils::PRETTY_TIME_LENGTH + 3;
        return line.size() >= start + 12 ? line.substr(start, 12) : std::string();
    }
};

class LogExceptionsTest : public LogExceptionsRunner {
protected:
    void SetUp() override { run_driver(TRACED_PROGRAM_LOG_EXCEPTIONS_PATH); }
};

class LogExceptionsElapsedTest : public LogExceptionsRunner {
protected:
    void SetUp() override { run_driver(TRACED_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH); }
};

class LogExceptionsAllFlagsTest : public LogExceptionsRunner {
protected:
    void SetUp() override { run_driver(TRACED_PROGRAM_LOG_EXCEPTIONS_ALL_PATH); }
};

class LogExceptionsInlinedTest : public LogExceptionsRunner {
protected:
    void SetUp() override { run_driver(TRACED_PROGRAM_LOG_EXCEPTIONS_INLINED_PATH); }
};

// Frames the driver leaves by exception, each named exactly once in the trace.
const std::vector<std::string> kFramesLeftByException = { "exc_thrower()", "exc_mid()" };
// Frames the driver leaves by longjmp: the two of scenario H (one leaf, one mid).
const std::vector<std::string> kFramesSkippedByLongjmp = { "exc_jump_mid()" };
// Frames that return normally although an exception or a longjmp passed nearby.
const std::vector<std::string> kFramesReturningNormally = {
    "exc_catcher()", "ExcGuard::~ExcGuard()", "exc_helper()", "exc_marker_a()", "exc_rec_catcher()",
    "exc_marker_b()", "exc_lib_throw()", "exc_so_throw()", "exc_rethrow_stmt()", "exc_rethrow_ptr()",
    "exc_nested_in_handler()", "exc_jump_root()", "exc_jump_after()", "exc_marker_h()",
    "exc_loop_jump()", "exc_loop_marker()", "exc_messages()", "exc_final_marker()",
};

} // namespace

// Every scenario is followed by a marker called from main (depth 1) or from the
// scenario's root frame. Stale records from any scenario would push the markers
// that follow it deeper — so this single test pins all of them, on GCC and Clang.
TEST_F(LogExceptionsTest, MarkersAfterEveryScenarioSitAtTheirTrueDepth) {
    EXPECT_EQ(depth_of(trace_lines, "exc_catcher"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_marker_a"), 1) << "after an exception caught two frames up:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_marker_b"), 1) << "after a throw four recursion levels down:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_lib_throw"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_so_throw"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_rethrow_stmt"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_rethrow_ptr"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_nested_in_handler"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_jump_root"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_jump_after"), 2)
            << "the call right after a longjmp landed must be a child of the setjmp frame:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_marker_h"), 1) << "after longjmp over two frames:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_loop_marker"), 2)
            << "after three longjmps out of the same call site:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_messages"), 1);
    EXPECT_EQ(depth_of(trace_lines, "exc_final_marker"), 1) << "at the very end:\n" << trace_content;
}

// The frames the exception unwinds through were entered normally and sit at
// their true depth; so does the helper a destructor runs DURING unwinding.
TEST_F(LogExceptionsTest, UnwoundFramesAndCleanupHelpersKeepTheirDepth) {
    EXPECT_EQ(depth_of(trace_lines, "exc_mid"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_thrower"), 3) << trace_content;
    // main -> exc_catcher -> exc_mid -> ExcGuard::~ExcGuard -> exc_helper
    EXPECT_EQ(depth_of(trace_lines, "ExcGuard::~ExcGuard"), 3) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_helper"), 4) << trace_content;
}

// Recursion produces one record per level with the same callee and caller; the
// rules must still pop the right one, both for the exceptional exit at depth
// four and for the plain void recursion whose exit hooks are tail-called at -O2.
TEST_F(LogExceptionsTest, RecursiveFramesPairLevelByLevel) {
    EXPECT_EQ(depths_of(trace_lines, "exc_rec("), (std::vector<int>{ 2, 3, 4, 5 })) << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "exc_void_rec"), (std::vector<int>{ 1, 2, 3, 4 })) << trace_content;
}

// The worker thread's own file: an exception caught inside the thread leaves
// the marker that follows at the depth of the frame that caught it.
TEST_F(LogExceptionsTest, WorkerThreadFileIsReconciledIndependently) {
    ASSERT_FALSE(worker_lines.empty()) << "no worker trace file produced";
    EXPECT_EQ(depth_of(worker_lines, "exc_thread_body"), 1) << worker_content;
    EXPECT_EQ(depth_of(worker_lines, "exc_thread_thrower"), 2) << worker_content;
    EXPECT_EQ(depth_of(worker_lines, "exc_thread_marker"), 1) << worker_content;
}

// A frame an exception leaves gets its tree glyph patched to '!' in place. GCC
// runs the exit hook of every unwound frame while the exception is in flight
// and marks it there; Clang runs no exit hook for them, so the catch interposer
// reclaims and marks them the moment the exception is caught.
TEST_F(LogExceptionsTest, FramesLeftByAnExceptionAreMarked) {
    for (const auto& function : kFramesLeftByException) {
        EXPECT_EQ(glyph_of(trace_lines, function), '!') << function << ":\n" << trace_content;
    }
    for (const auto& line : lines_of(trace_lines, "exc_rec(")) {
        EXPECT_EQ(tree_glyph_of(line), '!') << line;
    }
    ASSERT_FALSE(worker_lines.empty());
    EXPECT_EQ(glyph_of(worker_lines, "exc_thread_thrower()"), '!') << worker_content;
}

// Frames a longjmp skipped are found dead by the next hook and marked '~' on
// both compilers: the leaf that jumps and the frame between it and setjmp.
TEST_F(LogExceptionsTest, FramesSkippedByLongjmpAreMarked) {
    for (const auto& function : kFramesSkippedByLongjmp) {
        EXPECT_EQ(glyph_of(trace_lines, function), '~') << function << ":\n" << trace_content;
    }
    // exc_jump_leaf is entered four times (scenario H once, scenario I thrice);
    // every one of those frames longjmp'ed out.
    const auto leaves = lines_of(trace_lines, "exc_jump_leaf()");
    ASSERT_EQ(leaves.size(), 4u) << trace_content;
    for (const auto& line : leaves) {
        EXPECT_EQ(tree_glyph_of(line), '~') << line;
    }
}

// Frames that returned normally keep their plain glyph even when an exception
// or a longjmp passed right next to them (the catcher, the cleanup helper, the
// setjmp frame, the markers).
TEST_F(LogExceptionsTest, FramesThatReturnedNormallyAreNotMarked) {
    for (const auto& function : kFramesReturningNormally) {
        EXPECT_EQ(glyph_of(trace_lines, function), '|') << function << ":\n" << trace_content;
    }
    for (const auto& line : lines_of(trace_lines, "exc_void_rec")) {
        EXPECT_EQ(tree_glyph_of(line), '|') << line;
    }
}

// With LOG_ELAPSED every dead frame's placeholder is patched too: no
// "[  pending ]" may survive a clean exit in either per-thread file.
TEST_F(LogExceptionsElapsedTest, NoPendingLeftovers) {
    EXPECT_EQ(trace_content.find("[  pending ]"), std::string::npos) << trace_content;
    ASSERT_FALSE(worker_lines.empty());
    EXPECT_EQ(worker_content.find("[  pending ]"), std::string::npos) << worker_content;
}

// The duration field of a frame left by an exception carries the '!' flag in
// its otherwise always-blank byte 1. On GCC the exit hook measured a real
// duration ("[!  1.234ms]"); on Clang no exit hook ran, so the field reads
// "[! unwound ]" — the frame was reclaimed at the catch.
TEST_F(LogExceptionsElapsedTest, ExceptionExitsCarryFlaggedFields) {
    for (const auto& function : kFramesLeftByException) {
        const std::string line = find_unique_line(trace_lines, function);
        ASSERT_FALSE(line.empty()) << function;
        const std::string field = field_of(line);
        ASSERT_EQ(field.size(), 12u) << line;
#ifdef CSLG_COMPILER_IS_GNU
        EXPECT_EQ(field[1], '!') << line;
        std::string unflagged = field;
        unflagged[1] = ' ';
        EXPECT_TRUE(std::regex_match(unflagged, duration_field_regex())) << "no measured duration: " << line;
#else
        EXPECT_EQ(field, "[! unwound ]") << line;
#endif
    }
}

// Frames skipped by longjmp never ran an exit hook, so no duration exists for
// them: the placeholder becomes the flagged "[~ unwound ]" on both compilers.
TEST_F(LogExceptionsElapsedTest, LongjmpFramesReadUnwound) {
    for (const auto& function : kFramesSkippedByLongjmp) {
        const std::string line = find_unique_line(trace_lines, function);
        ASSERT_FALSE(line.empty()) << function;
        EXPECT_EQ(field_of(line), "[~ unwound ]") << line;
        EXPECT_EQ(tree_glyph_of(line), '~') << line;
    }
    for (const auto& line : lines_of(trace_lines, "exc_jump_leaf()")) {
        EXPECT_EQ(field_of(line), "[~ unwound ]") << line;
    }
}

// Frames that returned normally keep an unflagged, measured duration.
TEST_F(LogExceptionsElapsedTest, NormalExitsKeepPlainDurations) {
    for (const auto& function : kFramesReturningNormally) {
        const std::string line = find_unique_line(trace_lines, function);
        ASSERT_FALSE(line.empty()) << function;
        EXPECT_TRUE(std::regex_match(field_of(line), duration_field_regex())) << line;
        EXPECT_EQ(tree_glyph_of(line), '|') << line;
    }
}

// On GCC the unwound frames' durations are real measurements nested inside the
// catcher's: catcher >= mid >= thrower, as for any parent/child pair.
TEST_F(LogExceptionsElapsedTest, ExceptionalDurationsNestInsideTheCatcher) {
#ifndef CSLG_COMPILER_IS_GNU
    GTEST_SKIP() << "Clang runs no exit hook for unwound frames; their fields read unwound.";
#else
    auto ns_of = [&](const std::string& function) {
        std::string field = field_of(find_unique_line(trace_lines, function));
        if (field.size() == 12) field[1] = ' ';
        return parse_duration_ns(field);
    };
    const long long catcher = ns_of("exc_catcher()");
    const long long mid = ns_of("exc_mid()");
    const long long thrower = ns_of("exc_thrower()");
    ASSERT_GE(thrower, 0) << trace_content;
    EXPECT_GE(mid, thrower) << trace_content;
    EXPECT_GE(catcher, mid) << trace_content;
#endif
}

// Every entry line still parses as "[ts] [<12-byte field>] <tree> name  (called from: ...)":
// the flag byte and the "unwound" word stay inside the 12-byte field.
TEST_F(LogExceptionsElapsedTest, EveryLineKeepsTheFixedLayout) {
    const std::regex full(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[[ !~]( *\d+\.\d{3}(ns|us|ms|s )| unwound |  >999\.9s )\] .*\(called from: .+:.+\)$)");
    const std::regex event(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[(  throw   | rethrow  |  catch   | terminate)\] (\|  )*!! (throw|rethrow|catch|terminate) .*\(((thrown|rethrown|caught) at: .+|no handler found)\)$)");
    int entries = 0;
    int events = 0;
    for (const auto& line : trace_lines) {
        if (line.find("(called from:") != std::string::npos) {
            ++entries;
            EXPECT_TRUE(std::regex_match(line, full)) << line;
        } else if (line.find("!! ") != std::string::npos) {
            ++events;
            EXPECT_TRUE(std::regex_match(line, event)) << line;
        }
    }
    EXPECT_GT(entries, 30) << trace_content;
    EXPECT_GT(events, 20) << trace_content;
}

// With LOG_ADDR too, the glyph offset must skip both the duration column and the
// address column: the marks land on the glyph, and the address stays intact.
TEST_F(LogExceptionsAllFlagsTest, MarksLandOnTheGlyphBesideTheAddressColumn) {
    const std::regex marked(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[~ unwound \] addr: \[0x[0-9a-f]{16}\] \|  ~_ exc_jump_mid\(\)  \(called from: )");
    const std::string mid = find_unique_line(trace_lines, "exc_jump_mid()");
    EXPECT_TRUE(std::regex_search(mid, marked)) << mid;

    const std::regex plain(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[ *\d+\.\d{3}(ns|us|ms|s )\] addr: \[0x[0-9a-f]{16}\] \|  \|_ exc_jump_after\(\)  \(called from: )");
    const std::string after = find_unique_line(trace_lines, "exc_jump_after()");
    EXPECT_TRUE(std::regex_search(after, plain)) << after;
}

// Marked and unmarked lines at the same depth keep their glyphs in the same
// column, so the tree still reads as a tree.
TEST_F(LogExceptionsAllFlagsTest, TreeColumnsStillAlign) {
    const std::string mid = find_unique_line(trace_lines, "exc_jump_mid()");
    const std::string after = find_unique_line(trace_lines, "exc_jump_after()");
    ASSERT_FALSE(mid.empty());
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(mid.find("~_ "), after.find("|_ ")) << mid << "\n" << after;
    EXPECT_EQ(count_indentation_depth(mid), count_indentation_depth(after));
}

// ============================================================================
// LOG_EXCEPTIONS — the exception events themselves: throw, rethrow and catch
// lines with the exception's type, its what() text and the exact source line.
// ============================================================================

namespace {

// Every line of `lines` that carries the event glyph and `verb`, in file order.
std::vector<std::string> event_lines(const std::vector<std::string>& lines, const std::string& verb) {
    std::vector<std::string> found;
    for (const auto& line : lines) {
        if (line.find("!! " + verb + " ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// The unique event line containing `needle`, or "".
std::string unique_event_line(const std::vector<std::string>& lines, const std::string& needle) {
    std::string found;
    int matches = 0;
    for (const auto& line : lines) {
        if (line.find("!! ") != std::string::npos && line.find(needle) != std::string::npos) {
            found = line;
            ++matches;
        }
    }
    return matches == 1 ? found : "";
}

// The 12-byte field after the timestamp (empty when the line is too short).
std::string field_of_line(const std::string& line) {
    const size_t start = utils::PRETTY_TIME_LENGTH + 3;
    return line.size() >= start + 12 ? line.substr(start, 12) : std::string();
}

// Index of the first line containing `needle` (-1 when absent).
long index_of(const std::vector<std::string>& lines, const std::string& needle) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) return static_cast<long>(i);
    }
    return -1;
}

} // namespace

// A `throw` becomes a line under the throwing frame, naming the type, the
// what() text and the exact line of the throw statement (the driver printed it).
TEST_F(LogExceptionsTest, ThrowLinesNameTypeMessageAndExactSite) {
    ASSERT_TRUE(site_lines.count("THROW_A")) << "driver did not report THROW_A";
    const std::string a = unique_event_line(trace_lines, "!! throw std::runtime_error \"bad header\"");
    ASSERT_FALSE(a.empty()) << trace_content;
    EXPECT_NE(a.find("!! throw std::runtime_error \"bad header\"  (thrown at: "), std::string::npos) << a;
    EXPECT_NE(a.find("exceptions_traced_program.cpp:" + std::to_string(site_lines["THROW_A"]) + ")"),
              std::string::npos) << a;
    // A child of exc_thrower (depth 3).
    EXPECT_EQ(count_indentation_depth(a), 4) << a;

    const std::string b = unique_event_line(trace_lines, "!! throw std::logic_error \"deep\"");
    ASSERT_FALSE(b.empty()) << trace_content;
    EXPECT_NE(b.find("!! throw std::logic_error \"deep\"  (thrown at: "), std::string::npos) << b;
    EXPECT_NE(b.find(":" + std::to_string(site_lines["THROW_B"]) + ")"), std::string::npos) << b;
    EXPECT_EQ(count_indentation_depth(b), 6) << "a child of the deepest exc_rec:\n" << b;
}

// Throws born outside the program's own code are seen too: inside libstdc++
// (vector::at) and inside a shared library built without instrumentation.
TEST_F(LogExceptionsTest, ThrowsFromTheRuntimeAndFromForeignLibrariesAreSeen) {
    const std::string lib = unique_event_line(trace_lines, "!! throw std::out_of_range");
    ASSERT_FALSE(lib.empty()) << trace_content;
    EXPECT_NE(lib.find("\"vector::_M_range_check"), std::string::npos) << lib;
    EXPECT_NE(lib.find("(thrown at: "), std::string::npos) << lib;
    EXPECT_EQ(count_indentation_depth(lib), 2) << "a child of exc_lib_throw:\n" << lib;

    const std::string so = unique_event_line(trace_lines, "!! throw std::invalid_argument");
    ASSERT_FALSE(so.empty()) << trace_content;
    EXPECT_NE(so.find("\"from a library built without instrumentation\""), std::string::npos) << so;
    EXPECT_EQ(count_indentation_depth(so), 2) << "a child of exc_so_throw:\n" << so;
}

// `throw;` and std::rethrow_exception both produce a rethrow line at the exact
// statement, the latter saying how it was rethrown.
TEST_F(LogExceptionsTest, RethrowLinesNameTheStatementAndTheMechanism) {
    const std::string stmt = unique_event_line(trace_lines, "!! rethrow std::logic_error");
    ASSERT_FALSE(stmt.empty()) << trace_content;
    EXPECT_NE(stmt.find("(rethrown at: "), std::string::npos) << stmt;
    EXPECT_NE(stmt.find(":" + std::to_string(site_lines["RETHROW_E"]) + ")"), std::string::npos) << stmt;
    EXPECT_EQ(stmt.find("via std::rethrow_exception"), std::string::npos) << stmt;

    const std::string ptr = unique_event_line(trace_lines, "!! rethrow std::out_of_range");
    ASSERT_FALSE(ptr.empty()) << trace_content;
    EXPECT_NE(ptr.find(":" + std::to_string(site_lines["RETHROW_F"]) + " via std::rethrow_exception)"),
              std::string::npos) << ptr;
}

// A catch becomes a line under the catching frame with the type, the what()
// text and the exact line of the catch clause.
TEST_F(LogExceptionsTest, CatchLinesNameTypeMessageAndExactSite) {
    const std::string a = unique_event_line(trace_lines, "!! catch std::runtime_error \"bad header\"");
    ASSERT_FALSE(a.empty()) << trace_content;
    EXPECT_NE(a.find("(caught at: "), std::string::npos) << a;
    EXPECT_NE(a.find("exceptions_traced_program.cpp:" + std::to_string(site_lines["CATCH_A"]) + ")"),
              std::string::npos) << a;
    EXPECT_EQ(count_indentation_depth(a), 2) << "a child of exc_catcher:\n" << a;

    const std::string b = unique_event_line(trace_lines, "!! catch std::logic_error \"deep\"");
    ASSERT_FALSE(b.empty()) << trace_content;
    EXPECT_NE(b.find(":" + std::to_string(site_lines["CATCH_B"]) + ")"), std::string::npos) << b;
    EXPECT_EQ(count_indentation_depth(b), 2) << "a child of exc_rec_catcher:\n" << b;

    // Every catch clause of the driver reported its line; each must appear.
    for (const char* tag : { "CATCH_C", "CATCH_D", "CATCH_E", "CATCH_F", "CATCH_G1", "CATCH_G2",
                             "CATCH_K1", "CATCH_K2", "CATCH_K3" }) {
        ASSERT_TRUE(site_lines.count(tag)) << tag;
        const std::string needle = "(caught at: ";
        bool found = false;
        for (const auto& line : event_lines(trace_lines, "catch")) {
            if (line.find(":" + std::to_string(site_lines[tag]) + ")") != std::string::npos
                && line.find(needle) != std::string::npos) {
                found = true;
            }
        }
        EXPECT_TRUE(found) << tag << " line " << site_lines[tag] << " has no catch event:\n" << trace_content;
    }
}

// what() texts are shown on one line and cut to a sane length; a non-std type
// has no text and no quotes.
TEST_F(LogExceptionsTest, MessagesAreSanitizedForOneLine) {
    const std::string k1 = unique_event_line(trace_lines, "!! throw std::runtime_error \"line one");
    ASSERT_FALSE(k1.empty()) << trace_content;
    EXPECT_NE(k1.find("\"line one line two tabbed\""), std::string::npos) << k1;

    const std::string k2 = unique_event_line(trace_lines, "!! throw std::runtime_error \"xxxx");
    ASSERT_FALSE(k2.empty()) << trace_content;
    const std::string cut(utils::WHAT_TEXT_CAPACITY - 4, 'x');
    EXPECT_NE(k2.find("\"" + cut + "...\""), std::string::npos) << k2;
    EXPECT_EQ(k2.find(std::string(utils::WHAT_TEXT_CAPACITY, 'x')), std::string::npos) << k2;

    const std::string k3 = unique_event_line(trace_lines, "!! throw int  (thrown at: ");
    ASSERT_FALSE(k3.empty()) << trace_content;
    EXPECT_EQ(k3.find('"'), std::string::npos) << k3;
    const std::string k3c = unique_event_line(trace_lines, "!! catch int  (caught at: ");
    ASSERT_FALSE(k3c.empty()) << trace_content;
}

// The events sit in the flow where they happened: the throw after the thrower's
// entry, the cleanup helper between the throw and the catch, the catch before
// the marker that follows the scenario. A new exception thrown inside a catch
// handler nests under the handler's frame.
TEST_F(LogExceptionsTest, EventsFollowTheFlowOfExecution) {
    const long thrower = index_of(trace_lines, "exc_thrower()  (called from:");
    const long throw_a = index_of(trace_lines, "!! throw std::runtime_error \"bad header\"");
    const long helper = index_of(trace_lines, "exc_helper()");
    const long catch_a = index_of(trace_lines, "!! catch std::runtime_error \"bad header\"");
    const long marker_a = index_of(trace_lines, "exc_marker_a()");
    ASSERT_GE(thrower, 0);
    EXPECT_LT(thrower, throw_a);
    EXPECT_LT(throw_a, helper);
    EXPECT_LT(helper, catch_a);
    EXPECT_LT(catch_a, marker_a);

    const std::string outer = unique_event_line(trace_lines, "!! throw std::runtime_error \"outer\"");
    ASSERT_FALSE(outer.empty()) << trace_content;
    EXPECT_EQ(count_indentation_depth(outer), 2) << outer;
    const long catch_g1 = index_of(trace_lines, ":" + std::to_string(site_lines["CATCH_G1"]) + ")");
    const long throw_g2 = index_of(trace_lines, ":" + std::to_string(site_lines["THROW_G2"]) + ")");
    const long catch_g2 = index_of(trace_lines, ":" + std::to_string(site_lines["CATCH_G2"]) + ")");
    ASSERT_GE(catch_g1, 0) << trace_content;
    EXPECT_LT(catch_g1, throw_g2);
    EXPECT_LT(throw_g2, catch_g2);
}

// The worker thread's file carries its own throw and catch lines.
TEST_F(LogExceptionsTest, WorkerThreadFileCarriesItsOwnEvents) {
    ASSERT_FALSE(worker_lines.empty());
    const std::string t = unique_event_line(worker_lines, "!! throw std::runtime_error \"in a worker\"");
    ASSERT_FALSE(t.empty()) << worker_content;
    EXPECT_EQ(count_indentation_depth(t), 3) << t;
    const std::string c = unique_event_line(worker_lines, "!! catch std::runtime_error \"in a worker\"");
    ASSERT_FALSE(c.empty()) << worker_content;
    EXPECT_EQ(count_indentation_depth(c), 2) << c;
    EXPECT_NE(c.find(":" + std::to_string(site_lines["CATCH_L"]) + ")"), std::string::npos) << c;
    // ... and the main file does not.
    EXPECT_EQ(trace_content.find("\"in a worker\""), std::string::npos);
}

// A catcher whose frame size differs on every call (alloca): the catch is found
// at the catcher's exact level each time, so its line and the call after it sit
// under the catcher, and the records of the catcher and of its host survive
// all three catches — a level distance cached from the first call would have
// reclaimed them at the second.
TEST_F(LogExceptionsTest, CatchInAVariableSizeFrameKeepsTheCatcherAlive) {
    EXPECT_EQ(depth_of(trace_lines, "exc_vla_host"), 1) << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "exc_vla_catcher"), (std::vector<int>{ 2, 2, 2 })) << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "exc_vla_thrower"), (std::vector<int>{ 3, 3, 3 })) << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "exc_vla_marker"), (std::vector<int>{ 3, 3, 3 })) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "exc_marker_m"), 1) << trace_content;
    int catches = 0;
    for (const auto& line : event_lines(trace_lines, "catch")) {
        if (line.find(":" + std::to_string(site_lines["CATCH_M"]) + ")") != std::string::npos) {
            ++catches;
            EXPECT_EQ(count_indentation_depth(line), 3) << line;
        }
    }
    EXPECT_EQ(catches, 3) << trace_content;
    for (const auto& line : lines_of(trace_lines, "exc_vla_catcher")) {
        EXPECT_EQ(tree_glyph_of(line), '|') << line;
    }
    for (const auto& line : lines_of(trace_lines, "exc_vla_thrower")) {
        EXPECT_EQ(tree_glyph_of(line), '!') << line;
    }
}

// The tracer reads the thrown object's what() for the throw and catch lines.
// An instrumented what() override (and the helper it calls) must leave no lines
// of its own: the only traced call is the handler's own e.what().
TEST_F(LogExceptionsTest, InstrumentedWhatOverrideLeavesNoLinesOfItsOwn) {
    ASSERT_FALSE(unique_event_line(trace_lines, "!! throw ExcCustom \"custom what\"").empty()) << trace_content;
    ASSERT_FALSE(unique_event_line(trace_lines, "!! catch ExcCustom \"custom what\"").empty()) << trace_content;
    const auto what_lines = lines_of(trace_lines, "ExcCustom::what()");
    ASSERT_EQ(what_lines.size(), 1u) << "what() must be traced only where the program calls it:\n" << trace_content;
    EXPECT_NE(what_lines[0].find("exceptions_traced_program.cpp:" + std::to_string(site_lines["CATCH_N"]) + ")"),
              std::string::npos) << what_lines[0];
    EXPECT_EQ(lines_of(trace_lines, "exc_custom_text()").size(), 1u) << trace_content;
    EXPECT_EQ(trace_content.find("src/exceptions.cpp"), std::string::npos)
            << "a call attributed to the tracer's own source:\n" << trace_content;
}

// A thread cancelled inside a try block: pthread_cancel's forced unwind runs
// the catch(...) handler and its rethrow like an exception, but the C++ runtime
// never threw it and has no type for it. The events say what it was, at the
// handler's exact lines, in the cancelled thread's own file.
TEST_F(LogExceptionsTest, ForcedUnwindOfACancelledThreadIsLabeledAtItsCatchAndRethrow) {
    const std::vector<std::string>& lines = worker_lines_with("exc_cancel_body");
    ASSERT_FALSE(lines.empty()) << "no trace file of the cancelled worker";
    std::string content;
    for (const auto& line : lines) content += line + "\n";
    const std::string c = unique_event_line(lines, "!! catch (forced unwind)  (caught at: ");
    ASSERT_FALSE(c.empty()) << content;
    EXPECT_NE(c.find(":" + std::to_string(site_lines["CATCH_P"]) + ")"), std::string::npos) << c;
    EXPECT_EQ(count_indentation_depth(c), 2) << "a child of exc_cancel_body:\n" << c;
    const std::string r = unique_event_line(lines, "!! rethrow (forced unwind)  (rethrown at: ");
    ASSERT_FALSE(r.empty()) << content;
    EXPECT_NE(r.find(":" + std::to_string(site_lines["RETHROW_P"]) + ")"), std::string::npos) << r;
    EXPECT_EQ(count_indentation_depth(r), 2) << r;
    EXPECT_EQ(content.find("<unknown type>"), std::string::npos) << content;
    EXPECT_EQ(depth_of(lines, "exc_cancel_body"), 1) << content;
}

// With LOG_ELAPSED an event line carries its column word instead of a duration.
TEST_F(LogExceptionsElapsedTest, EventLinesCarryTheirColumnWord) {
    for (const auto& line : event_lines(trace_lines, "throw")) {
        EXPECT_EQ(field_of(line), "[  throw   ]") << line;
    }
    for (const auto& line : event_lines(trace_lines, "rethrow")) {
        EXPECT_EQ(field_of(line), "[ rethrow  ]") << line;
    }
    for (const auto& line : event_lines(trace_lines, "catch")) {
        EXPECT_EQ(field_of(line), "[  catch   ]") << line;
    }
    EXPECT_GE(event_lines(trace_lines, "throw").size(), 8u) << trace_content;
    EXPECT_GE(event_lines(trace_lines, "catch").size(), 12u) << trace_content;
}

// With LOG_ADDR the address column of an event line holds the site address.
TEST_F(LogExceptionsAllFlagsTest, EventLinesCarryTheSiteAddress) {
    const std::regex layout(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] \[  throw   \] addr: \[0x[0-9a-f]{16}\] (\|  )*!! throw std::runtime_error "bad header"  \(thrown at: .+:\d+\)$)");
    const std::string a = unique_event_line(trace_lines, "!! throw std::runtime_error \"bad header\"  (thrown at:");
    ASSERT_FALSE(a.empty()) << trace_content;
    EXPECT_TRUE(std::regex_match(a, layout)) << a;
}

// ---- an exception nobody catches -------------------------------------------

namespace {

class LogExceptionsUncaughtTest : public ::testing::Test {
protected:
    std::string trace_content;
    std::vector<std::string> trace_lines;
    std::string trace_file_path;
    // The trace file(s) of worker threads, next to the main file (the "thread"
    // mode starts one); `worker_lines` holds the last one found.
    std::vector<std::string> worker_paths;
    std::vector<std::string> worker_lines;
    std::map<std::string, int> site_lines;
    int run_status = -1;

    // Runs the terminate driver in `mode` (see uncaught_traced_program.cpp; ""
    // is the plain uncaught throw) and loads its trace and the "<TAG>=<line>"
    // pairs it printed. Every mode ends in std::terminate, so the process is
    // expected to die with SIGABRT.
    void run(const char* mode) {
        char tmp_path[] = "/tmp/cslg_uncaught_XXXXXX";
        int fd = mkstemp(tmp_path);
        ASSERT_GE(fd, 0) << "mkstemp failed";
        close(fd);
        trace_file_path = tmp_path;
        const std::string stdout_path = trace_file_path + ".stdout";

        std::string cmd = "CSLG_OUTPUT_FILE=\"" + trace_file_path + "\" \""
                        + UNCAUGHT_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH + "\" " + mode + " > \"" + stdout_path
                        + "\" 2>/dev/null";
        run_status = system(cmd.c_str());

        trace_content = read_file(trace_file_path);
        trace_lines = split_lines(trace_content);
        const std::string dir = trace_file_path.substr(0, trace_file_path.rfind('/'));
        const std::string base = trace_file_path.substr(trace_file_path.rfind('/') + 1);
        for (const auto& name : list_files_in(dir)) {
            if (name.rfind(base + "_tid_", 0) == 0) {
                worker_paths.push_back(dir + "/" + name);
                worker_lines = split_lines(read_file(worker_paths.back()));
            }
        }
        for (const auto& line : split_lines(read_file(stdout_path))) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            try {
                site_lines[line.substr(0, eq)] = std::stoi(line.substr(eq + 1));
            } catch (...) {
            }
        }
        unlink(stdout_path.c_str());

        ASSERT_NE(run_status, 0) << "the driver exited cleanly in mode '" << mode << "' — nothing to test";
        ASSERT_GE(trace_lines.size(), 2u) << "trace too short:\n" << trace_content;
    }

    // The event lines ("!! ...") of the trace, in file order.
    std::vector<std::string> event_lines() const {
        std::vector<std::string> events;
        for (const auto& line : trace_lines) {
            if (line.find("!! ") != std::string::npos) events.push_back(line);
        }
        return events;
    }

    static bool has(const std::string& line, const std::string& needle) {
        return line.find(needle) != std::string::npos;
    }

    // The tail of a site rendered from the reported line `tag`, e.g. ":42)".
    std::string site_of(const char* tag, const char* suffix = "") {
        return ":" + std::to_string(site_lines[tag]) + suffix + ")";
    }

    void TearDown() override {
        if (!trace_file_path.empty()) {
            unlink(trace_file_path.c_str());
        }
        for (const auto& path : worker_paths) {
            unlink(path.c_str());
        }
    }
};

} // namespace

// std::terminate runs no exit hook, so every frame active at the throw keeps
// its placeholder. The throw line is written before the runtime searches for a
// handler; when it finds none it calls __cxa_begin_catch itself before
// terminating, which the tracer turns into the "terminate" line — the last
// line of the trace, naming what killed the process. The default terminate
// handler's own rethrow and catch (to print its message) are not traced.
TEST_F(LogExceptionsUncaughtTest, TerminateLineIsTheLastLineAndActiveFramesStayPending) {
    ASSERT_NO_FATAL_FAILURE(run(""));

    const std::string& last = trace_lines.back();
    EXPECT_EQ(last.find("!! terminate std::logic_error \"nobody catches this\"  (no handler found)"),
              last.find("!! ")) << "last line:\n" << last << "\ntrace:\n" << trace_content;
    EXPECT_NE(last.find("!! terminate"), std::string::npos) << last;
    EXPECT_EQ(field_of_line(last), "[ terminate]") << last;
    EXPECT_EQ(count_indentation_depth(last), 3) << "a child of uncaught_leaf:\n" << last;

    const std::string& throw_line = trace_lines[trace_lines.size() - 2];
    EXPECT_NE(throw_line.find("!! throw std::logic_error \"nobody catches this\"  (thrown at: "),
              std::string::npos) << throw_line;
    EXPECT_NE(throw_line.find(site_of("THROW_UNCAUGHT")), std::string::npos) << throw_line;
    EXPECT_EQ(field_of_line(throw_line), "[  throw   ]") << throw_line;
    EXPECT_EQ(trace_content.find("!! rethrow"), std::string::npos) << "the terminate handler's rethrow leaked:\n"
                                                                     << trace_content;
    EXPECT_EQ(trace_content.find("!! catch"), std::string::npos) << "the terminate handler's catch leaked:\n"
                                                                   << trace_content;

    for (const char* fn : { " main ", "uncaught_outer", "uncaught_leaf" }) {
        const std::string line = find_unique_line(trace_lines, fn);
        ASSERT_FALSE(line.empty()) << fn << " line missing or not unique. Trace:\n" << trace_content;
        EXPECT_NE(line.find("[  pending ]"), std::string::npos) << line;
        EXPECT_EQ(tree_glyph_of(line) == '\0' || tree_glyph_of(line) == '|', true) << line;
    }
    const std::string done = find_unique_line(trace_lines, "uncaught_completed_work");
    ASSERT_FALSE(done.empty()) << trace_content;
    EXPECT_TRUE(std::regex_search(done, duration_field_regex())) << done;
    EXPECT_GE(parse_duration_ns(done), 1'000'000LL) << done;
}

// `throw;` in a handler with no outer handler: the runtime gives up inside
// __cxa_rethrow, which calls __cxa_begin_catch itself like __cxa_throw does.
// The trace shows the program's own catch and rethrow (at their exact lines,
// children of the handler's function), then the terminate line, and nothing
// of the terminate handler — no catch attributed to the runtime, no second
// rethrow.
TEST_F(LogExceptionsUncaughtTest, RethrowWithNoOuterHandlerEndsInTheTerminateLine) {
    ASSERT_NO_FATAL_FAILURE(run("rethrow"));
    const std::vector<std::string> events = event_lines();
    ASSERT_EQ(events.size(), 4u) << "throw, catch, rethrow, terminate expected:\n" << trace_content;
    EXPECT_TRUE(has(events[0], "!! throw std::logic_error \"nobody catches this\"  (thrown at: ")) << events[0];
    EXPECT_TRUE(has(events[1], "!! catch std::logic_error \"nobody catches this\"  (caught at: ")) << events[1];
    EXPECT_TRUE(has(events[1], site_of("CATCH_RETHROW"))) << events[1];
    EXPECT_TRUE(has(events[2], "!! rethrow std::logic_error  (rethrown at: ")) << events[2];
    EXPECT_TRUE(has(events[2], site_of("RETHROW"))) << events[2];
    EXPECT_TRUE(has(events[3], "!! terminate std::logic_error \"nobody catches this\"  (no handler found)"))
            << events[3];
    EXPECT_EQ(events[3], trace_lines.back()) << trace_content;
    EXPECT_EQ(field_of_line(events[3]), "[ terminate]") << events[3];
    for (size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(count_indentation_depth(events[i]), 2) << "a child of the handler's function:\n" << events[i];
        EXPECT_FALSE(has(events[i], "<unknown function>")) << events[i];
    }
}

// The same through std::rethrow_exception: the dependent exception it throws
// hides its primary object from the interposer, so the terminate line's
// what() comes from std::current_exception() inside the tracer.
TEST_F(LogExceptionsUncaughtTest, RethrowExceptionWithNoOuterHandlerEndsInTheTerminateLine) {
    ASSERT_NO_FATAL_FAILURE(run("rethrow_ptr"));
    const std::vector<std::string> events = event_lines();
    ASSERT_EQ(events.size(), 4u) << "throw, catch, rethrow, terminate expected:\n" << trace_content;
    EXPECT_TRUE(has(events[1], "!! catch std::logic_error \"nobody catches this\"  (caught at: ")) << events[1];
    EXPECT_TRUE(has(events[1], site_of("CATCH_RETHROW_PTR"))) << events[1];
    EXPECT_TRUE(has(events[2], "!! rethrow std::logic_error  (rethrown at: ")) << events[2];
    EXPECT_TRUE(has(events[2], site_of("RETHROW_PTR", " via std::rethrow_exception"))) << events[2];
    EXPECT_TRUE(has(events[3], "!! terminate std::logic_error \"nobody catches this\"  (no handler found)"))
            << events[3];
    EXPECT_EQ(events[3], trace_lines.back()) << trace_content;
    for (size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(count_indentation_depth(events[i]), 2) << events[i];
        EXPECT_FALSE(has(events[i], "<unknown function>")) << events[i];
    }
}

// An exception escaping a noexcept function never reaches the handler in the
// caller: GCC's landing pad calls the runtime's __cxa_call_terminate, Clang's
// calls a stub in the executable (__clang_call_terminate), and both are told
// from a real catch. The terminate line is a child of the noexcept function on
// both compilers, the frames the exception unwound are marked, and nothing
// looks like a catch.
TEST_F(LogExceptionsUncaughtTest, ExceptionEscapingNoexceptEndsInTheTerminateLine) {
    ASSERT_NO_FATAL_FAILURE(run("noexcept"));
    const std::vector<std::string> events = event_lines();
    ASSERT_EQ(events.size(), 2u) << "throw and terminate expected:\n" << trace_content;
    EXPECT_TRUE(has(events[0], "!! throw std::logic_error \"nobody catches this\"  (thrown at: ")) << events[0];
    EXPECT_TRUE(has(events[1],
                    "!! terminate std::logic_error \"nobody catches this\"  (thrown across a noexcept boundary)"))
            << events[1];
    EXPECT_EQ(events[1], trace_lines.back()) << trace_content;
    EXPECT_EQ(field_of_line(events[1]), "[ terminate]") << events[1];
    EXPECT_EQ(count_indentation_depth(events[1]), 3) << "a child of noexcept_wall:\n" << events[1];
    for (const char* fn : { "uncaught_outer", "uncaught_leaf" }) {
        const std::string line = find_unique_line(trace_lines, fn);
        ASSERT_FALSE(line.empty()) << fn << " line missing or not unique. Trace:\n" << trace_content;
        EXPECT_EQ(tree_glyph_of(line), '!') << "left by the exception:\n" << line;
    }
    EXPECT_EQ(tree_glyph_of(find_unique_line(trace_lines, "noexcept_wall")), '|') << trace_content;
}

// A handler that calls std::terminate() itself: the catch is a real one, then
// the std::terminate interposer writes the terminate line with the call's
// exact site and the what() of the exception being handled.
TEST_F(LogExceptionsUncaughtTest, HandlerCallingTerminateEndsInTheTerminateLineWithItsSite) {
    ASSERT_NO_FATAL_FAILURE(run("catch_terminate"));
    const std::vector<std::string> events = event_lines();
    ASSERT_EQ(events.size(), 3u) << "throw, catch, terminate expected:\n" << trace_content;
    EXPECT_TRUE(has(events[1], "!! catch std::logic_error \"nobody catches this\"  (caught at: ")) << events[1];
    EXPECT_TRUE(has(events[1], site_of("CATCH_TERMINATE"))) << events[1];
    EXPECT_TRUE(has(events[2], "!! terminate std::logic_error \"nobody catches this\"  (std::terminate called at: "))
            << events[2];
    EXPECT_TRUE(has(events[2], site_of("TERMINATE_CALL"))) << events[2];
    EXPECT_EQ(events[2], trace_lines.back()) << trace_content;
    EXPECT_EQ(field_of_line(events[2]), "[ terminate]") << events[2];
    EXPECT_EQ(count_indentation_depth(events[1]), 2) << events[1];
    EXPECT_EQ(count_indentation_depth(events[2]), 2) << events[2];
}

// A std::thread whose function throws: nothing on that thread catches, so the
// runtime terminates from there and the events belong to the worker's own
// trace file — the throw line and, as its last line, the terminate line. It
// reads "(no handler found)" where the thread routine has no handler of its
// own (the runtime gives up inside __cxa_throw, with no unwinding, so the
// worker's frames stay pending), or "(std::terminate called at: ...)" where
// an older runtime catches and terminates itself. The main file, blocked in
// join(), carries no event and keeps its frames pending.
TEST_F(LogExceptionsUncaughtTest, ThreadFunctionThrowingEndsInTheTerminateLineOfItsOwnFile) {
    ASSERT_NO_FATAL_FAILURE(run("thread"));
    ASSERT_EQ(worker_paths.size(), 1u) << "expected exactly one worker trace file";
    std::string worker_content;
    for (const auto& line : worker_lines) worker_content += line + "\n";
    ASSERT_FALSE(worker_lines.empty());
    const std::string& last = worker_lines.back();
    EXPECT_TRUE(has(last, "!! terminate std::logic_error \"nobody catches this\"  (")) << worker_content;
    EXPECT_TRUE(has(last, "(no handler found)") || has(last, "(std::terminate called at: ")) << last;
    EXPECT_EQ(field_of_line(last), "[ terminate]") << last;
    const std::string throw_line =
            unique_event_line(worker_lines, "!! throw std::logic_error \"nobody catches this\"  (thrown at: ");
    ASSERT_FALSE(throw_line.empty()) << worker_content;
    EXPECT_TRUE(has(throw_line, site_of("THROW_UNCAUGHT"))) << throw_line;
    if (has(last, "(no handler found)")) {
        EXPECT_EQ(count_indentation_depth(last), count_indentation_depth(throw_line))
                << "both children of uncaught_leaf:\n" << worker_content;
        for (const char* fn : { "uncaught_outer", "uncaught_leaf" }) {
            const std::string line = find_unique_line(worker_lines, fn);
            ASSERT_FALSE(line.empty()) << fn << " line missing or not unique:\n" << worker_content;
            EXPECT_NE(line.find("[  pending ]"), std::string::npos) << line;
        }
    }
    EXPECT_EQ(trace_content.find("!! "), std::string::npos) << "an event in the main file:\n" << trace_content;
    const std::string joiner = find_unique_line(trace_lines, "thread_function_throws");
    ASSERT_FALSE(joiner.empty()) << trace_content;
    EXPECT_NE(joiner.find("[  pending ]"), std::string::npos) << joiner;
}

// std::terminate() with no exception at all still leaves a line saying so,
// with the call's site.
TEST_F(LogExceptionsUncaughtTest, TerminateWithoutAnExceptionLeavesALine) {
    ASSERT_NO_FATAL_FAILURE(run("no_exception"));
    const std::vector<std::string> events = event_lines();
    ASSERT_EQ(events.size(), 1u) << "only the terminate line expected:\n" << trace_content;
    EXPECT_TRUE(has(events[0], "!! terminate (no active exception)  (std::terminate called at: ")) << events[0];
    EXPECT_TRUE(has(events[0], site_of("TERMINATE_CALL"))) << events[0];
    EXPECT_EQ(events[0], trace_lines.back()) << trace_content;
    EXPECT_EQ(field_of_line(events[0]), "[ terminate]") << events[0];
    EXPECT_EQ(count_indentation_depth(events[0]), 2) << "a child of terminate_without_exception:\n" << events[0];
}

// ============================================================================
// LOG_EXCEPTIONS — frames inlined into the catcher or into a retry loop share
// its physical frame; the DWARF inline chain tells the dead ones from the live
// host, at the catch and at the next enter, so the following calls sit at
// their true depth immediately (inlined_traced_program.cpp, always_inline,
// -O2).
// ============================================================================

// main -> inl_catcher(1) -> inl_middle(2, inlined) -> inl_thrower(3, inlined) throws;
// caught in inl_catcher, which then calls inl_after_catch: a child of the
// catcher (2), not of the stale inlined frames.
TEST_F(LogExceptionsInlinedTest, CallAfterCatchSitsUnderTheInlinedCatcherNotUnderTheUnwoundFrames) {
    EXPECT_EQ(depth_of(trace_lines, "inl_catcher"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_middle"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_thrower"), 3) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_after_catch"), 2)
            << "the call after the catch must not sit under the unwound inlined frames:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_marker_a"), 1) << trace_content;

    const std::string catch_line = unique_event_line(trace_lines, "!! catch std::runtime_error \"inlined throw\"");
    ASSERT_FALSE(catch_line.empty()) << trace_content;
    EXPECT_EQ(count_indentation_depth(catch_line), 2) << "the catch line is a child of the catcher:\n"
                                                        << catch_line;
    EXPECT_NE(catch_line.find(":" + std::to_string(site_lines["CATCH_INL"]) + ")"), std::string::npos)
            << catch_line;
    EXPECT_EQ(glyph_of(trace_lines, "inl_middle"), '!') << trace_content;
    EXPECT_EQ(glyph_of(trace_lines, "inl_thrower"), '!') << trace_content;
    EXPECT_EQ(glyph_of(trace_lines, "inl_after_catch"), '|') << trace_content;
}

// Three longjmps out of an inlined leaf re-entered from the same site: the
// stale records are reclaimed at each re-entry, so the marker after the loop is
// a child of the loop function (2), and every leaf frame is marked '~'.
TEST_F(LogExceptionsInlinedTest, RetryLoopWithAnInlinedLeafKeepsTheMarkerAtItsDepth) {
    EXPECT_EQ(depth_of(trace_lines, "inl_loop_jump"), 1) << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "inl_jump_leaf"), (std::vector<int>{ 2, 2, 2 })) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_loop_marker"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_marker_b"), 1) << trace_content;
    for (const auto& line : lines_of(trace_lines, "inl_jump_leaf")) {
        EXPECT_EQ(tree_glyph_of(line), '~') << line;
    }
}

// A lambda defined in inl_lambda_catcher, inlined into it, throws; the catcher
// catches. Its operator() is a local entity whose demangled name starts with the
// catcher's own, and the base names must still differ: the catch-site chain
// names the catcher, whose record lies below the dead lambda's. The call after
// the catch is then a child of the catcher (2), not of the lambda; the lambda's
// line carries '!'. (GCC spells the lambda "{lambda(int)#1}", Clang "$_0".)
TEST_F(LogExceptionsInlinedTest, CallAfterCatchingAnInlinedLambdaSitsUnderTheCatcher) {
    EXPECT_EQ(depth_of(trace_lines, "inl_lambda_catcher(int)  (called from:"), 1) << trace_content;
    std::vector<std::string> lambda_lines;
    for (const auto& line : lines_of(trace_lines, "::operator()(int) const")) {
        if (line.find("inl_lambda_catcher(int)::") != std::string::npos) lambda_lines.push_back(line);
    }
    ASSERT_EQ(lambda_lines.size(), 1u) << trace_content;
    EXPECT_EQ(count_indentation_depth(lambda_lines[0]), 2) << lambda_lines[0];
    EXPECT_EQ(tree_glyph_of(lambda_lines[0]), '!') << lambda_lines[0];
    const std::string catch_line = unique_event_line(trace_lines, "!! catch std::runtime_error \"lambda throw\"");
    ASSERT_FALSE(catch_line.empty()) << trace_content;
    EXPECT_EQ(count_indentation_depth(catch_line), 2) << "the catch line is a child of the catcher:\n" << catch_line;
    EXPECT_NE(catch_line.find(":" + std::to_string(site_lines["CATCH_LAMBDA"]) + ")"), std::string::npos) << catch_line;
    EXPECT_EQ(depth_of(trace_lines, "inl_after_lambda_catch"), 2)
            << "the call after the catch must not sit under the unwound lambda:\n" << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_marker_c"), 1) << trace_content;
}

// Functions without a DWARF linkage name — internal-linkage C++ functions on
// GCC (anonymous namespace, static, a lambda, a lambda passed to
// std::for_each) and a C function on both compilers — inlined into their hosts
// call another function. Each parent's record is live while the call runs, so
// the callee is its child (3) and the host's next call its sibling (2);
// nothing is reclaimed, so no line carries a mark. Naming the call site's
// function from the symbol table, which inside inlined code only knows the
// HOST, took the parent's live record for a dead one, and every later line sat
// one or more levels too shallow.
TEST_F(LogExceptionsInlinedTest, CallsFromInlinedInternalLinkageFunctionsSitUnderThem) {
    EXPECT_EQ(depth_of(trace_lines, "inl_anon_host"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_static_host"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_lambda_host()  (called from:"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_anon_parent"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_static_parent"), 2) << trace_content;
    const std::vector<std::string> lambda_parent = lines_of(trace_lines, "inl_lambda_host()::");
    ASSERT_EQ(lambda_parent.size(), 1u) << trace_content;
    EXPECT_EQ(count_indentation_depth(lambda_parent[0]), 2) << lambda_parent[0];
    EXPECT_EQ(tree_glyph_of(lambda_parent[0]), '|') << "a live frame was reclaimed:\n" << lambda_parent[0];
    EXPECT_EQ(depth_of(trace_lines, "inl_for_each_host()  (called from:"), 1) << trace_content;
    std::vector<std::string> for_each_lambdas;
    for (const auto& line : lines_of(trace_lines, "inl_for_each_host()::")) {
        for_each_lambdas.push_back(line);
    }
    ASSERT_EQ(for_each_lambdas.size(), 3u) << "one line per element of the std::for_each:\n" << trace_content;
    for (const auto& line : for_each_lambdas) {
        EXPECT_EQ(count_indentation_depth(line), 2) << line;
        EXPECT_EQ(tree_glyph_of(line), '|') << "a live frame was reclaimed:\n" << line;
    }
    EXPECT_EQ(depths_of(trace_lines, "inl_child"), (std::vector<int>{ 3, 3, 3, 3, 3, 3 }))
            << "each call must sit under its inlined parent:\n" << trace_content;
    EXPECT_EQ(depths_of(trace_lines, "inl_sibling"), (std::vector<int>{ 2, 2, 2, 2 })) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_c_host"), 1) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_c_parent"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_c_child"), 3) << "the call must sit under the inlined C function:\n"
                                                        << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_c_sibling"), 2) << trace_content;
    EXPECT_EQ(depth_of(trace_lines, "inl_marker_d"), 1) << "at the end:\n" << trace_content;
    for (const char* name : { "inl_anon_parent", "inl_static_parent", "inl_child", "inl_sibling",
                              "inl_c_parent", "inl_c_child", "inl_c_sibling" }) {
        for (const auto& line : lines_of(trace_lines, name)) {
            EXPECT_EQ(tree_glyph_of(line), '|') << "a live frame was reclaimed:\n" << line;
        }
    }
}

// ============================================================================
// Where the resolver's input lives — separate debug files, compressed debug
// sections, object files that cannot be read, addresses outside every object,
// and the executable layouts (PIE, non-PIE, stripped). These pin the
// symbolizer (libbacktrace, registered per object at its load bias) end to end.
// ============================================================================

namespace {

// Runs `program` with the given arguments and a fresh trace file; returns the
// trace. The program's stdout goes to `stdout_text`, its raw exit status to
// `status`.
std::string run_traced_program(const std::string& program, const std::string& args, int& status,
                               std::string& stdout_text) {
    char trace_path[] = "/tmp/cslg_resolver_trace_XXXXXX";
    int fd = mkstemp(trace_path);
    if (fd < 0) {
        status = -1;
        return "";
    }
    close(fd);
    char out_path[] = "/tmp/cslg_resolver_stdout_XXXXXX";
    fd = mkstemp(out_path);
    if (fd < 0) {
        unlink(trace_path);
        status = -1;
        return "";
    }
    close(fd);
    const std::string cmd = "CSLG_OUTPUT_FILE=\"" + std::string(trace_path) + "\" timeout 60 \"" + program + "\" "
                            + args + " > \"" + std::string(out_path) + "\" 2>&1";
    status = system(cmd.c_str());
    const std::string trace = read_file(trace_path);
    stdout_text = read_file(out_path);
    unlink(trace_path);
    unlink(out_path);
    return trace;
}

// The entry lines of a trace (everything with a "(called from:" column).
std::vector<std::string> entry_lines_of(const std::string& trace) {
    std::vector<std::string> entries;
    for (const std::string& line : split_lines(trace)) {
        if (line.find("(called from:") != std::string::npos) {
            entries.push_back(line);
        }
    }
    return entries;
}

// Whether the ELF file at `path` has a section called `name` — read from its
// section header string table, not by searching the file's bytes: a program
// that contains the symbolizer carries the section names of DWARF as string
// literals in its own data. Used to confirm a fixture's post-build step
// (strip, objcopy) really ran.
bool elf_has_section(const std::string& path, const std::string& name) {
    const std::string content = read_file(path);
    if (content.size() < sizeof(ElfW(Ehdr))) {
        return false;
    }
    ElfW(Ehdr) ehdr {};
    std::memcpy(&ehdr, content.data(), sizeof ehdr);
    if (ehdr.e_shoff == 0 || ehdr.e_shstrndx >= ehdr.e_shnum
        || ehdr.e_shoff + static_cast<size_t>(ehdr.e_shnum) * sizeof(ElfW(Shdr)) > content.size()) {
        return false;
    }
    std::vector<ElfW(Shdr)> headers(ehdr.e_shnum);
    std::memcpy(headers.data(), content.data() + ehdr.e_shoff, headers.size() * sizeof(ElfW(Shdr)));
    const ElfW(Shdr)& strings = headers[ehdr.e_shstrndx];
    if (strings.sh_offset + strings.sh_size > content.size()) {
        return false;
    }
    for (const ElfW(Shdr)& header : headers) {
        if (header.sh_name >= strings.sh_size) {
            continue;
        }
        const char* section_name = content.data() + strings.sh_offset + header.sh_name;
        if (std::strncmp(section_name, name.c_str(), strings.sh_size - header.sh_name) == 0) {
            return true;
        }
    }
    return false;
}

// Every build of traced_program.cpp — PIE, non-PIE, stripped with a debug
// file — must produce the trace of the default build: the same entries, the
// same names, the same call sites, line for line (timestamps aside). Comparing
// against the default build keeps the check exact at every optimization level:
// at -O2 an inlined callee reports its host's call site, and the reference
// trace reflects the same inlining decisions.
std::vector<std::string> entries_without_timestamps(const std::string& trace) {
    // The columns that legitimately differ between two runs: the timestamp,
    // the duration field of a LOG_ELAPSED build and the address column of a
    // LOG_ADDR build (the sanitizer configurations build the default library
    // with both on). What must agree is everything else: the tree, the names
    // and the call sites.
    static const std::regex varying_columns(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] (\[[^\]]{10}\] )?(addr: \[0x[0-9a-f]+\] )?)");
    std::vector<std::string> entries;
    for (const std::string& line : entry_lines_of(trace)) {
        entries.push_back(std::regex_replace(line, varying_columns, ""));
    }
    return entries;
}

void expect_same_trace_as_default_build(const std::string& trace, const char* what) {
    int status = 0;
    std::string out;
    const std::string reference = run_traced_program(TRACED_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_EQ(out.find("[call-stack-logger] WARNING"), std::string::npos)
            << "the default build printed a warning:\n" << out;
    const std::vector<std::string> expected = entries_without_timestamps(reference);
    const std::vector<std::string> actual = entries_without_timestamps(trace);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(actual, expected) << what << " differs from the default build.\nDefault build:\n" << reference
                                << "\n" << what << ":\n" << trace;
    EXPECT_NE(trace.find("|_ file_static_func(int)  (called from: "), std::string::npos) << what << ": trace:\n"
                                                                                           << trace;
}

// A call line whose callee is `function` and whose site is a line of `file`.
std::regex callee_at(const std::string& function, const std::string& file) {
    return std::regex("\\|_ " + function + "  \\(called from: [^\\n]*" + file + ":[0-9]+\\)");
}

} // namespace

// The library's DWARF data was moved to <lib>.debug after the build and the
// library carries only a .gnu_debuglink to it. libbacktrace follows the link
// (same directory, .debug/, /usr/lib/debug/) and checks the file's CRC; names
// and lines must come out as if the data were still inside the library.
TEST(SeparateDebugFileTest, DebuglinkFileSuppliesNamesAndLines) {
    ASSERT_FALSE(elf_has_section(SEPARATE_DEBUG_LIB_PATH, ".debug_info"))
            << "fixture still carries its debug info: the objcopy post-build step did not run";
    ASSERT_TRUE(elf_has_section(SEPARATE_DEBUG_LIB_PATH, ".gnu_debuglink"))
            << "fixture has no .gnu_debuglink section";
    ASSERT_TRUE(elf_has_section(std::string(SEPARATE_DEBUG_LIB_PATH) + ".debug", ".debug_info"))
            << "the separate debug file carries no debug info";

    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DEBUG_OBJECTS_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_TRUE(std::regex_search(trace, callee_at("separate_debug_helper\\(int\\)", "separate_debug_lib\\.cpp")))
            << "helper's call site not resolved through the .debug file. Trace:\n" << trace;
    EXPECT_TRUE(std::regex_search(trace, callee_at("separate_debug_entry", "debug_objects_traced_program\\.cpp")))
            << "entry not traced by name. Trace:\n" << trace;
}

// -gz=zlib: the debug sections carry the SHF_COMPRESSED flag and a compression
// header; libbacktrace inflates them itself (no zlib dependency).
TEST(CompressedDebugSectionsTest, ZlibCompressedLinesResolve) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DEBUG_OBJECTS_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_TRUE(std::regex_search(trace, callee_at("zlib_debug_helper\\(int\\)", "compressed_debug_lib\\.cpp")))
            << "call site inside the zlib-compressed library not resolved. Trace:\n" << trace;
}

// -gz=zstd, where the toolchain supports it: libbacktrace has its own zstd
// decoder as well.
TEST(CompressedDebugSectionsTest, ZstdCompressedLinesResolve) {
#ifndef CSLG_HAVE_ZSTD_DEBUG
    GTEST_SKIP() << "the toolchain does not support -gz=zstd";
#else
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DEBUG_OBJECTS_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_TRUE(std::regex_search(trace, callee_at("zstd_debug_helper\\(int\\)", "compressed_debug_lib\\.cpp")))
            << "call site inside the zstd-compressed library not resolved. Trace:\n" << trace;
#endif
}

// A plugin whose file was deleted after loading: the mapping's path ends in
// " (deleted)", the open fails, and every frame inside the plugin degrades to
// the documented text — the callee's name and the caller's location alike —
// while the program runs to completion.
TEST(DamagedObjectTest, DeletedPluginFileIsReportedNotRead) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "deleted", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ <could not open object file>  (called from: "), std::string::npos)
            << "plugin callee not reported as unreadable. Trace:\n" << trace;
    EXPECT_NE(trace.find("(called from: <could not open caller object file>:\?\?\?)"), std::string::npos)
            << "call site inside the plugin not reported as unreadable. Trace:\n" << trace;
    EXPECT_EQ(trace.find("dlopen_plugin_helper"), std::string::npos)
            << "a name came from a file that no longer exists. Trace:\n" << trace;
}

// A plugin file cut right after its last PT_LOAD segment: the loader maps it
// fine, but its section headers and debug sections lie beyond the end of the
// file. libbacktrace would mmap the missing range and take SIGBUS reading it;
// the resolver's layout check rejects the file first. The program must finish
// with the same degraded text as for a deleted file.
TEST(DamagedObjectTest, TruncatedPluginFileIsRejectedNotMapped) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "truncated", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish cleanly (a signal means the truncated file was mapped "
                            "and read): "
                         << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ <could not open object file>  (called from: "), std::string::npos)
            << "truncated plugin not reported as unreadable. Trace:\n" << trace;
    EXPECT_NE(trace.find("(called from: <could not open caller object file>:\?\?\?)"), std::string::npos)
            << trace;
}

// The deleted plugin's mapping shows "<path> (deleted)" in /proc/self/maps, and
// a file was created under exactly that name (here a copy of the same plugin,
// so that reading it would visibly succeed). It must not be read: whatever
// sits under that name is not the mapped file.
TEST(DamagedObjectTest, FilePlantedUnderTheDeletedMappingsNameIsNotRead) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "planted", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ <could not open object file>  (called from: "), std::string::npos)
            << "the planted file was read as the deleted plugin. Trace:\n" << trace;
    EXPECT_EQ(trace.find("dlopen_plugin_helper"), std::string::npos) << trace;
}

// dladdr() names the vdso "linux-vdso.so.1", a bare name with no file behind
// it. A resolver that opened that name would read a file of that name in the
// current directory (here a planted copy of the plugin, looked up at the
// offset of one of its functions) as the vdso's symbols.
TEST(DamagedObjectTest, VdsoIsNotLookedUpInTheCurrentDirectory) {
    int status = 0;
    std::string out;
    run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "vdso", status, out);
    ASSERT_EQ(status, 0) << out;
    if (out.find("VDSO_SKIP") != std::string::npos) {
        GTEST_SKIP() << out;
    }
    EXPECT_NE(out.find("vdso: NAME=<could not open object file>"), std::string::npos)
            << "the planted ./linux-vdso.so.1 was read as the vdso:\n" << out;
    EXPECT_EQ(out.find("dlopen_plugin"), std::string::npos) << out;
}

// A plugin whose DWARF data no reader can use (its first unit claims DWARF
// version 99) but whose symbol table is intact: the symbol table still names
// every function, the lines read "???". Rejecting the whole file would turn
// every name into "<could not open object file>", which is not what libbfd
// did with such a file either.
TEST(DamagedObjectTest, UnreadableDwarfKeepsTheSymbolNames) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "dwarf", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_EQ(trace.find("<could not open"), std::string::npos)
            << "the symbol table was discarded with the DWARF data. Trace:\n" << trace;
    EXPECT_NE(trace.find("|_ dlopen_plugin_entry  (called from: "), std::string::npos) << trace;
    EXPECT_NE(trace.find("|_ dlopen_plugin_helper(int)  (called from: dlopen_plugin_entry:\?\?\?)"),
              std::string::npos)
            << "the call site inside the plugin is named from the symbol table, without a line. Trace:\n"
            << trace;
}

// The plugin's index of its section-name table (e_shstrndx) points far past
// its section header table. libbacktrace takes the index as it is and reads a
// section header from beyond the mapped table; the layout check rejects the
// file first, and the plugin's frames get the documented text.
TEST(DamagedObjectTest, SectionNameIndexPastTheHeaderTableIsRejected) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "shstrndx", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish cleanly (a signal means the index was used): " << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ <could not open object file>  (called from: "), std::string::npos) << trace;
}

// The plugin's build-ID note claims a 16 MiB descriptor in a section of a few
// dozen bytes. libbacktrace's size check is the wrong way round, so it would
// read the "build ID" from far beyond the note; the layout check, which wants
// every note to fit its section, rejects the file first.
TEST(DamagedObjectTest, BuildIdNoteLargerThanItsSectionIsRejected) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "buildid", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish cleanly (a signal means the note was read): " << out;
    if (out.find("BUILDID_SKIP") != std::string::npos) {
        GTEST_SKIP() << out;
    }
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ <could not open object file>  (called from: "), std::string::npos) << trace;
}

// The plugin's separate debug file names a dwz alt file by a relative name,
// and a truncated ELF file sits under that name next to the debug file, which
// is also the current directory. libbacktrace maps an alt file without a size
// check and would fault reading this one; every file it opens goes through
// the layout check, so the alt file is rejected and the debug file is read
// without it: names and lines still resolve.
TEST(DamagedObjectTest, TruncatedAltFileIsRejectedAndTheDebugFileStillReads) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "truncated-alt", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish cleanly (a signal means the alt file was read): "
                         << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_TRUE(std::regex_search(trace, callee_at("dlopen_plugin_helper\\(int\\)", "dlopen_plugin\\.cpp")))
            << "the debug file was not read. Trace:\n" << trace;
}

// A FIFO sits under the debug file's name next to the plugin. Opening a FIFO
// for reading waits for a writer, and the lookup holds the resolver's lock:
// the whole program would hang. Only regular files are opened, so the debug
// file counts as missing and the plugin's own symbol table names its
// functions.
TEST(DamagedObjectTest, FifoUnderTheDebugFilesNameIsNotOpened) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "fifo-debuglink", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish (an exit status of 124 means it hung on the FIFO): "
                         << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_NE(trace.find("|_ dlopen_plugin_helper(int)  (called from: dlopen_plugin_entry:\?\?\?)"),
              std::string::npos)
            << "the plugin's symbol table did not name its functions. Trace:\n" << trace;
}

// A relative alt link is relative to the directory of the file that carries
// it, as gdb reads it: libbacktrace looked it up from the current directory,
// where a FIFO of that name waits here. The debug file is found and read.
TEST(DamagedObjectTest, RelativeAltLinkIsNotLookedUpInTheCurrentDirectory) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "fifo-alt", status, out);
    ASSERT_EQ(status, 0) << "the program did not finish (an exit status of 124 means it hung on the FIFO): "
                         << out;
    EXPECT_NE(out.find("plugin result=11"), std::string::npos) << out;
    EXPECT_TRUE(std::regex_search(trace, callee_at("dlopen_plugin_helper\\(int\\)", "dlopen_plugin\\.cpp")))
            << "the debug file was not read. Trace:\n" << trace;
}

// Addresses no loaded object contains (dladdr fails): the public API reports
// them with the documented texts instead of failing or crashing.
TEST(DamagedObjectTest, AddressOutsideEveryObjectIsReported) {
    int status = 0;
    std::string out;
    run_traced_program(DAMAGED_OBJECTS_PROGRAM_PATH, "nomodule", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_NE(out.find("tiny: NAME=<address to object not found> FILE=<caller address to object not found> LINE=none"),
              std::string::npos)
            << out;
    EXPECT_NE(out.find("heap: NAME=<address to object not found> FILE=<caller address to object not found> LINE=none"),
              std::string::npos)
            << out;
}

// reload_lib_a is loaded, called (so the resolver registers it) and unloaded;
// the smaller reload_lib_b then lands at another base, inside a's old code
// segment (the driver steers it there, see dlopen_reload_traced_program.cpp).
// b's frames must be named after b. With one libbacktrace state for every
// object, a's module stayed in the chain and answered first, so b's functions
// were reported under a's names and lines.
TEST(DlopenReloadTest, LibraryLoadedIntoAnUnloadedOnesRangeIsNamedAfterItself) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(DLOPEN_RELOAD_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    if (out.find("RELOAD_OVERLAP=1") == std::string::npos) {
        GTEST_SKIP() << "reload_lib_b did not land inside reload_lib_a's old code segment:\n" << out;
    }
    const size_t b_start = trace.find("|_ reload_b_entry  (called from: ");
    ASSERT_NE(b_start, std::string::npos) << "reload_b_entry is not named after itself. Trace:\n" << trace;
    EXPECT_TRUE(std::regex_search(trace, callee_at("reload_b_helper\\(int\\)", "reload_lib_b\\.cpp")))
            << "the call site inside reload_lib_b is not resolved from b. Trace:\n" << trace;
    EXPECT_EQ(trace.find("reload_a_", b_start), std::string::npos)
            << "a frame after the reload carries reload_lib_a's name. Trace:\n" << trace;
}

// The traced program also defines libbacktrace's public functions, as a program
// does that links another libbacktrace statically. The tracer's copy is renamed
// (cslg_vendored_ prefix), so the two never meet: the trace is the default
// build's, and the other copy's functions are never called (each would abort).
// Hidden visibility alone does not keep a static link apart: the linker took
// the program's definitions for the tracer's calls.
TEST(OtherLibbacktraceTest, TracerNeverCallsTheProgramsOwnLibbacktrace) {
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(OTHER_LIBBACKTRACE_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_EQ(out.find("OTHER_LIBBACKTRACE"), std::string::npos) << out;
    expect_same_trace_as_default_build(trace, "program with a libbacktrace of its own");
}

// Every global symbol the vendored libbacktrace defines carries the cslg_
// prefix (the rename in third_party/libbacktrace/config/cslg_backtrace_rename.h,
// and the extension's own cslg_backtrace_* functions). A libbacktrace update
// that adds an external function the rename does not list fails here, before
// a user's link does.
TEST(OtherLibbacktraceTest, VendoredCopyDefinesOnlyPrefixedSymbols) {
    const std::string command = std::string("nm -g --defined-only \"") + VENDORED_LIBBACKTRACE_PATH + "\"";
    FILE* nm = popen(command.c_str(), "r");
    ASSERT_NE(nm, nullptr) << command;
    std::string listing;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, nm)) > 0) {
        listing.append(buffer, got);
    }
    ASSERT_EQ(pclose(nm), 0) << command << ":\n" << listing;
    std::vector<std::string> unprefixed;
    size_t symbols = 0;
    for (const std::string& line : split_lines(listing)) {
        // "<value> <type> <name>"; member headers ("state.c.o:") and blank lines have fewer fields.
        std::istringstream fields(line);
        std::string value, type, name;
        if (!(fields >> value >> type >> name)) {
            continue;
        }
        ++symbols;
        // Names starting with "__" belong to the compiler's own instrumentation (sanitizers, coverage).
        if (name.rfind("cslg_", 0) != 0 && name.rfind("__", 0) != 0) {
            unprefixed.push_back(name);
        }
    }
    EXPECT_GT(symbols, 0u) << listing;
    EXPECT_TRUE(unprefixed.empty()) << "unprefixed global symbols in " << VENDORED_LIBBACKTRACE_PATH << ": "
                                    << ::testing::PrintToString(unprefixed);
}

// The same program as a position-independent executable (load bias = map
// start) and as a fixed-address one (load bias 0, map start 0x400000): both
// resolve every name and line. A resolver that handed libbacktrace dli_fbase
// instead of link_map::l_addr would miss every address of the non-PIE build.
TEST(PieTest, PieAndNonPieExecutablesResolveAlike) {
    int status = 0;
    std::string out;
    const std::string pie = run_traced_program(TRACED_PROGRAM_PIE_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    expect_same_trace_as_default_build(pie, "PIE executable");
    const std::string nopie = run_traced_program(TRACED_PROGRAM_NOPIE_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    expect_same_trace_as_default_build(nopie, "non-PIE executable");
}

// A stripped executable with its debug file next to it, made the way the
// README recommends: cslg_keep_debug_info() wrote the .debug file and the
// .gnu_debuglink right after the link, and a plain `strip --strip-all` came
// later. The link survives the strip, the debug file carries the symbol table
// and the DWARF data, so the trace is complete — and no warning is printed.
TEST(StrippedExecutableTest, StrippedWithDebuglinkResolvesFully) {
    ASSERT_FALSE(elf_has_section(STRIPPED_DEBUGLINK_PROGRAM_PATH, ".debug_info"))
            << "fixture was not stripped: the post-build step did not run";
    ASSERT_FALSE(elf_has_section(STRIPPED_DEBUGLINK_PROGRAM_PATH, ".symtab"));
    ASSERT_TRUE(elf_has_section(STRIPPED_DEBUGLINK_PROGRAM_PATH, ".gnu_debuglink"))
            << "cslg_keep_debug_info() left no .gnu_debuglink, or the strip removed it";
    ASSERT_TRUE(elf_has_section(std::string(STRIPPED_DEBUGLINK_PROGRAM_PATH) + ".debug", ".debug_info"));
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(STRIPPED_DEBUGLINK_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    expect_same_trace_as_default_build(trace, "stripped executable with a .gnu_debuglink file");
    EXPECT_EQ(out.find("[call-stack-logger] WARNING"), std::string::npos)
            << "a warning although the debug file was found:\n" << out;
}

// A stripped executable with MiniDebugInfo, as Fedora ships its binaries: the
// symbol table and the debug information are gone, but .gnu_debugdata holds an
// xz-compressed symbol table of the functions the dynamic symbol table lacks,
// and libbacktrace reads it. The functions are named (without lines: there is
// no DWARF), and the bare-executable warning stays silent — it looked for a
// .symtab section only and told the user nothing could be named. The test
// requires a static function's name, which no dynamic symbol table holds
// (not even with -rdynamic), and not a particular line: where a call is
// attributed depends on the optimizer (Clang 18 inlines func_a into main at
// -O2, and without DWARF there is no inline chain to place it).
TEST(StrippedExecutableTest, StrippedWithMiniDebugInfoIsNamedWithoutAWarning) {
#ifndef MINIDEBUGINFO_PROGRAM_PATH
    GTEST_SKIP() << "xz is not installed: the MiniDebugInfo fixture was not built";
#else
    ASSERT_FALSE(elf_has_section(MINIDEBUGINFO_PROGRAM_PATH, ".symtab"))
            << "fixture was not stripped: the post-build step did not run";
    ASSERT_FALSE(elf_has_section(MINIDEBUGINFO_PROGRAM_PATH, ".debug_info"));
    ASSERT_TRUE(elf_has_section(MINIDEBUGINFO_PROGRAM_PATH, ".gnu_debugdata"));
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(MINIDEBUGINFO_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_EQ(out.find("[call-stack-logger] WARNING"), std::string::npos)
            << "a warning although MiniDebugInfo names the functions:\n" << out;
    EXPECT_NE(trace.find(" file_static_func(int)  (called from: "), std::string::npos)
            << "MiniDebugInfo did not name the functions. Trace:\n" << trace;
#endif
}

// A stripped executable with nothing to fall back on: the program runs to
// completion, the trace file is well-formed, and the resolver says once on
// stderr that the executable has no symbol table and no debug information.
// What the trace contains depends on the dynamic symbol table: with the narrow
// symbol export of this build only the hooks are in it, so no frame can be
// named and none is written; with -rdynamic (CSLG_EXPORT_ALL_SYMBOLS, or a
// linker without --export-dynamic-symbol) every global function keeps its name
// there and is traced without a line. Either way nothing crashes and every
// line keeps the trace format.
TEST(StrippedExecutableTest, StrippedWithoutSymbolsDegradesGracefully) {
    ASSERT_FALSE(elf_has_section(STRIPPED_PROGRAM_PATH, ".debug_info"))
            << "fixture was not stripped: the post-build step did not run";
    ASSERT_FALSE(elf_has_section(STRIPPED_PROGRAM_PATH, ".gnu_debuglink"));
    int status = 0;
    std::string out;
    const std::string trace = run_traced_program(STRIPPED_PROGRAM_PATH, "", status, out);
    ASSERT_EQ(status, 0) << out;
    EXPECT_NE(out.find("[call-stack-logger] WARNING: the executable has no symbol table and no debug "
                       "information"),
              std::string::npos)
            << "no warning about the bare executable on stderr:\n" << out;
    EXPECT_NE(trace.find("=== New trace run: "), std::string::npos) << trace;
    const std::regex well_formed(
            R"(^\[\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3}\] (\|  )*(\|_ )?[^ ].*  \(called from: .+:(\d+|\?\?\?)\)$)");
    const std::vector<std::string> entries = entry_lines_of(trace);
    for (const std::string& entry : entries) {
        EXPECT_TRUE(std::regex_match(entry, well_formed)) << entry;
        EXPECT_NE(entry.find(":\?\?\?)"), std::string::npos)
                << "a line number without debug information: " << entry;
    }
#ifdef CSLG_PROGRAMS_EXPORT_ALL_SYMBOLS
    EXPECT_NE(trace.find("|_ func_a()  (called from: "), std::string::npos)
            << "with -rdynamic the dynamic symbol table still names the global functions. Trace:\n" << trace;
#else
    EXPECT_TRUE(entries.empty()) << "only the hooks are exported, so no frame can be named. Trace:\n" << trace;
#endif
}
