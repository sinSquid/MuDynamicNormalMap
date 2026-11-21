#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Mus {
    // AND/OR inside plugin names or keyword arguments are data, not operators.
    inline std::vector<std::string> SplitTopLevelConditions(std::string_view expression, std::string_view op) {
        std::vector<std::string> parts;
        auto append = [&](std::string_view part) {
            const auto first = part.find_first_not_of(" \t\r\n\f\v");
            if (first == std::string_view::npos) parts.emplace_back();
            else parts.emplace_back(part.substr(first, part.find_last_not_of(" \t\r\n\f\v") - first + 1));
        };
        std::size_t start = 0;
        unsigned depth = 0;
        for (std::size_t i = 0; !op.empty() && i < expression.size(); ++i) {
            if (expression[i] == '(') ++depth;
            else if (expression[i] == ')' && depth) --depth;
            else if (!depth && expression.substr(i, op.size()) == op) {
                append(expression.substr(start, i - start));
                i += op.size() - 1;
                start = i + 1;
            }
        }
        append(expression.substr(start));
        return parts;
    }
}
