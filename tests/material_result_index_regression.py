"""Generate a C++ differential fixture for the result index used during material installation."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
source = (root / "src/TaskManager.cpp").read_text(encoding="utf-8-sig")
build = source[source.index("using ResultIterator ="):source.index("// Serialize material installation")]
start = source.index("const auto paths = fallbackByPath.find")
end = source.index("if (found == textures.end())", start)
lookup = source[start:end].rsplit("}", 1)[0].replace("return RE::BSVisit::BSVisitControl::kContinue", "return textures.end()")
prefix = r"""
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
using bSlot=unsigned;
std::string lowLetter(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
bool IsSameString(std::string a,std::string b){return !a.empty()&&!b.empty()&&lowLetter(a)==lowLetter(b);}
bool IsContainString(std::string a,std::string b){return lowLetter(a).find(lowLetter(b))!=std::string::npos;}
struct Config{static Config& GetSingleton(){static Config c;return c;} bool overlay=true; bool GetApplyOverlay(){return overlay;}};
namespace RE{enum BIPED_OBJECT { kHead=0,kBody=2,kEditorTotal=32};struct BSGeometry{std::string name;bool dyn;bool AsDynamicTriShape()const{return dyn;}};}
struct Result{RE::BSGeometry* geometry;bSlot slot;std::string texturePath,geoName;};
struct Part{int slot;};struct Skin{int numPartitions;std::vector<Part> partitions;const Skin& GetRuntimeData()const{return *this;}};
int main(){std::mt19937 rng(14521);
std::vector<std::string> paths={"Textures/A.dds","textures/a.DDS","Textures/B.dds",""},names={"Body","BODY","L","Vagina","Anus","Canal",""};
RE::BSGeometry geometries[4]{};
int cases=0;
for(int batch=0;batch<250;++batch){
 std::vector<Result> input;
 for(int i=0,n=rng()%32;i<n;++i)input.push_back({&geometries[rng()%4],static_cast<unsigned>(rng()%5),paths[rng()%paths.size()],names[rng()%names.size()]});
 const auto& textures=input;
 Config::GetSingleton().overlay=(batch%2)==0;
"""
suffix = r"""
 for(bSlot slot=0;slot<6;++slot){
   auto old=!applyOverlay?textures.end():std::find_if(textures.begin(),textures.end(),[&](const auto& r){return r.slot==slot&&!IsContainString(r.geoName,"Vagina")&&!IsContainString(r.geoName,"Anus")&&!IsContainString(r.geoName,"Canal");});
   auto it=overlayBySlot.find(slot);assert(old==(it==overlayBySlot.end()?textures.end():it->second));
 }
 if(textures.size()>1)for(auto& geometry:geometries){
   auto old=std::find_if(textures.begin(),textures.end(),[&](const auto& r){return r.geometry==&geometry;});
   auto it=resultByGeometry.find(&geometry);assert(old==(it==resultByGeometry.end()?textures.end():it->second));
 }
 for(int k=0;k<100;++k){
  auto texturePath=paths[rng()%paths.size()];RE::BSGeometry shape{names[rng()%names.size()],(rng()%2)!=0};auto geo=&shape;
  Skin skin{};for(int j=0,n=rng()%7;j<n;++j)skin.partitions.push_back({static_cast<int>(rng()%8==0?0:29+rng()%8)});
  skin.numPartitions=static_cast<int>(skin.partitions.size());auto dismember=&skin;
  auto old=std::find_if(textures.begin(),textures.end(),[&](const Result& r){
    for(const auto& part:skin.partitions){int mapped=part.slot-30;if(mapped<0||mapped>=32){if(shape.dyn)mapped=0;else if(part.slot==0)mapped=2;else continue;}
    if(r.slot==mapped&&IsSameString(r.texturePath,texturePath)&&!IsSameString(r.geoName,shape.name))return true;}return false;});
  auto result=[&](){auto found=textures.end();
"""
tail = r"""
  return found;}();
  assert(result==old);++cases;
 }
}
std::cout<<"PASS "<<cases<<" ordered fallback cases and 1500 overlay lookups\n";}
"""
Path(sys.argv[1]).write_text(prefix + build + suffix + lookup + tail,encoding="utf-8")
