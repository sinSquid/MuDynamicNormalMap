#include "../include/ConditionSyntax.h"
#include <cassert>

int main() {
    using Mus::SplitTopLevelConditions;
    assert((SplitTopLevelConditions("HASKEYWORDEDITORID(ARMOR)ORISACTOR(x)", "OR") ==
        std::vector<std::string>{"HASKEYWORDEDITORID(ARMOR)", "ISACTOR(x)"}));
    const auto ands = SplitTopLevelConditions(
        "  HasKeywordEditorID(BRAND_OR_ARMOR) OR IsActor(ORDINATOR.esp|001) AND NOT IsChild()  ", "AND");
    assert(ands.size() == 2 && ands[1] == "NOT IsChild()");
    const auto ors = SplitTopLevelConditions(ands[0], "OR");
    assert(ors.size() == 2);
    assert(ors[0] == "HasKeywordEditorID(BRAND_OR_ARMOR)");
    assert(ors[1] == "IsActor(ORDINATOR.esp|001)");
    assert((SplitTopLevelConditions("IsFemale()AND NOT IsChild()", "AND") ==
        std::vector<std::string>{"IsFemale()", "NOT IsChild()"}));
    assert((SplitTopLevelConditions("IsFemale()ORIsChild()", "OR") ==
        std::vector<std::string>{"IsFemale()", "IsChild()"}));
    assert((SplitTopLevelConditions("", "AND") == std::vector<std::string>{""}));
    assert((SplitTopLevelConditions("None()", "") == std::vector<std::string>{"None()"}));
    assert((SplitTopLevelConditions("Bad(ANDOR", "OR") == std::vector<std::string>{"Bad(ANDOR"}));
    std::string name, arguments;
    assert(Mus::ReadConditionCall(" IsActor(My (OR) patch.esp|123) ", name, arguments));
    assert(name == "IsActor" && arguments == "My (OR) patch.esp|123");
    assert(Mus::ReadConditionCall("IsFemale", name, arguments) && name == "IsFemale" && arguments.empty());
    assert(!Mus::ReadConditionCall("IsFemale(", name, arguments));
    assert(!Mus::ReadConditionCall("IsFemale())", name, arguments));
    assert(!Mus::ReadConditionCall("IsFemale() ignored", name, arguments));
    assert(!Mus::ReadConditionCall("(IsFemale())", name, arguments));

}
