/*
 * Integration tests for Call Stack Logger.
 *
 * Executes the traced_test_program and parses its trace output file to verify
 * that function instrumentation, symbol resolution, and formatting work
 * correctly end-to-end.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

// TRACED_PROGRAM_PATH is defined by CMake as a compile definition
// pointing to the absolute path of the traced_test_program executable.
#ifndef TRACED_PROGRAM_PATH
    #error "TRACED_PROGRAM_PATH must be defined by CMake"
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
