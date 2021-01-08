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

#include <set>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace AppInstaller::Repository::Microsoft
{
    namespace
    {
        // Determines if the existing index is acceptable, or should be completely recreated.
        bool ShouldRecreateCache(SQLiteIndex& index)
        {
            // TODO: Implement me
            UNREFERENCED_PARAMETER(index);
            return false;
        }

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

        // Update installed packages in the index
        void UpdateIndex(SQLiteIndex& index)
        {
            // Get all unique ids in the index; create a set for easy removal in updates
            auto allIds = index.Search({});
            std::set<SQLite::rowid_t> idsSet;

            for (const auto& result : allIds.Matches)
            {
                idsSet.insert(result.first);
            }

            ARPHelper arpHelper;
            arpHelper.UpdateIndexFromARP(index, Manifest::ManifestInstaller::ScopeEnum::Machine, idsSet);
            arpHelper.UpdateIndexFromARP(index, Manifest::ManifestInstaller::ScopeEnum::User, idsSet);

            UpdateIndexFromMSIX(index, idsSet);

            // Any values still in the set were not found during inventory; remove them from the index
            for (const auto& id : idsSet)
            {
                index.RemoveManifestsById(id);
            }
        }

        // The factory for the predefined installed source.
        struct Factory : public ISourceFactory
        {
            // The name that we use for the CrossProcessReaderWriteLock
            constexpr static std::string_view c_fileLockName = "WinGet_SysInstCacheFile"sv;
            constexpr static std::wstring_view c_contentsMutexName = L"WinGet_SysInstCacheContents"sv;
            constexpr static std::string_view c_localCacheRelativeDirectory = "WinGet/SysInstCache"sv;
            constexpr static std::string_view c_cacheFileName = "cache.db"sv;
            constexpr static std::string_view c_sourceName = "*PredefinedInstalledSource"sv;

            static std::filesystem::path GetCacheDirectory()
            {
                std::filesystem::path result = Runtime::GetPathTo(Runtime::PathName::LocalCache);
                result /= c_localCacheRelativeDirectory;
                return result;
            }

            // Due to the time it takes to build out the view of the packages installed outside of our
            // control, we create a cache index. For synchronization, two CrossProcessReaderWriteLocks
            // will be used; one controls access to the file, while the other controls updating the
            // contents.
            //  1. Acquire a SHARED FILE lock.
            //  2. Attempt to acquire an EXCLUSIVE CONTENTS lock with a timeout of 0.
            //      a. If the EXCLUSIVE CONTENTS lock is acquired, update the existing cache CONTENTS.
            //          i. Release the EXCLUSIVE CONTENTS lock.
            //      b. If the EXCLUSIVE CONTENTS lock is not acquired, acquire a SHARED CONTENTS lock.
            //          i. This is simply to wait for the EXCLUSIVE CONTENTS lock to be released.
            //  3. If the existing cache is acceptable to use (schema version, cache version, etc.) return it.
            //  NOTE: Upon reaching this point, the cache should be recreated.
            //  4. Acquire an EXCLUSIVE FILE lock.
            //  5. Delete the existing cache FILE.
            //  6. Create a new cache FILE from scratch.
            //  7. Release the EXCLUSIVE FILE lock.
            //  8. Acquire a SHARED FILE lock.
            //  9. Return the cache.
            std::shared_ptr<ISource> Create(const SourceDetails& details, IProgressCallback&) override final
            {
                THROW_HR_IF(E_INVALIDARG, details.Type != PredefinedInstalledSourceFactory::Type());

                std::filesystem::path cacheDirectory = GetCacheDirectory();
                std::filesystem::path cacheFile = cacheDirectory / c_cacheFileName;

                // Attempt to use the cached index.
                try
                {
                    // The lock here indicates a use of the existing file; we may also write to the database.
                    auto sharedFileLock = Synchronization::CrossProcessReaderWriteLock::LockShared(c_fileLockName);
                    SQLiteIndex index = SQLiteIndex::Open(cacheFile.u8string(), SQLiteIndex::OpenDisposition::ReadWrite);

                    if (!ShouldRecreateCache(index))
                    {
                        {
                            wil::unique_mutex contentsLock;
                            contentsLock.create(c_contentsMutexName.data(), 0, SYNCHRONIZE);

                            DWORD status = 0;
                            auto exclusiveContentsLock = contentsLock.acquire(&status, 0);

                            if (exclusiveContentsLock)
                            {
                                UpdateIndex(index);
                            }
                            else
                            {
                                // We are simply waiting for the exclusiveContentsLock to be released by the other process.
                                contentsLock.acquire();
                            }
                        }

                        return std::make_shared<SQLiteIndexSource>(details, std::string{ c_sourceName }, std::move(index), std::move(sharedFileLock), true);
                    }
                }
                CATCH_LOG();

                // If we did not return the existing cache, attempt to create it anew.
                // If this fails then fall back to creating an in memory index.
                try
                {
                    {
                        // The lock here indicates that we will remove the existing file.
                        auto exclusiveFileLock = Synchronization::CrossProcessReaderWriteLock::LockExclusive(c_fileLockName);

                        // Remove all files in the cache directory before proceeding.
                        std::filesystem::remove_all(cacheDirectory);

                        std::filesystem::create_directories(cacheDirectory);

                        SQLiteIndex index = SQLiteIndex::CreateNew(cacheFile.u8string());
                        PopulateIndex(index);
                    }

                    // Reacquire a shared lock and reopen the index for further use.
                    auto sharedFileLock = Synchronization::CrossProcessReaderWriteLock::LockShared(c_fileLockName);
                    SQLiteIndex index = SQLiteIndex::Open(cacheFile.u8string(), SQLiteIndex::OpenDisposition::Read);

                    return std::make_shared<SQLiteIndexSource>(details, std::string{ c_sourceName }, std::move(index), std::move(sharedFileLock), true);
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
