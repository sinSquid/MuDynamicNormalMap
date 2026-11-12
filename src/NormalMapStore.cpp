#include "NormalMapStore.h"
#include "lz4.h"

namespace Mus {
    NormalMapStore::NormalMapStore()
        : diskWriter([this](std::uint64_t hash, TextureResourcePtr resource) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            try {
                CreateDiskCache(hash, std::move(resource));
                CheckDiskCacheCapacity();
            } catch (const std::exception& e) {
                logger::error("Optional disk cache write failed {:x}: {}", hash, e.what());
            }
        }) {
        // Persistent files must be counted even before the first cache lookup/write.
        try { Init(); }
        catch (const std::exception& e) { logger::error("Unable to index optional disk cache: {}", e.what()); }
    }

    NormalMapStore::~NormalMapStore() {
        stoppingMemoryCleanup = true;
        WaitForMemoryCleanup();
        ShutdownDiskWrites();
        ClearDiskCache();
    }

    void NormalMapStore::WaitForMemoryCleanup() noexcept {
        if (!memoryCleanup.valid()) return;
        try { memoryCleanup.get(); }
        catch (const std::exception& e) { logger::error("Memory cache cleanup failed: {}", e.what()); }
        catch (...) { logger::error("Memory cache cleanup failed"); }
    }

    void NormalMapStore::ShutdownDiskWrites() {
        std::lock_guard lifecycle(diskLifecycleLock);
        // Do not hold diskIOLock while joining a writer that may need it.
        diskWriter.Stop();
    }

    std::uint64_t NormalMapStore::EstimateDiskWriteBytes(const TextureResourcePtr& resource) {
        constexpr std::uint64_t overBudget = 128ull * 1024 * 1024 + 1;
        if (!resource || !resource->normalmapTexture2D) return overBudget;
        D3D11_TEXTURE2D_DESC desc{};
        resource->normalmapTexture2D->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0 || desc.Width > 16384 || desc.Height > 16384 ||
            desc.ArraySize != 1 || desc.MipLevels == 0 || desc.MipLevels > 15)
            return overBudget;
        std::uint64_t bytes = 0;
        for (UINT mip = 0; mip < desc.MipLevels; ++mip)
            bytes += static_cast<std::uint64_t>(std::max(desc.Width >> mip, 1u)) *
                std::max(desc.Height >> mip, 1u) * 4;
        // Conservatively charge compressed textures as RGBA too.
        return bytes;
    }

	void NormalMapStore::onEvent(const FrameEvent& e)
	{
		if (stoppingMemoryCleanup)
			return;
		if (memoryCleanup.valid() && memoryCleanup.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;
		if (lastTickTime + 100 > currentTime) //0.1sec
			return;
		lastTickTime = currentTime;
		WaitForMemoryCleanup();
		memoryCleanup = backGroundWorkerThreads->submitAsync([this] {
			ClearMemory();
		});
	}
	void NormalMapStore::onEvent(const QuitGameEvent& e)
	{
        stoppingMemoryCleanup = true;
        WaitForMemoryCleanup();
        ShutdownDiskWrites();
		ClearDiskCache();
	}

	void NormalMapStore::Init()
	{
        std::lock_guard lifecycle(diskLifecycleLock);
        diskWriter.PauseAndDiscard();
        struct ResumeWriter { DiskWriteQueue<TextureResourcePtr>& writer; ~ResumeWriter() { writer.Resume(); } } resume{diskWriter};
        std::lock_guard ioLock(diskIOLock);
		baseFolder = "";
        {
            std::lock_guard metadata(diskCacheLock);
            diskCacheInfoMap.clear();
        }
        const std::filesystem::path folder = GetCacheFileFolder();
        SetDiskCacheZeroSize();
        std::error_code ec;
        std::filesystem::directory_iterator it(folder, ec), end;
        while (!ec && it != end) {
            if (it->path().extension() == diskCacheExtension && it->is_regular_file(ec))
                AddDiskCacheSize(GetFileSize(it->path()));
            it.increment(ec);
        }
    }
	void NormalMapStore::ClearMemory()
    {
        std::lock_guard lg(lock);
        const std::size_t orgMapSize = map.size();
		for (auto it = map.begin(); it != map.end();)
		{
            if (!it->second->normalmapTexture2D || !it->second->normalmapShaderResourceView)
                it = map.erase(it);
            else if (GetRefCount(it->second->normalmapTexture2D.Get()) <= 1 || GetRefCount(it->second->normalmapShaderResourceView.Get()) <= 1)
                it = map.erase(it);
            else
				it++;
		}

		if (const auto garbageCount = orgMapSize - map.size(); garbageCount > 0)
            logger::debug("Removed {} RAM cache / Current remain {} RAM cache", garbageCount, map.size());
	}

	void NormalMapStore::AddResource(std::uint64_t a_hash, TextureResourcePtr a_resource)
    {
        if (!a_resource)
            return;
        {
            std::lock_guard lg(lock);
            map[a_hash] = a_resource;
        }
		if (Config::GetSingleton().GetDiskCache())
		{
            try {
                // Aliased results may replace fields in their shared wrapper later.
                // Pin only the final sampled COM resources, never that mutable wrapper.
                auto snapshot = std::make_shared<TextureResource>();
                snapshot->normalmapTexture2D = a_resource->normalmapTexture2D;
                snapshot->normalmapShaderResourceView = a_resource->normalmapShaderResourceView;
                const auto bytes = EstimateDiskWriteBytes(snapshot);
                diskWriter.Submit(a_hash, std::move(snapshot), bytes);
            } catch (const std::exception& e) {
                // Disk cache is optional: the valid RAM/GPU result must still apply.
                logger::error("Disk cache enqueue failed {:x}: {}", a_hash, e.what());
            }
		}
	}
	bool NormalMapStore::GetResource(std::uint64_t a_hash, TextureResourcePtr& a_resource, bool& isDiskCache)
	{
		TextureResourcePtr resource = nullptr;
        isDiskCache = false;
        {
            std::lock_guard lg(lock);
            auto found = map.find(a_hash);
            if (found != map.end())
                resource = found->second;
        }
		if (resource)
			logger::debug("Found exist resource {:x}", a_hash);
		else 
		{
            try {
                resource = GetDiskCache(a_hash);
            } catch (const std::exception& e) {
                logger::error("Disk cache read failed {:x}: {}", a_hash, e.what());
            }
            if (resource) {
                isDiskCache = true;
                std::lock_guard lg(lock);
				map[a_hash] = resource;
				logger::debug("Found disk cache resource {:x}", a_hash);
			}
		}
		if (resource)
			UpdateAccessTime(a_hash);
		a_resource = resource;
		return a_resource ? true : false;
	}

	void NormalMapStore::AddHashPair(std::uint64_t a_hash, std::uint64_t b_hash)
	{
        std::lock_guard lg(hashPairsLock);
		auto found = std::find_if(hashPairs.begin(), hashPairs.end(), [&](HashPair& pair) {
			return pair.find(a_hash) != pair.end() || pair.find(b_hash) != pair.end();
		});
		if (found != hashPairs.end())
		{
			found->insert(a_hash);
			found->insert(b_hash);
			logger::debug("Add hash pair {:x} - {:x}", a_hash, b_hash);
		}
		else
		{
			HashPair newPair;
			newPair.insert(a_hash);
			newPair.insert(b_hash);
			hashPairs.push_back(newPair);
			logger::debug("Create and add hash pair {:x} - {:x}", a_hash, b_hash);
		}
	}
	bool NormalMapStore::IsPairHashes(std::uint64_t a_hash, std::uint64_t b_hash)
    {
        std::lock_guard lg(hashPairsLock);
		bool isPair = std::find_if(hashPairs.cbegin(), hashPairs.cend(), [&](const HashPair& pair) {
			return pair.find(a_hash) != pair.cend() && pair.find(b_hash) != pair.cend();
		}) != hashPairs.cend();
		logger::debug("Is pair hashes : {:x} - {:x} => {}", a_hash, b_hash, isPair ? "O" : "X");
		return isPair;
	}
	void NormalMapStore::InitHashPair(std::uint64_t a_hash)
    {
        std::lock_guard lg(hashPairsLock);
		auto found = std::find_if(hashPairs.begin(), hashPairs.end(), [&](HashPair& pair) {
			return pair.find(a_hash) != pair.end();
		});
		if (found != hashPairs.end()) {
			found->clear();
			found->insert(a_hash);
		}
	}

	void NormalMapStore::CreateDiskCache(std::uint64_t a_hash, TextureResourcePtr a_resource)
	{
        std::lock_guard ioLock(diskIOLock);
		if (!a_resource || !a_resource->normalmapTexture2D || !a_resource->normalmapShaderResourceView)
			return;

		auto device = Shader::ShaderManager::GetSingleton().GetDevice();
		auto context = Shader::ShaderManager::GetSingleton().GetContext();

		DiskCacheInfo info;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> cacheTexture;
		a_resource->normalmapTexture2D->GetDesc(&info.texDesc);
        a_resource->normalmapShaderResourceView->GetDesc(&info.srvDesc);
        // Cache textures are sampled only; do not persist generation UAV flags.
        info.texDesc.Usage = D3D11_USAGE_DEFAULT;
        info.texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        info.texDesc.CPUAccessFlags = 0;
        info.texDesc.MiscFlags = 0;
        D3D11_TEXTURE2D_DESC desc = info.texDesc;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		info.mipLevels = info.texDesc.MipLevels;
        // Row pitches are populated from Map below; shape/format are checked there.
        if (info.mipLevels == 0 || info.mipLevels > 15 || desc.ArraySize != 1 ||
            desc.Width == 0 || desc.Height == 0 || desc.Width > 16384 || desc.Height > 16384 ||
            (desc.Format != DXGI_FORMAT_BC1_UNORM && desc.Format != DXGI_FORMAT_BC3_UNORM &&
             desc.Format != DXGI_FORMAT_BC7_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM))
            return;
		
		auto hr = device->CreateTexture2D(&desc, nullptr, &cacheTexture);
		if (FAILED(hr))
		{
			logger::error("Failed to create disk cache {:x}", a_hash);
			return;
		}

        {
			Shader::ShaderLocker sl(context);
            Shader::ShaderLockGuard slg(sl);
            context->CopyResource(cacheTexture.Get(), a_resource->normalmapTexture2D.Get());
        }

        RemoveDiskCache(a_hash);
        for (std::int32_t mip = -1; mip < 15; ++mip) {
            std::error_code ec;
            if (std::filesystem::exists(GetCacheFileName(a_hash, mip), ec) || ec)
                return;
        }
        struct PendingFiles {
            NormalMapStore* owner;
            std::vector<std::filesystem::path> paths;
            bool committed = false;
            ~PendingFiles() {
                if (!committed) {
                    for (const auto& path : paths) {
                        std::error_code ec;
                        std::filesystem::remove(path, ec);
                        if (ec)
                            owner->AddDiskCacheSize(GetFileSize(path));
                    }
                }
            }
        } pending{this};
        std::uint64_t writtenBytes = 0;
        try {
		for (UINT mipLevel = 0; mipLevel < desc.MipLevels; mipLevel++) {
			const UINT height = std::max(desc.Height >> mipLevel, 1u);
			std::size_t blockHeight = 0;

            std::uint32_t rowPitch = 0;
            std::vector<std::uint8_t> buffer;
            {
            Shader::MapGuard mg(context, cacheTexture.Get(), mipLevel, D3D11_MAP_READ);
			if (!mg.IsValid())
			{
				logger::error("Failed to map copy texture : {}", mg.GetHR());
				return;
			}

			rowPitch = mg.GetRowPitch();

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

            const auto byteSize = static_cast<std::uint64_t>(rowPitch) * blockHeight;
            if (byteSize == 0 || byteSize > 512ull * 1024 * 1024)
                return;
            buffer.resize(static_cast<std::size_t>(byteSize));
			for (std::size_t y = 0; y < blockHeight; y++) {
				memcpy(buffer.data() + y * rowPitch, mg.Get<std::uint8_t>() + y * rowPitch, rowPitch);
			}

            } // Unmap before CPU compression and file I/O.
            int maxSize = LZ4_compressBound(static_cast<int>(buffer.size()));
			std::vector<char> compressed(maxSize);

			std::uint64_t compressedSize = LZ4_compress_default((const char*)buffer.data(), compressed.data(), buffer.size(), maxSize);

            if (compressedSize == 0)
                return;
            std::filesystem::path filePath = GetCacheFileName(a_hash, mipLevel);
            std::filesystem::create_directories(filePath.parent_path());
            pending.paths.push_back(filePath);

			std::ofstream ofs(filePath, std::ios::binary);
			if (!ofs) {
				logger::error("Unable to write {} file {:x}", filePath.string(), a_hash);
				return;
			}
			ofs.exceptions(std::ios::failbit | std::ios::badbit);
			try {
				std::uint64_t origSize = buffer.size();
				ofs.write(reinterpret_cast<const char*>(&origSize), 8);
				ofs.write(reinterpret_cast<const char*>(&compressedSize), 8);
				ofs.write(compressed.data(), compressedSize);
				ofs.close();
				info.rowPitch.push_back(rowPitch);
				writtenBytes += GetFileSize(filePath);
			}
			catch (...) {
				logger::error("Unable to write {} file {:x}", filePath.string(), a_hash);
				return;
			}
		}
        if (!ValidateDiskCacheInfo(info))
            return;
        if (!Config::GetSingleton().GetClearDiskCache()) {

		std::filesystem::path infoPath = GetCacheFileName(a_hash, -1);
		pending.paths.push_back(infoPath);
        std::ofstream ofs(infoPath, std::ios::binary);
		if (!ofs) {
			logger::error("Unable to write {} file {:x}", infoPath.string(), a_hash);
			return;
		}
		ofs.exceptions(std::ios::failbit | std::ios::badbit);
		try {
			ofs.write(reinterpret_cast<const char*>(&info.mipLevels), sizeof(info.mipLevels));
			std::size_t rowPitchSize = info.rowPitch.size();
			ofs.write(reinterpret_cast<const char*>(&rowPitchSize), sizeof(rowPitchSize));
			for (auto& rowPitch : info.rowPitch) {
				ofs.write(reinterpret_cast<const char*>(&rowPitch), sizeof(rowPitch));
			}
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.Width), sizeof(info.texDesc.Width));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.Height), sizeof(info.texDesc.Height));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.MipLevels), sizeof(info.texDesc.MipLevels));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.ArraySize), sizeof(info.texDesc.ArraySize));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.Format), sizeof(info.texDesc.Format));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.SampleDesc.Count), sizeof(info.texDesc.SampleDesc.Count));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.SampleDesc.Quality), sizeof(info.texDesc.SampleDesc.Quality));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.Usage), sizeof(info.texDesc.Usage));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.BindFlags), sizeof(info.texDesc.BindFlags));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.CPUAccessFlags), sizeof(info.texDesc.CPUAccessFlags));
			ofs.write(reinterpret_cast<const char*>(&info.texDesc.MiscFlags), sizeof(info.texDesc.MiscFlags));

			ofs.write(reinterpret_cast<const char*>(&info.srvDesc.Format), sizeof(info.srvDesc.Format));
			ofs.write(reinterpret_cast<const char*>(&info.srvDesc.ViewDimension), sizeof(info.srvDesc.ViewDimension));
			ofs.write(reinterpret_cast<const char*>(&info.srvDesc.Texture2D.MipLevels), sizeof(info.srvDesc.Texture2D.MipLevels));
			ofs.write(reinterpret_cast<const char*>(&info.srvDesc.Texture2D.MostDetailedMip), sizeof(info.srvDesc.Texture2D.MostDetailedMip));
			ofs.close();
			writtenBytes += GetFileSize(infoPath);
		}
		catch (...) {
			logger::error("Unable to write {} file {:x}", infoPath.string(), a_hash);
			return;
		}
        }
        info.lastAccessTime = currentTime;
        {
            std::lock_guard lg(diskCacheLock);
            diskCacheInfoMap[a_hash] = info;
        }
        AddDiskCacheSize(writtenBytes);
        pending.committed = true;
        } catch (const std::exception& e) {
            logger::error("Unable to create disk cache {:x}: {}", a_hash, e.what());
        }
    }
    std::uint64_t NormalMapStore::MipByteSize(const DiskCacheInfo& info, std::uint32_t mip)
    {
        const auto height = std::max(info.texDesc.Height >> mip, 1u);
        const auto rows = info.texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? height : (height + 3) / 4;
        return static_cast<std::uint64_t>(info.rowPitch[mip]) * rows;
    }

    bool NormalMapStore::ValidateDiskCacheInfo(const DiskCacheInfo& info)
    {
        const auto& d = info.texDesc;
        if (info.mipLevels == 0 || info.mipLevels > 15 || info.rowPitch.size() != info.mipLevels ||
            d.MipLevels != info.mipLevels || d.Width == 0 || d.Height == 0 || d.Width > 16384 || d.Height > 16384 ||
            d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0 ||
            d.Usage != D3D11_USAGE_DEFAULT || d.BindFlags != D3D11_BIND_SHADER_RESOURCE ||
            d.CPUAccessFlags != 0 || d.MiscFlags != 0 ||
            info.srvDesc.Format != d.Format || info.srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            info.srvDesc.Texture2D.MostDetailedMip != 0 || info.srvDesc.Texture2D.MipLevels != info.mipLevels)
            return false;
        if (d.Format != DXGI_FORMAT_BC1_UNORM && d.Format != DXGI_FORMAT_BC3_UNORM &&
            d.Format != DXGI_FORMAT_BC7_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            return false;
        auto maxDimension = std::max(d.Width, d.Height);
        std::uint32_t maxMips = 1;
        while (maxDimension > 1) { maxDimension >>= 1; ++maxMips; }
        if (info.mipLevels > maxMips)
            return false;
        std::uint64_t total = 0;
        for (std::uint32_t mip = 0; mip < info.mipLevels; ++mip) {
            const auto width = std::max(d.Width >> mip, 1u);
            const auto minPitch = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? width * 4u :
                ((width + 3) / 4) * (d.Format == DXGI_FORMAT_BC1_UNORM ? 8u : 16u);
            if (info.rowPitch[mip] < minPitch || info.rowPitch[mip] > 1024u * 1024u)
                return false;
            total += MipByteSize(info, mip);
            if (total > 512ull * 1024 * 1024)
                return false;
        }
        return true;
    }

	TextureResourcePtr NormalMapStore::GetDiskCache(std::uint64_t a_hash)
	{
        std::lock_guard ioLock(diskIOLock);
		if (!Config::GetSingleton().GetDiskCache())
			return nullptr;

		bool isFound = false;
        DiskCacheInfo info;
		{
			std::lock_guard lg(diskCacheLock);
			auto found = diskCacheInfoMap.find(a_hash);
			if (found != diskCacheInfoMap.end())
			{
				isFound = true;
				info = found->second;
			}
		}

		if (!isFound)
		{
			if (Config::GetSingleton().GetClearDiskCache())
				return nullptr;
			std::filesystem::path infoPath = GetCacheFileName(a_hash, -1);
            std::error_code existsError;
            if (!std::filesystem::exists(infoPath, existsError) || existsError)
			{
				logger::error("Disk cache does not exists {:x}", a_hash);
				return nullptr;
			}
			std::ifstream ifs(infoPath, std::ios::binary);
			if (!ifs)
			{
				logger::error("Unable to read disk cache {:x}", a_hash);
				return nullptr;
			}
			ifs.exceptions(std::ios::failbit | std::ios::badbit);
			try {
				ifs.read(reinterpret_cast<char*>(&info.mipLevels), sizeof(info.mipLevels));
				std::size_t rowPitchSize = 0;
				ifs.read(reinterpret_cast<char*>(&rowPitchSize), sizeof(rowPitchSize));
                const auto metadataSize = sizeof(std::uint32_t) + sizeof(std::size_t) +
                    static_cast<std::uint64_t>(rowPitchSize) * sizeof(std::uint32_t) + 15 * sizeof(std::uint32_t);
                if (info.mipLevels == 0 || info.mipLevels > 15 || rowPitchSize != info.mipLevels ||
                    GetFileSize(infoPath) != metadataSize)
                    return nullptr;
                for (std::size_t i = 0; i < rowPitchSize; i++) {
					std::uint32_t rowPitch = 0;
					ifs.read(reinterpret_cast<char*>(&rowPitch), sizeof(rowPitch));
					info.rowPitch.push_back(rowPitch);
				}
				ifs.read(reinterpret_cast<char*>(&info.texDesc.Width), sizeof(info.texDesc.Width));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.Height), sizeof(info.texDesc.Height));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.MipLevels), sizeof(info.texDesc.MipLevels));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.ArraySize), sizeof(info.texDesc.ArraySize));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.Format), sizeof(info.texDesc.Format));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.SampleDesc.Count), sizeof(info.texDesc.SampleDesc.Count));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.SampleDesc.Quality), sizeof(info.texDesc.SampleDesc.Quality));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.Usage), sizeof(info.texDesc.Usage));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.BindFlags), sizeof(info.texDesc.BindFlags));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.CPUAccessFlags), sizeof(info.texDesc.CPUAccessFlags));
				ifs.read(reinterpret_cast<char*>(&info.texDesc.MiscFlags), sizeof(info.texDesc.MiscFlags));

				ifs.read(reinterpret_cast<char*>(&info.srvDesc.Format), sizeof(info.srvDesc.Format));
				ifs.read(reinterpret_cast<char*>(&info.srvDesc.ViewDimension), sizeof(info.srvDesc.ViewDimension));
				ifs.read(reinterpret_cast<char*>(&info.srvDesc.Texture2D.MipLevels), sizeof(info.srvDesc.Texture2D.MipLevels));
				ifs.read(reinterpret_cast<char*>(&info.srvDesc.Texture2D.MostDetailedMip), sizeof(info.srvDesc.Texture2D.MostDetailedMip));
			}
			catch (...) {
				logger::error("Unable to read disk cache {:x}", a_hash);
				return nullptr;
            }

		}

        if (!ValidateDiskCacheInfo(info))
            return nullptr;
        std::vector<D3D11_SUBRESOURCE_DATA> initDatas(info.mipLevels);
		std::vector<std::vector<std::uint8_t>> buffers(info.mipLevels);

		for (UINT mipLevel = 0; mipLevel < info.mipLevels; mipLevel++) {
			std::filesystem::path filePath = GetCacheFileName(a_hash, mipLevel);
			std::ifstream ifs(filePath, std::ios::binary);
            std::error_code existsError;
            if (!std::filesystem::exists(filePath, existsError) || existsError)
			{
				logger::error("{} file does not exists {:x}", filePath.string(), a_hash);
				return nullptr;
			}
			if (!ifs)
			{
				logger::error("Unable to read {} file {:x}", filePath.string(), a_hash);
				return nullptr;
			}
			ifs.exceptions(std::ios::failbit | std::ios::badbit);

			std::uint64_t origSize = 0;
			std::uint64_t compressedSize = 0;
			std::vector<char> compressed;
			try {

				ifs.read(reinterpret_cast<char*>(&origSize), sizeof(origSize));
				ifs.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
                const auto expectedSize = MipByteSize(info, mipLevel);
                const auto fileSize = GetFileSize(filePath);
                if (origSize != expectedSize || compressedSize == 0 ||
                    compressedSize > static_cast<std::uint64_t>(LZ4_compressBound(static_cast<int>(expectedSize))) ||
                    fileSize < 16 || compressedSize != fileSize - 16)
                    return nullptr;
                compressed.resize(static_cast<std::size_t>(compressedSize));
				ifs.read(compressed.data(), compressedSize);
			}
			catch (...) {
				logger::error("Unable to read {} file {:x}", filePath.string(), a_hash);
				return nullptr;
			}

			buffers[mipLevel].resize(origSize);
			int result = LZ4_decompress_safe(compressed.data(),
				reinterpret_cast<char*>(buffers[mipLevel].data()),
				static_cast<int>(compressedSize),
				static_cast<int>(origSize)
			);

			if (result != static_cast<int>(origSize)) {
				logger::error("Failed to decompress {} file {:x}", filePath.string(), a_hash);
				return nullptr;
			}

			initDatas[mipLevel].pSysMem = buffers[mipLevel].data();
			initDatas[mipLevel].SysMemPitch = info.rowPitch[mipLevel];
			initDatas[mipLevel].SysMemSlicePitch = 0;
		}

		TextureResourcePtr result = std::make_shared<TextureResource>();
		auto hr = Shader::ShaderManager::GetSingleton().GetDevice()->CreateTexture2D(&info.texDesc, initDatas.data(), &result->normalmapTexture2D);
		if (FAILED(hr)) {
			logger::error("Failed to create dst texture {:x} : {}", a_hash, hr);
			return nullptr;
		}
		hr = Shader::ShaderManager::GetSingleton().GetDevice()->CreateShaderResourceView(result->normalmapTexture2D.Get(), &info.srvDesc, &result->normalmapShaderResourceView);
		if (FAILED(hr)) {
			logger::error("Failed to create dst shader resource view {:x} : {}", a_hash, hr);
			return nullptr;
		}
        if (!isFound) {
            std::lock_guard lg(diskCacheLock);
            diskCacheInfoMap[a_hash] = info;
        }
		return result;
	}
	void NormalMapStore::RemoveDiskCache(std::uint64_t a_hash)
	{
        std::lock_guard ioLock(diskIOLock);
        // Include unindexed files from a previous session or failed write.
        bool allRemoved = true;
        for (std::int32_t mip = -1; mip < 15; ++mip) {
            const std::filesystem::path path = GetCacheFileName(a_hash, mip);
            const auto bytes = GetFileSize(path);
            std::error_code ec;
            const bool removed = std::filesystem::remove(path, ec);
            if (removed)
                RemoveDiskCacheSize(bytes);
            if (ec)
                allRemoved = false;
        }
        if (allRemoved) {
            std::lock_guard lg(diskCacheLock);
            diskCacheInfoMap.erase(a_hash);
        }
    }

	void NormalMapStore::ClearDiskCache()
	{
        std::lock_guard lifecycle(diskLifecycleLock);
        diskWriter.PauseAndDiscard();
        struct ResumeWriter { DiskWriteQueue<TextureResourcePtr>& writer; ~ResumeWriter() { writer.Resume(); } } resume{diskWriter};
        std::lock_guard ioLock(diskIOLock);
		if (!Config::GetSingleton().GetClearDiskCache())
            return;
        {
            std::lock_guard lg(diskCacheLock);
            diskCacheInfoMap.clear();
        }

        const std::filesystem::path folder = GetCacheFileFolder();
        std::error_code ec;
        std::uint64_t remainingBytes = 0;
        std::filesystem::directory_iterator it(folder, ec), end;
        while (!ec && it != end) {
            const auto file = it->path();
            if (file.extension() == diskCacheExtension && it->is_regular_file(ec)) {
                std::filesystem::remove(file, ec);
                if (ec) {
                    remainingBytes += GetFileSize(file);
                    ec.clear();
                }
            }
            it.increment(ec);
        }
        SetDiskCacheZeroSize();
        AddDiskCacheSize(remainingBytes);
	}

	std::string NormalMapStore::GetCacheFileFolder()
	{
		if (baseFolder.empty())
			baseFolder = Config::GetSingleton().GetDiskCacheFolder() + "\\v2\\";
		return baseFolder;
	}
	std::string NormalMapStore::GetCacheFileName(std::uint64_t a_hash, std::int32_t a_mipLevel)
	{
		if (a_mipLevel < 0)
			return GetCacheFileFolder() + std::format("{:016x}", a_hash) + diskCacheExtension;
		return GetCacheFileFolder() + std::format("{:016x}", a_hash) + "_" + std::to_string(a_mipLevel) + diskCacheExtension;
	}

	std::uint32_t NormalMapStore::GetRefCount(ID3D11Texture2D* texture)
	{
		if (!texture)
			return 0;

		texture->AddRef();
		return texture->Release();
	}
	std::uint32_t NormalMapStore::GetRefCount(ID3D11ShaderResourceView* texture)
	{
		if (!texture)
			return 0;

		texture->AddRef();
		return texture->Release();
	}

	bool NormalMapStore::CheckDiskCacheCapacity()
	{
        std::lock_guard ioLock(diskIOLock);
		bool isRemoved = false;
		while (GetSizeAsMB(GetTotalDiskCacheSize()) > Config::GetSingleton().GetDiskCacheLimitMB()) {
            const auto before = GetTotalDiskCacheSize();
            RemoveOldDiskCache();
            if (GetTotalDiskCacheSize() >= before)
                break;
            isRemoved = true;
		}
		return isRemoved;
	}
	void NormalMapStore::UpdateAccessTime(std::uint64_t a_hash)
	{
		if (!Config::GetSingleton().GetDiskCache())
            return;
        std::lock_guard lg(diskCacheLock);
		auto found = diskCacheInfoMap.find(a_hash);
		if (found != diskCacheInfoMap.end())
			found->second.lastAccessTime = currentTime;
	}
	void NormalMapStore::RemoveOldDiskCache()
	{
        DiskCacheInfoMap diskCacheInfo_;
        {
            std::lock_guard lg(diskCacheLock);
            diskCacheInfo_ = diskCacheInfoMap;
        }

        if (diskCacheInfo_.empty()) {
            // Persistent entries have not necessarily been looked up this session.
            // Evict only names in our v2 schema, leaving unrelated files alone.
            std::error_code ec;
            std::filesystem::directory_iterator it(GetCacheFileFolder(), ec), end;
            std::filesystem::file_time_type oldest = std::filesystem::file_time_type::max();
            std::uint64_t oldestHash = 0;
            bool found = false;
            while (!ec && it != end) {
                const auto name = it->path().filename().string();
                if (it->path().extension() == diskCacheExtension && name.size() >= 16 + diskCacheExtension.size() &&
                    std::all_of(name.begin(), name.begin() + 16, [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    })) {
                    const auto hash = std::stoull(name.substr(0, 16), nullptr, 16);
                    bool ownName = false;
                    for (std::int32_t mip = -1; mip < 15 && !ownName; ++mip)
                        ownName = it->path().filename() == std::filesystem::path(GetCacheFileName(hash, mip)).filename();
                    const auto time = it->last_write_time(ec);
                    if (!ec && ownName && time < oldest) {
                        oldest = time;
                        oldestHash = hash;
                        found = true;
                    }
                    ec.clear();
                }
                it.increment(ec);
            }
            if (found)
                RemoveDiskCache(oldestHash);
            return;
        }

		auto minIt = std::min_element(diskCacheInfo_.begin(), diskCacheInfo_.end(), 
						 [] (std::pair<std::uint64_t, DiskCacheInfo> a, std::pair<std::uint64_t, DiskCacheInfo> b) {
			return a.second.lastAccessTime < b.second.lastAccessTime;
		});
		RemoveDiskCache(minIt->first);
	}
}
