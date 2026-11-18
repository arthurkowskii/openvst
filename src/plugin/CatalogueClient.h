// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>
#include "ModuleStore.h"

namespace openvst {

struct CatalogueEntry {
    juce::String id, name, vendor, kind, version, digest, status;
    juce::int64 size = 0;
    std::vector<ModuleParamSpec> params;
};

// Minimal read-only catalogue client. The server is data: nothing it returns
// is trusted beyond bounded parsing, and a package is only installed after its
// exact digest has been verified by ModuleStore. Message thread only.
class CatalogueClient {
public:
    explicit CatalogueClient(juce::String baseURL);

    struct ListResult { bool ok = false; juce::String error; std::vector<CatalogueEntry> modules; };
    ListResult list(const juce::String& kind, const juce::String& query = {});

    struct DownloadResult { bool ok = false; juce::String error; juce::File file; juce::int64 bytes = 0; };
    DownloadResult download(const CatalogueEntry& entry, const juce::File& partFile,
                            std::function<bool(double)> progress = {});

    const juce::String& baseURL() const { return base; }

private:
    juce::String base;
    static juce::URL endpoint(const juce::String& base, const juce::String& path);
    static bool readBounded(juce::InputStream&, juce::String& out, juce::String& error);
};

} // namespace openvst
