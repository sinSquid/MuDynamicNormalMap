"""Run the real debug-layer ledger against small engine material/texture fixtures.
Usage: python tests/debug_diffuse_lifecycle_regression.py OUTPUT.cpp
Compile the generated translation unit with a C++20 compiler.
"""
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
ledger = (repo / "pregnancy/src/Rendering/InstalledLayers.cpp").read_text(encoding="utf-8-sig")
identity = (repo / "pregnancy/src/Rendering/InstalledLayerIdentity.h").read_text(encoding="utf-8-sig")
ledger = ledger[ledger.index("namespace PNS {"):]
identity = identity[identity.index("namespace PNS::Rendering"):]
prefix = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
template<class T> struct Ptr {
    T* v=nullptr;
    Ptr()=default; Ptr(T* p):v(p){} void reset(T* p=nullptr){v=p;}
    T* get()const{return v;} T* Get()const{return v;} T* operator->()const{return v;}
    explicit operator bool()const{return v!=nullptr;} bool operator==(const Ptr&) const=default;
};
struct ID3D11ShaderResourceView{int id;};
namespace REX::W32 {using ID3D11ShaderResourceView=::ID3D11ShaderResourceView;}
namespace Microsoft::WRL {template<class T> using ComPtr=Ptr<T>;}
namespace RE {
using FormID=std::uint32_t;
template<class T> using NiPointer=Ptr<T>;
template<class T> using BSTSmartPointer=Ptr<T>;
struct NiSourceTexture {std::string name;struct GPU{ID3D11ShaderResourceView* resourceView;}* rendererTexture;};
using NiSourceTexturePtr=Ptr<NiSourceTexture>;
struct BSTextureSet {enum class Texture{kNormal,kDiffuse};const char* diffusePath="skin.dds";const char* GetTexturePath(Texture t){return t==Texture::kDiffuse ? diffusePath : "normal.dds";}};
struct BSLightingShaderMaterialBase {NiSourceTexturePtr normalTexture,diffuseTexture;Ptr<BSTextureSet> textureSet;};
struct BSLightingShaderProperty {BSLightingShaderMaterialBase* material;};
struct BSGeometry {struct Runtime{Ptr<BSLightingShaderProperty> shaderProperty;}rt; Runtime& GetGeometryRuntimeData(){return rt;}};
struct Root{std::vector<BSGeometry*> shapes;};
struct Actor{FormID id=9;Root* root;Root* Get3D(){return root;}FormID GetFormID(){return id;}};
namespace BSVisit {enum class BSVisitControl{kContinue};
template<class Visitor>void TraverseScenegraphGeometries(Root* root,Visitor visit){for(auto* g:root->shapes)visit(g);}}
namespace BSGraphics {struct Renderer{static Renderer* GetSingleton(){static Renderer r;return &r;}void Lock(){}void Unlock(){}};}
}
template<class T,class U>T skyrim_cast(U* p){return static_cast<T>(p);}
template<class T,class U>T netimmerse_cast(U* p){return static_cast<T>(p);}
namespace PNS {namespace SkinPaths{std::string Relative(const char* s){return s;}}
bool SessionActive(){return true;}
RE::NiSourceTexturePtr ResolveRestoredBase(RE::NiSourceTexture* t){return t;}
void ForgetInstalledGeometry(RE::FormID, RE::BSGeometry*);
}
"""
suffix = r"""
int main(){
 using namespace PNS;
 ID3D11ShaderResourceView a{1},b{2},foreign{3};
 RE::NiSourceTexture::GPU ga{&a},gb{&b},gd{&a},gf{&foreign};
 RE::NiSourceTexture normal{"mu:body",&ga}, applied{"mu:body",&gd}, albedo{"skin",&gb},other{"mu:body",&gf};
 RE::BSTextureSet set;
 RE::BSLightingShaderMaterialBase m{&normal,&albedo,&set},clone;
 RE::BSLightingShaderProperty prop{&m};
 RE::BSGeometry g{{&prop}};RE::Root root{{&g}};RE::Actor actor{9,&root};
 auto put=[&](RE::BSLightingShaderMaterialBase& mat){
   auto orig=mat.diffuseTexture;mat.diffuseTexture=&applied;
   RecordInstalledLayer(&actor,&g,&mat,&normal,nullptr,&applied,orig.get());
 };
 put(m);assert(!HasInstalledLayer(9)&&layers.at(9).size()==1);
 assert(RestoreInstalledLayer(&actor)&&m.diffuseTexture.get()==&applied);
 assert(!RetireInstalledGeometry(9,&g)&&m.diffuseTexture.get()==&albedo&&layers.empty());
 put(m);
 // Regeneration rebinds only the canonical named normal; the private debug alias remains immutable.
 ga.resourceView=&b;
 clone=m;prop.material=&clone;OnInstalledMaterialCloned(9,&g,&m,&clone);
 assert(!RetireInstalledGeometry(9,&g)&&clone.diffuseTexture.get()==&albedo);
 put(clone);clone.diffuseTexture=&other;
 assert(!RetireInstalledGeometry(9,&g)&&clone.diffuseTexture.get()==&other);
 clone.diffuseTexture=&albedo;put(clone);gd.resourceView=&foreign;
 assert(!RetireInstalledGeometry(9,&g)&&clone.diffuseTexture.get()==&applied);
 gd.resourceView=&a;clone.diffuseTexture=&albedo;
 put(clone);RestoreDisabledDebugLayers();
 assert(layers.empty()&&clone.diffuseTexture.get()==&albedo);
 put(clone);set.diffusePath="other-skin.dds";
 assert(!RetireInstalledGeometry(9,&g)&&clone.diffuseTexture.get()==&applied);
 set.diffusePath="skin.dds";clone.diffuseTexture=&albedo;
 put(clone);ForgetInstalledLayer(9);assert(layers.empty());
 put(clone);ClearInstalledLayers();assert(layers.empty());
 clone.normalTexture=&applied;clone.diffuseTexture=&applied;
 RecordInstalledLayer(&actor,&g,&clone,&applied,&albedo,&applied,&other);
 assert(HasInstalledLayer(9)&&OwnsInstalledGeometry(9,&g));
 RestoreDisabledDebugLayers();
 assert(HasInstalledLayer(9)&&clone.normalTexture.get()==&applied&&clone.diffuseTexture.get()==&other);
 assert(RestoreInstalledLayer(&actor)&&clone.normalTexture.get()==&albedo&&clone.diffuseTexture.get()==&other&&layers.empty());
 std::cout<<"PASS ordinary/positive-layer debug, repeated install, named alias, clone, external takeover, clear and unload\n";
}
"""
Path(sys.argv[1]).write_text(prefix + identity + ledger + suffix, encoding="utf-8")
