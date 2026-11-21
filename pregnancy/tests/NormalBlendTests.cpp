// SPDX-License-Identifier: GPL-3.0-only
#include "Rendering/NormalBlend.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cstring>
using namespace PNS::Rendering;
namespace {
    void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void HR(HRESULT r) { Require(SUCCEEDED(r),"D3D11 operation failed"); }
    struct Image { ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view; };
    Image Upload(ID3D11Device* device, unsigned width, unsigned height, const std::vector<std::uint8_t>& pixels) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0}; Image result;
        HR(device->CreateTexture2D(&desc,&data,&result.texture)); HR(device->CreateShaderResourceView(result.texture.Get(),nullptr,&result.view)); return result;
    }
    std::vector<std::uint8_t> Solid(unsigned w, unsigned h, std::array<std::uint8_t,4> color) {
        std::vector<std::uint8_t> pixels(w*h*4); for(size_t i=0;i<pixels.size();i+=4) std::copy(color.begin(),color.end(),pixels.begin()+i); return pixels;
    }
    std::vector<std::uint8_t> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, unsigned mip=0) {
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); unsigned w=std::max(1u,desc.Width>>mip),h=std::max(1u,desc.Height>>mip);
        desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&desc,nullptr,&staging));context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture,mip,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::vector<std::uint8_t> result(w*h*4);for(unsigned y=0;y<h;++y) std::memcpy(result.data()+y*w*4,static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch,w*4);
        context->Unmap(staging.Get(),0);return result;
    }
    void MorphTests() {
        Require(NormalizeMorph(-1,0,1)==0,"negative morph");Require(NormalizeMorph(0,0,1)==0,"minimum morph");
        Require(NormalizeMorph(2,0,1)==1,"maximum clamp");Require(NormalizeMorph(3,2,6)==0.25f,"custom range");
        Require(NormalizeMorph(std::numeric_limits<float>::quiet_NaN(),0,1)==0,"NaN morph");
        Require(NormalizeMorph(std::numeric_limits<float>::infinity(),0,1)==0,"infinite morph");
        Require(NormalizeMorph(0,1,1)==0,"empty range");Require(NormalizeMorph(0,2,1)==0,"reversed range");
        const auto limit = std::numeric_limits<float>::max();
        Require(NormalizeMorph(0,-limit,limit)==0.5f,"finite endpoints must not overflow");
        Require(NormalizeMorph(limit,-limit,limit)==1,"finite maximum endpoint");
    }
    void MaskTests() {
        const std::vector<std::uint16_t> indices{0,1,2,0,2,3};
        // Four unrelated UV layouts exercise mesh-space selection without a body-specific UV mask.
        for(unsigned layout=0;layout<4;++layout) {
            float u=0.05f+layout*0.2f;
            std::vector<MaskVertex> vertices{{-10,5,60,u,0.2f},{10,5,60,u+0.15f,0.2f},{10,5,85,u+0.15f,0.8f},{-10,5,85,u,0.8f},{0,0,0,0,0},{0,0,100,1,1}};
            auto mask=RasterizePregnancyMask(vertices,indices,{});Require(!mask.empty(),"mesh mask exists for UV layout");
            const unsigned cx=unsigned((u+0.075f)*256),cy=unsigned((0.2f+(69-60)/25.0f*0.6f)*256);
            Require(mask[cy*256+cx]>230,"belly center selected");Require(mask[0]==0,"outside body unaffected");
            for(auto& v:vertices) v.y=-5;Require(RasterizePregnancyMask(vertices,indices,{}).empty(),"back is excluded");
        }
        Require(RasterizePregnancyMask({},indices,{}).empty(),"empty geometry");
        const std::vector<MaskVertex> torso{{-25,5,50,0,0},{25,5,50,1,0},{25,5,100,1,1},
            {-25,5,100,0,1},{0,0,0,0,0}};
        auto mask = RasterizePregnancyMask(torso,indices,{});
        auto sample = [&](float x, float z) { return mask[unsigned((z-50)/50*256)*256+unsigned((x+25)/50*256)]; };
        Require(sample(-7,83)>240 && sample(7,83)>240 && sample(0,69)>240, "breasts and belly selected");
        Require(sample(0,83)==0 && sample(20,83)==0 && sample(0,54)==0, "sternum, outer arms and hips excluded");
    }
    void GPUTests() {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
        constexpr unsigned w=17,h=19;auto basePixels=Solid(w,h,{128,128,255,51}),targetPixels=Solid(w,h,{255,128,128,204});
        auto base=Upload(device.Get(),w,h,basePixels),target=Upload(device.Get(),w,h,targetPixels);
        NormalBlend blend;Require(blend.Initialize(device.Get()),blend.Error().c_str());Require(blend.CreateOutput(w,h),blend.Error().c_str());
        std::vector<std::uint8_t> mask(256*256,255);Require(blend.SetMask(mask,256),"upload mask");
        // Verify command execution restores caller context state.
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> sentinel;HR(device->CreateBuffer(&cb,nullptr,&sentinel));auto* pointer=sentinel.Get();context->CSSetConstantBuffers(0,1,&pointer);
        Require(blend.Render(context.Get(),base.view.Get(),target.view.Get(),{0,0.85f,0.008f,true}),"render zero");
        Require(Read(device.Get(),context.Get(),blend.Texture())==basePixels,"zero morph preserves source including alpha exactly");
        float previous=0;
        for(float t:{0.13f,0.47f,0.71f,1.0f}) {
            Require(blend.Render(context.Get(),base.view.Get(),target.view.Get(),{t,0.85f,0.008f,true}),"render interpolation");
            auto output=Read(device.Get(),context.Get(),blend.Texture());
            Require(output[0]>previous,"arbitrary morph values produce progressive normals");previous=output[0];
            Require(std::abs(int(output[3])-int(std::lround(51+153*t)))<=1,"endpoint alpha interpolates");
            const float nx=output[0]/127.5f-1,ny=output[1]/127.5f-1,nz=output[2]/127.5f-1;
            Require(std::abs(nx*nx+ny*ny+nz*nz-1)<0.025f,"blended normal is unit length");
        }
        auto end=Read(device.Get(),context.Get(),blend.Texture());Require(end[0]>252 && end[2]<131,"full morph reaches target");
        ComPtr<ID3D11Buffer> after;context->CSGetConstantBuffers(0,1,&after);Require(after.Get()==sentinel.Get(),"caller pipeline state restored");
        Require(Read(device.Get(),context.Get(),base.texture.Get())==basePixels,"shared source texture unchanged");
        D3D11_TEXTURE2D_DESC outputDesc{};blend.Texture()->GetDesc(&outputDesc);Require(outputDesc.MipLevels>1,"output mip chain exists");
        auto mip=Read(device.Get(),context.Get(),blend.Texture(),outputDesc.MipLevels-1);Require(mip[0]>252,"mip chain populated");
        std::fill(mask.begin(),mask.end(),0);Require(blend.SetMask(mask,256),"empty mask upload");
        Require(blend.Render(context.Get(),base.view.Get(),target.view.Get(),{1,1,0.008f,true}),"render empty mask");
        Require(Read(device.Get(),context.Get(),blend.Texture())==basePixels,"outside belly original RGBA preserved");
        // Two actors share a cached Mu normal. A matching authored skin and endpoint
        // contribute zero delta even when the Mu silhouette differs from that skin.
        auto muPixels = Solid(w,h,{128,255,128,101});
        auto mu = Upload(device.Get(),w,h,muPixels);
        std::fill(mask.begin(),mask.end(),255);
        Require(blend.SetMask(mask,256),"full pregnancy mask");
        Require(blend.Render(context.Get(),mu.view.Get(),base.view.Get(),{1,1,0.01f,true},base.view.Get()),"compose neutral endpoint");
        Require(Read(device.Get(),context.Get(),blend.Texture()) == muPixels,"Mu mesh normal survives a neutral pregnancy layer");
        NormalBlend npc;
        Require(npc.Initialize(device.Get()) && npc.CreateOutput(w,h) && npc.SetMask(mask,256),"NPC output");
        Require(npc.Render(context.Get(),mu.view.Get(),target.view.Get(),{0.5f,1,0.01f,true},base.view.Get()),"NPC morph 50 percent");
        auto pregnant = Read(device.Get(),context.Get(),npc.Texture());
        std::vector<std::uint8_t> half(256*256,0);
        for (unsigned y=0;y<256;++y) std::fill_n(half.begin()+256*y,128,255);
        Require(npc.SetMask(half,256),"regional Dino mask");
        Require(npc.Render(context.Get(),mu.view.Get(),base.view.Get(),{1,1,0.01f,true,true},base.view.Get()),"override Dino from assigned skin");
        const auto layered = Read(device.Get(),context.Get(),npc.Texture());
        Require(std::equal(basePixels.begin(),basePixels.begin()+4,layered.begin()),"pregnancy region uses NPC skin");
        Require(std::equal(muPixels.end()-4,muPixels.end(),layered.end()-4),"outside pregnancy region keeps raw Dino RGBA");
        NormalBlend secondSkin;
        Require(secondSkin.Initialize(device.Get()) && secondSkin.CreateOutput(w,h) && secondSkin.SetMask(half,256),"second actor skin setup");
        Require(secondSkin.Render(context.Get(),mu.view.Get(),nullptr,{1,1,0.008f,false,true},target.view.Get()),"same pregnancy strength with a different OSkin normal");
        auto otherSkin = Read(device.Get(),context.Get(),secondSkin.Texture());
        Require(std::equal(targetPixels.begin(),targetPixels.begin()+4,otherSkin.begin()) &&
                std::equal(muPixels.end()-4,muPixels.end(),otherSkin.end()-4),"second NPC retains its selected skin and Dino outside the mask");
        Require(Read(device.Get(),context.Get(),npc.Texture()) == layered,"second skin assignment leaves the first NPC unchanged");
        for (auto format : {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM}) {
            D3D11_TEXTURE2D_DESC bd{};
            bd.Width=bd.Height=4; bd.MipLevels=bd.ArraySize=bd.SampleDesc.Count=1;
            bd.Format=format; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            std::array<std::uint8_t,16> block{};
            if (format==DXGI_FORMAT_BC5_UNORM) block[0]=block[1]=block[8]=block[9]=128;
            D3D11_SUBRESOURCE_DATA init{block.data(),16,0};
            Image bc5; HR(device->CreateTexture2D(&bd,&init,&bc5.texture));
            HR(device->CreateShaderResourceView(bc5.texture.Get(),nullptr,&bc5.view));
            Require(npc.Render(context.Get(),mu.view.Get(),nullptr,{1,1,0.01f,false,true},bc5.view.Get()),"BC5 tangent-space skin override");
            auto texels = Read(device.Get(),context.Get(),npc.Texture());
            Require(texels[2]>250 && texels[3]==muPixels[3],"BC5 restores tangent Z and retains Dino alpha");
        }
        Require(pregnant[0]>170 && pregnant[1]>210 && pregnant[2]<100,"combine Mu shape with pregnancy delta");
        Require(Read(device.Get(),context.Get(),mu.texture.Get()) == muPixels,"shared Mu cache stays original");
        Require(Read(device.Get(),context.Get(),blend.Texture()) == muPixels,"other actor retains independent output");
        Require(npc.Render(context.Get(),mu.view.Get(),target.view.Get(),{0,1,0.01f,true},base.view.Get()),"return to zero");
        Require(Read(device.Get(),context.Get(),npc.Texture()) == muPixels,"zero restores Mu RGBA");
        // Procedural mode smooths a local normal detail and preserves specularity.
        auto detailPixels=Solid(32,32,{128,128,255,91});const auto index=(16*32+16)*4;detailPixels[index]=255;detailPixels[index+2]=128;
        auto detail=Upload(device.Get(),32,32,detailPixels);NormalBlend procedural;Require(procedural.Initialize(device.Get()),"initialize procedural");
        Require(procedural.CreateOutput(32,32),"create procedural output");std::fill(mask.begin(),mask.end(),255);Require(procedural.SetMask(mask,256),"procedural mask");
        Require(procedural.Render(context.Get(),detail.view.Get(),nullptr,{1,1,0.05f,false}),"render without any pregnancy texture");
        auto smooth=Read(device.Get(),context.Get(),procedural.Texture());Require(smooth[index]<200 && smooth[index+2]>220,"procedural detail smoothing");
        Require(smooth[index+3]==91,"procedural mode retains skin specularity");
        Require(Read(device.Get(),context.Get(),blend.Texture())==muPixels,"independent actors have independent outputs");
    }
}
int main() {
    try {MorphTests();MaskTests();GPUTests();std::cout<<"PASS: morph bounds, four UV layouts, belly isolation, WARP shader interpolation, normal normalization, alpha, mipmaps, pipeline restoration and actor isolation\n";return 0;}
    catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
