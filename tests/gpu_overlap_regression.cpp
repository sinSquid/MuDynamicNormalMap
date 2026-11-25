#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include "../include/GPURasterBudget.h"
using Microsoft::WRL::ComPtr;
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void HR(HRESULT hr){Check(SUCCEEDED(hr),"D3D11 failed");}
struct V3{float x,y,z;};struct V2{float x,y;};
int main(){
    const DirectX::XMFLOAT2 zeroUV{0, 0}, u{3, 0}, v{0, 3};
    {
        auto budget = Mus::RasterGeometrySampleLimit(256, 256), batch = Mus::RasterBatchSampleLimit;
        const auto before = budget;
        bool parallel = false;
        const DirectX::XMFLOAT2 farUV{2048, 2048};
        Check(Mus::ReserveGPURasterSamples(zeroUV, farUV, farUV, 256, 256, budget, batch, parallel),
            "degenerate UV rejected the entire geometry batch");
        Check(!parallel && budget == before && batch == Mus::RasterBatchSampleLimit,
            "degenerate UV charged pixel-loop work");
        Check(Mus::ReserveGPURasterSamples({-2048,-2048}, zeroUV, farUV, 256, 256, budget, batch, parallel),
            "distinct collinear vertices rejected the batch");
        Check(!parallel && budget == before && batch == Mus::RasterBatchSampleLimit,
            "collinear UV consumed the next triangle's budget");
        Check(Mus::ReserveGPURasterSamples(zeroUV, u, v, 256, 256, budget, batch, parallel) && budget < before,
            "valid triangle following a collinear triangle failed");
    }
    for (unsigned size : {2048, 4096}) {
        auto budget = Mus::RasterGeometrySampleLimit(size, size), batch = Mus::RasterBatchSampleLimit;
        bool parallel = false;
        Check(Mus::ReserveGPURasterSamples(zeroUV, u, v, size, size, budget, batch, parallel) && parallel,
            "common 3x3 UV unexpectedly rejected");
    }
    {
        auto budget = Mus::RasterGeometrySampleLimit(256, 256), batch = Mus::RasterBatchSampleLimit;
        const auto before = budget;
        bool parallel = false;
        Check(!Mus::ReserveGPURasterSamples({-65504, 0}, {65504, 0}, {0, 65504}, 256, 256, budget, batch, parallel),
            "extreme UV accepted");
        Check(budget == before && batch == Mus::RasterBatchSampleLimit, "rejected UV consumed budget");
    }
    {
        auto budget = Mus::RasterGeometrySampleLimit(5, 3), batch = Mus::RasterBatchSampleLimit;
        const auto before = budget;
        bool parallel = false;
        Check(!Mus::ReserveGPURasterSamples({0, 0}, {1024, 0}, {0, 512}, 5, 3, budget, batch, parallel),
            "tiny texture allowed excessive work on one shader lane");
        Check(!parallel && budget == before && batch == Mus::RasterBatchSampleLimit, "lane rejection consumed budget");
    }
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3DBlob> code,error;
    auto hr=D3DCompileFromFile(L"shader/UpdateNormalMap.hlsl",nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());HR(hr);
    ComPtr<ID3D11ComputeShader> shader;HR(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    std::mt19937 random(71);unsigned runs=0;
    for(const auto dims:{std::pair{5u,3u},std::pair{64u,32u},std::pair{67u,61u}})for(float shift:{-1.f,0.f,1.f}){
        const auto [width,height]=dims;
        std::array<V3,8> zero{};
        std::array<V3,8> normals{{{0,0,1},{0,0,1},{0,0,1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{1,0,0}}};
        std::array<V2,8> uvs{{{0,0},{1,0},{1,1},{0,1},{shift,shift},{3+shift,shift},{3+shift,3+shift},{shift,3+shift}}};
        // Leading degenerate triangle ensures nonzero indicesStart in every run.
        std::array<unsigned,15> indices{0,0,0,0,1,2,0,2,3,4,5,6,4,6,7};
        std::array<ComPtr<ID3D11Buffer>,6> buffers;std::array<ComPtr<ID3D11ShaderResourceView>,6> views;
        for(unsigned slot=0;slot<6;++slot){
            const void* data=slot==1?static_cast<const void*>(uvs.data()):slot==2?static_cast<const void*>(normals.data()):slot==5?static_cast<const void*>(indices.data()):zero.data();
            const unsigned stride=slot==1?8:slot==5?4:12,count=slot==5?15:8;
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=stride*count;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=stride;
            D3D11_SUBRESOURCE_DATA initial{data,0,0};HR(device->CreateBuffer(&desc,&initial,&buffers[slot]));
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=count;
            HR(device->CreateShaderResourceView(buffers[slot].Get(),&srv,&views[slot]));
        }
        std::array<unsigned,16> constants{width,height,3,15,0,0,0,0,0,0,8,0};
        constants[12] = (3 * width + 1) * (3 * height + 1) > 32768;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> constant;HR(device->CreateBuffer(&cb,nullptr,&constant));
        D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> output,owner,read;ComPtr<ID3D11UnorderedAccessView> colorUAV,ownerUAV;
        HR(device->CreateTexture2D(&td,nullptr,&output));HR(device->CreateUnorderedAccessView(output.Get(),nullptr,&colorUAV));
        auto ownerDesc=td;ownerDesc.Format=DXGI_FORMAT_R32_UINT;
        HR(device->CreateTexture2D(&ownerDesc,nullptr,&owner));HR(device->CreateUnorderedAccessView(owner.Get(),nullptr,&ownerUAV));
        auto staging=td;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&staging,nullptr,&read));
        const auto readPixels=[&](ID3D11Texture2D* source){
            context->CopyResource(read.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
            std::vector<float> pixels(width*height*4);
            for(unsigned y=0;y<height;++y)std::memcpy(pixels.data()+y*width*4,static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*16);
            context->Unmap(read.Get(),0);return pixels;
        };
        {
            ComPtr<ID3D11Texture2D> secondOutput;
            ComPtr<ID3D11UnorderedAccessView> secondUAV;
            HR(device->CreateTexture2D(&td, nullptr, &secondOutput));
            HR(device->CreateUnorderedAccessView(secondOutput.Get(), nullptr, &secondUAV));
            ID3D11ShaderResourceView* inputs[6]{};
            for (unsigned i = 0; i < 6; ++i) inputs[i] = views[i].Get();
            context->CSSetShaderResources(0, 6, inputs);
            context->CSSetConstantBuffers(0, 1, constant.GetAddressOf());
            context->CSSetShader(shader.Get(), nullptr, 0);
            const auto dispatchGeometry = [&](ID3D11UnorderedAccessView* destination, unsigned begin, unsigned end) {
                const float clear[4]{};
                const UINT clearOwner[4]{};
                context->ClearUnorderedAccessViewFloat(destination, clear);
                context->ClearUnorderedAccessViewUint(ownerUAV.Get(), clearOwner);
                ID3D11UnorderedAccessView* outputs[]{destination, ownerUAV.Get()};
                context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
                constants[2] = begin;
                constants[3] = end;
                for (unsigned pass = 0; pass < 2; ++pass) {
                    constants[11] = pass;
                    context->UpdateSubresource(constant.Get(), 0, nullptr, constants.data(), 0, 0);
                    context->Dispatch(1, constants[12] ? 64 : 1, 1);
                }
                ID3D11UnorderedAccessView* none[2]{};
                context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
            };
            // The second geometry uses lower triangle IDs. Without clearing the
            // owner map, it would stay blank. Neither dispatch waits for the GPU.
            dispatchGeometry(colorUAV.Get(), 9, 15);
            dispatchGeometry(secondUAV.Get(), 3, 9);
            const auto firstPixels = readPixels(output.Get());
            const auto secondPixels = readPixels(secondOutput.Get());
            for (std::size_t i = 0; i < firstPixels.size(); i += 4) {
                Check(std::abs(firstPixels[i] - 1.f) < 1e-6f && firstPixels[i + 3] == 1.f,
                    "owner reuse damaged the first geometry");
                Check(std::abs(secondPixels[i] - .5f) < 1e-6f &&
                    std::abs(secondPixels[i + 1] - 1.f) < 1e-6f && secondPixels[i + 3] == 1.f,
                    "owner reuse retained stale priorities");
            }
        }
        std::vector<float> baseline;
        for(unsigned trial=0;trial<25;++trial){
            const float clear[4]{};const UINT clearOwner[4]{};context->ClearUnorderedAccessViewFloat(colorUAV.Get(),clear);context->ClearUnorderedAccessViewUint(ownerUAV.Get(),clearOwner);
            ID3D11ShaderResourceView* inputs[6]{};for(unsigned i=0;i<6;++i)inputs[i]=views[i].Get();
            context->CSSetShaderResources(0,6,inputs);context->CSSetConstantBuffers(0,1,constant.GetAddressOf());context->CSSetShader(shader.Get(),nullptr,0);
            ID3D11UnorderedAccessView* outputs[]{colorUAV.Get(),ownerUAV.Get()};context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
            for(unsigned pass=0;pass<2;++pass){
                constants[11]=pass;
                std::vector<std::pair<unsigned,unsigned>> chunks;
                if(trial==0)chunks={{3,15}}; // All triangles in one dispatch.
                else if(trial==1)chunks={{9,15},{3,9}}; // High priorities submitted first.
                else{
                    chunks={{3,6},{6,9},{9,12},{12,15}};
                    std::shuffle(chunks.begin(),chunks.end(),random);
                }
                for(const auto& [begin,end]:chunks){constants[2]=begin;constants[3]=end;context->UpdateSubresource(constant.Get(),0,nullptr,constants.data(),0,0);context->Dispatch(1,constants[12] ? 64 : 1,1);}
                // Trial zero diagnoses the claim phase. All other trials match
                // production: no intermediate readback/Map/CPU completion wait.
                if(pass==0 && trial==0){
                    ID3D11UnorderedAccessView* none[2]{};context->CSSetUnorderedAccessViews(0,2,none,nullptr);
                    const auto untouched=readPixels(output.Get());Check(std::all_of(untouched.begin(),untouched.end(),[](float value){return value==0;}),"ownership phase shaded prematurely");
                    context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
                }
            }
            ID3D11UnorderedAccessView* none[2]{};context->CSSetUnorderedAccessViews(0,2,none,nullptr);
            // Production drops its temporary ownership COM references after the
            // final submission, before waiting for any GPU completion/readback.
            if(trial==24){ownerUAV.Reset();owner.Reset();}
            auto pixels=readPixels(output.Get());
            for(std::size_t i=0;i<pixels.size();i+=4){
                Check(std::abs(pixels[i]-1.f)<1e-6f && std::abs(pixels[i+1]-.5f)<1e-6f && std::abs(pixels[i+2]-.5f)<1e-6f && pixels[i+3]==1.f,"highest triangle did not own overlap");
            }
            if(trial==0)baseline=pixels;else Check(pixels==baseline,"split/order changed overlap result");
            ++runs;
        }
    }
    std::cout<<"PASS: "<<runs<<" WARP multi-period overlap runs, rotated submission order and owner reuse\n";
}
