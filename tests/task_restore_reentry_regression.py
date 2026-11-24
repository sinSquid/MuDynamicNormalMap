"""Generate a game-thread reentry fixture around the actual RemoveNormalMap body.
Usage: python tests/task_restore_reentry_regression.py OUTPUT.cpp
The fixture controls synchronous LoadTexture reentry; it does not emulate Skyrim GC.
"""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
s=(root/'src/TaskManager.cpp').read_text(encoding='utf-8-sig')
a=s.index('bool TaskManager::RemoveNormalMap(');b=s.index('\n\tEventResult TaskManager::ProcessEvent(const SKSE::NiNodeUpdateEvent*',a)
body=s[a:b]
prefix=r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
template<class T> struct Ptr {
    T* value=nullptr;
    Ptr()=default;Ptr(T* p):value(p){}
    T* get()const{return value;}T* Get()const{return value;}T* operator->()const{return value;}
    explicit operator bool()const{return value!=nullptr;}
};
struct ID3D11ShaderResourceView{int id;};
namespace Microsoft::WRL{template<class T>using ComPtr=Ptr<T>;}
namespace RE{
using FormID=std::uint32_t;
template<class T>using NiPointer=Ptr<T>;template<class T>using BSTSmartPointer=Ptr<T>;
struct NiSourceTexture{std::string name;struct GPU{ID3D11ShaderResourceView* resourceView;}* rendererTexture;};
using NiSourceTexturePtr=Ptr<NiSourceTexture>;
struct BSTextureSet{enum class Texture{kNormal};std::string path;};
struct BSLightingShaderMaterialBase{NiSourceTexturePtr normalTexture,diffuseTexture;Ptr<BSTextureSet> textureSet;};
struct BSShaderProperty{enum class EShaderPropertyFlag{kModelSpaceNormals};};
struct BSLightingShaderProperty{
    BSLightingShaderMaterialBase* material;
    struct Flags{bool enabled=true;bool all(BSShaderProperty::EShaderPropertyFlag)const{return enabled;}} flags;
};
struct BSGeometry{struct Runtime{Ptr<BSLightingShaderProperty> shaderProperty;} runtime;bool owned=false,record=false;Runtime& GetGeometryRuntimeData(){return runtime;}};
struct Root{std::vector<BSGeometry*> geometries;};
struct Actor{FormID formID=7;struct Loaded{Ptr<Root> data3D;}* loadedData;};
namespace BSVisit{enum class BSVisitControl{kContinue};template<class F>void TraverseScenegraphGeometries(Root* root,F f){for(auto* g:root->geometries)f(g);}}
}
template<class T,class U>T netimmerse_cast(U* value){return static_cast<T>(value);}
template<class T,class U>T skyrim_cast(U* value){return static_cast<T>(value);}
int lockDepth=0,loads=0;bool busy=false;std::uint64_t generation=1;
std::function<void()> onLoad;
std::unordered_map<std::string,RE::NiSourceTexture*> named,files;
RE::Actor* currentActor=nullptr;
namespace PNS{
std::uint64_t SessionGeneration(){return generation;}
bool HasInstalledLayer(RE::FormID){for(auto* g:currentActor->loadedData->data3D->geometries)if(g->record)return true;return false;}
bool OwnsInstalledGeometry(RE::FormID,RE::BSGeometry* g){return g->owned;}
bool RetireInstalledGeometry(RE::FormID,RE::BSGeometry* g){bool result=g->record;g->record=false;g->owned=false;return result;}
void PruneInstalledLayers(RE::FormID,const std::unordered_set<RE::BSGeometry*>&){}
}
namespace Mus{
struct MaterialLock{MaterialLock(){++lockDepth;}~MaterialLock(){--lockDepth;}};
bool IsInvalidActor(RE::Actor* actor){return !actor||!actor->loadedData||!actor->loadedData->data3D;}
std::string lowLetter(std::string text){return text;}
std::string GetTexturePath(RE::BSTextureSet* set,RE::BSTextureSet::Texture){return set->path;}
bool IsCreatedByMDNM(const char* name){return std::string(name).starts_with("mu:");}
namespace Shader{
struct TextureLoadManager{
    static TextureLoadManager& GetSingleton(){static TextureLoadManager t;return t;}
    bool OwnsNamedTexture(RE::NiSourceTexture* texture){auto it=named.find(texture->name);return it!=named.end()&&it->second==texture;}
    static void LoadTexture(const char* path,int,RE::NiSourceTexturePtr& result,bool){
        assert(lockDepth==0);++loads;
        if(onLoad){auto callback=std::move(onLoad);onLoad={};callback();}
        auto it=files.find(path);if(it!=files.end())result=it->second;
    }
};
}
struct Entry{std::string name;std::string GetTextureName()const{return name;}};
struct TaskManager{
    std::shared_mutex lastNormalMapLock;
    std::unordered_map<RE::FormID,std::vector<Ptr<Entry>>> lastNormalMap;
    bool IsExistLastNormalMap(RE::FormID id){return lastNormalMap.contains(id);}
    bool GetIsUpdating(RE::FormID){return busy;}
    void RemoveLastNormalMap(RE::FormID id){
        assert(lockDepth>0);
        for(auto entry:lastNormalMap[id]){auto it=named.find(entry->name);if(it!=named.end())it->second->rendererTexture->resourceView=nullptr;}
        lastNormalMap.erase(id);
    }
    bool RemoveNormalMap(RE::Actor* actor);
};
"""
suffix=r"""
}
struct Shape{
    RE::BSTextureSet set{"skin.dds"};
    RE::BSLightingShaderMaterialBase material;
    RE::BSLightingShaderProperty shader{&material};
    RE::BSGeometry geometry{{&shader}};
    explicit Shape(RE::NiSourceTexture* normal){material.normalTexture=normal;material.textureSet=&set;}
};
int main(){
    for(int scenario=0;scenario<10;++scenario){
        named.clear();files.clear();onLoad={};loads=0;busy=false;generation=1;
        ID3D11ShaderResourceView oldView{1},newView{2},skinView{3},outsideView{4};
        RE::NiSourceTexture::GPU oldGPU{&oldView},newGPU{&newView},skinGPU{&skinView},outsideGPU{&outsideView};
        RE::NiSourceTexture old{"mu:old",&oldGPU},added{"mu:new",&newGPU},skin{"skin.dds",&skinGPU},outside{"external",&outsideGPU};
        named[old.name]=&old;files["skin.dds"]=&skin;
        Shape first(&old),second(&added),third(&skin);
        RE::Root root{{&first.geometry}},differentRoot;
        RE::Actor::Loaded loaded{&root};RE::Actor actor{7,&loaded};currentActor=&actor;
        Mus::Entry oldEntry{old.name},newEntry{added.name};Mus::TaskManager manager;manager.lastNormalMap[7]={&oldEntry};
        if(scenario==0)first.material.diffuseTexture=&old; // ordinary Mu debug does not pin forever
        if(scenario==1)onLoad=[&]{root.geometries.push_back(&second.geometry);named[added.name]=&added;manager.lastNormalMap[7].push_back(&newEntry);};
        if(scenario==2)onLoad=[&]{busy=true;};
        if(scenario==3){outside.name=old.name;onLoad=[&]{first.material.normalTexture=&outside;};}
        if(scenario==4)onLoad=[&]{loaded.data3D=&differentRoot;};
        if(scenario==5){files.clear();}
        if(scenario==6)onLoad=[&]{root.geometries.push_back(&third.geometry);}; // unrelated ordinary addition allows cleanup
        if(scenario==7)onLoad=[&]{root.geometries.push_back(&second.geometry);second.geometry.owned=second.geometry.record=true;};
        if(scenario==8)onLoad=[&]{oldGPU.resourceView=&newView;};
        if(scenario==9)onLoad=[&]{++generation;};
        const bool result=manager.RemoveNormalMap(&actor);
        assert(lockDepth==0 && loads==1);
        if(scenario==0||scenario==6){assert(result&&!manager.lastNormalMap.contains(7));assert(first.material.normalTexture.get()==&skin);}
        else assert(!result&&manager.lastNormalMap.contains(7));
        if(scenario==1){assert(newGPU.resourceView==&newView);assert(second.material.normalTexture.get()==&added);}
        if(scenario==2){assert(oldGPU.resourceView==&oldView);assert(first.material.normalTexture.get()==&old);}
        if(scenario==3){assert(first.material.normalTexture.get()==&outside);assert(outsideGPU.resourceView==&outsideView);
            assert(manager.RemoveNormalMap(&actor));assert(outsideGPU.resourceView==&outsideView);}
        if(scenario==4)assert(first.material.normalTexture.get()==&old);
        if(scenario==5)assert(oldGPU.resourceView==&oldView);
        if(scenario==7){assert(second.geometry.record);assert(newGPU.resourceView==&newView);}
        if(scenario==8){assert(first.material.normalTexture.get()==&old);assert(oldGPU.resourceView==&newView);}
        if(scenario==9){assert(first.material.normalTexture.get()==&old);assert(oldGPU.resourceView==&oldView);}
    }
    std::cout<<"PASS: actual two-phase restore; new named/private layer, WIP, same-name external replacement, changed SRV/session/root, failed load, ordinary/debug full cleanup\n";
}
"""
Path(sys.argv[1]).write_text(prefix+body+suffix,encoding='utf-8')
