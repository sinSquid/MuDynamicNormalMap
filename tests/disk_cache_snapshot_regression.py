"""Generate a focused test of the actual disk-read/clear/validation methods.

Usage: python tests/disk_cache_snapshot_regression.py OUTPUT.cpp
Compile as C++20 or newer with Windows SDK and the project's real LZ4 library.
Only D3D creation and unrelated writer/index services are stubbed. Disk files,
LZ4 payloads, lock interleavings and read-budget accounting are exercised.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "src/NormalMapStore.cpp").read_text(encoding="utf-8-sig")
header = (root / "include/NormalMapStore.h").read_text(encoding="utf-8-sig")


def extract(text, marker):
    start = text.index(marker)
    pos = text.index("{", start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


methods = "\n".join(extract(source, marker) for marker in (
    "std::uint64_t NormalMapStore::MipByteSize",
    "bool NormalMapStore::ValidateDiskCacheInfo",
    "bool NormalMapStore::ValidateComponents",
    "std::uint64_t NormalMapStore::EstimateDiskWriteBytes",
    "void NormalMapStore::AddResource",
    "bool NormalMapStore::GetResource",
    "TextureResourcePtr NormalMapStore::GetDiskCache",
    "void NormalMapStore::ClearDiskCache",
    "void NormalMapStore::UpdateAccessTime",
))
# Instrument the real decompressor boundary, without replacing its behavior.
methods = methods.replace("LZ4_decompress_safe(", "ObservedDecompress(")
info = extract(header, "struct DiskCacheInfo {") + ";"
budget = re.search(r"static constexpr std::uint64_t diskReadBudget = [^;]+;", header).group()
prefix = r'''
#include <d3d11.h>
#include <lz4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace std::chrono_literals;
namespace fs = std::filesystem;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
namespace logger {
    template<class... T> void error(T&&...) {}
    template<class... T> void debug(T&&...) {}
}
std::function<void()> decodeHook, createHook;
std::atomic<unsigned> decodeCalls{0}, createCalls{0};
int ObservedDecompress(const char* input, char* output, int compressed, int size) {
    ++decodeCalls;
    if (decodeHook) decodeHook();
    return LZ4_decompress_safe(input, output, compressed, size);
}
std::uint64_t GetFileSize(const fs::path& path) {
    std::error_code ec; auto size=fs::file_size(path,ec); return ec ? 0 : size;
}
struct FakeTexture {
    std::vector<std::vector<char>> mips;
    D3D11_TEXTURE2D_DESC desc{};
    void GetDesc(D3D11_TEXTURE2D_DESC* out) const { *out=desc; }
};
struct FakePointer {
    std::shared_ptr<FakeTexture> value;
    FakeTexture* Get() const { return value.get(); }
    FakeTexture* operator->() const { return value.get(); }
    explicit operator bool() const { return bool(value); }
};
struct TextureResource {
    std::vector<std::uint64_t> componentHashes;
    FakePointer normalmapTexture2D, normalmapShaderResourceView;
};
using TextureResourcePtr=std::shared_ptr<TextureResource>;
struct Device {
    bool failTexture=false, failView=false;
    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, FakePointer* out) {
        ++createCalls;
        if(createHook) createHook();
        if(failTexture) return E_FAIL;
        out->value=std::make_shared<FakeTexture>();
        out->value->desc=*desc;
        for(unsigned mip=0;mip<desc->MipLevels;++mip) {
            auto* bytes=static_cast<const char*>(data[mip].pSysMem);
            out->value->mips.emplace_back(bytes,bytes+data[mip].SysMemPitch*std::max(1u,desc->Height>>mip));
        }
        return S_OK;
    }
    HRESULT CreateShaderResourceView(FakeTexture*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,FakePointer* out) {
        if(failView) return E_FAIL;
        out->value=std::make_shared<FakeTexture>();return S_OK;
    }
} device;
namespace Shader { struct ShaderManager {
    static ShaderManager& GetSingleton(){ static ShaderManager self;return self; }
    Device* GetDevice(){return &device;}
}; }
struct Config {
    std::atomic<bool> clear=false;
    static Config& GetSingleton(){static Config self;return self;}
    bool GetDiskCache()const{return true;}
    bool GetClearDiskCache()const{return clear.load();}
};
template<class T> struct DiskWriteQueue {
    void PauseAndDiscard(){} void Resume(){}
    void Submit(std::uint64_t,T,std::uint64_t){} // Publication tested; asynchronous writes are unrelated.
};
struct NormalMapStore {
    INFO
    BUDGET
    std::atomic<std::uint64_t> diskReadBytes{0};
    std::recursive_mutex diskIOLock;
    std::mutex diskCacheLock,diskLifecycleLock;
    DiskWriteQueue<TextureResourcePtr> diskWriter;
    std::unordered_map<std::uint64_t,DiskCacheInfo> diskCacheInfoMap;
    std::unordered_map<std::uint64_t,fs::file_time_type> diskAccessTimes;
    std::mutex lock;
    std::unordered_map<std::uint64_t,TextureResourcePtr> map;
    fs::path folder;
    std::string diskCacheExtension=".mdncache";
    std::string GetCacheFileFolder(){return folder.string();}
    std::string GetCacheFileName(std::uint64_t hash,int mip){
        return (folder/(std::to_string(hash)+"_"+std::to_string(mip)+diskCacheExtension)).string();
    }
    void IndexDiskCache(){} // Unrelated eviction accounting, not the metadata map.
    static bool ValidateDiskCacheInfo(const DiskCacheInfo&);
    static bool ValidateComponents(std::uint64_t,const std::vector<std::uint64_t>&);
    static std::uint64_t MipByteSize(const DiskCacheInfo&,std::uint32_t);
    static std::uint64_t EstimateDiskWriteBytes(const TextureResourcePtr&);
    void AddResource(std::uint64_t,TextureResourcePtr);
    bool GetResource(std::uint64_t,TextureResourcePtr&,bool&);
    void UpdateAccessTime(std::uint64_t);
    TextureResourcePtr GetDiskCache(std::uint64_t);
    void ClearDiskCache();
};
'''.replace("INFO", info).replace("BUDGET", budget)
suffix = r'''
void WriteEntry(NormalMapStore& store,std::uint64_t hash,char tag) {
    std::lock_guard transaction(store.diskIOLock);
    NormalMapStore::DiskCacheInfo info;
    info.componentHashes={hash};info.mipLevels=2;info.rowPitch={16,8};
    info.texDesc.Width=info.texDesc.Height=4;info.texDesc.MipLevels=2;info.texDesc.ArraySize=1;
    info.texDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;info.texDesc.SampleDesc.Count=1;
    info.texDesc.Usage=D3D11_USAGE_DEFAULT;info.texDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    info.srvDesc.Format=info.texDesc.Format;info.srvDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    info.srvDesc.Texture2D.MipLevels=2;
    std::ofstream meta(store.GetCacheFileName(hash,-1),std::ios::binary|std::ios::trunc);
    auto write=[&](const auto& value){meta.write(reinterpret_cast<const char*>(&value),sizeof(value));};
    std::uint32_t count=1;std::size_t pitches=2;
    write(count);write(hash);write(info.mipLevels);write(pitches);
    for(auto pitch:info.rowPitch)write(pitch);
    auto& d=info.texDesc;
    write(d.Width);write(d.Height);write(d.MipLevels);write(d.ArraySize);write(d.Format);
    write(d.SampleDesc.Count);write(d.SampleDesc.Quality);write(d.Usage);write(d.BindFlags);write(d.CPUAccessFlags);write(d.MiscFlags);
    write(info.srvDesc.Format);write(info.srvDesc.ViewDimension);write(info.srvDesc.Texture2D.MipLevels);write(info.srvDesc.Texture2D.MostDetailedMip);
    meta.close();
    for(unsigned mip=0;mip<2;++mip) {
        std::vector<char> raw(NormalMapStore::MipByteSize(info,mip),tag+mip);
        std::vector<char> compressed(LZ4_compressBound(static_cast<int>(raw.size())));
        auto size=LZ4_compress_default(raw.data(),compressed.data(),static_cast<int>(raw.size()),static_cast<int>(compressed.size()));
        Check(size>0,"compress fixture");
        std::uint64_t original=raw.size(),stored=size;
        std::ofstream file(store.GetCacheFileName(hash,mip),std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&original),8);file.write(reinterpret_cast<const char*>(&stored),8);
        file.write(compressed.data(),size);
    }
}
void CheckTag(const TextureResourcePtr& result,char tag) {
    Check(result && result->normalmapTexture2D.Get(),"missing texture result");
    const auto& mips=result->normalmapTexture2D.Get()->mips;
    Check(mips.size()==2,"mip count");
    for(unsigned i=0;i<2;++i) Check(std::all_of(mips[i].begin(),mips[i].end(),[&](char c){return c==tag+i;}),"mixed transaction mips");
}
int main(int argc,char** argv) {
    Check(argc==2,"supply isolated fixture directory");
    NormalMapStore store;store.folder=argv[1];fs::create_directories(store.folder);
    WriteEntry(store,7,'A');
    std::promise<void> entered,release;
    auto releaseSignal=release.get_future().share();
    std::atomic<bool> first=true;
    decodeHook=[&]{if(first.exchange(false)){entered.set_value();releaseSignal.wait();}};
    auto read=std::async(std::launch::async,[&]{return store.GetDiskCache(7);});
    const bool reached=entered.get_future().wait_for(5s)==std::future_status::ready;
    const bool unlocked=reached && store.diskIOLock.try_lock();
    bool budgetMiss=false;
    if(unlocked) {
        store.diskIOLock.unlock();
        // Reserve the rest of the global budget without allocating a GiB.
        const auto fakeCharge=NormalMapStore::diskReadBudget-store.diskReadBytes.load()-1;
        store.diskReadBytes.fetch_add(fakeCharge);
        budgetMiss=!store.GetDiskCache(7);
        store.diskReadBytes.fetch_sub(fakeCharge);
        Config::GetSingleton().clear=true;store.ClearDiskCache();Config::GetSingleton().clear=false;
        WriteEntry(store,7,'B'); // Reader must not mix the new mip 1 into old mip 0.
    }
    release.set_value();auto old=read.get();decodeHook={};
    Check(reached && unlocked,"disk lock held during decompression");
    Check(budgetMiss,"aggregate read budget not enforced");
    CheckTag(old,'A');
    Check(store.diskCacheInfoMap.empty(),"old reader resurrected cleared metadata");
    Check(store.diskReadBytes==0,"read reservation leaked");
    bool d3dUnlocked=false;
    createHook=[&]{std::thread other([&]{d3dUnlocked=store.diskIOLock.try_lock();if(d3dUnlocked)store.diskIOLock.unlock();});other.join();};
    CheckTag(store.GetDiskCache(7),'B');createHook={};
    Check(d3dUnlocked,"disk lock held during D3D creation");
    Check(store.diskReadBytes==0,"successful read reservation leaked");

    fs::resize_file(store.GetCacheFileName(7,1),1);
    const auto beforeDecode=decodeCalls.load();
    Check(!store.GetDiskCache(7),"truncated last mip accepted");
    Check(decodeCalls==beforeDecode,"decompressed before complete mip snapshot");
    Check(store.diskReadBytes==0,"read failure reservation leaked");
    WriteEntry(store,7,'C');
    { std::ofstream file(store.GetCacheFileName(7,0),std::ios::binary|std::ios::trunc);
      std::uint64_t orig=64,size=1;char invalid=0;
      file.write(reinterpret_cast<char*>(&orig),8);file.write(reinterpret_cast<char*>(&size),8);file.write(&invalid,1); }
    Check(!store.GetDiskCache(7),"invalid LZ4 payload accepted");
    Check(store.diskReadBytes==0,"decompression failure reservation leaked");
    WriteEntry(store,7,'D');device.failTexture=true;
    Check(!store.GetDiskCache(7),"D3D failure ignored");device.failTexture=false;
    Check(store.diskReadBytes==0,"D3D failure reservation leaked");
    CheckTag(store.GetDiskCache(7),'D');
    Check(store.diskReadBytes==0,"final reservation leaked");

    // Exercise the production entry and publisher, not a hand-written map insertion.
    // Case 0: newer RAM wins across clear. Case 1: it also wins when disk D3D fails.
    // Case 2: disk-only clear does not invalidate a previously captured valid snapshot.
    for(unsigned mode=0;mode<3;++mode) {
        const std::uint64_t hash=20+mode;
        WriteEntry(store,hash,'E');
        WriteEntry(store,hash+100,'N');
        auto newer=store.GetDiskCache(hash+100);
        newer->componentHashes={hash};
        std::promise<void> reading,resume;
        auto resumeSignal=resume.get_future().share();
        std::atomic<bool> stopFirst=true;
        decodeHook=[&]{if(stopFirst.exchange(false)){reading.set_value();resumeSignal.wait();}};
        TextureResourcePtr selected;bool fromDisk=true;
        auto entryRead=std::async(std::launch::async,[&]{return store.GetResource(hash,selected,fromDisk);});
        const bool paused=reading.get_future().wait_for(5s)==std::future_status::ready;
        if(paused) {
            Config::GetSingleton().clear=true;store.ClearDiskCache();Config::GetSingleton().clear=false;
            if(mode<2) store.AddResource(hash,newer);
            if(mode==1) device.failTexture=true;
        }
        resume.set_value();const bool hit=entryRead.get();decodeHook={};device.failTexture=false;
        Check(paused && hit,"production GetResource did not complete");
        if(mode<2) {
            Check(selected==newer && store.map.at(hash)==newer,"late disk read overwrote newer AddResource");
            Check(!fromDisk,"RAM winner was incorrectly classified as disk");
            CheckTag(selected,'N');
        } else {
            Check(fromDisk && store.map.at(hash)==selected,"disk-only clear unexpectedly invalidated snapshot");
            CheckTag(selected,'E');
        }
        Check(store.diskCacheInfoMap.empty(),"GetResource resurrected cleared disk metadata");
        TextureResourcePtr resident;bool residentDisk=true;
        Check(store.GetResource(hash,resident,residentDisk) && resident==selected && !residentDisk,"next read was not a RAM hit");
        Check(store.diskReadBytes==0,"production entry leaked read budget");
    }
    std::cout<<"PASS actual GetResource/AddResource/GetDiskCache/ClearDiskCache: RAM publication race, disk-only clear, snapshot, budget and failures\n";
}
'''
Path(sys.argv[1]).write_text(prefix + methods + suffix, encoding="utf-8")
