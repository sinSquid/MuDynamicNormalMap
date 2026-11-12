#include "IniConfig.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace Mus::IniConfig {
    namespace {
        struct Handle {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            Handle() = default;
            explicit Handle(HANDLE h) : value(h) {}
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
        };
        struct TemporaryFile {
            std::filesystem::path path;
            bool owned = false;
            ~TemporaryFile() { if (owned && !path.empty()) DeleteFileW(path.c_str()); }
        };
        struct Line { std::string text, ending; };
        struct Setting { std::string_view section, key; std::string value; bool found = false; };

        std::string_view Trim(std::string_view text) {
            const auto first = text.find_first_not_of(" \t\r");
            if (first == text.npos) return {};
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }
        std::string Number(float value) {
            std::array<char, 64> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                std::chars_format::general, std::numeric_limits<float>::max_digits10);
            if (result.ec != std::errc{}) throw std::runtime_error("Cannot format INI number");
            return {buffer.data(), result.ptr};
        }
        bool ReadBytes(HANDLE file, std::string& bytes) {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024)
                return false;
            bytes.resize(static_cast<std::size_t>(size.QuadPart));
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
            DWORD read = 0;
            return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
        }
        std::string Rewrite(std::string input, const RuntimeTuning& tuning) {
            const bool bom = input.starts_with("\xef\xbb\xbf");
            if (bom) input.erase(0, 3);
            std::vector<Line> lines;
            std::string newline = "\r\n";
            bool haveNewline = false;
            for (std::size_t pos = 0; pos < input.size();) {
                const auto end = input.find('\n', pos);
                if (end == input.npos) { lines.push_back({input.substr(pos), ""}); break; }
                const bool cr = end > pos && input[end - 1] == '\r';
                const auto ending = cr ? "\r\n" : "\n";
                if (!haveNewline) { newline = ending; haveNewline = true; }
                lines.push_back({input.substr(pos, end - pos - (cr ? 1 : 0)), ending});
                pos = end + 1;
            }
            std::array<Setting, 4> settings{{
                {"General", "UpdateDelayTick", std::to_string(tuning.updateDelayTick)},
                {"Performance", "UpdateDistance", Number(tuning.updateDistance)},
                {"RealtimeDetect", "DetectDistance", Number(tuning.detectDistance)},
                {"RealtimeDetect", "DetectTickMS", std::to_string(tuning.detectTickMS)}
            }};
            std::string section;
            for (auto& line : lines) {
                const auto comment = line.text.find_first_of("#;");
                const auto active = std::string_view(line.text).substr(0, comment);
                const auto trimmed = Trim(active);
                if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                    section = std::string(trimmed.substr(1, trimmed.size() - 2));
                    continue;
                }
                const auto equals = active.find('=');
                if (equals == active.npos) continue;
                const auto key = Trim(active.substr(0, equals));
                for (auto& setting : settings) {
                    if (section != setting.section || key != setting.key) continue;
                    auto begin = equals + 1;
                    while (begin < active.size() && (active[begin] == ' ' || active[begin] == '\t')) ++begin;
                    auto end = active.size();
                    while (end > begin && (active[end - 1] == ' ' || active[end - 1] == '\t' || active[end - 1] == '\r')) --end;
                    line.text.replace(begin, end - begin, setting.value);
                    setting.found = true;
                    break;
                }
            }
            // Insert missing keys at the end of the last matching section.
            for (const auto sectionName : {"General", "Performance", "RealtimeDetect"}) {
                std::vector<Line> additions;
                for (const auto& setting : settings)
                    if (!setting.found && setting.section == sectionName)
                        additions.push_back({std::string(setting.key) + "=" + setting.value, newline});
                if (additions.empty()) continue;
                std::size_t insertAt = lines.size();
                bool inSection = false, foundSection = false;
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    const auto active = std::string_view(lines[i].text).substr(0, lines[i].text.find_first_of("#;"));
                    const auto trimmed = Trim(active);
                    if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                        if (inSection) insertAt = i;
                        inSection = trimmed.substr(1, trimmed.size() - 2) == sectionName;
                        if (inSection) { foundSection = true; insertAt = lines.size(); }
                    }
                }
                if (!foundSection) additions.insert(additions.begin(), {"[" + std::string(sectionName) + "]", newline});
                if (insertAt > 0 && lines[insertAt - 1].ending.empty()) lines[insertAt - 1].ending = newline;
                lines.insert(lines.begin() + insertAt, additions.begin(), additions.end());
            }
            std::string result = bom ? "\xef\xbb\xbf" : "";
            for (const auto& line : lines) { result += line.text; result += line.ending; }
            return result;
        }
    }

    bool SaveRuntimeTuning(const std::filesystem::path& path, const RuntimeTuning& tuning, std::string& error) {
        static std::mutex saveMutex;
        static std::atomic<std::uint64_t> sequence{0};
        std::lock_guard lock(saveMutex);
        error.clear();
        RuntimeTuning normalized;
        if (!NormalizeRuntimeTuning(tuning, normalized, error)) return false;
        try {
            const auto absolute = std::filesystem::absolute(path);
            std::error_code ec;
            if (path.empty() || !std::filesystem::is_directory(absolute.parent_path(), ec) || ec) {
                error = "INI parent directory does not exist";
                return false;
            }
            Handle source(CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            const auto sourceError = source.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (source.value == INVALID_HANDLE_VALUE && sourceError != ERROR_FILE_NOT_FOUND) {
                error = "Cannot open INI (Win32 " + std::to_string(sourceError) + ")";
                return false;
            }
            std::string original;
            if (source.value != INVALID_HANDLE_VALUE && !ReadBytes(source.value, original)) {
                error = "Cannot read INI or INI exceeds 16 MiB";
                return false;
            }
            if (original.starts_with("\xff\xfe") || original.starts_with("\xfe\xff") || original.find('\0') != original.npos) {
                error = "UTF-16/UTF-32 INI is unsupported; file was not changed";
                return false;
            }
            const auto rewritten = Rewrite(original, normalized);
            TemporaryFile temporary{absolute.wstring() + L".menu.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence)};
            {
                Handle output(CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
                if (output.value == INVALID_HANDLE_VALUE) { error = "Cannot create temporary INI"; return false; }
                temporary.owned = true;
                DWORD written = 0;
                if (!WriteFile(output.value, rewritten.data(), static_cast<DWORD>(rewritten.size()), &written, nullptr) ||
                    written != rewritten.size() || !FlushFileBuffers(output.value)) {
                    error = "Cannot flush temporary INI";
                    return false;
                }
            }
            if (source.value != INVALID_HANDLE_VALUE) {
                Handle check(CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                std::string latest;
                if (check.value == INVALID_HANDLE_VALUE || !ReadBytes(check.value, latest) || latest != original) {
                    error = "INI changed during save; retry after reviewing external edits";
                    return false;
                }
                const auto backup = absolute.wstring() + L".menu.bak";
                if (!ReplaceFileW(absolute.c_str(), temporary.path.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
                    error = "Cannot replace INI (Win32 " + std::to_string(GetLastError()) + ")";
                    return false;
                }
            } else if (!MoveFileExW(temporary.path.c_str(), absolute.c_str(), MOVEFILE_WRITE_THROUGH)) {
                error = "Cannot create INI; another editor may have created it";
                return false;
            }
            temporary.path.clear();
            return true;
        } catch (const std::exception& e) {
            error = std::string("INI save failed: ") + e.what();
            return false;
        }
    }
}
