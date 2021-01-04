// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Public/AppInstallerRepositorySource.h"
#include "SourceFactory.h"

#include <string_view>

namespace AppInstaller::Repository::Microsoft
{
    using namespace std::string_view_literals;

    // A source of installed packages on the local system.
    // Arg  ::  Not used.
    // Data ::  Not used.
    struct PredefinedInstalledSourceFactory
    {
        // Get the type string for this source.
        static constexpr std::string_view Type()
        {
            return "Microsoft.Predefined.Installed"sv;
        }

        // Creates a source factory for this type.
        static std::unique_ptr<ISourceFactory> Create();
    };
}
