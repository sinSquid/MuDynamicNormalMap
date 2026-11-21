"""Compile the current Hash::Update body against small engine-layout fixtures.

Run from a Visual Studio developer shell. This exercises input validation, not
Skyrim rendering or concurrent scene graph mutation.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "src/ActorVertexHasher.cpp").read_text(encoding="utf-8-sig")
start = source.index("bool ActorVertexHasher::Hash::Update(RE::BSGeometry* a_geo)")
end = source.index("bool ActorVertexHasher::Hash::CheckTexture", start)
body = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
namespace DirectX { struct XMVECTOR { float value[4]; }; }
namespace RE {
namespace BSGraphics { struct Vertex { enum { VF_VERTEX=1, VF_UV=2 }; }; }
struct Desc {
    unsigned flags=3, stride=4;
    bool HasFlag(unsigned flag) const { return (flags & flag) != 0; }
    unsigned GetSize() { return stride; } // Match CommonLib's non-const API.
};
struct NiPoint3 { float x,y,z; };
struct Buffer { std::uint8_t* rawVertexData; };
struct Partition { Buffer* buffData; };
struct NiSkinPartition {
    unsigned numPartitions=1, vertexCount=4;
    std::vector<Partition> partitions;
};
template<class T> using NiPointer = std::shared_ptr<T>;
struct BSTriShape {
    struct Data { unsigned vertexCount=4; } data;
    const Data& GetTrishapeRuntimeData() const { return data; }
};
struct BSDynamicTriShape : BSTriShape {
    struct Data { void* dynamicData=nullptr; } dynamic;
    const Data& GetDynamicTrishapeRuntimeData() const { return dynamic; }
};
struct Morph { NiPoint3* vertexData=nullptr; unsigned modelVertexCount=0; };
struct BSGeometry {
    bool dynamic=false, tri=true;
    BSTriShape shape;
    BSDynamicTriShape dynamicShape;
    struct Data { Desc vertexDesc; } data;
    NiPointer<NiSkinPartition> skin;
    Morph* morph=nullptr;
    const BSTriShape* AsTriShape() { return !tri ? nullptr : dynamic ? &dynamicShape : &shape; }
    const BSDynamicTriShape* AsDynamicTriShape() { return dynamic ? &dynamicShape : nullptr; }
    const Data& GetGeometryRuntimeData() { return data; }
};
}
namespace Mus {
struct Config {
    int mode=1;
    static Config& GetSingleton() { static Config config; return config; }
    int GetRealtimeDetectHead() const { return mode; }
};
auto GetSkinPartition(RE::BSGeometry* geometry) { return geometry->skin; }
struct GeometryData { static auto GetMorphExtraData(RE::BSGeometry* geometry) { return geometry->morph; } };
struct ActorVertexHasher { struct Hash {
    std::size_t bytes=0;
    int digests=0;
    bool isCheckTexture=false;
    bool Update(RE::BSGeometry* geometry);
    void Update(const void* data, std::size_t count) { assert(data); bytes += count; }
    void Reset() { bytes=0; }
    void GetNewHash() { ++digests; }
    bool CheckTexture(RE::BSGeometry*) { return true; }
}; };
'''
tests = r'''
}
int main() {
    std::uint8_t raw[64]{};
    RE::Buffer buffer{raw};
    auto skin=std::make_shared<RE::NiSkinPartition>();
    skin->partitions.push_back({&buffer});
    RE::BSGeometry geometry;
    geometry.skin=skin;
    Mus::ActorVertexHasher::Hash hash;
    assert(!hash.Update(nullptr));
    assert(hash.Update(&geometry) && hash.bytes==16);
    geometry.tri=false; assert(!hash.Update(&geometry)); geometry.tri=true;
    skin->numPartitions=0; assert(!hash.Update(&geometry)); skin->numPartitions=1;
    skin->partitions.clear(); assert(!hash.Update(&geometry)); skin->partitions.push_back({nullptr});
    assert(!hash.Update(&geometry)); skin->partitions[0].buffData=&buffer;
    buffer.rawVertexData=nullptr; assert(!hash.Update(&geometry)); buffer.rawVertexData=raw;
    geometry.shape.data.vertexCount=0; skin->vertexCount=0; assert(!hash.Update(&geometry));
    skin->vertexCount=4; assert(hash.Update(&geometry));
    geometry.data.vertexDesc.flags=1; assert(!hash.Update(&geometry)); geometry.data.vertexDesc.flags=3;
    geometry.data.vertexDesc.stride=0; assert(!hash.Update(&geometry)); geometry.data.vertexDesc.stride=4;
    geometry.dynamic=true;
    RE::NiPoint3 vertices[4]{};
    RE::Morph morph{vertices,3};
    geometry.morph=&morph;
    assert(hash.Update(&geometry) && hash.bytes==16); // Too-small FOD: static fallback.
    morph.modelVertexCount=4;
    assert(hash.Update(&geometry) && hash.bytes==16+sizeof(vertices));
    morph.vertexData=nullptr;
    assert(hash.Update(&geometry) && hash.bytes==16);
    Mus::Config::GetSingleton().mode=2;
    assert(hash.Update(&geometry) && hash.bytes==16); // Dynamic buffer not ready.
    DirectX::XMVECTOR dynamic[4]{};
    geometry.dynamicShape.dynamic.dynamicData=dynamic;
    assert(hash.Update(&geometry) && hash.bytes==16+sizeof(dynamic));
    Mus::Config::GetSingleton().mode=0;
    assert(!hash.Update(&geometry));
}
'''
out = root / "build/targeted-tests"
out.mkdir(parents=True, exist_ok=True)
cpp = out / "hash_input_fixture.cpp"
exe = out / "hash_input_fixture.exe"
cpp.write_text(fixture + body + tests, encoding="utf-8")
subprocess.run(["cl", "/nologo", "/std:c++20", "/EHsc", str(cpp),
                "/Fo" + str(out / "hash_input_fixture.obj"), "/Fe" + str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print("PASS: actual Hash::Update body rejects invalid buffers and bounds FOD reads")
