#include "ObjectNormalMapUpdater.h"
#include "TextureRowSlices.h"
#include "NormalMath.h"
#include "CPURasterTiles.h"
#include <array>
#include <bit>
#include "FutureBatch.h"
#include "SettingsCatalog.h"

namespace Mus {
    namespace {
        class GPUTimeScope {
        public:
            GPUTimeScope(ID3D11Device* device, ID3D11DeviceContext* context, std::string name)
                : context_(context), name_(std::move(name))
            {
                if (!PerformanceCheck || !device || !context) return;
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                HRESULT hr = device->CreateQuery(&desc, disjoint_.GetAddressOf());
                desc.Query = D3D11_QUERY_TIMESTAMP;
                if (SUCCEEDED(hr)) hr = device->CreateQuery(&desc, start_.GetAddressOf());
                if (SUCCEEDED(hr)) hr = device->CreateQuery(&desc, end_.GetAddressOf());
                if (FAILED(hr)) {
                    logger::warn("GPU timing query creation failed for {} ({})", name_, hr);
                    return;
                }
                Shader::ShaderLocker locker(context_);
                Shader::ShaderLockGuard guard(locker);
                context_->Begin(disjoint_.Get());
                context_->End(start_.Get());
                active_ = true;
            }

            GPUTimeScope(const GPUTimeScope&) = delete;
            GPUTimeScope& operator=(const GPUTimeScope&) = delete;

            // Cancellation closes the disjoint query without polling readback.
            ~GPUTimeScope() { Close(false); }

            void Finish()
            {
                if (!active_) return;
                Close(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                Shader::ShaderLocker locker(context_);
                const auto read = [&](ID3D11Query* query, auto& value) {
                    do {
                        HRESULT hr;
                        {
                            Shader::ShaderLockGuard guard(locker);
                            hr = context_->GetData(query, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        }
                        if (hr == S_OK) return true;
                        if (hr != S_FALSE) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } while (std::chrono::steady_clock::now() < deadline);
                    return false;
                };
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
                UINT64 start = 0, end = 0;
                if (!read(disjoint_.Get(), data) || data.Disjoint || !data.Frequency ||
                    !read(start_.Get(), start) || !read(end_.Get(), end) || end < start) return;

                const double elapsed = static_cast<double>(end - start) / data.Frequency * 1000.0;
                const double frameTime = (PerformanceCheckTick ? RE::GetSecondsSinceLastFrame() : TimeTick60) * 1000.0;
                const double tick = frameTime > 0 ? frameTime : TimeTick60 * 1000.0;
                logger::info("{} time: {:.6f}ms => {:.6f}%", name_, elapsed, elapsed / tick * 100.0);
                if (PerformanceCheckConsolePrint)
                    if (const auto console = RE::ConsoleLog::GetSingleton(); console)
                        console->Print("%s time: %.6fms => %.6f%%", name_.c_str(), elapsed, elapsed / tick * 100.0);
            }

        private:
            void Close(bool completed) noexcept
            {
                if (!active_) return;
                active_ = false;
                Shader::ShaderLocker locker(context_);
                Shader::ShaderLockGuard guard(locker);
                if (completed) context_->End(end_.Get());
                context_->End(disjoint_.Get());
                if (completed) context_->Flush();
            }

            ID3D11DeviceContext* context_;
            std::string name_;
            Microsoft::WRL::ComPtr<ID3D11Query> disjoint_, start_, end_;
            bool active_ = false;
        };
    }

    ObjectNormalMapUpdater::~ObjectNormalMapUpdater() { WaitForCleanup(); }

    void ObjectNormalMapUpdater::FinishCleanup(std::future<void>& future) noexcept {
        if (!future.valid()) return;
        try { future.get(); }
        catch (const std::exception& e) { logger::error("GPU resource cleanup failed: {}", e.what()); }
        catch (...) { logger::error("GPU resource cleanup failed"); }
    }

    void ObjectNormalMapUpdater::WaitForCleanup() noexcept {
        // No resource-map/context locks may be held while these jobs finish.
        FinishCleanup(resourceCleanup);
    }

    void ObjectNormalMapUpdater::onEvent(const QuitGameEvent& e) {
        cleanupStopped = true;
        WaitForCleanup();
    }

	void ObjectNormalMapUpdater::onEvent(const FrameEvent& e)
	{
        if (cleanupStopped) return;
        if (resourceCleanup.valid() && resourceCleanup.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        {
            std::shared_lock mapLock(resourceDataMapLock);
            if (resourceDataMap.empty()) return;
        }

		static std::clock_t lastTickTime = currentTime;
		if (lastTickTime + 100 > currentTime) //0.1sec
			return;
		lastTickTime = currentTime;
        FinishCleanup(resourceCleanup);
        resourceCleanup = backGroundWorkerThreads->submitAsync([this]() {
            std::lock_guard lg(resourceDataMapLock);
            for (auto it = resourceDataMap.begin(); it != resourceDataMap.end();)
			{
				if (it->get()->time < 0)
				{
                    if (it->get()->IsQueryDone())
                        it->get()->time = currentTime;
				}
				else
				{
                    logger::debug("{} : Removed garbage texture resource", it->get()->textureName);
                    it = resourceDataMap.erase(it);
					continue;
				}
                it++;
            }
            if (resourceDataMap.size() > 0)
                logger::debug("Current remain texture resource {}", resourceDataMap.size());
		});
	}

	void ObjectNormalMapUpdater::onEvent(const PlayerCellChangeEvent&)
	{
        // Geometry buffers are owned only by their active update batch.
	}

	bool ObjectNormalMapUpdater::Init()
	{
        if (Config::GetSingleton().GetGPUEnable())
        {
            for (std::uint8_t i = 0; i <= isSecondGPUEnabled; i++)
            {
                isValidGPU[i] = false;

                if (isSecondGPUEnabled != 0 && i == 0 && !Config::GetSingleton().GetProcessingInLoadingWithMainGPU())
                    continue;

                const auto device = GetDevice(i);
                if (!device)
                {
                    logger::error("{} : Invalid device", __func__);
                    continue;
                }
                HRESULT hr;
                {
                    D3D11_SAMPLER_DESC samplerDesc = {};
                    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
                    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                    samplerDesc.MipLODBias = 0.0f;
                    samplerDesc.MaxAnisotropy = 4;
                    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
                    samplerDesc.BorderColor[0] = 0.0f;
                    samplerDesc.BorderColor[1] = 0.0f;
                    samplerDesc.BorderColor[2] = 0.0f;
                    samplerDesc.BorderColor[3] = 0.0f;
                    samplerDesc.MinLOD = 0;
                    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
                    hr = device->CreateSamplerState(&samplerDesc, samplerState[i].ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        logger::error("{} : Failed to create samplerState ({})", __func__, hr);
                        continue;
                    }
                }

                if (!CreateConstBuffer(device, sizeof(UpdateNormalMapBufferData), updateNormalMapBuffer[i]))
                    continue;
                if (!CreateConstBuffer(device, sizeof(MergeTextureBufferData), mergeTextureBuffer[i]))
                    continue;
                if (!CreateConstBuffer(device, sizeof(GenerateMipsBufferData), generateMipsBuffer[i]))
                    continue;

                Shader::ShaderManager::GetSingleton().ResetShader();
                if (updateNormalMapShader[i] = Shader::ShaderManager::GetSingleton().GetComputeShader(device, UpdateNormalMapShaderName.data()); !updateNormalMapShader[i])
                    continue;
                if (mergeTexture[i] = Shader::ShaderManager::GetSingleton().GetComputeShader(device, MergeTextureShaderName.data()); !mergeTexture[i])
                    continue;
                if (bleedTexture[i] = Shader::ShaderManager::GetSingleton().GetComputeShader(device, BleedTextureShaderName.data()); !bleedTexture[i])
                    continue;
                if (generateMips[i] = Shader::ShaderManager::GetSingleton().GetComputeShader(device, GenerateMipsShaderName.data()); !generateMips[i])
                    continue;

				logger::info("Initialized for {}", i == 0 ? "Main GPU" : "Second GPU");
				isValidGPU[i] = true;
            }
		}
		return true;
	}

	void ObjectNormalMapUpdater::LoadCacheResource(RE::FormID a_actorID, GeometryDataPtr a_data, UpdateSet& a_updateSet, MergedTextureGeometries& mergedTextureGeometries, ResourceDatas& resourceDatas, UpdateResult& results, const std::string& generationSignature)
	{
        // Geometry storage stays fixed throughout this call. Retain the first
        // duplicate identity, exactly as the previous find_if lookups did.
        std::unordered_map<RE::BSGeometry*, GeometryData::GeometriesInfo*> geometryByIdentity;
        geometryByIdentity.reserve(a_data->geometries.size());
        for (auto& geometry : a_data->geometries)
            geometryByIdentity.try_emplace(geometry.geometry, &geometry);
        const auto findGeometry = [&](RE::BSGeometry* identity) -> GeometryData::GeometriesInfo* {
            const auto found = geometryByIdentity.find(identity);
            return found == geometryByIdentity.end() ? nullptr : found->second;
        };

		//hash update with geo hash + texture hash
        for (auto it = a_updateSet.begin(); it != a_updateSet.end();)
        {
			const auto found = findGeometry(it->first);
			if (!found)
			{
                logger::error("{}::{:x} : Geometry {} not found in data", __func__, a_actorID, it->second.geometryName);
				it = a_updateSet.erase(it);
				continue;
			}
            found->hash = GetHash(it->second, found->hash, generationSignature);
            it++;
		}

		//find texture from cache
        std::sort(a_updateSet.begin(), a_updateSet.end(), [&](const auto& a, const auto& b) {
            // Missing identities were removed above; equal counts stay equivalent.
            return geometryByIdentity.at(a.first)->objInfo.vertexCount() >
                geometryByIdentity.at(b.first)->objInfo.vertexCount();
        });
        // Snapshot complete groups before cache hits erase entries from the update set.
        std::unordered_map<std::string, std::vector<std::uint64_t>> expectedComponents;
        for (const auto& update : a_updateSet)
        {
            const auto geometry = findGeometry(update.first);
            if (geometry)
                expectedComponents[update.second.textureName].push_back(geometry->hash);
        }
        for (auto& [name, hashes] : expectedComponents)
        {
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        }
        std::unordered_set<std::string> cachedTextureNames;
        for (auto it = a_updateSet.begin(); it != a_updateSet.end();)
        {
            const auto found = findGeometry(it->first);
            if (!found)
            {
                logger::error("{}::{:x} : Geometry {} not found in data", __func__, a_actorID, it->second.geometryName);
                it++;
                continue;
            }
            if (cachedTextureNames.contains(it->second.textureName))
            {
                it++;
                continue;
            }

            TextureResourcePtr textureResource = nullptr;
            bool diskCache = false;
            NormalMapStore::GetSingleton().GetResource(found->hash, textureResource, diskCache);
            if (!textureResource)
            {
                it++;
                continue;
            }
            if (textureResource->componentHashes != expectedComponents.at(it->second.textureName))
            {
                logger::info("{}::{:x}::{} : Found exist resource from {} ({:x}), but components differ. so skip", __func__, a_actorID,
                             it->second.geometryName, (diskCache ? "disk cache" : "GPU"), found->hash);
                it++;
                continue;
            }

            logger::info("{}::{:x}::{} : Found exist resource from {} ({:x})", __func__, a_actorID, it->second.geometryName, (diskCache ? "disk cache" : "GPU"), found->hash);
            NormalMapResult newNormalMapResult;
            newNormalMapResult.slot = it->second.slot;
            newNormalMapResult.geometry = it->first;
            newNormalMapResult.vertexCount = found->objInfo.vertexCount();
            newNormalMapResult.geoName = it->second.geometryName;
            newNormalMapResult.texturePath = it->second.srcTexturePath.empty() ? it->second.detailTexturePath : it->second.srcTexturePath;
            newNormalMapResult.textureName = it->second.textureName;
            newNormalMapResult.texture = textureResource;
            newNormalMapResult.hash = found->hash;
            newNormalMapResult.existResource = true;
            newNormalMapResult.diskCache = diskCache;
            results.push_back(newNormalMapResult);

            TextureResourceDataPtr newResourceData = std::make_shared<TextureResourceData>();
            newResourceData->textureName = it->second.textureName;
            resourceDatas.push_back(newResourceData);

            cachedTextureNames.insert(it->second.textureName);
            it = a_updateSet.erase(it);
        }

		//shere resource and use GPU resource instead of disk cache if multiple cache is loaded on the same texture name
		for (auto& result : results) {
			if (!result.existResource)
				continue;
			auto resultFound = std::find_if(results.begin(), results.end(), [&](NormalMapResult& r) {
				return result.geometry != r.geometry && result.textureName == r.textureName && result.existResource == r.existResource;
											});
			if (resultFound == results.end())
				continue;

			NormalMapResult* srcResult = nullptr;
			NormalMapResult* dstResult = nullptr;
			if (result.diskCache == resultFound->diskCache)
			{
				if (result.vertexCount >= resultFound->vertexCount)
				{
					srcResult = &result;
					dstResult = &*resultFound;
				}
				else
				{
					srcResult = &*resultFound;
					dstResult = &result;
				}
			}
			else
			{
				if (!result.diskCache)
				{
					srcResult = &result;
					dstResult = &*resultFound;
				}
				else
				{
					srcResult = &*resultFound;
					dstResult = &result;
				}
			}
			dstResult->diskCache = srcResult->diskCache;
			dstResult->texture = srcResult->texture;
			logger::info("{}::{:x}::{} : Use share resource {:x}", __func__, a_actorID, dstResult->geoName, srcResult->hash);
		}

		//share resource if textureName is the same
        for (auto it = a_updateSet.begin(); it != a_updateSet.end();)
        {
			const auto found = findGeometry(it->first);
			if (!found)
			{
                it++;
				continue;
			}

			if (std::find_if(results.begin(), results.end(), [&](NormalMapResult& result) { return it->first == result.geometry; }) != results.end())
            {
                it++;
                continue;
            }

			const auto resultFound = std::find_if(results.begin(), results.end(), [&](NormalMapResult& result) {
                return it->first != result.geometry && it->second.textureName == result.textureName && result.existResource;
			});
			if (resultFound == results.end())
            {
                it++;
                continue;
            }

			logger::info("{}::{:x}::{} : Use share resource {:x}", __func__, a_actorID, it->second.geometryName, resultFound->hash);

			NormalMapResult newNormalMapResult;
			newNormalMapResult.slot = it->second.slot;
			newNormalMapResult.geometry = it->first;
			newNormalMapResult.vertexCount = found->objInfo.vertexCount();
			newNormalMapResult.geoName = it->second.geometryName;
			newNormalMapResult.texturePath = it->second.srcTexturePath.empty() ? it->second.detailTexturePath : it->second.srcTexturePath;
			newNormalMapResult.textureName = it->second.textureName;
			newNormalMapResult.texture = resultFound->texture;
			newNormalMapResult.hash = found->hash;
			newNormalMapResult.existResource = true;
			newNormalMapResult.diskCache = resultFound->diskCache;
			results.push_back(newNormalMapResult);

			TextureResourceDataPtr newResourceData = std::make_shared<TextureResourceData>();
			newResourceData->textureName = it->second.textureName;
			resourceDatas.push_back(newResourceData);

			mergedTextureGeometries.insert(it->first);
            it = a_updateSet.erase(it);
		}
	}

	bool ObjectNormalMapUpdater::CreateGeometryResourceData(RE::FormID a_actorID, GeometryDataPtr a_data)
	{
		a_data->GetGeometryData();

		if (a_data->vertices.empty() || a_data->indices.empty()
			|| a_data->vertices.size() != a_data->uvs.size()
			|| a_data->geometries.empty() || a_actorID == 0)
		{
			logger::error("{} : Invalid parameters", __func__);
			return false;
        }
        a_data->GeometryProcessing();

		if (a_data->vertices.size() != a_data->uvs.size() ||
			a_data->vertices.size() != a_data->normals.size() ||
			a_data->vertices.size() != a_data->tangents.size() ||
			a_data->vertices.size() != a_data->bitangents.size())
		{
			logger::error("{} : Invalid geometry", __func__);
			return false;
		}

		return true;
	}
	void ObjectNormalMapUpdater::ClearGeometryResourceData()
	{
        // Kept for callers; geometry buffers now retire with the active batch.
	}

	ObjectNormalMapUpdater::GeometryResourceDataPtr ObjectNormalMapUpdater::CreateGeometryBuffers(RE::FormID a_actorID, GeometryDataPtr a_data)
	{
		const auto device = GetDevice();
		if (!device)
		{
			logger::error("{} : Invalid device", __func__);
			return nullptr;
		}
		GeometryResourceDataPtr newGeometryResourceData = std::make_shared<GeometryResourceData>();
		if (!CreateStructuredBuffer(device, a_data->vertices.data(), UINT(sizeof(DirectX::XMFLOAT3) * a_data->vertices.size()), sizeof(DirectX::XMFLOAT3), newGeometryResourceData->vertexBuffer, newGeometryResourceData->vertexSRV))
			return nullptr;
		if (!CreateStructuredBuffer(device, a_data->uvs.data(), UINT(sizeof(DirectX::XMFLOAT2) * a_data->uvs.size()), sizeof(DirectX::XMFLOAT2), newGeometryResourceData->uvBuffer, newGeometryResourceData->uvSRV))
			return nullptr;
		if (!CreateStructuredBuffer(device, a_data->normals.data(), UINT(sizeof(DirectX::XMFLOAT3) * a_data->normals.size()), sizeof(DirectX::XMFLOAT3), newGeometryResourceData->normalBuffer, newGeometryResourceData->normalSRV))
			return nullptr;
		if (!CreateStructuredBuffer(device, a_data->tangents.data(), UINT(sizeof(DirectX::XMFLOAT3) * a_data->tangents.size()), sizeof(DirectX::XMFLOAT3), newGeometryResourceData->tangentBuffer, newGeometryResourceData->tangentSRV))
			return nullptr;
		if (!CreateStructuredBuffer(device, a_data->bitangents.data(), UINT(sizeof(DirectX::XMFLOAT3) * a_data->bitangents.size()), sizeof(DirectX::XMFLOAT3), newGeometryResourceData->bitangentBuffer, newGeometryResourceData->bitangentSRV))
			return nullptr;
		if (!CreateStructuredBuffer(device, a_data->indices.data(), UINT(sizeof(std::uint32_t) * a_data->indices.size()), sizeof(std::uint32_t), newGeometryResourceData->indicesBuffer, newGeometryResourceData->indicesSRV))
			return nullptr;

        return newGeometryResourceData;
	}

	std::uint64_t ObjectNormalMapUpdater::GetHash(const UpdateTextureSet& updateSet, std::uint64_t geoHash, const std::string& generationSignature)
	{
		return std::hash<std::string>()(generationSignature + "|" + std::to_string(updateSet.slot) + "|"
										+ updateSet.srcTexturePath + "|"
										+ updateSet.detailTexturePath + "|"
										+ updateSet.overlayTexturePath + "|"
										+ updateSet.maskTexturePath + "|"
										+ FloatValueText(updateSet.detailStrength) + "|"
										+ std::to_string(geoHash));
	}

	ObjectNormalMapUpdater::UpdateResult ObjectNormalMapUpdater::UpdateObjectNormalMap(RE::FormID a_actorID, GeometryDataPtr a_data, UpdateSet& a_updateSet)
    {
        RefGuard rg(this);

		const std::string_view _func_ = __func__;

		UpdateResult results;

		HRESULT hr;
		const auto device = GetDevice();
		const auto context = GetContext();
		if (!device || !context)
		{
			logger::error("{}::{:x} : Invalid renderer", _func_, a_actorID);
			return results;
		}

		logger::debug("{}::{:x} : updating... {}", _func_, a_actorID, a_updateSet.size());
		Shader::ShaderLocker sl(context);

		MergedTextureGeometries mergedTextureGeometries;
        ResourceDatas resourceDatas;
        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        const auto compression = SelectTextureCompression(tuning->TextureCompress, tuning->TextureCompressQuality,
            sl.IsSecondGPU(), isImmediately.load());
        const auto generationSignature = RuntimeGenerationSignature(*tuning, false, compression);
		LoadCacheResource(a_actorID, a_data, a_updateSet, mergedTextureGeometries, resourceDatas, results, generationSignature);

        const bool tangentZCorrection = Config::GetSingleton().GetTangentZCorrection();

        std::uint64_t batchSamples = RasterBatchSampleLimit;
        for (const auto& update : a_updateSet)
        {
            auto found = std::find_if(a_data->geometries.cbegin(), a_data->geometries.cend(), [&](const GeometryData::GeometriesInfo& geosInfo) {
                return geosInfo.geometry == update.first;
            });
            if (found == a_data->geometries.cend())
            {
                logger::error("{}::{:x} : Geometry {} not found in data", _func_, a_actorID, update.second.geometryName);
                continue;
            }
            const GeometryData::ObjectInfo& objInfo = found->objInfo;
            const auto hash = found->hash;

            if (Config::GetSingleton().GetUpdateNormalMapTime1())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, false, false);

            TextureResourceDataPtr newResourceData = std::make_shared<TextureResourceData>();
            newResourceData->geometry = update.first;
            newResourceData->textureName = update.second.textureName;

            D3D11_TEXTURE2D_DESC srcStagingDesc = {}, detailStagingDesc = {}, overlayStagingDesc = {}, maskStagingDesc = {}, dstDesc = {};

            if (!update.second.srcTexturePath.empty())
            {
                if (!IsDetailNormalMap(update.second.srcTexturePath))
                {
                    logger::info("{}::{:x}::{} : {} src texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.srcTexturePath);

                    if (LoadTextureCPU(device, context, update.second.srcTexturePath, srcStagingDesc, newResourceData->srcTexture2D))
                    {
                        dstDesc = srcStagingDesc;
                        dstDesc.Width = Config::GetSingleton().GetTextureWidth();
                        dstDesc.Height = Config::GetSingleton().GetTextureHeight();
                    }
                }
            }
            if (!update.second.detailTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} detail texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.detailTexturePath);

                if (LoadTextureCPU(device, context, update.second.detailTexturePath, detailStagingDesc, newResourceData->detailTexture2D))
                {
                    dstDesc = detailStagingDesc;
                    dstDesc.Width = std::max(dstDesc.Width, Config::GetSingleton().GetTextureWidth());
                    dstDesc.Height = std::max(dstDesc.Height, Config::GetSingleton().GetTextureHeight());
                }
            }
            if (!Config::GetSingleton().GetIgnoreMissingNormalMap() && !newResourceData->srcTexture2D && !newResourceData->detailTexture2D)
            {
                logger::error("{}::{:x}::{} : NormalMap is missing", _func_, a_actorID, update.second.geometryName);
                continue;
            }

            if (!update.second.overlayTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} overlay texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.overlayTexturePath);
                LoadTextureCPU(device, context, update.second.overlayTexturePath, overlayStagingDesc, newResourceData->overlayTexture2D);
            }

            if (!update.second.maskTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} mask texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.maskTexturePath);
                LoadTextureCPU(device, context, update.second.maskTexturePath, maskStagingDesc, newResourceData->maskTexture2D);
            }

            dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dstDesc.Usage = D3D11_USAGE_STAGING;
            dstDesc.BindFlags = 0;
            dstDesc.MiscFlags = 0;
            dstDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            dstDesc.ArraySize = 1;
            dstDesc.SampleDesc.Count = 1;
            // A missing source and detail map can still use the configured size.
            if (!dstDesc.Width) dstDesc.Width = Config::GetSingleton().GetTextureWidth();
            if (!dstDesc.Height) dstDesc.Height = Config::GetSingleton().GetTextureHeight();
            if (!dstDesc.Width || !dstDesc.Height) return {};
            dstDesc.MipLevels = Config::GetSingleton().GetUseMipMap()
                ? std::bit_width(std::max(dstDesc.Width, dstDesc.Height)) : 1;

            const UINT width = dstDesc.Width;
            const UINT height = dstDesc.Height;
            const std::uint32_t totalTris = objInfo.indicesCount() / 3;
            const std::uint32_t vertexEnd = a_data->vertices.size();
            auto geometrySamples = RasterGeometrySampleLimit(width, height);
            CPURasterTiles tiles(width, height);
            for (UINT i = 0; i < totalTris; ++i) {
                const auto index = objInfo.indicesStart + i * 3;
                const auto i0 = a_data->indices[index], i1 = a_data->indices[index + 1], i2 = a_data->indices[index + 2];
                if (i0 >= vertexEnd || i1 >= vertexEnd || i2 >= vertexEnd) continue;
                RasterTriangle triangle{};
                if (!ProjectRasterTriangle(triangle, a_data->uvs[i0], a_data->uvs[i1], a_data->uvs[i2], width, height, index))
                    continue;
                if (!ReserveRasterSamples(triangle, width, height, geometrySamples, batchSamples)) {
                    logger::warn("{}::{:x}::{} : CPU raster sample budget exceeded", _func_, a_actorID, update.second.geometryName);
                    // Dropping only this geometry could publish an incomplete
                    // normal for a group that shares the same texture.
                    return {};
                }
                tiles.Add(triangle);
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> dstTexture2D;
            hr = device->CreateTexture2D(&dstDesc, nullptr, &dstTexture2D);
            if (FAILED(hr))
            {
                logger::error("{}::{:x}::{} : Failed to create dst staging texture ({})", _func_, a_actorID, update.second.geometryName, hr);
                continue;
            }

			Shader::MapGuard dstmg(context, dstTexture2D.Get(), 0, D3D11_MAP_WRITE);
            if (!dstmg.IsValid())
            {
                logger::error("{}::{:x}::{} : Failed to map dst texture ({})", _func_, a_actorID, update.second.geometryName, dstmg.GetHR());
                continue;
            }
            Shader::MapGuard srcmg(context, newResourceData->srcTexture2D.Get(), 0, D3D11_MAP_READ);
            if (newResourceData->srcTexture2D && !srcmg.IsValid())
            {
                logger::error("{}::{:x}::{} : Failed to map src texture ({})", _func_, a_actorID, update.second.geometryName, srcmg.GetHR());
			}
            Shader::MapGuard detailmg(context, newResourceData->detailTexture2D.Get(), 0, D3D11_MAP_READ);
            if (newResourceData->detailTexture2D && !detailmg.IsValid())
            {
                logger::error("{}::{:x}::{} : Failed to map detail texture ({})", _func_, a_actorID, update.second.geometryName, detailmg.GetHR());
            }
            Shader::MapGuard overlaymg(context, newResourceData->overlayTexture2D.Get(), 0, D3D11_MAP_READ);
            if (newResourceData->overlayTexture2D && !overlaymg.IsValid())
            {
                logger::error("{}::{:x}::{} : Failed to map overlay texture ({})", _func_, a_actorID, update.second.geometryName, overlaymg.GetHR());
            }
            Shader::MapGuard maskmg(context, newResourceData->maskTexture2D.Get(), 0, D3D11_MAP_READ);
            if (newResourceData->maskTexture2D && !maskmg.IsValid())
            {
                logger::error("{}::{:x}::{} : Failed to map mask texture ({})", _func_, a_actorID, update.second.geometryName, maskmg.GetHR());
            }

            std::uint8_t* dstData = dstmg.Get<std::uint8_t>();
            std::uint8_t* srcData = srcmg.IsValid() ? srcmg.Get<std::uint8_t>() : nullptr;
            std::uint8_t* detailData = detailmg.IsValid() ? detailmg.Get<std::uint8_t>() : nullptr; 
            std::uint8_t* overlayData = overlaymg.IsValid() ? overlaymg.Get<std::uint8_t>() : nullptr;
            std::uint8_t* maskData = maskmg.IsValid() ? maskmg.Get<std::uint8_t>() : nullptr;
            
			auto tp = currentProcessingThreads.load();
            // init dst texture
            {
                tp->Execute([&] {
                    tbb::parallel_for(
                        tbb::blocked_range<UINT>(0, height),
                        [&](const tbb::blocked_range<UINT>& r) {
                            const std::int32_t ti = tbb::this_task_arena::current_thread_index();
                            for (UINT y = r.begin(); y != r.end(); ++y)
                            {
                                std::uint8_t* rowData = dstData + y * dstmg.GetRowPitch();
                                for (UINT x = 0; x < width; x++)
                                {
                                    std::uint32_t* pixel = reinterpret_cast<uint32_t*>(rowData + x * 4);
                                    *pixel = emptyColor.GetReverse();
                                }
                            }
                        },
                        tbb::auto_partitioner()
					);
                });
            }

            const bool hasSrcData = (srcData != nullptr);
            const bool hasDetailData = (detailData != nullptr);
            const bool hasOverlayData = (overlayData != nullptr);
            const bool hasMaskData = (maskData != nullptr);

            if (Config::GetSingleton().GetUpdateNormalMapTime1())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, true, false);


            const float WidthF = static_cast<const float>(width);
            const float HeightF = static_cast<const float>(height);
            const float invWidth = 1.0f / WidthF;
            const float invHeight = 1.0f / HeightF;
            const float srcWidthF = hasSrcData ? static_cast<const float>(srcStagingDesc.Width) : 0.0f;
            const float srcHeightF = hasSrcData ? static_cast<const float>(srcStagingDesc.Height) : 0.0f;
            const float detailWidthF = hasDetailData ? static_cast<const float>(detailStagingDesc.Width) : 0.0f;
            const float detailHeightF = hasDetailData ? static_cast<const float>(detailStagingDesc.Height) : 0.0f;
            const float overlayWidthF = hasOverlayData ? static_cast<const float>(overlayStagingDesc.Width) : 0.0f;
            const float overlayHeightF = hasOverlayData ? static_cast<const float>(overlayStagingDesc.Height) : 0.0f;
            const float maskWidthF = hasMaskData ? static_cast<const float>(maskStagingDesc.Width) : 0.0f;
            const float maskHeightF = hasMaskData ? static_cast<const float>(maskStagingDesc.Height) : 0.0f;

            const float detailStrength = update.second.detailStrength;

            if (Config::GetSingleton().GetUpdateNormalMapTime2())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, false, false);

            // Each worker owns all writes to a canonical tile, including UVs
            // that wrap to it from another period.
            tp->Execute([&] {
                tbb::parallel_for(tbb::blocked_range<std::size_t>(0, tiles.Size()), [&](const auto& range) {
                    for (auto tile = range.begin(); tile != range.end(); ++tile) {
                        if (!tiles.HasRegions(tile)) continue;
                        const auto tileX = tiles.TileX(tile), tileY = tiles.TileY(tile);
                        std::array<std::uint32_t, RasterTileSize * RasterTileSize> owners{};
                        tiles.VisitRegions(tile, [&](const RasterTriangle& triangle, const RasterRegion& region) {
                            const auto priority = triangle.index / 3 + 1;
                            for (auto y = region.top; y < region.bottom; ++y) {
                                const auto row = (WrapTextureCoordinate(y, height) - tileY) * RasterTileSize;
                                for (auto x = region.left; x < region.right; ++x) {
                                    DirectX::XMFLOAT3 bary;
                                    if (triangle.Covers(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, bary)) {
                                        auto& owner = owners[row + WrapTextureCoordinate(x, width) - tileX];
                                        owner = std::max(owner, priority);
                                    }
                                }
                            }
                        });
                        tiles.VisitRegions(tile, [&](const RasterTriangle& triangle, const RasterRegion& region) {
                            const auto index = triangle.index;
                            const auto i0 = a_data->indices[index], i1 = a_data->indices[index + 1], i2 = a_data->indices[index + 2];
                            const DirectX::XMVECTOR n0v = DirectX::XMLoadFloat3(&a_data->normals[i0]);
                            const DirectX::XMVECTOR n1v = DirectX::XMLoadFloat3(&a_data->normals[i1]);
                            const DirectX::XMVECTOR n2v = DirectX::XMLoadFloat3(&a_data->normals[i2]);

                            const DirectX::XMVECTOR t0v = DirectX::XMLoadFloat3(&a_data->tangents[i0]);
                            const DirectX::XMVECTOR t1v = DirectX::XMLoadFloat3(&a_data->tangents[i1]);
                            const DirectX::XMVECTOR t2v = DirectX::XMLoadFloat3(&a_data->tangents[i2]);

                            const DirectX::XMVECTOR b0v = DirectX::XMLoadFloat3(&a_data->bitangents[i0]);
                            const DirectX::XMVECTOR b1v = DirectX::XMLoadFloat3(&a_data->bitangents[i1]);
                            const DirectX::XMVECTOR b2v = DirectX::XMLoadFloat3(&a_data->bitangents[i2]);

                            for (auto y = region.top; y < region.bottom; ++y) {
                                const std::uint32_t wrapY = WrapTextureCoordinate(y, height);
                                const float mY = static_cast<const float>(wrapY) * invHeight;

                                std::uint8_t* srcRowData = nullptr;
                                if (hasSrcData)
                                {
                                    const float srcY = mY * srcHeightF;
                                    srcRowData = srcData + static_cast<const UINT>(srcY) * srcmg.GetRowPitch();
                                }

                                std::uint8_t* detailRowData = nullptr;
                                if (hasDetailData)
                                {
                                    const float detailY = mY * detailHeightF;
                                    detailRowData = detailData + static_cast<const UINT>(detailY) * detailmg.GetRowPitch();
                                }

                                std::uint8_t* overlayRowData = nullptr;
                                if (hasOverlayData)
                                {
                                    const float overlayY = mY * overlayHeightF;
                                    overlayRowData = overlayData + static_cast<const UINT>(overlayY) * overlaymg.GetRowPitch();
                                }

                                std::uint8_t* maskRowData = nullptr;
                                if (hasMaskData)
                                {
                                    const float maskY = mY * maskHeightF;
                                    maskRowData = maskData + static_cast<const UINT>(maskY) * maskmg.GetRowPitch();
                                }

                                std::uint8_t* rowData = dstData + wrapY * dstmg.GetRowPitch();
                                for (std::int32_t x = region.left; x < region.right; x++)
                                {
                                    DirectX::XMFLOAT3 bary;
                                    if (!triangle.Covers(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, bary))
                                        continue;

                                    const std::uint32_t wrapX = WrapTextureCoordinate(x, width);
                                    if (owners[(wrapY - tileY) * RasterTileSize + (wrapX - tileX)] != index / 3 + 1)
                                        continue;
                                    const float mX = static_cast<const float>(wrapX) * invWidth;

                                    RGBA dstColor;
                                    RGBA overlayColor(1.0f, 1.0f, 1.0f, 0.0f);
                                    if (hasOverlayData)
                                    {
                                        const float overlayX = mX * overlayWidthF;
                                        const std::uint32_t* overlayPixel = reinterpret_cast<std::uint32_t*>(overlayRowData + static_cast<const UINT>(overlayX) * 4);
                                        overlayColor.SetReverse(*overlayPixel);
                                    }
                                    if (overlayColor.a < 1.0f)
                                    {
                                        RGBA maskColor(1.0f, 1.0f, 1.0f, 0.0f);
                                        if (hasMaskData && hasSrcData)
                                        {
                                            const float maskX = mX * maskWidthF;
                                            const std::uint32_t* maskPixel = reinterpret_cast<std::uint32_t*>(maskRowData + static_cast<const UINT>(maskX) * 4);
                                            maskColor.SetReverse(*maskPixel);
                                        }
                                        if (maskColor.a < 1.0f)
                                        {
                                            RGBA detailColor(0.5f, 0.5f, 1.0f, 0.5f);
                                            if (hasDetailData)
                                            {
                                                const float detailX = mX * detailWidthF;
                                                const std::uint32_t* detailPixel = reinterpret_cast<std::uint32_t*>(detailRowData + static_cast<const UINT>(detailX) * 4);
                                                detailColor.SetReverse(*detailPixel);
                                                detailColor = RGBA::lerp(RGBA(0.5f, 0.5f, 1.0f, detailColor.a), detailColor, detailStrength);
                                            }

                                            const float denomal = (bary.x + bary.y + floatPrecision);
                                            const DirectX::XMVECTOR n01 = SlerpVector(n0v, n1v, bary.y / denomal);
                                            const DirectX::XMVECTOR n = NormalizeOr(SlerpVector(n01, n2v, bary.z), DirectX::XMVectorSet(0, 0, 1, 0));

                                            DirectX::XMVECTOR normalResult = emptyVector;
                                            if (detailColor.a > 0.0f)
                                            {
                                                const DirectX::XMVECTOR t01 = SlerpVector(t0v, t1v, bary.y / denomal);
                                                const DirectX::XMVECTOR t = SlerpVector(t01, t2v, bary.z);

                                                const DirectX::XMVECTOR b01 = SlerpVector(b0v, b1v, bary.y / denomal);
                                                const DirectX::XMVECTOR b = SlerpVector(b01, b2v, bary.z);

                                                const auto basis = BuildNormalTangents(n, t, b);
                                                const DirectX::XMMATRIX tbn = DirectX::XMMATRIX(basis.tangent, basis.bitangent, n, DirectX::XMVectorSet(0, 0, 0, 1));

                                                const DirectX::XMFLOAT4 detailColorF(
                                                    detailColor.r * 2.0f - 1.0f,
                                                    detailColor.g * 2.0f - 1.0f,
                                                    detailColor.b * 2.0f - 1.0f,
                                                    0.0f);
                                                const DirectX::XMVECTOR detailNormalVec = DirectX::XMVectorSet(
                                                    detailColorF.x,
                                                    detailColorF.y,
                                                    tangentZCorrection ? std::sqrt(std::max(0.0f, 1.0f - detailColorF.x * detailColorF.x - detailColorF.y * detailColorF.y)) : detailColorF.z,
                                                    0.0f);

                                                const DirectX::XMVECTOR detailNormal = NormalizeOr(
                                                    DirectX::XMVector3TransformNormal(detailNormalVec, tbn), n);
                                                normalResult = NormalizeOr(
                                                    DirectX::XMVectorLerp(n, detailNormal, detailColor.a), n);
                                            }
                                            else
                                            {
                                                normalResult = n;
                                            }
                                            const DirectX::XMVECTOR normalVec = DirectX::XMVectorMultiplyAdd(normalResult, halfVec, halfVec);
                                            dstColor = RGBA(DirectX::XMVectorGetX(normalVec), DirectX::XMVectorGetZ(normalVec), DirectX::XMVectorGetY(normalVec));
                                        }
                                        if (maskColor.a > 0.0f && hasSrcData)
                                        {
                                            const float srcX = mX * srcWidthF;
                                            const std::uint32_t* srcPixel = reinterpret_cast<std::uint32_t*>(srcRowData + static_cast<const UINT>(srcX) * 4);
                                            RGBA srcColor;
                                            srcColor.SetReverse(*srcPixel);
                                            dstColor = RGBA::lerp(dstColor, srcColor, maskColor.a);
                                        }
                                    }
                                    if (overlayColor.a > 0.0f)
                                    {
                                        dstColor = RGBA::lerp(dstColor, overlayColor, overlayColor.a);
                                    }

                                    std::uint32_t* dstPixel = reinterpret_cast<std::uint32_t*>(rowData + wrapX * 4);
                                    *dstPixel = dstColor.GetReverse() | 0xFF000000;
                                }
                            }
                        });
                    }
                });
            });
            if (Config::GetSingleton().GetUpdateNormalMapTime2())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, true, false);

            NormalMapResult newNormalMapResult;
            newNormalMapResult.slot = update.second.slot;
            newNormalMapResult.geometry = update.first;
            newNormalMapResult.vertexCount = objInfo.vertexCount();
            newNormalMapResult.geoName = update.second.geometryName;
            newNormalMapResult.texturePath = update.second.srcTexturePath.empty() ? update.second.detailTexturePath : update.second.srcTexturePath;
            newNormalMapResult.textureName = update.second.textureName;
            newNormalMapResult.texture = std::make_shared<TextureResource>();
            newNormalMapResult.texture->normalmapTexture2D = dstTexture2D;
            newNormalMapResult.texture->normalmapShaderResourceView = nullptr;
            newNormalMapResult.texture->normalmapUnorderedAccessView = nullptr;
            newNormalMapResult.hash = hash;

            results.push_back(newNormalMapResult);
            resourceDatas.push_back(newResourceData);
        }
		PostProcessing(device, context, resourceDatas, results, mergedTextureGeometries, compression);
		return results;
	}

	ObjectNormalMapUpdater::UpdateResult ObjectNormalMapUpdater::UpdateObjectNormalMapGPU(RE::FormID a_actorID, GeometryDataPtr a_data, UpdateSet& a_updateSet)
	{
		const std::string_view _func_ = __func__;
		UpdateResult results;
        if (!isValidGPU[isSecondGPUEnabled])
            return results;

        RefGuard rg(this);

		HRESULT hr;
		const auto device = GetDevice();
		const auto context = GetContext();
		if (!device || !context)
		{
			logger::error("{}::{:x} : Invalid renderer", _func_, a_actorID);
			return results;
		}

		if (!a_data)
		{
			logger::error("{}::{:x} : Invalid GeometryData", _func_, a_actorID);
			return results;
		}

		Shader::ShaderLocker sl(context);

		logger::debug("{}::{:x} : updating... {}", _func_, a_actorID, a_updateSet.size());

		MergedTextureGeometries mergedTextureGeometries;
        ResourceDatas resourceDatas;
        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        const auto compression = SelectTextureCompression(tuning->TextureCompress, tuning->TextureCompressQuality,
            sl.IsSecondGPU(), isImmediately.load());
        const auto generationSignature = RuntimeGenerationSignature(*tuning, true, compression);
		LoadCacheResource(a_actorID, a_data, a_updateSet, mergedTextureGeometries, resourceDatas, results, generationSignature);

        // Cached normal maps need no geometry uploads. CPU processing above remains
        // unchanged because it supplies the geometry hashes used by cache lookup.
        if (a_updateSet.empty())
            return results;
        const GeometryResourceDataPtr geoData = CreateGeometryBuffers(a_actorID, a_data);
        if (!geoData)
            return {};

        const bool tangentZCorrection = Config::GetSingleton().GetTangentZCorrection();

        // One scratch owner map per batch. The immediate context orders
        // each geometry's last use before the next geometry clears it.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> pixelOwnerTexture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> pixelOwnerUAV;
        UINT ownerWidth = 0, ownerHeight = 0;
        for (const auto& update : a_updateSet)
        {
            const auto found = std::find_if(a_data->geometries.cbegin(), a_data->geometries.cend(), [&](const GeometryData::GeometriesInfo& geosInfo) {
                return geosInfo.geometry == update.first;
            });
            if (found == a_data->geometries.cend())
            {
                logger::error("{}::{:x} : Geometry {} not found in data", _func_, a_actorID, update.second.geometryName);
                continue;
            }
            const GeometryData::ObjectInfo& objInfo = found->objInfo;
            const auto hash = found->hash;

            if (Config::GetSingleton().GetUpdateNormalMapTime1())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, false, false);

            TextureResourceDataPtr newResourceData = std::make_shared<TextureResourceData>();
            newResourceData->geometry = update.first;
            newResourceData->textureName = update.second.textureName;

            D3D11_TEXTURE2D_DESC srcDesc = {}, detailDesc = {}, overlayDesc = {}, maskDesc = {}, dstDesc = {};
            D3D11_SHADER_RESOURCE_VIEW_DESC dstShaderResourceViewDesc = {};

            if (!update.second.srcTexturePath.empty())
            {
                if (!IsDetailNormalMap(update.second.srcTexturePath))
                {
                    logger::info("{}::{:x}::{} : {} src texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.srcTexturePath);
                    if (LoadTexture(device, context, update.second.srcTexturePath, srcDesc, dstShaderResourceViewDesc, newResourceData->srcTexture2D, newResourceData->srcShaderResourceView))
                    {
                        dstDesc = srcDesc;
                        dstDesc.Width = Config::GetSingleton().GetTextureWidth();
                        dstDesc.Height = Config::GetSingleton().GetTextureHeight();
                    }
                }
            }
            if (!update.second.detailTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} detail texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.detailTexturePath);
                if (LoadTexture(device, context, update.second.detailTexturePath, detailDesc, dstShaderResourceViewDesc, newResourceData->detailTexture2D, newResourceData->detailShaderResourceView))
                {
                    dstDesc = detailDesc;
                    dstDesc.Width = std::max(dstDesc.Width, Config::GetSingleton().GetTextureWidth());
                    dstDesc.Height = std::max(dstDesc.Height, Config::GetSingleton().GetTextureHeight());
                }
            }
            if (!Config::GetSingleton().GetIgnoreMissingNormalMap() && !newResourceData->srcShaderResourceView && !newResourceData->detailShaderResourceView)
            {
                logger::error("{}::{:x}::{} : NormalMap is missing", _func_, a_actorID, update.second.geometryName);
                continue;
            }

            if (!update.second.overlayTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} overlay texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.overlayTexturePath);
                LoadTexture(device, context, update.second.overlayTexturePath, overlayDesc, newResourceData->overlayTexture2D, newResourceData->overlayShaderResourceView);
            }

            if (!update.second.maskTexturePath.empty())
            {
                logger::info("{}::{:x}::{} : {} mask texture loading...)", _func_, a_actorID, update.second.geometryName, update.second.maskTexturePath);
                LoadTexture(device, context, update.second.maskTexturePath, maskDesc, newResourceData->maskTexture2D, newResourceData->maskShaderResourceView);
            }

            dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dstDesc.Usage = D3D11_USAGE_DEFAULT;
            dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            dstDesc.ArraySize = 1;
            dstDesc.MiscFlags = 0;
            dstDesc.CPUAccessFlags = 0;
            dstDesc.SampleDesc.Count = 1;
            // A missing source and detail map can still use the configured size.
            if (!dstDesc.Width) dstDesc.Width = Config::GetSingleton().GetTextureWidth();
            if (!dstDesc.Height) dstDesc.Height = Config::GetSingleton().GetTextureHeight();
            if (!dstDesc.Width || !dstDesc.Height) return {};
            dstDesc.MipLevels = Config::GetSingleton().GetUseMipMap()
                ? std::bit_width(std::max(dstDesc.Width, dstDesc.Height)) : 1;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> dstTexture2D = nullptr;
            hr = device->CreateTexture2D(&dstDesc, nullptr, &dstTexture2D);
            if (FAILED(hr))
            {
                logger::error("{}::{:x}::{} : Failed to create dst texture 2d ({})", _func_, a_actorID, update.second.geometryName, hr);
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> dstShaderResourceView = nullptr;
            dstShaderResourceViewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dstShaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            dstShaderResourceViewDesc.Texture2D.MipLevels = dstDesc.MipLevels;
            dstShaderResourceViewDesc.Texture2D.MostDetailedMip = 0;
            hr = device->CreateShaderResourceView(dstTexture2D.Get(), &dstShaderResourceViewDesc, &dstShaderResourceView);
            if (FAILED(hr))
            {
                logger::error("{}::{:x}::{} : Failed to create dst shader resource view ({})", _func_, a_actorID, update.second.geometryName, hr);
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> dstUnorderedAccessView = nullptr;
            D3D11_UNORDERED_ACCESS_VIEW_DESC dstUnorderedViewDesc = {};
            dstUnorderedViewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dstUnorderedViewDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            dstUnorderedViewDesc.Texture2D.MipSlice = 0;
            hr = device->CreateUnorderedAccessView(dstTexture2D.Get(), &dstUnorderedViewDesc, &dstUnorderedAccessView);
            if (FAILED(hr))
            {
                logger::error("{}::{:x}::{} : Failed to create dst unordered access view ({})", _func_, a_actorID, update.second.geometryName, hr);
                continue;
            }

            if (!pixelOwnerUAV || ownerWidth != dstDesc.Width || ownerHeight != dstDesc.Height) {
                pixelOwnerUAV.Reset();
                D3D11_TEXTURE2D_DESC ownerDesc = dstDesc;
                ownerDesc.Format = DXGI_FORMAT_R32_UINT;
                ownerDesc.MipLevels = 1;
                ownerDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                hr = device->CreateTexture2D(&ownerDesc, nullptr, pixelOwnerTexture.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    logger::error("{}::{:x}::{} : Failed to create pixel ownership texture ({})", _func_, a_actorID, update.second.geometryName, hr);
                    continue;
                }
                hr = device->CreateUnorderedAccessView(pixelOwnerTexture.Get(), nullptr, pixelOwnerUAV.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    logger::error("{}::{:x}::{} : Failed to create pixel ownership view ({})", _func_, a_actorID, update.second.geometryName, hr);
                    continue;
                }
                ownerWidth = dstDesc.Width;
                ownerHeight = dstDesc.Height;
            }


            {
                Shader::ShaderLockGuard slg(sl);
                context->ClearUnorderedAccessViewUint(dstUnorderedAccessView.Get(), clearValue);
                const UINT unowned[4] = {};
                context->ClearUnorderedAccessViewUint(pixelOwnerUAV.Get(), unowned);
            }


            const UINT width = dstDesc.Width;
            const UINT height = dstDesc.Height;

            if (Config::GetSingleton().GetUpdateNormalMapTime1())
                PerformanceLog(std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName, true, false);

            UpdateNormalMapBufferData cbData = {};
            cbData.texWidth = dstDesc.Width;
            cbData.texHeight = dstDesc.Height;
            cbData.indicesStart = objInfo.indicesStart;
            cbData.indicesEnd = objInfo.indicesEnd;
            cbData.hasSrcTexture = newResourceData->srcShaderResourceView ? 1 : 0;
            cbData.hasDetailTexture = newResourceData->detailShaderResourceView ? 1 : 0;
            cbData.hasOverlayTexture = newResourceData->overlayShaderResourceView ? 1 : 0;
            cbData.hasMaskTexture = newResourceData->maskShaderResourceView ? 1 : 0;
            cbData.tangentZCorrection = tangentZCorrection ? 1 : 0;
            cbData.detailStrength = update.second.detailStrength;
            cbData.vertexEnd = a_data->vertices.size();
            
            const std::uint32_t totalTris = objInfo.indicesCount() / 3;
            if (totalTris == 0) continue;
            // Join every ownership submission before queueing any color work.
            // Immediate-context command order supplies the inter-dispatch barrier;
            // no CPU/GPU wait or per-chunk ownership reset is needed.
            const bool measureGPU = Config::GetSingleton().GetUpdateNormalMapTime2();
            for (UINT ownershipPass = 0; ownershipPass < 2; ++ownershipPass)
            {
                cbData.ownershipPass = ownershipPass;
                const std::string phaseLabel = ownershipPass == 0 ? "::ownership" : "::shade";
                if (sl.IsSecondGPU() || isNoSplitGPU)
                {
                    const std::uint32_t dispatch = (totalTris + 64 - 1) / 64;

                    std::optional<GPUTimeScope> timing;
                    if (measureGPU)
                        timing.emplace(device, context, std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName + phaseLabel);

                    bool mapSucceeded = true;
                    {
                        UpdateNormalMapBackup sb;
                        Shader::ShaderLockGuard slg(sl);
                        ShaderBackupGuard sbg(context, sb);
                        {
                            Shader::MapGuard mg(context, updateNormalMapBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                            if (!mg.IsValid())
                            {
                                logger::error("{}::{:x}::{} : Failed to map buffer ({})", _func_, a_actorID, update.second.geometryName, mg.GetHR());
                                mapSucceeded = false;
                            }
                            else *mg.Get<UpdateNormalMapBufferData>() = cbData;
                        }
                        if (mapSucceeded) {
                            context->CSSetShader(updateNormalMapShader[isSecondGPUEnabled].Get(), nullptr, 0);
                            context->CSSetConstantBuffers(0, 1, updateNormalMapBuffer[isSecondGPUEnabled].GetAddressOf());
                            context->CSSetShaderResources(0, 1, geoData->vertexSRV.GetAddressOf());
                            context->CSSetShaderResources(1, 1, geoData->uvSRV.GetAddressOf());
                            context->CSSetShaderResources(2, 1, geoData->normalSRV.GetAddressOf());
                            context->CSSetShaderResources(3, 1, geoData->tangentSRV.GetAddressOf());
                            context->CSSetShaderResources(4, 1, geoData->bitangentSRV.GetAddressOf());
                            context->CSSetShaderResources(5, 1, geoData->indicesSRV.GetAddressOf());
                            context->CSSetShaderResources(6, 1, newResourceData->srcShaderResourceView.GetAddressOf());
                            context->CSSetShaderResources(7, 1, newResourceData->detailShaderResourceView.GetAddressOf());
                            context->CSSetShaderResources(8, 1, newResourceData->overlayShaderResourceView.GetAddressOf());
                            context->CSSetShaderResources(9, 1, newResourceData->maskShaderResourceView.GetAddressOf());
                            context->CSSetUnorderedAccessViews(0, 1, dstUnorderedAccessView.GetAddressOf(), nullptr);
                            context->CSSetUnorderedAccessViews(1, 1, pixelOwnerUAV.GetAddressOf(), nullptr);
                            context->CSSetSamplers(0, 1, samplerState[isSecondGPUEnabled].GetAddressOf());
                            context->Dispatch(dispatch, 1, 1);
                        }
                    }

                    if (!mapSucceeded) return {};
                    if (timing) timing->Finish();
                }
                else
                {
                    const std::uint32_t subResolution = std::max(1u, 4096u / (1u << divideTaskQ));
                    const std::uint32_t subSize = std::min(totalTris, std::max(1u, (width / subResolution) * (height / subResolution)));

                    const std::uint32_t numSubTris = (totalTris + subSize - 1) / subSize;
                    const std::uint32_t dispatch = (numSubTris + 64 - 1) / 64;
                    std::atomic_bool batchSucceeded{true};
                    FutureBatch gpuTasks;
                    for (std::size_t subIndex = 0; subIndex < subSize; subIndex++)
                    {
                        const std::uint32_t indicesStart = objInfo.indicesStart + subIndex * numSubTris * 3;
                        if (indicesStart >= objInfo.indicesEnd) break;
                        auto cbData_ = cbData;
                        cbData_.indicesStart = indicesStart;
                        cbData_.indicesEnd = std::min(objInfo.indicesEnd, indicesStart + numSubTris * 3);
                        gpuTasks.Submit(*gpuTask, [&, cbData_, subIndex, phaseLabel]() {
                            std::optional<GPUTimeScope> timing;
                            if (measureGPU)
                                timing.emplace(device, context, std::string(_func_) + "::" + GetHexStr(a_actorID) + "::" + update.second.geometryName + "::" + std::to_string(subIndex) + phaseLabel);

                            bool mapSucceeded = true;
                            {
                                UpdateNormalMapBackup sb;
                                Shader::ShaderLockGuard slg(sl);
                                ShaderBackupGuard sbg(context, sb);
                                {
                                    Shader::MapGuard mg(context, updateNormalMapBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                                    if (!mg.IsValid())
                                    {
                                        logger::error("{}::{:x}::{} : Failed to map buffer ({})", _func_, a_actorID, update.second.geometryName, mg.GetHR());
                                        batchSucceeded.store(false, std::memory_order_relaxed);
                                        mapSucceeded = false;
                                    }
                                    else *mg.Get<UpdateNormalMapBufferData>() = cbData_;
                                }
                                if (mapSucceeded) {
                                    context->CSSetShader(updateNormalMapShader[isSecondGPUEnabled].Get(), nullptr, 0);
                                    context->CSSetConstantBuffers(0, 1, updateNormalMapBuffer[isSecondGPUEnabled].GetAddressOf());
                                    context->CSSetShaderResources(0, 1, geoData->vertexSRV.GetAddressOf());
                                    context->CSSetShaderResources(1, 1, geoData->uvSRV.GetAddressOf());
                                    context->CSSetShaderResources(2, 1, geoData->normalSRV.GetAddressOf());
                                    context->CSSetShaderResources(3, 1, geoData->tangentSRV.GetAddressOf());
                                    context->CSSetShaderResources(4, 1, geoData->bitangentSRV.GetAddressOf());
                                    context->CSSetShaderResources(5, 1, geoData->indicesSRV.GetAddressOf());
                                    context->CSSetShaderResources(6, 1, newResourceData->srcShaderResourceView.GetAddressOf());
                                    context->CSSetShaderResources(7, 1, newResourceData->detailShaderResourceView.GetAddressOf());
                                    context->CSSetShaderResources(8, 1, newResourceData->overlayShaderResourceView.GetAddressOf());
                                    context->CSSetShaderResources(9, 1, newResourceData->maskShaderResourceView.GetAddressOf());
                                    context->CSSetUnorderedAccessViews(0, 1, dstUnorderedAccessView.GetAddressOf(), nullptr);
                                    context->CSSetUnorderedAccessViews(1, 1, pixelOwnerUAV.GetAddressOf(), nullptr);
                                    context->CSSetSamplers(0, 1, samplerState[isSecondGPUEnabled].GetAddressOf());
                                    context->Dispatch(dispatch, 1, 1);
                                }
                            }

                            if (mapSucceeded && timing) timing->Finish();
                        });
                    }
                    gpuTasks.Join();
                    if (!batchSucceeded.load(std::memory_order_relaxed))
                        return {};
                }
            }

            NormalMapResult newNormalMapResult;
            newNormalMapResult.slot = update.second.slot;
            newNormalMapResult.geometry = update.first;
            newNormalMapResult.vertexCount = objInfo.vertexCount();
            newNormalMapResult.geoName = update.second.geometryName;
            newNormalMapResult.texturePath = update.second.srcTexturePath.empty() ? update.second.detailTexturePath : update.second.srcTexturePath;
            newNormalMapResult.textureName = update.second.textureName;
            newNormalMapResult.texture = std::make_shared<TextureResource>();
            newNormalMapResult.texture->normalmapTexture2D = dstTexture2D;
            newNormalMapResult.texture->normalmapShaderResourceView = dstShaderResourceView;
            newNormalMapResult.texture->normalmapUnorderedAccessView = dstUnorderedAccessView;
            newNormalMapResult.hash = hash;

            results.push_back(newNormalMapResult);
            resourceDatas.push_back(newResourceData);
        }
        // No output aliases the scratch map; the context retains queued uses.
        pixelOwnerUAV.Reset();
        pixelOwnerTexture.Reset();
		PostProcessingGPU(device, context, resourceDatas, results, mergedTextureGeometries, compression);
		return results;
	}

	bool ObjectNormalMapUpdater::IsDetailNormalMap(const std::string& a_normalMapPath)
	{
		constexpr std::string_view n_suffix = "_n";
		if (a_normalMapPath.empty())
			return false;
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();
		return stringEndsWith(filename, n_suffix.data());
	}

	bool ObjectNormalMapUpdater::LoadTexture(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& filePath, D3D11_TEXTURE2D_DESC& texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC& srvDesc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texOutput, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srvOutput)
	{
		if (!device || !context || filePath.empty())
			return false;

		HRESULT hr;
		D3D11_TEXTURE2D_DESC tmpTexDesc;
		D3D11_SHADER_RESOURCE_VIEW_DESC tmpSRVDesc;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		if (Shader::ShaderManager::GetSingleton().IsSecondGPUResource(context))
		{
			if (!Shader::TextureLoadManager::GetSingleton().GetTextureFromFile(device, context, filePath, tmpTexDesc, tmpSRVDesc, DXGI_FORMAT_UNKNOWN, false, texture))
				return false;
		}
		else
		{
			if (!Shader::TextureLoadManager::GetSingleton().GetTexture2D(filePath, tmpTexDesc, tmpSRVDesc, DXGI_FORMAT_UNKNOWN, false, texture))
				return false;
		}

		tmpSRVDesc.Texture2D.MipLevels = 1;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		hr = device->CreateShaderResourceView(texture.Get(), &tmpSRVDesc, &srv);
		if (FAILED(hr))
		{
			logger::error("Failed to create shader resource view ({}|{})", hr, filePath);
			return false;
		}
		texDesc = tmpTexDesc;
		srvDesc = tmpSRVDesc;
		texOutput = texture;
		srvOutput = srv;
		return true;
	}
	bool ObjectNormalMapUpdater::LoadTexture(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& filePath, D3D11_TEXTURE2D_DESC& texDesc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texOutput, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srvOutput)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		return LoadTexture(device, context, filePath, texDesc, srvDesc, texOutput, srvOutput);
	}

	bool ObjectNormalMapUpdater::LoadTextureCPU(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& filePath, D3D11_TEXTURE2D_DESC& texDesc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& output)
	{
		if (!device || !context || filePath.empty())
			return false;

		HRESULT hr;
		D3D11_SHADER_RESOURCE_VIEW_DESC tmpSRVDesc;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		if (Shader::ShaderManager::GetSingleton().IsSecondGPUResource(context))
		{
			if (!Shader::TextureLoadManager::GetSingleton().GetTextureFromFile(device, context, filePath, texDesc, tmpSRVDesc, DXGI_FORMAT_R8G8B8A8_UNORM, true, texture))
				return false;
		}
		else
		{
			if (!Shader::TextureLoadManager::GetSingleton().GetTexture2D(filePath, texDesc, tmpSRVDesc, DXGI_FORMAT_R8G8B8A8_UNORM, true, texture))
				return false;
		}
		texture->GetDesc(&texDesc);
		if (texDesc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)
		{
			output = texture;
			return true;
		}
		texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		texDesc.Usage = D3D11_USAGE_STAGING;
		texDesc.MiscFlags = 0;
		texDesc.BindFlags = 0;
		hr = device->CreateTexture2D(&texDesc, nullptr, output.GetAddressOf());
		if (FAILED(hr))
		{
			logger::error("Failed to create texture 2d ({})", hr);
			return false;
		}
		return CopySubresourceRegion(device, context, output.Get(), texture.Get(), 0, 0);
	}

	DirectX::XMVECTOR ObjectNormalMapUpdater::SlerpVector(const DirectX::XMVECTOR& a, const DirectX::XMVECTOR& b, const float& t)
    {
        return SlerpUnitVector(a, b, t);
    }

	bool ObjectNormalMapUpdater::CreateConstBuffer(ID3D11Device* device, UINT byteWidth, Microsoft::WRL::ComPtr<ID3D11Buffer>& bufferOut)
    {
        if (!device)
            return false;
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = byteWidth;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        auto hr = device->CreateBuffer(&cbDesc, nullptr, bufferOut.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            logger::error("{} : Failed to create const buffer ({})", __func__, hr);
            return false;
        }
        return true;
    }

	bool ObjectNormalMapUpdater::CreateStructuredBuffer(ID3D11Device* device, const void* data, UINT size, UINT stride, Microsoft::WRL::ComPtr<ID3D11Buffer>& bufferOut, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srvOut)
	{
		if (!device)
			return false;

		D3D11_BUFFER_DESC desc = {};
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.ByteWidth = size;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;
		desc.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA initData = {};
		initData.pSysMem = data;

		Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
		auto hr = device->CreateBuffer(&desc, &initData, &buffer);
		if (FAILED(hr)) {
			logger::error("{} : Failed to create buffer ({})", __func__, hr);
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = size / stride;

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		hr = device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv);
		if (FAILED(hr)) {
			logger::error("{} : Failed to create buffer shader resource view ({})", __func__, hr);
			return false;
		}

		bufferOut = buffer;
		srvOut = srv;
		return true;
	};

	bool ObjectNormalMapUpdater::CopySubresourceRegion(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* dstTexture, ID3D11Texture2D* srcTexture, UINT dstMipMapLevel, UINT srcMipMapLevel)
	{
		if (!device || !context || !dstTexture || !srcTexture)
			return false;

		D3D11_TEXTURE2D_DESC dstDesc, srcDesc;
		dstTexture->GetDesc(&dstDesc);
		srcTexture->GetDesc(&srcDesc);
        if (dstMipMapLevel >= dstDesc.MipLevels || srcMipMapLevel >= srcDesc.MipLevels) return false;

		const UINT srcWidth = std::max(srcDesc.Width >> srcMipMapLevel, 1u);
		const UINT srcHeight = std::max(srcDesc.Height >> srcMipMapLevel, 1u);
		const UINT dstWidth = std::max(dstDesc.Width >> dstMipMapLevel, 1u);
		const UINT dstHeight = std::max(dstDesc.Height >> dstMipMapLevel, 1u);
		if (srcWidth != dstWidth || srcHeight != dstHeight)
			return false;

		Shader::ShaderLocker sl(context);

		if (sl.IsSecondGPU() || isNoSplitGPU || (dstWidth <= 256 && dstHeight <= 256))
		{
            Shader::ShaderLockGuard slg(sl);
			context->CopySubresourceRegion(dstTexture, dstMipMapLevel, 0, 0, 0, srcTexture, srcMipMapLevel, nullptr);
			return true;
		}

		const std::uint32_t subResolution = 4096 / (1u << divideTaskQ);
		const std::uint32_t subY = std::min(dstHeight, 
											   std::max(1u, dstWidth / subResolution) 
											   * std::max(1u, dstHeight / subResolution));
        const auto slices = MakeTextureRowSlices(dstHeight, subY, DirectX::IsCompressed(dstDesc.Format) ? 4u : 1u);
		FutureBatch gpuTasks;
		for (std::uint32_t sy = 0; sy < slices.size(); sy++)
		{
			D3D11_BOX box = {};
			box.left = 0;
			box.right = dstWidth;
			box.top = slices[sy].top;
			box.bottom = slices[sy].bottom;
			box.front = 0;
			box.back = 1;

			gpuTasks.Submit(*gpuTask, [&, box, dstWidth, dstHeight, sy]() {
				std::optional<GPUTimeScope> timing;
				if (Config::GetSingleton().GetTextureCopyTime())
				    timing.emplace(device, context, std::string("CopySubresourceRegion") + "::" + std::to_string(dstWidth) + "::" + std::to_string(dstHeight) + "::" + std::to_string(sy));

				{
                    Shader::ShaderLockGuard slg(sl);
                    context->CopySubresourceRegion(
                        dstTexture, dstMipMapLevel,
                        box.left, box.top, 0,
                        srcTexture, srcMipMapLevel,
                        &box);
                }

				if (timing) timing->Finish();
			});
		}
		gpuTasks.Join();
		return true;
	}
	bool ObjectNormalMapUpdater::CopySubresourceRegion(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* dstTexture, ID3D11Texture2D* srcTexture)
	{
		if (!device || !context || !dstTexture || !srcTexture)
			return false;

		D3D11_TEXTURE2D_DESC dstDesc, srcDesc;
		dstTexture->GetDesc(&dstDesc);
		srcTexture->GetDesc(&srcDesc);
		if (dstDesc.MipLevels != srcDesc.MipLevels)
			return false;

		for (UINT mipLevel = 0; mipLevel < dstDesc.MipLevels; mipLevel++) {
            if (!CopySubresourceRegion(device, context, dstTexture, srcTexture, mipLevel, mipLevel))
                return false;
		}
        return true;
	}

	bool ObjectNormalMapUpdater::CopySubresourceFromBuffer(ID3D11Device* device, ID3D11DeviceContext* context, std::vector<std::uint8_t>& buffer, UINT rowPitch, UINT mipLevel, ID3D11Texture2D* dstTexture)
	{
		if (!device || !context || buffer.empty() || !dstTexture)
			return false;

		D3D11_TEXTURE2D_DESC desc;
		dstTexture->GetDesc(&desc);
        if (mipLevel >= desc.MipLevels) return false;

		Shader::ShaderLocker sl(context);
		const UINT width = std::max(1u, desc.Width >> mipLevel);
		const UINT height = std::max(1u, desc.Height >> mipLevel);
        std::size_t minRowPitch = 0, minSlicePitch = 0;
        if (FAILED(DirectX::ComputePitch(desc.Format, width, height, minRowPitch, minSlicePitch)) ||
            rowPitch < minRowPitch || buffer.size() < static_cast<std::uint64_t>(rowPitch) * DirectX::ComputeScanlines(desc.Format, height))
            return false;

		if (sl.IsSecondGPU() || isNoSplitGPU || (width <= 256 && height <= 256))
		{
			std::optional<GPUTimeScope> timing;
			if (Config::GetSingleton().GetTextureCopyTime())
			    timing.emplace(device, context, std::string("CopySubresourceFromBuffer") + "::" + std::to_string(width) + "::" + std::to_string(height) );

			{
                Shader::ShaderLockGuard slg(sl);
                context->UpdateSubresource(dstTexture, mipLevel, nullptr, buffer.data(), rowPitch, 0);
            }

			if (timing) timing->Finish();
			return true;
		}

		const std::uint32_t subResolution = 4096 / (1u << divideTaskQ);
		const std::uint32_t subY = std::min(height,
											   std::max(1u, width / subResolution) 
											   * std::max(1u, height / subResolution));
        const UINT blockHeight = DirectX::IsCompressed(desc.Format) ? 4u : 1u;
        const auto slices = MakeTextureRowSlices(height, subY, blockHeight);

		FutureBatch gpuTasks;
		for (std::uint32_t sy = 0; sy < slices.size(); sy++)
		{
			D3D11_BOX box = {};
			box.left = 0;
			box.right = width;
			box.top = slices[sy].top;
			box.bottom = slices[sy].bottom;
			box.front = 0;
			box.back = 1;

			const UINT blockY = box.top / blockHeight;
			const std::uint8_t* bufferStart = buffer.data() + static_cast<std::size_t>(blockY) * rowPitch;

			gpuTasks.Submit(*gpuTask, [&, mipLevel, box, bufferStart, sy]() {
				std::optional<GPUTimeScope> timing;
				if (Config::GetSingleton().GetTextureCopyTime())
				    timing.emplace(device, context, std::string("CopySubresourceFromBuffer") + "::" + std::to_string(width) + "::" + std::to_string(height) + "::" + std::to_string(sy));

				{
                    Shader::ShaderLockGuard slg(sl);
                    context->UpdateSubresource(
                        dstTexture,
                        mipLevel,
                        &box,
                        bufferStart,
                        rowPitch,
                        0);
                }

				if (timing) timing->Finish();
			});
		}
		gpuTasks.Join();
		return true;
	}
	bool ObjectNormalMapUpdater::CopySubresourceFromBuffer(ID3D11Device* device, ID3D11DeviceContext* context, std::vector<std::vector<std::uint8_t>>& buffers, std::vector<UINT>& rowPitches, ID3D11Texture2D* dstTexture)
	{
		if (!device || !context || buffers.empty() || !dstTexture)
			return false;

		D3D11_TEXTURE2D_DESC desc;
		dstTexture->GetDesc(&desc);

		if (buffers.size() < desc.MipLevels || rowPitches.size() < desc.MipLevels)
            return false;
        for (UINT mipLevel = 0; mipLevel < desc.MipLevels; mipLevel++) {
			if (!CopySubresourceFromBuffer(device, context, buffers[mipLevel], rowPitches[mipLevel], mipLevel, dstTexture))
                return false;
		}
		return true;
	}

	void ObjectNormalMapUpdater::PostProcessing(ID3D11Device *device, ID3D11DeviceContext *context, ResourceDatas &resourceDatas, UpdateResult &results, MergedTextureGeometries &mergedTextureGeometries, const TextureCompressionPlan& compression)
	{
        std::unordered_set<RE::BSGeometry*> failedCopyResources;
        std::unordered_set<const TextureResource*> compressionFallbacks;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingScratch;
        const auto rejectTexture = [&](const NormalMapResult& failed) {
            for (const auto& candidate : results)
                if (candidate.texture == failed.texture)
                    failedCopyResources.insert(candidate.geometry);
        };
        const auto publishResource = [&](const NormalMapResult& result) {
            if (compressionFallbacks.contains(result.texture.get())) return;
            // This immutable certificate belongs to this exact completed image.
            auto& hashes = result.texture->componentHashes;
            hashes.clear();
            for (const auto& alias : results)
                if (!alias.existResource && alias.texture == result.texture)
                    hashes.push_back(alias.hash);
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
            NormalMapStore::GetSingleton().AddResource(result.hash, result.texture);
        };

		//merge texture
		{
			bool merged = false;
            auto results_ = results;
            std::sort(results_.begin(), results_.end(), [](NormalMapResult& a, NormalMapResult& b) {
                return a.vertexCount > b.vertexCount;
            });
            for (const auto& dst : results_)
            {
                if (dst.existResource || failedCopyResources.contains(dst.geometry))
                    continue;
                if (mergedTextureGeometries.find(dst.geometry) != mergedTextureGeometries.end())
                    continue;

                for (auto& src : results)
                {
                    if (src.existResource || failedCopyResources.contains(src.geometry))
                        continue;
					if (src.geometry == dst.geometry)
						continue;
					if (mergedTextureGeometries.find(src.geometry) != mergedTextureGeometries.end())
						continue;
					if (src.textureName != dst.textureName)
						continue;
					if (src.vertexCount > dst.vertexCount)
						continue;

					auto srcResource = std::find_if(resourceDatas.begin(), resourceDatas.end(), [&](TextureResourceDataPtr& resource) { return resource->geometry == src.geometry; });
					if (srcResource == resourceDatas.end())
                    {
                        rejectTexture(dst);
                        rejectTexture(src);
                        break;
                    }
					logger::info("{} : Merge texture {} into {}...", src.textureName, src.geoName, dst.geoName);
					bool mergeResult = false;
					mergeResult = MergeTexture(device, context, *srcResource,
												  dst.texture->normalmapTexture2D.Get(), src.texture->normalmapTexture2D.Get());
					if (mergeResult)
					{
						src.texture = dst.texture;
						mergedTextureGeometries.insert(src.geometry);
						merged = true;
					}
                    else
                    {
                        rejectTexture(dst);
                        rejectTexture(src);
                        break;
                    }
				}
			}
		}

		const bool isSecondGPU = Shader::ShaderManager::GetSingleton().IsSecondGPUResource(context);
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!GenerateMips(device, context, resourceData, result.texture->normalmapTexture2D.Get()))
                rejectTexture(result);
        }
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!CompressTexture(device, context, resourceData, result.texture->normalmapTexture2D, compression, stagingScratch)) {
                if (!result.texture->normalmapTexture2D) rejectTexture(result);
                else compressionFallbacks.insert(result.texture.get());
            }
        }
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!isSecondGPU)
            {
                D3D11_TEXTURE2D_DESC desc;
                result.texture->normalmapTexture2D->GetDesc(&desc);
                HRESULT hr;
                if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
                {
                    resourceData->stagingTexture2D = result.texture->normalmapTexture2D;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;
                    hr = device->CreateTexture2D(&desc, nullptr, result.texture->normalmapTexture2D.ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        logger::error("{} : Failed to create Texture2D ({})", resourceData->textureName, hr);
                        rejectTexture(result);
                        continue;
                    }
                    if (!CopySubresourceRegion(device, context, result.texture->normalmapTexture2D.Get(), resourceData->stagingTexture2D.Get()))
                    {
                        rejectTexture(result);
                        continue;
                    }
                }
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                srvDesc.Format = desc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = desc.MipLevels;
                srvDesc.Texture2D.MostDetailedMip = 0;
                hr = device->CreateShaderResourceView(result.texture->normalmapTexture2D.Get(), &srvDesc, result.texture->normalmapShaderResourceView.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    logger::error("{} : Failed to create ShaderResourceView ({})", resourceData->textureName, hr);
                    rejectTexture(result);
                    continue;
                }

                result.texture->normalmapUnorderedAccessView.Reset();
                resourceData->GetQuery(device, context);
				{
                    std::lock_guard lg(resourceDataMapLock);
                    resourceDataMap.push_back(resourceData);
                }
                logger::info("{} : normalmap created", result.textureName, result.geoName);
                publishResource(result);
            }
            else
            {
                if (CopyResourceSecondToMain(resourceData, result.texture->normalmapTexture2D, result.texture->normalmapShaderResourceView))
                {
                    logger::info("{} : normalmap created", result.textureName, result.geoName);
                    result.texture->normalmapUnorderedAccessView.Reset();
                    publishResource(result);
                }
                else
                    rejectTexture(result);
            }
        }
        std::erase_if(results, [&](const auto& result) {
            return failedCopyResources.find(result.geometry) != failedCopyResources.end();
        });
	}
    void ObjectNormalMapUpdater::PostProcessingGPU(ID3D11Device* device, ID3D11DeviceContext* context, ResourceDatas& resourceDatas, UpdateResult& results, MergedTextureGeometries& mergedTextureGeometries, const TextureCompressionPlan& compression)
    {
        std::unordered_set<RE::BSGeometry*> failedCopyResources;
        std::unordered_set<const TextureResource*> compressionFallbacks;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingScratch;
        const auto rejectTexture = [&](const NormalMapResult& failed) {
            for (const auto& candidate : results)
                if (candidate.texture == failed.texture)
                    failedCopyResources.insert(candidate.geometry);
        };
        const auto publishResource = [&](const NormalMapResult& result) {
            if (compressionFallbacks.contains(result.texture.get())) return;
            // This immutable certificate belongs to this exact completed image.
            auto& hashes = result.texture->componentHashes;
            hashes.clear();
            for (const auto& alias : results)
                if (!alias.existResource && alias.texture == result.texture)
                    hashes.push_back(alias.hash);
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
            NormalMapStore::GetSingleton().AddResource(result.hash, result.texture);
        };

        // merge texture
        {
            bool merged = false;
            auto results_ = results;
            std::sort(results_.begin(), results_.end(), [](NormalMapResult& a, NormalMapResult& b) {
                return a.vertexCount > b.vertexCount;
            });
            for (const auto& dst : results_)
            {
                if (dst.existResource || failedCopyResources.contains(dst.geometry))
                    continue;
                if (mergedTextureGeometries.find(dst.geometry) != mergedTextureGeometries.end())
                    continue;

                for (auto& src : results)
                {
                    if (src.existResource || failedCopyResources.contains(src.geometry))
                        continue;
                    if (src.geometry == dst.geometry)
                        continue;
                    if (mergedTextureGeometries.find(src.geometry) != mergedTextureGeometries.end())
                        continue;
                    if (src.textureName != dst.textureName)
                        continue;
                    if (src.vertexCount > dst.vertexCount)
                        continue;

                    auto resourceIt = std::find_if(resourceDatas.begin(), resourceDatas.end(), [&](TextureResourceDataPtr& resource) { return resource->geometry == src.geometry; });
                    if (resourceIt == resourceDatas.end())
                    {
                        rejectTexture(dst);
                        rejectTexture(src);
                        break;
                    }
                    logger::info("{} : Merge texture {} into {}...", src.textureName, src.geoName, dst.geoName);
                    bool mergeResult = false;
                    mergeResult = MergeTextureGPU(device, context, *resourceIt,
                                                  dst.texture->normalmapUnorderedAccessView.Get(),
                                                  dst.texture->normalmapTexture2D.Get(),
                                                  src.texture->normalmapShaderResourceView.Get());
                    if (mergeResult)
                    {
                        src.texture = dst.texture;
                        mergedTextureGeometries.insert(src.geometry);
                        merged = true;
                    }
                    else
                    {
                        rejectTexture(dst);
                        rejectTexture(src);
                        break;
                    }
                }
            }
        }

        const bool isSecondGPU = Shader::ShaderManager::GetSingleton().IsSecondGPUResource(context);
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!GenerateMipsGPU(device, context, resourceData, result.texture->normalmapShaderResourceView.Get(), result.texture->normalmapTexture2D.Get()))
                rejectTexture(result);
        }
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!CompressTexture(device, context, resourceData, result.texture->normalmapTexture2D, compression, stagingScratch)) {
                if (!result.texture->normalmapTexture2D) rejectTexture(result);
                else compressionFallbacks.insert(result.texture.get());
            }
        }
        for (std::uint32_t i = 0; i < results.size(); i++)
        {
            auto& result = results[i];
            auto& resourceData = resourceDatas[i];
            if (failedCopyResources.contains(result.geometry))
                continue;
            if (result.existResource)
                continue;
            if (mergedTextureGeometries.find(result.geometry) != mergedTextureGeometries.end())
                continue;
            if (!isSecondGPU)
            {
                D3D11_TEXTURE2D_DESC desc;
                result.texture->normalmapTexture2D->GetDesc(&desc);
                HRESULT hr;
                if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
                {
                    resourceData->stagingTexture2D = result.texture->normalmapTexture2D;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;
                    hr = device->CreateTexture2D(&desc, nullptr, result.texture->normalmapTexture2D.ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        logger::error("{} : Failed to create Texture2D ({})", resourceData->textureName, hr);
                        rejectTexture(result);
                        continue;
                    }
                    if (!CopySubresourceRegion(device, context, result.texture->normalmapTexture2D.Get(), resourceData->stagingTexture2D.Get()))
                    {
                        rejectTexture(result);
                        continue;
                    }
                }
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                srvDesc.Format = desc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = desc.MipLevels;
                srvDesc.Texture2D.MostDetailedMip = 0;
                hr = device->CreateShaderResourceView(result.texture->normalmapTexture2D.Get(), &srvDesc, result.texture->normalmapShaderResourceView.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    logger::error("{} : Failed to create ShaderResourceView ({})", resourceData->textureName, hr);
                    rejectTexture(result);
                    continue;
                }

                result.texture->normalmapUnorderedAccessView.Reset();
                resourceData->GetQuery(device, context);
                {
                    std::lock_guard lg(resourceDataMapLock);
                    resourceDataMap.push_back(resourceData);
                }
                logger::info("{} : normalmap created", result.textureName, result.geoName);
                publishResource(result);
            }
            else
            {
                if (CopyResourceSecondToMain(resourceData, result.texture->normalmapTexture2D, result.texture->normalmapShaderResourceView))
                {
                    logger::info("{} : normalmap created", result.textureName, result.geoName);
                    result.texture->normalmapUnorderedAccessView.Reset();
                    publishResource(result);
                }
                else
                    rejectTexture(result);
            }
        }
        std::erase_if(results, [&](const auto& result) {
            return failedCopyResources.find(result.geometry) != failedCopyResources.end();
        });
	}

	bool ObjectNormalMapUpdater::MergeTexture(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& srcResourceData, ID3D11Texture2D* dstTex, ID3D11Texture2D* srcTex)
	{
		if (!device || !context)
			return false;

		logger::info("{}::{} : Merge texture...", __func__, srcResourceData->textureName);

		if (Config::GetSingleton().GetMergeTime())
			PerformanceLog(std::string(__func__) + "::" + srcResourceData->textureName, false, false);

		Shader::ShaderLocker sl(context);

		D3D11_TEXTURE2D_DESC desc;
		dstTex->GetDesc(&desc);

		Shader::MapGuard dstmg(context, dstTex, 0, D3D11_MAP_READ_WRITE);
        if (!dstmg.IsValid())
        {
            logger::error("{}::{} : Failed to map dst texture ({})", __func__, srcResourceData->textureName, dstmg.GetHR());
            return false;
        }
        Shader::MapGuard srcmg(context, srcTex, 0, D3D11_MAP_READ);
        if (!srcmg.IsValid())
        {
            logger::error("{}::{} : Failed to map src texture ({})", __func__, srcResourceData->textureName, srcmg.GetHR());
            return false;
        }

        auto tp = currentProcessingThreads.load();
		const UINT width = desc.Width;
		const UINT height = desc.Height;

		std::uint8_t* dst_pData = dstmg.Get<std::uint8_t>();
        std::uint8_t* src_pData = srcmg.Get<std::uint8_t>();
        tp->Execute([&] {
            tbb::parallel_for(
                tbb::blocked_range<UINT>(0, height),
                [&](const tbb::blocked_range<UINT>& r) {
                    for (UINT y = r.begin(); y != r.end(); ++y)
                    {
                        for (UINT x = 0; x < width; x++)
                        {
                            std::uint8_t* dstRowData = dst_pData + y * dstmg.GetRowPitch();
                            std::uint8_t* srcRowData = src_pData + y * srcmg.GetRowPitch();
                            std::uint32_t* dstPixel = reinterpret_cast<uint32_t*>(dstRowData + x * 4);
                            std::uint32_t* srcPixel = reinterpret_cast<uint32_t*>(srcRowData + x * 4);
                            RGBA dstPixelColor, srcPixelColor;
                            dstPixelColor.SetReverse(*dstPixel);
                            srcPixelColor.SetReverse(*srcPixel);
                            if (dstPixelColor.a < 1.0f && srcPixelColor.a < 1.0f)
                                continue;

                            RGBA dstColor = RGBA::lerp(srcPixelColor, dstPixelColor, dstPixelColor.a);
                            dstColor.a = 1.0f;
                            *dstPixel = dstColor.GetReverse();
                        }
                    }
                },
                tbb::auto_partitioner());
        });

		if (Config::GetSingleton().GetMergeTime())
			PerformanceLog(std::string(__func__) + "::" + srcResourceData->textureName, true, false);
		logger::debug("{}::{} : Merge texture done", __func__, srcResourceData->textureName);
		return true;
	}
    bool ObjectNormalMapUpdater::MergeTextureGPU(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& resourceData, ID3D11UnorderedAccessView* dstUAV, ID3D11Texture2D* dstTex, ID3D11ShaderResourceView* srcSRV)
	{
		if (!device || !context || !srcSRV || !dstTex)
			return false;

		const std::string _func_ = __func__;
		logger::info("{}::{} : Merge texture...", _func_, resourceData->textureName);

		Shader::ShaderLocker sl(context);

		HRESULT hr;

		D3D11_TEXTURE2D_DESC desc;
		dstTex->GetDesc(&desc);

		const UINT width = desc.Width;
		const UINT height = desc.Height;

		MergeTextureBufferData cbData = {};
        cbData.width = width;
        cbData.height = height;
        cbData.widthStart = 0;
        cbData.heightStart = 0;

		if (sl.IsSecondGPU() || isNoSplitGPU)
		{
			const DirectX::XMUINT2 dispatch = { (width + 8 - 1) / 8, (height + 8 - 1) / 8 };

			std::optional<GPUTimeScope> timing;
			if (Config::GetSingleton().GetMergeTime())
			    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(width) + "|" + std::to_string(height) );

			{
                MergeTextureBackup sb;
                Shader::ShaderLockGuard slg(sl);
                ShaderBackupGuard sbg(context, sb);
                {
                    Shader::MapGuard mg(context, mergeTextureBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                    if (!mg.IsValid())
                    {
                        logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                        return false;
                    }
                    *mg.Get<MergeTextureBufferData>() = cbData;
                }
                context->CSSetShader(mergeTexture[isSecondGPUEnabled].Get(), nullptr, 0);
                context->CSSetConstantBuffers(0, 1, mergeTextureBuffer[isSecondGPUEnabled].GetAddressOf());
                context->CSSetShaderResources(0, 1, &srcSRV);
                context->CSSetUnorderedAccessViews(0, 1, &dstUAV, nullptr);
                context->Dispatch(dispatch.x, dispatch.y, 1);
            }

			if (timing) timing->Finish();
		}
		else
		{
			const std::uint32_t subResolution = std::max(1u, 4096u / (1u << divideTaskQ));
			const std::uint32_t subY = std::min(height, std::max(1u, width / subResolution) * std::max(1u, height / subResolution));
			const std::uint32_t unitY = (height + subY - 1) / subY;
			const DirectX::XMUINT2 dispatch = { (width + 8 - 1) / 8, (std::min(height, unitY) + 8 - 1) / 8 };

			std::atomic_bool batchSucceeded{true};
			FutureBatch gpuTasks;
			for (std::uint32_t sy = 0; sy < subY; sy++)
			{
				auto cbData_ = cbData;
				cbData_.widthStart = 0;
				cbData_.heightStart = unitY * sy;
                gpuTasks.Submit(*gpuTask, [&, cbData_, sy]() {
					std::optional<GPUTimeScope> timing;
					if (Config::GetSingleton().GetMergeTime())
					    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(width) + "|" + std::to_string(height) + "::" + std::to_string(sy));

					{
                        MergeTextureBackup sb;
                        Shader::ShaderLockGuard slg(sl);
                        ShaderBackupGuard sbg(context, sb);
                        {
                            Shader::MapGuard mg(context, mergeTextureBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                            if (!mg.IsValid())
                            {
                                logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                                batchSucceeded.store(false, std::memory_order_relaxed);
                                return;
                            }
                            *mg.Get<MergeTextureBufferData>() = cbData_;
                        }
                        context->CSSetShader(mergeTexture[isSecondGPUEnabled].Get(), nullptr, 0);
                        context->CSSetConstantBuffers(0, 1, mergeTextureBuffer[isSecondGPUEnabled].GetAddressOf());
                        context->CSSetShaderResources(0, 1, &srcSRV);
                        context->CSSetUnorderedAccessViews(0, 1, &dstUAV, nullptr);
                        context->Dispatch(dispatch.x, dispatch.y, 1);
                    }

					if (timing) timing->Finish();
				});
			}
			gpuTasks.Join();
			if (!batchSucceeded.load(std::memory_order_relaxed))
			    return false;
		}
		logger::debug("{}::{} : Merge texture done", _func_, resourceData->textureName);
		return true;
	}

	bool ObjectNormalMapUpdater::GenerateMips(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& resourceData, ID3D11Texture2D* texInOut)
	{
		if (!device || !context || !texInOut)
			return false;

		Shader::ShaderLocker sl(context);

		HRESULT hr;
		const std::string _func_ = __func__;

		logger::info("{}::{} : Generate Mips...", _func_, resourceData->textureName);

		if (Config::GetSingleton().GetGenerateMipsTime())
			PerformanceLog(_func_ + "::" + resourceData->textureName
							  , false, false);

		resourceData->generateMipsData.texture2D = texInOut;

		D3D11_TEXTURE2D_DESC desc;
		resourceData->generateMipsData.texture2D->GetDesc(&desc);

		Shader::MultiMapGuard mmg(context, resourceData->generateMipsData.texture2D.Get(), desc.MipLevels, D3D11_MAP_READ_WRITE);
        if (!mmg.IsValid())
        {
            logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mmg.GetHR());
            return false;
        }

        auto tp = currentProcessingThreads.load();
		const UINT width = desc.Width;
		const UINT height = desc.Height; 

		//mipLevel 0
		const DirectX::XMINT2 offsets[8] = {
			DirectX::XMINT2(-1, -1), // left up
			DirectX::XMINT2(0, -1), // up
			DirectX::XMINT2(1, -1), // right up
			DirectX::XMINT2(-1,  0), // left
			DirectX::XMINT2(1,  0), // right
			DirectX::XMINT2(-1,  1), // left down
			DirectX::XMINT2(0,  1), // down
			DirectX::XMINT2(1,  1)  // right down
        };
        struct ColorMap
        {
            std::uint32_t* src = nullptr;
            RGBA resultsColor;
        };
		for (std::uint8_t i = 0; i < 2; i++)
        {
            struct alignas(64) ThreadLocalBuffer
            {
                std::vector<ColorMap> data;
            };
            std::vector<ThreadLocalBuffer> colorMapProcesses(tp->GetThreadSize());
            std::uint8_t* pData = mmg.Get<std::uint8_t>(0);
            const UINT rowPitch = mmg.GetRowPitch(0);
            tp->Execute([&] {
                tbb::parallel_for(
                    tbb::blocked_range<UINT>(0, height),
                    [&](const tbb::blocked_range<UINT>& r) {
                        const std::int32_t ti = tbb::this_task_arena::current_thread_index();
                        for (UINT y = r.begin(); y != r.end(); ++y)
                        {
                            std::uint8_t* rowData = pData + y * rowPitch;
                            for (UINT x = 0; x < width; x++)
                            {
                                std::uint32_t* pixel = reinterpret_cast<uint32_t*>(rowData + x * 4);
                                RGBA color;
                                color.SetReverse(*pixel);
                                if (color.A() == 0xFF)
                                    continue;

                                RGBA averageColor(0.0f, 0.0f, 0.0f, 0.0f);
                                std::uint8_t validCount = 0;
                                for (std::uint8_t i = 0; i < 8; i++)
                                {
                                    DirectX::XMINT2 nearCoord = {(INT)x + offsets[i].x, (INT)y + offsets[i].y};
                                    if (nearCoord.x < 0 || nearCoord.y < 0 ||
                                        nearCoord.x >= width || nearCoord.y >= height)
                                        continue;

                                    std::uint8_t* nearRowData = pData + nearCoord.y * rowPitch;
                                    std::uint32_t* nearPixel = reinterpret_cast<uint32_t*>(nearRowData + nearCoord.x * 4);
                                    RGBA nearColor;
                                    nearColor.SetReverse(*nearPixel);
                                    if (nearColor.A() == 0xFF)
                                    {
                                        averageColor += nearColor;
                                        validCount++;
                                    }
                                }
                                if (validCount == 0)
                                    continue;
                                RGBA resultsColor = averageColor / validCount;
                                colorMapProcesses[ti].data.push_back(ColorMap{pixel, resultsColor});
                            }
                        }
                    },
                    tbb::auto_partitioner()
				);
            });
			std::vector<ColorMap> resultsColorMap;
            for (auto& process : colorMapProcesses)
			{
                resultsColorMap.append_range(process.data);
			}

			tp->Execute([&] {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, resultsColorMap.size()),
                    [&](const tbb::blocked_range<std::size_t>& r) {
                        const std::int32_t ti = tbb::this_task_arena::current_thread_index();
                        for (std::size_t i = r.begin(); i != r.end(); ++i)
                        {
                            *resultsColorMap[i].src = resultsColorMap[i].resultsColor.GetReverse();
                        }
                    },
                    tbb::auto_partitioner()
				);
            });
		}

		const DirectX::XMUINT2 sampleOffsets[4] = {
			DirectX::XMUINT2(0u, 0u), // left up
			DirectX::XMUINT2(1u, 0u), // right up
			DirectX::XMUINT2(0u, 1u), // left down
			DirectX::XMUINT2(1u, 1u)  // right down
		};
		for (UINT mipLevel = 1; mipLevel < desc.MipLevels; mipLevel++)
		{
			const UINT mipWidth = std::max(width >> mipLevel, 1u);
			const UINT mipHeight = std::max(height >> mipLevel, 1u);

			const UINT srcMipLevel = mipLevel - 1;
			const UINT srcMipWidth = std::max(width >> srcMipLevel, 1u);
			const UINT srcMipHeight = std::max(height >> srcMipLevel, 1u);

			std::uint8_t* dst_pData = mmg.Get<std::uint8_t>(mipLevel);
            std::uint8_t* src_pData = mmg.Get<std::uint8_t>(srcMipLevel);
            const UINT dstRowPitch = mmg.GetRowPitch(mipLevel);
            const UINT srcRowPitch = mmg.GetRowPitch(srcMipLevel);

			tp->Execute([&] {
                tbb::parallel_for(
                    tbb::blocked_range<UINT>(0, mipHeight),
                    [&](const tbb::blocked_range<UINT>& r) {
                        for (UINT y = r.begin(); y != r.end(); ++y)
                        {
                            std::uint8_t* dstRowData = dst_pData + y * dstRowPitch;
                            for (UINT x = 0; x < mipWidth; x++)
                            {
                                std::uint32_t* pixel = reinterpret_cast<uint32_t*>(dstRowData + x * 4);
                                RGBA averageColor(0.0f, 0.0f, 0.0f, 0.0f);
                                std::uint8_t validCount = 0;
#pragma unroll(4)
                                for (std::uint8_t i = 0; i < 4; i++)
                                {
                                    DirectX::XMUINT2 srcCoord = {x * 2 + sampleOffsets[i].x, y * 2 + sampleOffsets[i].y};
                                    if (srcCoord.x >= srcMipWidth || srcCoord.y >= srcMipHeight)
                                        continue;
                                    std::uint8_t* srcRowData = src_pData + srcCoord.y * srcRowPitch;
                                    std::uint32_t* srcPixel = reinterpret_cast<uint32_t*>(srcRowData + srcCoord.x * 4);
                                    RGBA srcColor;
                                    srcColor.SetReverse(*srcPixel);
                                    if (srcColor.A() == 0xFF)
                                    {
                                        averageColor += srcColor;
                                        validCount++;
                                    }
                                }
                                if (validCount == 0)
                                {
#pragma unroll(8)
                                    for (std::uint8_t i = 0; i < 8; i++)
                                    {
                                        DirectX::XMINT2 nearCoord = {(INT)x + offsets[i].x, (INT)y + offsets[i].y};
                                        if (nearCoord.x < 0 || nearCoord.y < 0 ||
                                            nearCoord.x >= mipWidth || nearCoord.y >= mipHeight)
                                            continue;
#pragma unroll(4)
                                        for (std::uint8_t j = 0; j < 4; j++)
                                        {
                                            DirectX::XMUINT2 srcCoord = {nearCoord.x * 2 + sampleOffsets[j].x, nearCoord.y * 2 + sampleOffsets[j].y};
                                            if (srcCoord.x >= srcMipWidth || srcCoord.y >= srcMipHeight)
                                                continue;
                                            std::uint8_t* srcRowData = src_pData + srcCoord.y * srcRowPitch;
                                            std::uint32_t* srcPixel = reinterpret_cast<uint32_t*>(srcRowData + srcCoord.x * 4);
                                            RGBA srcColor;
                                            srcColor.SetReverse(*srcPixel);
                                            if (srcColor.A() == 0xFF)
                                            {
                                                averageColor += srcColor;
                                                validCount++;
                                            }
                                        }
                                    }
                                }
                                if (validCount == 0)
                                {
                                    *pixel = emptyColor.GetReverse();
                                    continue;
                                }
                                RGBA resultsColor = averageColor / validCount;
                                resultsColor.a = 1.0f;
                                *pixel = resultsColor.GetReverse();
                            }
                        }
                    },
                    tbb::auto_partitioner()
				);
            });
		}
		logger::debug("{}::{} : Generate Mips done", _func_, resourceData->textureName);

		if (Config::GetSingleton().GetGenerateMipsTime())
			PerformanceLog(_func_ + "::" + resourceData->textureName
							  , true, false);
		return true;
	}
	bool ObjectNormalMapUpdater::GenerateMipsGPU(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& resourceData, ID3D11ShaderResourceView* srvInOut, ID3D11Texture2D* texInOut)
	{
		if (!device || !context || !srvInOut || !texInOut)
			return false;

		Shader::ShaderLocker sl(context);

		HRESULT hr;
		const std::string _func_ = __func__;

		logger::info("{}::{} : Generate Mips...", _func_, resourceData->textureName);

		D3D11_TEXTURE2D_DESC desc;
		texInOut->GetDesc(&desc);
		const UINT width = desc.Width;
		const UINT height = desc.Height;

        resourceData->generateMipsData.srvs.reserve(desc.MipLevels);
        resourceData->generateMipsData.uavs.reserve(desc.MipLevels);

		for (UINT mipLevel = 0; mipLevel < desc.MipLevels; mipLevel++)
		{
			const UINT mipWidth = std::max(width >> mipLevel, 1u);
			const UINT mipHeight = std::max(height >> mipLevel, 1u);

			if (mipLevel == 0)
			{
				hr = device->CreateTexture2D(&desc, nullptr, &resourceData->generateMipsData.texture2D);
				if (FAILED(hr)) {
					logger::error("{} : Failed to create texture 2d ({})", __func__, hr);
					return false;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
				srvInOut->GetDesc(&srvDesc);
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;
                resourceData->generateMipsData.srvs.push_back(nullptr);
                hr = device->CreateShaderResourceView(resourceData->generateMipsData.texture2D.Get(), &srvDesc, &resourceData->generateMipsData.srvs[0]);
				if (FAILED(hr)) {
					logger::error("{} : Failed to create shader resource view ({})", __func__, hr);
					return false;
				}

				Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex = resourceData->generateMipsData.texture2D;
				Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSRV = srvInOut;

				GenerateMipsBufferData cbData = {};
				cbData.width = mipWidth;
				cbData.height = mipHeight;
				cbData.widthStart = 0;
				cbData.heightStart = 0;
				cbData.mipLevel = 0;
				cbData.srcWidth = mipWidth;
				cbData.srcHeight = mipHeight;

				for (std::uint8_t i = 0; i < 2; i++)
				{
					if (i > 0)
					{
						if (Config::GetSingleton().GetGPUForceSync())
							WaitForGPU(device, context).Wait();
						srcTex = texInOut;
						srcSRV = resourceData->generateMipsData.srvs[0];
					}

					Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> dstUAV = nullptr;
					D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
					uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					uavDesc.Texture2D.MipSlice = 0;
					hr = device->CreateUnorderedAccessView(srcTex.Get(), &uavDesc, &dstUAV);
					if (FAILED(hr)) {
						logger::error("{} : Failed to create unordered access view ({})", _func_, hr);
						return false;
					}
					resourceData->generateMipsData.uavs.push_back(dstUAV);

					if (sl.IsSecondGPU() || isNoSplitGPU || (256 >= mipWidth && 256 >= mipHeight))
					{
						const DirectX::XMUINT2 dispatch = { (mipWidth + 8 - 1) / 8, (mipHeight + 8 - 1) / 8 };

						std::optional<GPUTimeScope> timing;
						if (Config::GetSingleton().GetGenerateMipsTime())
						    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(mipWidth) + "|" + std::to_string(mipHeight) );

						{
                            GenerateMipsBackup sb;
                            Shader::ShaderLockGuard slg(sl);
                            ShaderBackupGuard sbg(context, sb);
                            {
                                Shader::MapGuard mg(context, generateMipsBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                                if (!mg.IsValid())
                                {
                                    logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                                    return false;
                                }
                                *mg.Get<GenerateMipsBufferData>() = cbData;
                            }
                            context->CSSetShader(bleedTexture[isSecondGPUEnabled].Get(), nullptr, 0);
                            context->CSSetConstantBuffers(0, 1, generateMipsBuffer[isSecondGPUEnabled].GetAddressOf());
                            context->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
                            context->CSSetUnorderedAccessViews(0, 1, dstUAV.GetAddressOf(), nullptr);
                            context->Dispatch(dispatch.x, dispatch.y, 1);
                        }

						if (timing) timing->Finish();
					}
					else
					{
						const std::uint32_t subResolution = std::max(1u, 2048u / (1u << divideTaskQ));
						const std::uint32_t subY = std::min(mipHeight,
														  std::max(1u, mipWidth / subResolution)
														  * std::max(1u, mipHeight / subResolution));
						const std::uint32_t unitY = (mipHeight + subY - 1) / subY;
						const DirectX::XMUINT2 dispatch = { (mipWidth + 8 - 1) / 8, (std::min(mipHeight, unitY) + 8 - 1) / 8 };

						std::atomic_bool batchSucceeded{true};
						FutureBatch gpuTasks;
						for (std::uint32_t sy = 0; sy < subY; sy++)
						{
							auto cbData_ = cbData;
							cbData_.widthStart = 0;
							cbData_.heightStart = unitY * sy;
							
							gpuTasks.Submit(*gpuTask, [&, cbData_, sy, mipWidth, mipHeight, dispatch, srcSRV, dstUAV]() {
								std::optional<GPUTimeScope> timing;
								if (Config::GetSingleton().GetGenerateMipsTime())
								    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(mipLevel) + "::" + std::to_string(mipWidth) + "|" + std::to_string(mipHeight) + "::" + std::to_string(sy) );

								{
                                    GenerateMipsBackup sb;
                                    Shader::ShaderLockGuard slg(sl);
                                    ShaderBackupGuard sbg(context, sb);
                                    {
                                        Shader::MapGuard mg(context, generateMipsBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                                        if (!mg.IsValid())
                                        {
                                            logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                                            batchSucceeded.store(false, std::memory_order_relaxed);
                                            return;
                                        }
                                        *mg.Get<GenerateMipsBufferData>() = cbData_;
                                    }
                                    context->CSSetShader(bleedTexture[isSecondGPUEnabled].Get(), nullptr, 0);
                                    context->CSSetConstantBuffers(0, 1, generateMipsBuffer[isSecondGPUEnabled].GetAddressOf());
                                    context->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
                                    context->CSSetUnorderedAccessViews(0, 1, dstUAV.GetAddressOf(), nullptr);
                                    context->Dispatch(dispatch.x, dispatch.y, 1);
                                }

								if (timing) timing->Finish();
							});
						}
						gpuTasks.Join();
						if (!batchSucceeded.load(std::memory_order_relaxed))
						    return false;
					}
				}
			}
			else
			{
				Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSRV = nullptr;
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = mipLevel - 1;
				srvDesc.Texture2D.MipLevels = 1;
				hr = device->CreateShaderResourceView(texInOut, &srvDesc, &srcSRV);
				if (FAILED(hr)) {
					logger::error("{} : Failed to create shader resource view ({})", _func_, hr);
					return false;
				}
				resourceData->generateMipsData.srvs.push_back(srcSRV);

				Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> dstUAV = nullptr;
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = mipLevel;
				hr = device->CreateUnorderedAccessView(texInOut, &uavDesc, &dstUAV);
				if (FAILED(hr)) {
					logger::error("{} : Failed to create unordered access view ({})", _func_, hr);
					return false;
				}
				resourceData->generateMipsData.uavs.push_back(dstUAV);

				GenerateMipsBufferData cbData = {};
				cbData.width = mipWidth;
				cbData.height = mipHeight;
				cbData.widthStart = 0;
				cbData.heightStart = 0;
				cbData.mipLevel = mipLevel;
				cbData.srcWidth = mipLevel > 0 ? std::max(width >> (mipLevel - 1u), 1u) : mipWidth;
				cbData.srcHeight = mipLevel > 0 ? std::max(height >> (mipLevel - 1u), 1u) : mipHeight;

				if (sl.IsSecondGPU() || isNoSplitGPU || (256 >= mipWidth && 256 >= mipHeight))
				{
					const DirectX::XMUINT2 dispatch = { (mipWidth + 8 - 1) / 8, (mipHeight + 8 - 1) / 8 };

					std::optional<GPUTimeScope> timing;
					if (Config::GetSingleton().GetGenerateMipsTime())
					    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(mipWidth) + "|" + std::to_string(mipHeight) );

					{
                        GenerateMipsBackup sb;
                        Shader::ShaderLockGuard slg(sl);
                        ShaderBackupGuard sbg(context, sb);
                        {
                            Shader::MapGuard mg(context, generateMipsBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                            if (!mg.IsValid())
                            {
                                logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                                return false;
                            }
                            *mg.Get<GenerateMipsBufferData>() = cbData;
                        }
                        context->CSSetShader(generateMips[isSecondGPUEnabled].Get(), nullptr, 0);
                        context->CSSetConstantBuffers(0, 1, generateMipsBuffer[isSecondGPUEnabled].GetAddressOf());
                        context->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
                        context->CSSetUnorderedAccessViews(0, 1, dstUAV.GetAddressOf(), nullptr);
                        context->Dispatch(dispatch.x, dispatch.y, 1);
                    }

					if (timing) timing->Finish();
				}
				else
				{
					const std::uint32_t subResolution = std::max(1u, 2048u / (1u << divideTaskQ));
					const std::uint32_t subY = std::min(mipHeight,
														std::max(1u, mipWidth / subResolution)
														* std::max(1u, mipHeight / subResolution));
					const std::uint32_t unitY = (mipHeight + subY - 1) / subY;
					const DirectX::XMUINT2 dispatch = { (mipWidth + 8 - 1) / 8, (std::min(mipHeight, unitY) + 8 - 1) / 8 };

					std::atomic_bool batchSucceeded{true};
					FutureBatch gpuTasks;
					for (std::uint32_t sy = 0; sy < subY; sy++)
					{
						auto cbData_ = cbData;
						cbData_.widthStart = 0;
						cbData_.heightStart = unitY * sy;

						gpuTasks.Submit(*gpuTask, [&, cbData_, sy, mipWidth, mipHeight, dispatch, srcSRV, dstUAV]() {
							std::optional<GPUTimeScope> timing;
							if (Config::GetSingleton().GetGenerateMipsTime())
							    timing.emplace(device, context, _func_ + "::" + resourceData->textureName + "::" + std::to_string(mipLevel) + "::" + std::to_string(mipWidth) + "|" + std::to_string(mipHeight) + "::" + std::to_string(sy) );

							{
                                GenerateMipsBackup sb;
                                Shader::ShaderLockGuard slg(sl);
                                ShaderBackupGuard sbg(context, sb);
                                {
                                    Shader::MapGuard mg(context, generateMipsBuffer[isSecondGPUEnabled].Get(), 0, D3D11_MAP_WRITE_DISCARD, false);
                                    if (!mg.IsValid())
                                    {
                                        logger::error("{}::{} : Failed to map buffer ({})", _func_, resourceData->textureName, mg.GetHR());
                                        batchSucceeded.store(false, std::memory_order_relaxed);
                                        return;
                                    }
                                    *mg.Get<GenerateMipsBufferData>() = cbData_;
                                }
                                context->CSSetShader(generateMips[isSecondGPUEnabled].Get(), nullptr, 0);
                                context->CSSetConstantBuffers(0, 1, generateMipsBuffer[isSecondGPUEnabled].GetAddressOf());
                                context->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
                                context->CSSetUnorderedAccessViews(0, 1, dstUAV.GetAddressOf(), nullptr);
                                context->Dispatch(dispatch.x, dispatch.y, 1);
                            }

							if (timing) timing->Finish();
						});
					}
					gpuTasks.Join();
					if (!batchSucceeded.load(std::memory_order_relaxed))
					    return false;
				}
			}
		}
		logger::debug("{}::{} : Generate Mips done", _func_, resourceData->textureName);
		return true;
	}

	bool ObjectNormalMapUpdater::CompressTexture(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& resourceData, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texInOut, const TextureCompressionPlan& compression, Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingScratch)
	{
		if (!device || !context || !texInOut)
			return false;
        if (compression.backend == TextureCompressionBackend::None) return true;

		const bool isSecondGPU = Shader::ShaderManager::GetSingleton().IsSecondGPUResource(context);
        const bool isGPUCompress = compression.backend == TextureCompressionBackend::GPU;

		logger::info("{}::{} : Compress texture with {}...", __func__, resourceData->textureName, isGPUCompress ? "GPU" : "CPU");

		bool isCompressed = false;
		if (isGPUCompress)
		{
			D3D11_TEXTURE2D_DESC desc;
			texInOut->GetDesc(&desc);
			if (desc.Usage == D3D11_USAGE_DEFAULT && Config::GetSingleton().GetGPUForceSync())
				WaitForGPU(device, context).Wait();

			if (Config::GetSingleton().GetCompressTime())
				PerformanceLog(std::string(__func__) + "::" + resourceData->textureName, false, false);

			std::uint8_t quality = 0;
			if (compression.quality < 3)
			{
				quality = 0;
			}
			else if (compression.quality < 6)
			{
				quality = 1;
			}
			else
			{
				quality = 2;
			}
			isCompressed = Shader::TextureLoadManager::GetSingleton().CompressTexture(device, context, DXGI_FORMAT_BC7_UNORM, isSecondGPU, quality, texInOut);

			if (Config::GetSingleton().GetCompressTime())
				PerformanceLog(std::string(__func__) + "::" + resourceData->textureName, true, false);
		}
		else
		{
			isCompressed = CompressTextureBC7(device, context, resourceData, texInOut, compression.quality, stagingScratch);
		}

		if (!isCompressed || !texInOut)
			return false;

		logger::debug("{}::{} : Compress texture done", __func__, resourceData->textureName);
		return isCompressed;
	}
	bool ObjectNormalMapUpdater::CompressTextureBC7(ID3D11Device* device, ID3D11DeviceContext* context, TextureResourceDataPtr& resourceData, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texInOut, int quality, Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingScratch)
	{
		if (!device || !context || !texInOut)
			return false;

		if (GetSIMDType() == SIMDType::noSIMD)
			return false;

		Shader::ShaderLocker sl(context);

		if (Config::GetSingleton().GetCompressTime())
			PerformanceLog(std::string(__func__) + "::" + resourceData->textureName, false, false);

		HRESULT hr;

		D3D11_TEXTURE2D_DESC srcDesc;
		texInOut->GetDesc(&srcDesc); 
		D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;

		if (srcDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
		if (srcDesc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)
		{
			stagingTexture = texInOut;
		}
		else
		{
            if (Config::GetSingleton().GetGPUForceSync())
				WaitForGPU(device, context).Wait();

			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.ArraySize = 1;
			stagingDesc.BindFlags = 0;
			stagingDesc.MiscFlags = 0;
			stagingDesc.SampleDesc.Count = 1;
            D3D11_TEXTURE2D_DESC cached{};
            if (stagingScratch) stagingScratch->GetDesc(&cached);
            if (!stagingScratch || cached.Width != stagingDesc.Width || cached.Height != stagingDesc.Height ||
                cached.MipLevels != stagingDesc.MipLevels || cached.Format != stagingDesc.Format) {
                hr = device->CreateTexture2D(&stagingDesc, nullptr, stagingScratch.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    logger::error("{}::{} : Failed to create staging texture ({})", __func__, resourceData->textureName, hr);
                    return false;
                }
            }
            stagingTexture = stagingScratch;
			if (!CopySubresourceRegion(device, context, stagingTexture.Get(), texInOut.Get()))
                return false;
		}

		std::vector<D3D11_SUBRESOURCE_DATA> initData(srcDesc.MipLevels);
		std::vector<std::vector<std::uint8_t>> bc7Buffers(srcDesc.MipLevels);
		std::vector<UINT> rowPitches(srcDesc.MipLevels);

		ispc::bc7e_sse2_compress_block_params params;
		
		switch (quality) {
		case 0:
			ispc::bc7e_sse2_compress_block_params_init_ultrafast(&params, false);
            break;
        default:
		case 1:
			ispc::bc7e_sse2_compress_block_params_init_veryfast(&params, false);
			break;
		case 2:
			ispc::bc7e_sse2_compress_block_params_init_fast(&params, false);
			break;
		case 3:
			ispc::bc7e_sse2_compress_block_params_init_basic(&params, false);
			break;
		case 4:
			ispc::bc7e_sse2_compress_block_params_init(&params, false);
			break;
		case 5:
			ispc::bc7e_sse2_compress_block_params_init_slow(&params, false);
			break;
		case 6:
			ispc::bc7e_sse2_compress_block_params_init_veryslow(&params, false);
			break;
		case 7:
			ispc::bc7e_sse2_compress_block_params_init_slowest(&params, false);
			break;
		}

		using CompFunc = void(*)(std::uint32_t, std::uint64_t*, const std::uint32_t*, void*);
        CompFunc compFunc = nullptr;
        switch (GetSIMDType())
        {
        case SIMDType::avx2:
            compFunc = [](uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA, void* pComp_params) {
                ispc::bc7e_avx2_compress_blocks(num_blocks, pBlocks, pPixelsRGBA, reinterpret_cast<ispc::bc7e_avx2_compress_block_params*>(pComp_params));
            };
            break;
        case SIMDType::avx:
            compFunc = [](uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA, void* pComp_params) {
                ispc::bc7e_avx_compress_blocks(num_blocks, pBlocks, pPixelsRGBA, reinterpret_cast<ispc::bc7e_avx_compress_block_params*>(pComp_params));
            };
            break;
        case SIMDType::sse4:
            compFunc = [](uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA, void* pComp_params) {
                ispc::bc7e_sse4_compress_blocks(num_blocks, pBlocks, pPixelsRGBA, reinterpret_cast<ispc::bc7e_sse4_compress_block_params*>(pComp_params));
            };
            break;
        case SIMDType::sse2:
            compFunc = [](uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA, void* pComp_params) {
                ispc::bc7e_sse2_compress_blocks(num_blocks, pBlocks, pPixelsRGBA, reinterpret_cast<ispc::bc7e_sse2_compress_block_params*>(pComp_params));
            };
            break;
        default:
            return false;
        }

		auto tp = currentProcessingThreads.load();
		for (UINT mipLevel = 0; mipLevel < srcDesc.MipLevels; mipLevel++)
        {
            Shader::MapGuard mg(context, stagingTexture.Get(), mipLevel, D3D11_MAP_READ);
            if (!mg.IsValid())
            {
                logger::error("{}::{} : Failed to src map ({})", __func__, resourceData->textureName, mg.GetHR());
                return false;
            }
			const UINT mipWidth = std::max(1u, srcDesc.Width >> mipLevel);
			const UINT mipHeight = std::max(1u, srcDesc.Height >> mipLevel);
			const UINT bc7Width = (mipWidth + 4 - 1) / 4;
			const UINT bc7Height = (mipHeight + 4 - 1) / 4;
			bc7Buffers[mipLevel].resize(static_cast<const std::size_t>(bc7Width) * bc7Height * 16);
            rowPitches[mipLevel] = bc7Width * 16;

            std::uint8_t* srcData = mg.Get<std::uint8_t>();
            const UINT rowPitch = mg.GetRowPitch();

            tp->Execute([&] {
                tbb::parallel_for(
                    tbb::blocked_range<UINT>(0, bc7Height),
                    [&](const tbb::blocked_range<UINT>& r) {
                        const std::uint32_t blockCount = (r.end() - r.begin()) * bc7Width;
                        std::vector<std::uint32_t> pixelsLocal(blockCount * 16);
                        std::uint32_t* pixels = pixelsLocal.data();
                        std::size_t pixelIndex = 0;
                        std::uint64_t* dstBlocks = reinterpret_cast<std::uint64_t*>(bc7Buffers[mipLevel].data()) + (r.begin() * bc7Width * 2);
                        for (UINT by = r.begin(); by < r.end(); by++)
                        {
                            const UINT by_ = by * 4;
                            for (UINT bx = 0; bx < bc7Width; bx++)
                            {
                                const UINT bx_ = bx * 4;
                                for (UINT y = 0; y < 4; y++)
                                {
                                    const UINT py = by_ + y;
                                    const UINT clampedPy = std::min(py, mipHeight - 1);
                                    const std::uint8_t* rowData = srcData + (clampedPy * rowPitch);

                                    for (UINT x = 0; x < 4; x++)
                                    {
                                        const UINT px = bx_ + x;
                                        const UINT clampedPx = std::min(px, mipWidth - 1);
                                        pixels[pixelIndex++] = *reinterpret_cast<const std::uint32_t*>(rowData + clampedPx * 4);
                                    }
                                }
                            }
                        }
                        compFunc(blockCount, dstBlocks, pixels, &params);
                    },
                    tbb::auto_partitioner()
                );
            });
			initData[mipLevel].pSysMem = bc7Buffers[mipLevel].data();
			initData[mipLevel].SysMemPitch = rowPitches[mipLevel];
			initData[mipLevel].SysMemSlicePitch = 0;
		};

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
		D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
		if (sl.IsSecondGPU())
		{
			dstDesc.Format = DXGI_FORMAT_BC7_UNORM;
			dstDesc.Usage = D3D11_USAGE_STAGING;
			dstDesc.BindFlags = 0;
			dstDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			dstDesc.MiscFlags = 0;
			hr = device->CreateTexture2D(&dstDesc, initData.data(), &texture2D);
			if (FAILED(hr))
			{
                logger::error("{}::{} : Failed to create dst texture 2d ({})", __func__, resourceData->textureName, hr);
				return false;
			}
		}
		else
		{
			dstDesc.Format = DXGI_FORMAT_BC7_UNORM;
			dstDesc.Usage = D3D11_USAGE_DEFAULT;
			dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			dstDesc.CPUAccessFlags = 0;
			dstDesc.MiscFlags = 0;
			hr = device->CreateTexture2D(&dstDesc, nullptr, &texture2D);
			if (FAILED(hr))
			{
                logger::error("{}::{} : Failed to create dst texture 2d ({})", __func__, resourceData->textureName, hr);
				return false;
			}

			if (!CopySubresourceFromBuffer(device, context, bc7Buffers, rowPitches, texture2D.Get()))
				return false;
		}

		texInOut = texture2D;

		if (Config::GetSingleton().GetCompressTime())
			PerformanceLog(std::string(__func__) + "::" + resourceData->textureName, true, false);
		return true;
	}

	bool ObjectNormalMapUpdater::CopyResourceSecondToMain(TextureResourceDataPtr& resourceData, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texInOut, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srvInOut)
	{
		if (!Shader::ShaderManager::GetSingleton().IsValidSecondGPU() || !texInOut)
			return false;

		logger::info("{}::{} : Copy resource from sub to main GPU...", __func__, resourceData->textureName);

		HRESULT hr;

		D3D11_TEXTURE2D_DESC desc;
		texInOut->GetDesc(&desc);

		if (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)
		{
			resourceData->copySecondToMainData.copyTexture2D = texInOut;
		}
		else
		{
            if (Config::GetSingleton().GetGPUForceSync())
				WaitForGPU(Shader::ShaderManager::GetSingleton().GetSecondDevice(), Shader::ShaderManager::GetSingleton().GetSecondContext()).Wait();

			D3D11_TEXTURE2D_DESC copyDesc = desc;
			copyDesc.Usage = D3D11_USAGE_STAGING;
			copyDesc.BindFlags = 0;
			copyDesc.MiscFlags = 0;
			copyDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			copyDesc.ArraySize = 1;
			copyDesc.SampleDesc.Count = 1;

			hr = Shader::ShaderManager::GetSingleton().GetSecondDevice()->CreateTexture2D(&copyDesc, nullptr, &resourceData->copySecondToMainData.copyTexture2D);
			if (FAILED(hr)) {
                logger::error("{}::{} : Failed to create copy texture : {}", __func__, resourceData->textureName, hr);
				return false;
			}

			Shader::ShaderLocker sl(Shader::ShaderManager::GetSingleton().GetSecondContext());
            Shader::ShaderLockGuard slg(sl);
			Shader::ShaderManager::GetSingleton().GetSecondContext()->CopyResource(resourceData->copySecondToMainData.copyTexture2D.Get(), texInOut.Get());
		}

		std::vector<std::vector<std::uint8_t>> buffers(desc.MipLevels);
		std::vector<UINT> rowPitches(desc.MipLevels);

		for (UINT mipLevel = 0; mipLevel < desc.MipLevels; mipLevel++) {
			const UINT height = std::max(desc.Height >> mipLevel, 1u);
			std::size_t blockHeight = 0;

			Shader::MapGuard mg(Shader::ShaderManager::GetSingleton().GetSecondContext(), resourceData->copySecondToMainData.copyTexture2D.Get(), mipLevel, D3D11_MAP_READ);
            if (!mg.IsValid())
			{
                logger::error("{}::{} : Failed to map copy texture : {}", __func__, resourceData->textureName, mg.GetHR());
				return false;
			}

			rowPitches[mipLevel] = mg.GetRowPitch();

			if (desc.Format == DXGI_FORMAT_BC7_UNORM || desc.Format == DXGI_FORMAT_BC3_UNORM || desc.Format == DXGI_FORMAT_BC1_UNORM)
			{
				blockHeight = std::size_t(height + 3) / 4;
			}
			else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
			{
				blockHeight = height;
			}
			else
			{
				blockHeight = height;
			}

			buffers[mipLevel].resize(rowPitches[mipLevel] * blockHeight);
			for (std::size_t y = 0; y < blockHeight; y++) {
				memcpy(buffers[mipLevel].data() + y * rowPitches[mipLevel], mg.Get<std::uint8_t>() + y * rowPitches[mipLevel], rowPitches[mipLevel]);
			}
		}

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		hr = Shader::ShaderManager::GetSingleton().GetDevice()->CreateTexture2D(&desc, nullptr, &texture2D);
		if (FAILED(hr)) {
            logger::error("{}::{} : Failed to create dst texture : {}", __func__, resourceData->textureName, hr);
			return false;
		}

		if (!CopySubresourceFromBuffer(Shader::ShaderManager::GetSingleton().GetDevice(), Shader::ShaderManager::GetSingleton().GetContext(), buffers, rowPitches, texture2D.Get()))
			return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		hr = Shader::ShaderManager::GetSingleton().GetDevice()->CreateShaderResourceView(texture2D.Get(), &srvDesc, &srv);
		if (FAILED(hr)) {
			logger::error("Failed to create dst shader resource view : {}", hr);
			return false;
		}

		texInOut = texture2D;
		srvInOut = srv;
		logger::debug("{}::{} : Copy resource from second to main GPU done", __func__, resourceData->textureName);
		return true;
	}

	WaitForGPU::WaitForGPU(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
		: device(a_device), context(a_context), sl(a_context)
	{
		if (!device || !context)
			return;

		D3D11_QUERY_DESC queryDesc = {};
		queryDesc.Query = D3D11_QUERY_EVENT;
		queryDesc.MiscFlags = 0;

		HRESULT hr = device->CreateQuery(&queryDesc, &query);
		if (FAILED(hr)) {
			return;
		}
		
        {
            Shader::ShaderLockGuard slg(sl);
            context->Flush();
            context->End(query.Get());
        }
		logger::trace("Wait for Renderer...");
	}

	void WaitForGPU::Wait()
	{
		if (!device || !context || !query)
			return;
		HRESULT hr;
		while (true) {
            {
                Shader::ShaderLockGuard slg(sl);
                hr = context->GetData(query.Get(), nullptr, 0, 0);
            }
			if (FAILED(hr))
				break;
			if (hr == S_OK)
				break;
            if (!isImmediately)
                std::this_thread::sleep_for(waitSleepTime);
            else
                std::this_thread::yield();
		}
		logger::trace("Wait for Renderer done");
		return;
	}
}
