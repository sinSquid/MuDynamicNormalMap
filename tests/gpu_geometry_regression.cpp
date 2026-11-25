#include "GeometryContentHash.h"
#include "TextureRowSlices.h"
#include "NormalMath.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 operation failed"); }
static void TestRows() {
    for (unsigned block : {1u, 4u}) for (unsigned height = 1; height <= 513; ++height)
        for (unsigned requested : {1u, 2u, 3u, 7u, 37u, 128u, 512u, 1024u}) {
            unsigned next = 0;
            for (const auto& slice : Mus::MakeTextureRowSlices(height, requested, block)) {
                Check(slice.top == next && slice.top < slice.bottom && slice.bottom <= height, "gap/overlap/empty copy slice");
                Check(slice.top % block == 0 && (slice.bottom == height || slice.bottom % block == 0), "unaligned BC slice");
                next = slice.bottom;
            }
            Check(next == height, "incomplete copy coverage");
        }
}
static void TestMathAndHash() {
    for (unsigned size : {1u, 2u, 3u, 5u, 512u, 768u, 1024u}) for (int i = -4096; i < 4096; ++i) {
        const auto expected = ((i % int(size)) + int(size)) % int(size);
        Check(Mus::WrapTextureCoordinate(i, size) == unsigned(expected), "texture coordinate wrap");
    }
    using namespace DirectX;
    const auto axis = XMVectorSet(0, 0, 1, 0);
    for (const auto other : {axis, XMVectorNegate(axis), XMVectorSet(1, 0, 0, 0)})
        for (float t : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
            const auto value = Mus::SlerpUnitVector(axis, other, t);
            const auto length = XMVectorGetX(XMVector3Length(value));
            Check(std::isfinite(length) && std::abs(length - 1.0f) < 1e-5f, "slerp equal/opposite normals");
        }
    std::array<int, 9> normals{0,0,100, 0,0,100, 0,0,100};
    std::array<int, 9> tangents{100,0,0, 100,0,0, 100,0,0}, bitangents{0,100,0, 0,100,0, 0,100,0};
    std::array<float, 6> uvs{0,0,1,0,0,1};
    std::array<std::uint32_t, 3> indices{0,1,2};
    auto hash = [&](unsigned offset = 0) { return Mus::GeometryContentHash(std::as_bytes(std::span(normals)),
        std::as_bytes(std::span(uvs)), std::as_bytes(std::span(tangents)), std::as_bytes(std::span(bitangents)), indices, offset); };
    const auto original = hash();
    Check(original != XXH3_64bits(normals.data(), sizeof(normals)), "old normal-only key reused");
    uvs[0] = .25f; Check(hash() != original, "UV change cache collision"); uvs[0] = 0;
    tangents[0] = -100; Check(hash() != original, "tangent change cache collision"); tangents[0] = 100;
    bitangents[1] = -100; Check(hash() != original, "bitangent change cache collision"); bitangents[1] = 100;
    std::swap(indices[1], indices[2]); Check(hash() != original, "topology change cache collision"); std::swap(indices[1], indices[2]);
    for (auto& index : indices) index += 17;
    Check(hash(17) == original, "unrelated geometry batch offset changed cache key");
}
static void TestCopy(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format, unsigned blockBytes) {
    const bool compressed = format != DXGI_FORMAT_R8G8B8A8_UNORM;
    const unsigned width = compressed ? 520 : 517, height = compressed ? 516 : 513;
    const unsigned block = compressed ? 4 : 1, rows = (height + block - 1) / block;
    const unsigned pitch = ((width + block - 1) / block) * blockBytes;
    std::vector<std::uint8_t> input(rows * pitch);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = std::uint8_t((i * 13 + i / pitch) & 255);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=format; desc.Usage=D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> src,dst,staging;
    HR(device->CreateTexture2D(&desc,nullptr,&src)); HR(device->CreateTexture2D(&desc,nullptr,&dst));
    for (const auto& row : Mus::MakeTextureRowSlices(height, 37, block)) {
        D3D11_BOX box{0,row.top,0,width,row.bottom,1};
        context->UpdateSubresource(src.Get(),0,&box,input.data()+(row.top/block)*pitch,pitch,0);
        context->CopySubresourceRegion(dst.Get(),0,0,row.top,0,src.Get(),0,&box);
    }
    desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    HR(device->CreateTexture2D(&desc,nullptr,&staging)); context->CopyResource(staging.Get(),dst.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    for (unsigned y=0;y<rows;++y)
        Check(std::memcmp(input.data()+y*pitch,static_cast<std::uint8_t*>(mapped.pData)+y*mapped.RowPitch,pitch)==0,"striped WARP copy differs");
    context->Unmap(staging.Get(),0);
}
static ComPtr<ID3DBlob> Compile(const wchar_t* path) {
    ComPtr<ID3DBlob> code,error;
    const auto hr=D3DCompileFromFile(path,nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if (FAILED(hr) && error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
    HR(hr); return code;
}
static void TestShader(ID3D11Device* device, ID3D11DeviceContext* context) {
    auto code=Compile(L"shader/UpdateNormalMap.hlsl");
    Compile(L"shader/MergeTexture.hlsl"); Compile(L"shader/GenerateMips.hlsl"); Compile(L"shader/BleedTexture.hlsl");
    ComPtr<ID3D11ComputeShader> shader; HR(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    std::array<ComPtr<ID3D11Buffer>,6> buffers;
    std::array<ComPtr<ID3D11ShaderResourceView>,6> views;
    std::array<DirectX::XMFLOAT3,4> zero{};
    std::array<DirectX::XMFLOAT2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    std::array<unsigned,6> indices{0,1,2,0,2,3};
    for(unsigned i=0;i<6;++i) {
        const void* data=i==1 ? static_cast<const void*>(uv.data()) : i==5 ? static_cast<const void*>(indices.data()) : zero.data();
        const unsigned stride=i==1 ? 8 : i==5 ? 4 : 12, count=i==5 ? 6 : 4;
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=stride*count; desc.Usage=D3D11_USAGE_IMMUTABLE;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride=stride;
        D3D11_SUBRESOURCE_DATA initial{data,0,0}; HR(device->CreateBuffer(&desc,&initial,&buffers[i]));
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=count;
        HR(device->CreateShaderResourceView(buffers[i].Get(),&srv,&views[i]));
    }
    std::array<unsigned,16> constants{5,3,0,6,0,0,0,0,0,0,4,0};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{constants.data(),0,0};ComPtr<ID3D11Buffer> constant;HR(device->CreateBuffer(&cb,&initial,&constant));
    D3D11_TEXTURE2D_DESC td{};td.Width=5;td.Height=3;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output,read;ComPtr<ID3D11UnorderedAccessView> uav;
    HR(device->CreateTexture2D(&td,nullptr,&output));HR(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    auto ownerDesc=td;ownerDesc.Format=DXGI_FORMAT_R32_UINT;
    ComPtr<ID3D11Texture2D> owner;ComPtr<ID3D11UnorderedAccessView> ownerUAV;
    HR(device->CreateTexture2D(&ownerDesc,nullptr,&owner));HR(device->CreateUnorderedAccessView(owner.Get(),nullptr,&ownerUAV));
    const UINT clearOwner[4]{};context->ClearUnorderedAccessViewUint(ownerUAV.Get(),clearOwner);
    const float clear[4]{};context->ClearUnorderedAccessViewFloat(uav.Get(),clear);
    ID3D11ShaderResourceView* resources[6]{};for(unsigned i=0;i<6;++i)resources[i]=views[i].Get();
    context->CSSetShaderResources(0,6,resources);context->CSSetConstantBuffers(0,1,constant.GetAddressOf());
    ID3D11UnorderedAccessView* outputs[]{uav.Get(),ownerUAV.Get()};
    context->CSSetShader(shader.Get(),nullptr,0);context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
    for(unsigned pass=0;pass<2;++pass){constants[11]=pass;context->UpdateSubresource(constant.Get(),0,nullptr,constants.data(),0,0);context->Dispatch(1,1,1);}
    ID3D11UnorderedAccessView* noUAV[2]{};context->CSSetUnorderedAccessViews(0,2,noUAV,nullptr);
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    HR(device->CreateTexture2D(&td,nullptr,&read));context->CopyResource(read.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(unsigned y=0;y<3;++y)for(unsigned x=0;x<5;++x) {
        const auto* value=reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch)+x*4;
        for(unsigned c=0;c<4;++c)Check(std::isfinite(value[c]),"non-finite degenerate normal shader output");
        Check(value[3]==1.0f,"NPOT raster left a pixel unwritten");
        Check(std::abs(value[0]-.5f)<1e-5f && std::abs(value[1]-1.0f)<1e-5f && std::abs(value[2]-.5f)<1e-5f,"degenerate normal fallback differs");
    }
    context->Unmap(read.Get(),0);
}
int main() {
    TestRows();TestMathAndHash();
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    TestCopy(device.Get(),context.Get(),DXGI_FORMAT_BC1_UNORM,8);
    TestCopy(device.Get(),context.Get(),DXGI_FORMAT_BC7_UNORM,16);
    TestCopy(device.Get(),context.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,4);
    TestShader(device.Get(),context.Get());
    std::cout<<"PASS: geometry cache inputs, batch-offset invariance, slerp/wrap, row slicing, BC1/BC7/RGBA WARP copies, all four shaders and NPOT/zero-basis rendering\n";
}
