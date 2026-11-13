#include "IniConfig.h"
#include "SettingsCatalog.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
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
            bool Close() {
                if (value == INVALID_HANDLE_VALUE) return true;
                const auto file = value;
                value = INVALID_HANDLE_VALUE;
                return CloseHandle(file) != FALSE;
            }
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
        std::string Failure(std::string_view stage, const std::filesystem::path& path, DWORD code) {
            wchar_t message[1024]{};
            const auto length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                code, 0, message, static_cast<DWORD>(std::size(message)), nullptr);
            const auto count = WideCharToMultiByte(CP_UTF8, 0, message, length, nullptr, 0, nullptr, nullptr);
            std::string detail(count, '\0');
            if (count) WideCharToMultiByte(CP_UTF8, 0, message, length, detail.data(), count, nullptr, nullptr);
            const auto last = detail.find_last_not_of(" \t\r\n");
            detail.resize(last == detail.npos ? 0 : last + 1);
            const auto utf8 = path.u8string();
            return std::string(stage) + " [path=" + std::string(utf8.begin(), utf8.end()) +
                "] (Win32 " + std::to_string(code) + "): " + detail;
        }
        bool ReadBytes(HANDLE file, std::string& bytes) {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size)) return false;
            if (size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
                SetLastError(ERROR_FILE_TOO_LARGE);
                return false;
            }
            bytes.resize(static_cast<std::size_t>(size.QuadPart));
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
            DWORD read = 0;
            if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return false;
            if (read != bytes.size()) { SetLastError(ERROR_HANDLE_EOF); return false; }
            return true;
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
            std::vector<Setting> settings;
            std::vector<std::string_view> sections;
            for (const auto& descriptor : GetMenuSettingsCatalog()) {
                auto value = SettingValueText(descriptor, tuning);
                // The loader trims values and treats #/; as comments. Reject
                // strings that could change meaning or inject INI content.
                if (value.find_first_of("\r\n#;") != value.npos ||
                    value.find('\0') != value.npos || Trim(value) != value)
                    throw std::runtime_error("Cannot safely serialize INI setting " + std::string(descriptor.key));
                settings.push_back({descriptor.section, descriptor.key, std::move(value)});
                if (std::find(sections.begin(), sections.end(), descriptor.section) == sections.end())
                    sections.push_back(descriptor.section);
            }
            std::string section;
            for (auto& line : lines) {
                const auto comment = line.text.find_first_of("#;");
                const auto active = std::string_view(line.text).substr(0, comment);
                const auto trimmed = Trim(active);
                if (!trimmed.empty() && trimmed.front() == '[') {
                    section = trimmed.size() >= 2 && trimmed.back() == ']' ?
                        std::string(Trim(trimmed.substr(1, trimmed.size() - 2))) : std::string{};
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
            for (const auto sectionName : sections) {
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
                    if (!trimmed.empty() && trimmed.front() == '[') {
                        if (inSection) insertAt = i;
                        inSection = trimmed.size() >= 2 && trimmed.back() == ']' &&
                            Trim(trimmed.substr(1, trimmed.size() - 2)) == sectionName;
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
                error = Failure("Validate INI parent directory", absolute.parent_path(), ec ? static_cast<DWORD>(ec.value()) : ERROR_PATH_NOT_FOUND);
                return false;
            }
            Handle source(CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            const auto sourceError = source.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (source.value == INVALID_HANDLE_VALUE && sourceError != ERROR_FILE_NOT_FOUND && sourceError != ERROR_PATH_NOT_FOUND) {
                error = Failure("Open INI", absolute, sourceError);
                return false;
            }
            std::string original;
            if (source.value != INVALID_HANDLE_VALUE && !ReadBytes(source.value, original)) {
                error = Failure("Read INI (maximum 16 MiB)", absolute, GetLastError());
                return false;
            }
            const bool existed = source.value != INVALID_HANDLE_VALUE;
            if (!source.Close()) { error = Failure("Close source INI", absolute, GetLastError()); return false; }
            if (original.starts_with("\xff\xfe") || original.starts_with("\xfe\xff") || original.find('\0') != original.npos) {
                error = Failure("Reject unsupported UTF-16/UTF-32 INI", absolute, ERROR_NO_UNICODE_TRANSLATION);
                return false;
            }
            const auto rewritten = Rewrite(original, normalized);
            TemporaryFile temporary{absolute.wstring() + L".menu.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence)};
            {
                Handle output(CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
                if (output.value == INVALID_HANDLE_VALUE) { error = Failure("Create temporary INI", temporary.path, GetLastError()); return false; }
                temporary.owned = true;
                DWORD written = 0;
                if (!WriteFile(output.value, rewritten.data(), static_cast<DWORD>(rewritten.size()), &written, nullptr)) {
                    error = Failure("Write temporary INI", temporary.path, GetLastError()); return false;
                }
                if (written != rewritten.size()) {
                    error = Failure("Short write temporary INI", temporary.path, ERROR_WRITE_FAULT); return false;
                }
                if (!FlushFileBuffers(output.value)) {
                    error = Failure("Flush temporary INI", temporary.path, GetLastError()); return false;
                }
                if (!output.Close()) {
                    error = Failure("Close temporary INI", temporary.path, GetLastError()); return false;
                }
            }
            if (existed) {
                {
                    Handle check(CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (check.value == INVALID_HANDLE_VALUE) {
                        error = Failure("Reopen INI for conflict check", absolute, GetLastError()); return false;
                    }
                    std::string latest;
                    if (!ReadBytes(check.value, latest)) {
                        error = Failure("Read INI for conflict check", absolute, GetLastError()); return false;
                    }
                    if (latest != original) {
                        error = Failure("INI changed during save; review external edits and retry", absolute, ERROR_FILE_INVALID); return false;
                    }
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (!GetFileInformationByHandle(check.value, &info)) {
                        error = Failure("Inspect INI attributes", absolute, GetLastError());
                        return false;
                    }
                    if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                        error = Failure("INI is read-only; clear the read-only attribute and retry", absolute, ERROR_ACCESS_DENIED);
                        return false;
                    }
                    if (!check.Close()) {
                        error = Failure("Close conflict-check INI", absolute, GetLastError()); return false;
                    }
                }
                // Keep the previous user file independently of the rename. No source
                // handle may remain open when MO2's virtual filesystem commits it.
                const std::filesystem::path backup = absolute.wstring() + L".menu.bak";
                if (!CopyFileW(absolute.c_str(), backup.c_str(), FALSE)) {
                    error = Failure("Back up INI", backup, GetLastError()); return false;
                }
            }
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (existed ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(temporary.path.c_str(), absolute.c_str(), flags)) {
                error = Failure("Commit temporary INI", absolute, GetLastError()); return false;
            }
            temporary.path.clear();
            return true;
        } catch (const std::exception& e) {
            error = std::string("INI save failed: ") + e.what();
            return false;
        }
    }
}
