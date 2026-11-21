#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Mus {
    inline std::string_view TrimConditionToken(std::string_view token) {
        const auto first = token.find_first_not_of(" \t\r\n\f\v");
        if (first == token.npos) return {};
        return token.substr(first, token.find_last_not_of(" \t\r\n\f\v") - first + 1);
    }

    // Parse one call without splitting parentheses that belong to an argument,
    // such as a plugin filename. Only whitespace may follow the closing ')'.
    inline bool ReadConditionCall(std::string_view token, std::string& name, std::string& arguments) {
        token = TrimConditionToken(token);
        name.clear();
        arguments.clear();
        const auto open = token.find('(');
        if (open == token.npos) {
            if (token.find(')') != token.npos) return false;
            name = token;
            return true;
        }
        name = TrimConditionToken(token.substr(0, open));
        if (name.empty() || name.find(')') != name.npos) return false;
        unsigned depth = 1;
        for (auto i = open + 1; i < token.size(); ++i) {
            if (token[i] == '(') ++depth;
            else if (token[i] == ')' && --depth == 0) {
                if (!TrimConditionToken(token.substr(i + 1)).empty()) return false;
                arguments = TrimConditionToken(token.substr(open + 1, i - open - 1));
                return true;
            }
        }
        return false;
    }

    // AND/OR inside plugin names or keyword arguments are data, not operators.
    inline std::vector<std::string> SplitTopLevelConditions(std::string_view expression, std::string_view op) {
        std::vector<std::string> parts;
        auto append = [&](std::string_view part) {
            parts.emplace_back(TrimConditionToken(part));
        };
        std::size_t start = 0;
        unsigned depth = 0;
        for (std::size_t i = 0; !op.empty() && i < expression.size(); ++i) {
            if (expression[i] == '(') ++depth;
            else if (expression[i] == ')' && depth) --depth;
            else if (!depth && expression.substr(i, op.size()) == op &&
                (i == 0 || expression[i - 1] == ')' ||
                    std::string_view(" \t\r\n\f\v").find(expression[i - 1]) != std::string_view::npos)) {
                append(expression.substr(start, i - start));
                i += op.size() - 1;
                start = i + 1;
            }
        }
        append(expression.substr(start));
        return parts;
    }
}
