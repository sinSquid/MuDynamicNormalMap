"""Generate a fixture using the actual condition parser implementation.
Usage: python tests/condition_parser_regression.py OUTPUT.cpp
"""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'src/Condition.cpp').read_text(encoding='utf-8-sig')
a=source.index('ConditionManager::Condition ConditionManager::ParseConditions(')
b=source.index('bool ConditionManager::GetConditionFunction(',a)
body=source[a:b]
prefix=r"""
#include "ConditionSyntax.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace logger { template<class... T> void debug(T&&...){} template<class... T> void error(T&&...){} }
namespace magic_enum { template<class T> std::string_view enum_name(T){return "test";} }
namespace Mus {
std::string lowLetter(std::string text){for(auto& c:text)c=char(std::tolower(static_cast<unsigned char>(c)));return text;}
bool stringStartsWith(const std::string& text,const std::string& prefix){return lowLetter(text).starts_with(lowLetter(prefix));}
void trim(std::string& text){text=TrimConditionToken(text);}
std::vector<std::string> split(const std::string& text,const std::string& separator){
    std::vector<std::string> result;std::size_t start=0,end;
    while((end=text.find(separator,start))!=text.npos){result.emplace_back(TrimConditionToken(std::string_view(text).substr(start,end-start)));start=end+separator.size();}
    result.emplace_back(TrimConditionToken(std::string_view(text).substr(start)));return result;
}
std::uint32_t GetHex(const std::string& text){return std::strtoul(text.c_str(),nullptr,16);}
class ConditionManager {
public:
    enum ConditionType {IsFemale,IsChild,IsActor,HasKeywordEditorID,None,MissingFunction,Error};
    struct ConditionItem {bool NOT=false;ConditionType type=Error;std::string pluginName,arg;std::uint32_t id=0;};
    using ConditionItemOr=std::vector<ConditionItem>;
    struct Condition {std::string originalCondition,fileName;std::vector<ConditionItemOr> AND;};
    std::unordered_map<std::string,ConditionType> ConditionMap{{"isfemale",IsFemale},{"ischild",IsChild},{"isactor",IsActor},{"haskeywordeditorid",HasKeywordEditorID},{"none",None},{"missingfunction",MissingFunction}};
    Condition ParseConditions(Condition condition);
    const ConditionType GetConditionType(std::string line,ConditionItem& item);
    bool GetConditionFunction(ConditionItem& item){return item.type!=MissingFunction && item.type!=Error;}
};
"""
suffix=r"""
}
int main(){
    Mus::ConditionManager manager;
    using Type=Mus::ConditionManager::ConditionType;
    for(const auto* text:{"IsFemale() OR","OR IsFemale()","IsFemale() AND","NOT","IsFemale() OR Bogus()",
        "MissingFunction() OR None()","IsFemale(","IsFemale())","IsFemale() junk","IsActor(a|1|b|extra)"}){
        auto result=manager.ParseConditions({text,"bad.ini",{}});
        assert(result.AND.size()==1 && result.AND.front().empty());
    }
    auto empty=manager.ParseConditions({"  ","default.ini",{}});
    assert(empty.AND.size()==1 && empty.AND[0].size()==1 && empty.AND[0][0].type==Type::None);
    auto upper=manager.ParseConditions({"HASKEYWORDEDITORID(ARMOR)ORISACTOR(x.esp|123)","upper.ini",{}});
    assert(upper.AND.size()==1 && upper.AND[0].size()==2);
    assert(upper.AND[0][0].arg=="ARMOR" && upper.AND[0][1].pluginName=="x.esp");
    auto continued=manager.ParseConditions({"IsFemale() OR IsActor(My (OR) patch.esp|123) AND NOT IsChild()","continued.ini",{}});
    assert(continued.AND.size()==2 && continued.AND[0].size()==2 && continued.AND[1].size()==1);
    assert(continued.AND[0][1].pluginName=="My (OR) patch.esp" && continued.AND[0][1].id==0x123);
    assert(continued.AND[1][0].NOT);
    auto repeated=manager.ParseConditions(continued);
    assert(repeated.AND.size()==2);
}
"""
Path(sys.argv[1]).write_text(prefix+body+suffix,encoding='utf-8')
