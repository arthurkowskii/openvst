// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace openvst {

// A parameter as declared by a module package. `id` is the permanent
// automation identity; `endpoint` is the Cmajor event endpoint the value is
// written to. Both are recorded in install.json and never derived at runtime.
struct ModuleParamSpec {
    juce::String id, name, endpoint, unit;
    float min = 0.0f, max = 1.0f, def = 0.0f;
};

// Everything the catalogue said about one immutable package version, plus the
// digests of the exact bytes that were verified. Paths are relative to the
// install root and were validated before anything was written.
struct ModulePackage {
    juce::String id, version, name, kind;   // kind: "effect" or "instrument"
    juce::String vendor, license;
    juce::String packageSHA256;             // digest of the exact ZIP
    juce::int64 packageSize = 0;
    juce::String sourceSHA256;              // digest of the .cmajor source bytes
    juce::String manifestSHA256;            // digest of openvst.json bytes
    juce::String sourceRel, manifestRel, patchRel, uiRel; // relative, safe
    juce::String mainProcessor;
    std::vector<ModuleParamSpec> params;
};

struct InstalledModule {
    ModulePackage package;
    juce::File root;             // <store>/<id>/<version>-<digest>
    juce::File sourceFile, manifestFile, patchFile, uiFile;
    juce::String installedAt;    // ISO-8601 UTC, informational
};

// Content-addressed module store. Every method except download() is meant to
// be called on the message thread; nothing here touches the audio callback.
// The store directory is the only writable place this class touches, and every
// path taken from an archive is validated and then rebuilt from validated
// components, never used as-is.
class ModuleStore {
public:
    explicit ModuleStore(juce::File root);

    // Direct children only, never follows symlinks. Malformed entries are
    // skipped and described in problems().
    std::vector<InstalledModule> scan() const;
    const juce::StringArray& problems() const { return problems_; }

    struct InstallResult { bool ok = false; juce::String error; InstalledModule installed; };
    // expectedSHA256 may be empty for a local import (digest computed from the
    // file). It is mandatory for a downloaded package.
    InstallResult install(const juce::File& zip, const juce::String& expectedSHA256,
                          std::function<bool(double)> progress = {});

    struct DownloadResult { bool ok = false; juce::String error; juce::int64 bytes = 0; };
    // Writes url to partFile. Resumes with Range when partFile exists and the
    // server answers 206. Verifies expectedSHA256 before returning ok. On any
    // failure partFile may remain for a later resume; it is never published.
    static DownloadResult download(const juce::URL& url, const juce::File& partFile,
                                   const juce::String& expectedSHA256,
                                   std::function<bool(double)> progress = {});

    static juce::String sha256File(const juce::File&);

    // Removes only <store>/<id>/<version>-<digest>; refuses anything else.
    bool remove(const InstalledModule&);

private:
    juce::File root_;
    mutable juce::StringArray problems_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleStore)
};

} // namespace openvst
