#include "parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

static std::string StripFlagPrefix(const std::string& name) {
    // Longer prefixes must be checked before shorter ones to avoid partial matches
    // (e.g. "DFFlag" before "DFlag", "DFInt" before "DFlag")
    static const char* prefixes[] = {
        "SFFlag", "DFFlag", "FFloat",
        "DFInt",  "SFInt",  "FFlag",
        "DFlag",  "FLog",   "FInt",
        "SFlag",
    };
    for (const char* prefix : prefixes) {
        size_t len = strlen(prefix);
        if (name.size() > len && name.substr(0, len) == prefix) {
            return name.substr(len);
        }
    }
    return name;
}

ParseResult FlagParser::ParseText(const std::string& text) {
    ParseResult result;
    result.success = true;

    std::istringstream stream(text);
    std::string line;
    int lineNum = 0;

    while (std::getline(stream, line)) {
        lineNum++;
        line = Trim(line);

        if (line.empty() || line[0] == '#' || line[0] == '/')
            continue;

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            result.errors.push_back("Line " + std::to_string(lineNum) + ": missing '=' separator");
            continue;
        }

        std::string rawName = Trim(line.substr(0, eqPos));
        std::string value = Trim(line.substr(eqPos + 1));

        std::string name = StripFlagPrefix(rawName);

        if (name.empty()) {
            result.errors.push_back("Line " + std::to_string(lineNum) + ": empty flag name");
            continue;
        }

        result.flags.push_back({name, StripQuotes(value), rawName});
    }

    if (result.flags.empty() && !result.errors.empty())
        result.success = false;

    return result;
}

// Minimal JSON parser for {"key": value, ...} format
ParseResult FlagParser::ParseJson(const std::string& json) {
    ParseResult result;
    result.success = true;

    std::string s = Trim(json);
    if (s.empty() || s.front() != '{' || s.back() != '}') {
        result.errors.push_back("Invalid JSON: expected object");
        result.success = false;
        return result;
    }

    s = s.substr(1, s.size() - 2);

    size_t pos = 0;
    while (pos < s.size()) {
        // Skip whitespace
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == ','))
            pos++;

        if (pos >= s.size()) break;

        // Read key
        if (s[pos] != '"') {
            result.errors.push_back("Expected '\"' at position " + std::to_string(pos));
            break;
        }
        pos++;
        size_t keyStart = pos;
        while (pos < s.size() && s[pos] != '"') pos++;
        std::string key = s.substr(keyStart, pos - keyStart);
        pos++; // skip closing "

        // Skip colon
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == ':'))
            pos++;

        // Read value
        std::string value;
        if (pos < s.size() && s[pos] == '"') {
            pos++;
            size_t valStart = pos;
            while (pos < s.size() && s[pos] != '"') pos++;
            value = s.substr(valStart, pos - valStart);
            pos++;
        } else if (pos < s.size() && (s[pos] == 't' || s[pos] == 'f')) {
            size_t valStart = pos;
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ' ' && s[pos] != '\n')
                pos++;
            value = s.substr(valStart, pos - valStart);
        } else {
            size_t valStart = pos;
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ' ' && s[pos] != '\n')
                pos++;
            value = Trim(s.substr(valStart, pos - valStart));
        }

        std::string strippedKey = StripFlagPrefix(key);

        if (!strippedKey.empty())
            result.flags.push_back({strippedKey, value, key});
    }

    if (result.flags.empty()) {
        result.errors.push_back("No flags parsed from JSON");
        result.success = false;
    }

    return result;
}

ParseResult FlagParser::Parse(const std::string& input) {
    std::string trimmed = Trim(input);
    if (!trimmed.empty() && trimmed.front() == '{')
        return ParseJson(trimmed);
    return ParseText(trimmed);
}

ParseResult FlagParser::LoadFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        ParseResult result;
        result.success = false;
        result.errors.push_back("Failed to open file: " + filepath);
        return result;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return Parse(ss.str());
}

bool FlagParser::SaveToFile(const std::string& filepath, const std::vector<ParsedFlag>& flags) {
    std::ofstream file(filepath);
    if (!file.is_open())
        return false;

    file << "{\n";
    for (size_t i = 0; i < flags.size(); ++i) {
        file << "    \"" << flags[i].name << "\": ";

        bool isNum = true;
        for (char c : flags[i].value) {
            if (!std::isdigit(c) && c != '-' && c != '+' && c != '.') {
                isNum = false;
                break;
            }
        }

        if (flags[i].value == "true" || flags[i].value == "false" || isNum) {
            file << flags[i].value;
        } else {
            file << "\"" << flags[i].value << "\"";
        }

        if (i + 1 < flags.size()) file << ",";
        file << "\n";
    }
    file << "}\n";

    return true;
}
