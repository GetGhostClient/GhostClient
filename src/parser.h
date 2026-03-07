#pragma once
#include <string>
#include <vector>
#include <utility>

struct ParsedFlag {
    std::string name;
    std::string value;
    std::string originalName;
};

struct ParseResult {
    std::vector<ParsedFlag> flags;
    std::vector<std::string> errors;
    bool success;
};

namespace FlagParser {
    // Parse "FlagName=Value" format, one per line
    ParseResult ParseText(const std::string& text);

    // Parse JSON format: {"FlagName": value, ...}
    ParseResult ParseJson(const std::string& json);

    // Auto-detect format and parse
    ParseResult Parse(const std::string& input);

    // Load and parse from file
    ParseResult LoadFile(const std::string& filepath);

    // Save flags to JSON file
    bool SaveToFile(const std::string& filepath, const std::vector<ParsedFlag>& flags);
}
