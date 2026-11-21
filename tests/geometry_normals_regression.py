"""Extract real face/normal generation into an offline serial harness.
Usage: python tests/geometry_normals_regression.py OUTPUT.cpp [BASE_REVISION]
"""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(subprocess.check_output(['git','show',sys.argv[2]+':src/Geometry.cpp'],cwd=root).decode('utf-8-sig')
        if len(sys.argv)>2 else (root/'src/Geometry.cpp').read_text(encoding='utf-8-sig'))
a=source.index('void GeometryData::CreateFaceData()')
b=source.index('void GeometryData::Subdivision(',a)
code=source[a:b]
prefix=r"""
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include <string>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace logger { template<class... T> void debug(T&&...){} }
template<class... T> void PerformanceLog(T&&...){}
namespace tbb {
    template<class T> struct blocked_range { T first,last; blocked_range(T a,T b):first(a),last(b){} T begin()const{return first;}T end()const{return last;} };
    struct auto_partitioner{};
    template<class R,class F> void parallel_for(R range,F body,auto_partitioner){body(range);}
}
struct Config {
    static Config& GetSingleton(){static Config c;return c;}
    bool GetGeometryDataTime(){return false;}
    bool GetAllowInvertNormalSmooth(){return false;}
};
struct Pool{template<class F>void Execute(F body){body();}};
const auto emptyVector=DirectX::XMVectorZero();
constexpr float floatPrecision=1e-6f;
struct GeometryData {
    struct Info {bool hasNormals=false,hasTangents=false,hasBitangents=false;std::string name;};
    struct ObjectInfo {Info info;unsigned vertexStart=0,vertexEnd=3,normalStart=0,normalEnd=0,tangentStart=0,tangentEnd=0,bitangentStart=0,bitangentEnd=0;};
    struct Geo{ObjectInfo objInfo;};
    struct FaceNormal{unsigned i0,i1,i2;DirectX::XMFLOAT3 normal;};
    struct FaceTangent{unsigned i0,i1,i2;DirectX::XMFLOAT3 tangent,bitangent;};
    Pool pool;Pool* tp=&pool;Info mainInfo;std::vector<Geo> geometries{Geo{}};
    std::vector<DirectX::XMFLOAT3> vertices,normals,tangents,bitangents;
    std::vector<DirectX::XMFLOAT2> uvs;
    std::vector<unsigned> indices;
    std::vector<std::vector<unsigned>> vertexToFaceMap,weldedVertices;
    std::vector<FaceNormal> faceNormals;
    std::vector<FaceTangent> faceTangents;
    void CreateFaceData();void RecalculateNormals(float);
};
"""
suffix=r"""
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    for(float angle:{0.f,60.f}) for(float uvSign:{1.f,-1.f}) {
        GeometryData geometry;
        geometry.vertices={{0,0,0},{1,0,0},{0,1,0}};
        geometry.uvs={{0,0},{uvSign,0},{0,1}};
        // The second triangle has zero area and shares two valid vertices.
        geometry.indices={0,1,2,0,0,1};
        geometry.vertexToFaceMap={{0,1},{0,1},{0}};
        geometry.weldedVertices={{0},{1},{2}};
        geometry.CreateFaceData();geometry.RecalculateNormals(angle);
        Check(geometry.normals.size()==3,"zero smoothing produced no normals");
        for(const auto& normal:geometry.normals){
            Check(std::isfinite(normal.x)&&std::isfinite(normal.y)&&std::isfinite(normal.z),"degenerate face contaminated normal");
            Check(std::abs(normal.z-1.f)<.001f,"valid neighbor normal lost");
        }
        for(const auto& tangent:geometry.tangents)
            Check(std::isfinite(tangent.x)&&std::isfinite(tangent.y)&&std::isfinite(tangent.z),"nonfinite tangent");
        Check(geometry.geometries[0].objInfo.normalEnd==3,"normal offsets absent");
        for(const auto& tangent:geometry.tangents)
            Check(std::abs(tangent.x-uvSign)<.001f,"UV tangent orientation lost");
        for(const auto& bitangent:geometry.bitangents)
            Check(std::abs(bitangent.y-1.f)<.001f,"mirrored UV bitangent handedness lost");
    }
    std::cout<<"PASS: actual CreateFaceData/RecalculateNormals, zero-area neighbor, 0/60-degree smoothing, mirrored UV handedness\n";
}
"""
Path(sys.argv[1]).write_text(prefix+code+suffix,encoding='utf-8')
