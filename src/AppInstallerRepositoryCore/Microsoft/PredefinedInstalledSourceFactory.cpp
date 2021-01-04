// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Microsoft/ARPHelper.h"
#include "Microsoft/PredefinedInstalledSourceFactory.h"
#include "Microsoft/SQLiteIndex.h"
#include "Microsoft/SQLiteIndexSource.h"
#include <winget/ManifestInstaller.h>

#include <winget/Registry.h>
#include <AppInstallerArchitecture.h>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace AppInstaller::Repository::Microsoft
{
    namespace
    {
        // Populates the index with the entries from MSIX.
        void PopulateIndexFromMSIX(SQLiteIndex& index)
        {
            using namespace winrt::Windows::ApplicationModel;
            using namespace winrt::Windows::Management::Deployment;

            // TODO: Consider if Optional packages should also be enumerated
            PackageManager packageManager;
            auto packages = packageManager.FindPackagesForUserWithPackageTypes({}, PackageTypes::Main);

            // Reuse the same manifest object, as we will be setting the same values every time.
            Manifest::Manifest manifest;
            // Add one installer for storing the package family name.
            manifest.Installers.emplace_back();
            // Every package will have the same tags currently.
            manifest.Tags = { "msix" };

            // Fields in the index but not populated:
            //  AppMoniker - Not sure what we would put.
            //  Channel - We don't know this information here.
            //  Commands - We could open the manifest and look for these eventually.
            //  Tags - Not sure what else we could put in here.
            for (const auto& package : packages)
            {
                // System packages are part of the OS, and cannot be managed by the user.
                // Filter them out as there is no point in showing them in a package manager.
                auto signatureKind = package.SignatureKind();
                if (signatureKind == PackageSignatureKind::System)
                {
                    continue;
                }

                auto packageId = package.Id();
                Utility::NormalizedString familyName = Utility::ConvertToUTF8(packageId.FamilyName());

                manifest.Id = familyName;

                // Attempt to get the DisplayName. Since this will retrieve the localized value, it has a chance to fail.
                // Rather than completely skip this package in that case, we will simply fall back to using the package name below.
                try
                {
                    manifest.Name = Utility::ConvertToUTF8(package.DisplayName());
                }
                catch (const winrt::hresult_error& hre)
                {
                    AICLI_LOG(Repo, Info, << "winrt::hresult_error[0x" << Logging::SetHRFormat << hre.code() << ": " <<
                        Utility::ConvertToUTF8(hre.message()) << "] exception thrown when getting DisplayName for " << familyName);
                }
                catch (...)
                {
                    AICLI_LOG(Repo, Info, << "Unknown exception thrown when getting DisplayName for " << familyName);
                }

                if (manifest.Name.empty())
                {
                    manifest.Name = Utility::ConvertToUTF8(packageId.Name());
                }

                std::ostringstream strstr;
                auto packageVersion = packageId.Version();
                strstr << packageVersion.Major << '.' << packageVersion.Minor << '.' << packageVersion.Build << '.' << packageVersion.Revision;

                manifest.Version = strstr.str();
                
                manifest.Installers[0].PackageFamilyName = familyName;

                // Use the family name as a unique key for the path
                auto manifestId = index.AddManifest(manifest, std::filesystem::path{ packageId.FamilyName().c_str() });

                index.SetMetadataByManifestId(manifestId, PackageVersionMetadata::InstalledType, 
                    Manifest::ManifestInstaller::InstallerTypeToString(Manifest::ManifestInstaller::InstallerTypeEnum::Msix));
            }
        }

        // Put installed packages into the index
        void PopulateIndex(SQLiteIndex& index)
        {
            ARPHelper arpHelper;
            arpHelper.PopulateIndexFromARP(index, Manifest::ManifestInstaller::ScopeEnum::Machine);
            arpHelper.PopulateIndexFromARP(index, Manifest::ManifestInstaller::ScopeEnum::User);

            PopulateIndexFromMSIX(index);
        }

        // The factory for the predefined installed source.
        struct Factory : public ISourceFactory
        {
            // The name that we use for the CrossProcessReaderWriteLock
            constexpr static std::string_view c_lockName = "WinGet_SysInstCache"sv;
            constexpr static std::string_view c_localCacheRelativeDirectory = "WinGet/SysInstCache"sv;
            constexpr static std::string_view c_cacheFileName = "cache.db"sv;
            constexpr static std::string_view c_sourceName = "*PredefinedInstalledSource"sv;

            static std::filesystem::path GetCacheDirectory()
            {
                std::filesystem::path result = Runtime::GetPathTo(Runtime::PathName::LocalCache);
                result /= c_localCacheRelativeDirectory;
                return result;
            }

            std::shared_ptr<ISource> Create(const SourceDetails& details, IProgressCallback&) override final
            {
                THROW_HR_IF(E_INVALIDARG, details.Type != PredefinedInstalledSourceFactory::Type());

                std::filesystem::path cacheDirectory = GetCacheDirectory();
                std::filesystem::path cacheFile = cacheDirectory / c_cacheFileName;

                // Attempt to use the cached index; if this fails then fall back to creating an in memory index.
                try
                {
                    // The read lock here indicates a use of the existing file; we may also write to the database.
                    // It should be thought of as shared access.
                    auto sharedLock = Synchronization::CrossProcessReaderWriteLock::LockForRead(c_lockName);
                }
                CATCH_LOG();

                // If we did not return the existing cache, attempt to create it anew.
                try
                {
                    {
                        // The write lock here indicates that we will remove the existing file.
                        // It should be thought of as exclusive access.
                        auto exclusiveLock = Synchronization::CrossProcessReaderWriteLock::LockForWrite(c_lockName);

                        // Remove all files in the cache directory before proceeding.
                        std::filesystem::remove_all(cacheDirectory);

                        std::filesystem::create_directories(cacheDirectory);

                        SQLiteIndex index = SQLiteIndex::CreateNew(cacheFile.u8string());
                        PopulateIndex(index);
                    }

                    // Reacquire a shared lock and reopen the index for further use.
                    auto sharedLock = Synchronization::CrossProcessReaderWriteLock::LockForRead(c_lockName);
                    SQLiteIndex index = SQLiteIndex::Open(cacheFile.u8string(), SQLiteIndex::OpenDisposition::Read);

                    return std::make_shared<SQLiteIndexSource>(details, std::string{ c_sourceName }, std::move(index), std::move(sharedLock), true);
                }
                CATCH_LOG();

                // If we did not return the new cache, attempt to create an in memory cache to hobble along.
                AICLI_LOG(Repo, Info, << "Creating PredefinedInstalledSource in memory");

                // Create an in memory index
                SQLiteIndex index = SQLiteIndex::CreateNew(SQLITE_MEMORY_DB_CONNECTION_TARGET, Schema::Version::Latest());
                PopulateIndex(index);

                return std::make_shared<SQLiteIndexSource>(details, std::string{ c_sourceName }, std::move(index), Synchronization::CrossProcessReaderWriteLock{}, true);
            }

            void Add(SourceDetails&, IProgressCallback&) override final
            {
                // Add should never be needed, as this is predefined.
                THROW_HR(E_NOTIMPL);
            }

            void Update(const SourceDetails&, IProgressCallback&) override final
            {
                // Update could be used later, but not for now.
                THROW_HR(E_NOTIMPL);
            }

            void Remove(const SourceDetails&, IProgressCallback&) override final
            {
                // Similar to add, remove should never be needed.
                THROW_HR(E_NOTIMPL);
            }
        };
    }

    std::unique_ptr<ISourceFactory> PredefinedInstalledSourceFactory::Create()
    {
        return std::make_unique<Factory>();
    }
}
