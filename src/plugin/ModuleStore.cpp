// SPDX-License-Identifier: AGPL-3.0-only
#include "ModuleStore.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

// Trust model: a package is hostile until every entry name, every declared
// path and every document has been validated. Nothing from the archive is used
// as a filesystem path directly; validated components are rebuilt under the
// staging root. Unknown or ambiguous input is refused, never repaired.

namespace openvst {

namespace {

//==============================================================================
// Bounds. Every one of these is a refusal threshold, not a hint.

constexpr int      kMaxEntries          = 20000;
constexpr juce::int64 kMaxPackageBytes  = juce::int64 (512) * 1024 * 1024;
constexpr juce::int64 kMaxTotalExtracted = juce::int64 (2) * 1024 * 1024 * 1024;
constexpr juce::int64 kMaxEntryBytes    = juce::int64 (256) * 1024 * 1024;
constexpr int      kMaxPathChars        = 240;
constexpr juce::int64 kMaxManifestBytes = 64 * 1024;
constexpr juce::int64 kMaxUiBytes       = 64 * 1024;
constexpr juce::int64 kMaxPatchBytes    = 1024 * 1024;
constexpr juce::int64 kMaxInstallJsonBytes = 64 * 1024;
constexpr int      kMaxParams           = 8;
constexpr int      kMaxResources        = 256;
constexpr int      kMaxIdChars          = 200;
constexpr int      kMaxVersionChars     = 32;
constexpr int      kMaxNameChars        = 256;
constexpr int      kMaxShortFieldChars  = 128;  // param id/endpoint, mainProcessor
constexpr int      kMaxUnitChars        = 32;
constexpr int      kMaxInstalledAtChars = 64;

constexpr int      kChunkSize           = 64 * 1024;
constexpr int      kLockWaitMs          = 10000;
constexpr int      kLockPollMs          = 50;
constexpr juce::int64 kLockStaleMs      = juce::int64 (10) * 60 * 1000;
constexpr juce::int64 kStagingStaleMs   = juce::int64 (24) * 60 * 60 * 1000;

const char* const kStagingDirName = ".staging";
const char* const kLocksDirName   = ".locks";
const char* const kManifestName   = "openvst.json";
const char* const kPackageSchema  = "openvst.package/0";
const char* const kInstallSchema  = "openvst.install/0";

//==============================================================================
// String predicates

bool isAsciiPrintable (const juce::String& s)
{
    for (auto c : s)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

bool isHexDigitLower (juce::juce_wchar c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// juce::Uuid seeds a Random from the clock, so two threads starting within the
// same tick can draw the same value. A process-wide counter makes the staging
// name unique inside the process, and the pid keeps it unique across writers.
int currentProcessId()
{
   #if JUCE_WINDOWS
    return (int) GetCurrentProcessId();
   #else
    return (int) getpid();
   #endif
}

juce::String uniqueStagingName (const juce::String& digest)
{
    static std::atomic<juce::uint32> counter { 0 };
    return digest + "-" + juce::String (currentProcessId())
         + "-" + juce::String ((int) ++counter)
         + "-" + juce::Uuid().toString();
}

// Any case: used for caller-supplied digests, compared case-insensitively.
bool isHex64 (const juce::String& s)
{
    if (s.length() != 64)
        return false;
    for (auto c : s)
    {
        const auto l = juce::CharacterFunctions::toLowerCase (c);
        if (! isHexDigitLower (l))
            return false;
    }
    return true;
}

// Canonical form we write and therefore require when reading back.
bool isCanonicalHex64 (const juce::String& s)
{
    if (s.length() != 64)
        return false;
    for (auto c : s)
        if (! isHexDigitLower (c))
            return false;
    return true;
}

bool isDigitRun (const juce::String& s, int maxLen)
{
    if (s.isEmpty() || s.length() > maxLen)
        return false;
    if (s.length() > 1 && s[0] == '0')
        return false;
    for (auto c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

bool isSemVer (const juce::String& v)
{
    if (v.isEmpty() || v.length() > kMaxVersionChars)
        return false;
    auto parts = juce::StringArray::fromTokens (v, ".", "");
    if (parts.size() != 3)
        return false;
    for (const auto& p : parts)
        if (! isDigitRun (p, 9))
            return false;
    return true;
}

bool isReverseDnsId (const juce::String& id)
{
    if (id.isEmpty() || id.length() > kMaxIdChars)
        return false;

    int labels = 0;
    int start = 0;
    for (int i = 0; i <= id.length(); ++i)
    {
        if (i != id.length() && id[i] != '.')
            continue;

        const int len = i - start;
        if (len < 1 || len > 63)
            return false;

        for (int k = 0; k < len; ++k)
        {
            const auto c = id[start + k];
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            if (! allowed)
                return false;
            if (c == '-' && (k == 0 || k == len - 1))
                return false;
        }

        ++labels;
        start = i + 1;
    }
    return labels >= 2;
}

bool isReservedDeviceName (const juce::String& component)
{
    static const char* const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    // Windows reserves the device name before the first dot as well.
    const auto base = component.upToFirstOccurrenceOf (".", false, false).toUpperCase();
    for (auto* name : reserved)
        if (base == name)
            return true;
    return false;
}

juce::StringArray splitPath (const juce::String& p)
{
    juce::StringArray parts;
    int start = 0;
    for (;;)
    {
        const int slash = p.indexOfChar (start, '/');
        if (slash < 0)
        {
            parts.add (p.substring (start));
            break;
        }
        parts.add (p.substring (start, slash));
        start = slash + 1;
    }
    return parts;
}

// The single gate every archive path and every recorded relative path passes
// through. Anything ambiguous is refused; nothing is normalised.
bool isSafeRelativePath (const juce::String& path, juce::String& why)
{
    if (path.isEmpty())
    {
        why = "empty path";
        return false;
    }
    if (path.length() > kMaxPathChars || path.getNumBytesAsUTF8() > kMaxPathChars)
    {
        why = "path exceeds 240 characters";
        return false;
    }
    if (! isAsciiPrintable (path))
    {
        why = "path is not printable ASCII";
        return false;
    }
    if (path[0] == '/')
    {
        why = "absolute path";
        return false;
    }
    if (path.containsChar ('\\'))
    {
        why = "backslash in path";
        return false;
    }
    if (path.containsChar (':'))
    {
        why = "colon in path";
        return false;
    }
    if (path.containsAnyOf ("?*<>|\""))
    {
        why = "character illegal on Windows";
        return false;
    }
    if (path.endsWithChar ('/'))
    {
        why = "trailing separator in file path";
        return false;
    }

    const auto parts = splitPath (path);
    for (const auto& part : parts)
    {
        if (part.isEmpty())
        {
            why = "empty path component";
            return false;
        }
        if (part == "." || part == "..")
        {
            why = "relative component in path";
            return false;
        }
        if (part.endsWithChar ('.') || part.endsWithChar (' '))
        {
            why = "component ends with a dot or space";
            return false;
        }
        if (isReservedDeviceName (part))
        {
            why = "reserved device name";
            return false;
        }
    }
    return true;
}

bool isAllowedSuffix (const juce::String& path)
{
    static const char* const suffixes[] = {
        ".cmajor", ".cmajorpatch", ".json", ".wav", ".flac", ".aif", ".aiff",
        ".ogg", ".opus", ".mp3", ".png", ".jpg", ".jpeg", ".webp"
    };

    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    const int dot = name.lastIndexOfChar ('.');
    if (dot <= 0)
        return false;

    const auto ext = name.substring (dot).toLowerCase();
    for (auto* s : suffixes)
        if (ext == s)
            return true;
    return false;
}

juce::String foldCase (const juce::String& s)
{
    return s.toLowerCase();
}

juce::String describe (const juce::String& s)
{
    if (s.length() <= 120 && isAsciiPrintable (s))
        return s;
    return "<unprintable name>";
}

//==============================================================================
// I/O primitives

juce::String sha256Of (const juce::MemoryBlock& block)
{
    return juce::SHA256 (block.getData(), block.getSize()).toHexString();
}

juce::String sha256FileImpl (const juce::File& f)
{
    if (! f.existsAsFile())
        return {};
    return juce::SHA256 (f).toHexString();
}

// Best effort by design: a filesystem that cannot sync is not a reason to
// refuse an otherwise valid package, but a successful sync is what makes the
// publish durable.
void syncPath (const juce::File& f, bool isDirectory)
{
    const auto path = f.getFullPathName();

   #if JUCE_WINDOWS
    const DWORD access = GENERIC_READ;
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileW (path.toWideCharPointer(), access, sharing, nullptr,
                            OPEN_EXISTING, flags, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers (h);
        CloseHandle (h);
    }
   #else
    const int flags = isDirectory ? (O_RDONLY | O_CLOEXEC | O_DIRECTORY) : (O_RDONLY | O_CLOEXEC);
    const int fd = ::open (path.toRawUTF8(), flags);
    if (fd >= 0)
    {
        ::fsync (fd);
        ::close (fd);
    }
   #endif
}

// Atomic exclusive creation, the only portable way to take a lock that two
// processes cannot both hold.
bool createExclusiveFile (const juce::File& f, const juce::String& contents,
                          bool& alreadyExists, juce::String& why)
{
    alreadyExists = false;
    const auto path = f.getFullPathName();

   #if JUCE_WINDOWS
    HANDLE h = CreateFileW (path.toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
        {
            alreadyExists = true;
            return false;
        }
        why = "cannot create lock file (" + juce::String ((int) e) + ")";
        return false;
    }
    DWORD written = 0;
    const auto utf8 = contents.toRawUTF8();
    WriteFile (h, utf8, (DWORD) std::strlen (utf8), &written, nullptr);
    CloseHandle (h);
    return true;
   #else
    const int fd = ::open (path.toRawUTF8(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        if (errno == EEXIST)
        {
            alreadyExists = true;
            return false;
        }
        why = "cannot create lock file (errno " + juce::String (errno) + ")";
        return false;
    }
    const auto utf8 = contents.toRawUTF8();
    if (*utf8 != 0)
    {
        // The lock payload is diagnostic only: a failed write still leaves an
        // exclusively created lock file, which is all the lock needs.
        const auto written = ::write (fd, utf8, std::strlen (utf8));
        (void) written;
    }
    ::close (fd);
    return true;
   #endif
}

// juce::File::createDirectory() reports EEXIST as a failure, so two writers
// creating the same parent at the same moment cannot both succeed. The
// directory existing afterwards is the real postcondition.
bool ensureDirectory (const juce::File& dir)
{
    if (dir.isDirectory())
        return true;
    if (dir.createDirectory().wasOk())
        return true;
    return dir.isDirectory();
}

struct DirectoryGuard
{
    juce::File dir;
    bool armed = true;

    explicit DirectoryGuard (juce::File d) : dir (std::move (d)) {}
    ~DirectoryGuard() { if (armed && dir != juce::File()) dir.deleteRecursively(); }

    void release() { armed = false; }
    JUCE_DECLARE_NON_COPYABLE (DirectoryGuard)
};

struct ModuleLock
{
    juce::File file;
    bool held = false;

    ModuleLock() = default;

    ~ModuleLock()
    {
        if (held)
            file.deleteFile();
    }

    JUCE_DECLARE_NON_COPYABLE (ModuleLock)
};

bool acquireModuleLock (const juce::File& locksDir, const juce::String& moduleId,
                        ModuleLock& lock, juce::String& why)
{
    if (! ensureDirectory (locksDir))
    {
        why = "cannot create the store lock directory";
        return false;
    }

    const auto lockFile = locksDir.getChildFile (moduleId + ".lock");
    const auto payload = "pid " + juce::String (currentProcessId()) + "\n";
    const double deadline = juce::Time::getMillisecondCounterHiRes() + (double) kLockWaitMs;
    bool staleRemoved = false;

    for (;;)
    {
        bool exists = false;
        if (createExclusiveFile (lockFile, payload, exists, why))
        {
            lock.file = lockFile;
            lock.held = true;
            return true;
        }
        if (! exists)
            return false;

        if (! staleRemoved)
        {
            const auto modified = lockFile.getLastModificationTime().toMilliseconds();
            const auto now = juce::Time::getCurrentTime().toMilliseconds();
            if (modified > 0 && now - modified > kLockStaleMs)
            {
                // One attempt only: a lock that reappears immediately belongs
                // to a live writer and must still be waited for.
                lockFile.deleteFile();
                staleRemoved = true;
                continue;
            }
        }

        if (juce::Time::getMillisecondCounterHiRes() >= deadline)
        {
            why = "timed out waiting for the module lock";
            return false;
        }
        juce::Thread::sleep (kLockPollMs);
    }
}

bool readFileBounded (const juce::File& f, juce::int64 maxBytes,
                      juce::MemoryBlock& out, juce::String& why)
{
    if (f.isSymbolicLink())
    {
        why = "file is a symbolic link";
        return false;
    }
    if (! f.existsAsFile())
    {
        why = "file is missing";
        return false;
    }
    const auto size = f.getSize();
    if (size < 0 || size > maxBytes)
    {
        why = "file is too large";
        return false;
    }
    if (! f.loadFileAsData (out))
    {
        why = "cannot read file";
        return false;
    }
    if (out.getSize() != (size_t) size)
    {
        why = "file changed while being read";
        return false;
    }
    if (out.getSize() > 0 && std::memchr (out.getData(), 0, out.getSize()) != nullptr)
    {
        why = "file contains a NUL byte";
        return false;
    }
    return true;
}

bool blockToUtf8 (const juce::MemoryBlock& block, juce::String& out, juce::String& why)
{
    if (block.getSize() > 0 && std::memchr (block.getData(), 0, block.getSize()) != nullptr)
    {
        why = "document contains a NUL byte";
        return false;
    }
    out = juce::String::fromUTF8 ((const char*) block.getData(), (int) block.getSize());
    return true;
}

bool readEntryBounded (juce::ZipFile& zip, int index, juce::int64 maxBytes,
                       juce::MemoryBlock& out, juce::String& why)
{
    const auto* entry = zip.getEntry (index);
    if (entry == nullptr)
    {
        why = "archive entry is missing";
        return false;
    }
    if (entry->uncompressedSize < 0 || entry->uncompressedSize > maxBytes)
    {
        why = "archive entry exceeds the size limit";
        return false;
    }

    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (index));
    if (in == nullptr)
    {
        why = "archive entry cannot be opened";
        return false;
    }

    out.setSize ((size_t) entry->uncompressedSize);
    size_t got = 0;
    while (got < (size_t) entry->uncompressedSize)
    {
        const auto remaining = (int) std::min ((juce::int64) kChunkSize,
                                               (juce::int64) entry->uncompressedSize - (juce::int64) got);
        const int n = in->read ((char*) out.getData() + got, remaining);
        if (n <= 0)
        {
            why = "archive entry ended before its declared size";
            return false;
        }
        got += (size_t) n;
    }
    {
        // JUCE's own extractor uses this exact probe: a readByte() at the end
        // of a buffered entry can report a byte that is not there.
        char extra = 0;
        if (in->read (&extra, 1) > 0)
        {
            why = "archive entry holds more than its declared size";
            return false;
        }
    }
    return true;
}

juce::String hashZipEntry (juce::ZipFile& zip, int index)
{
    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (index));
    if (in == nullptr)
        return {};
    return juce::SHA256 (*in).toHexString();
}

// Resolve a validated relative path under a root, refusing any component that
// is a symlink. `out` is built from validated components, never from the raw
// string.
bool resolveInside (const juce::File& root, const juce::String& relative,
                    juce::File& out)
{
    juce::String why;
    if (! isSafeRelativePath (relative, why))
        return false;

    const auto parts = splitPath (relative);
    juce::File f = root;
    for (int i = 0; i < parts.size(); ++i)
    {
        f = f.getChildFile (parts[i]);
        if (f.isSymbolicLink())
            return false;
        if (i < parts.size() - 1 && f.exists() && ! f.isDirectory())
            return false;
    }
    if (! f.isAChildOf (root))
        return false;
    out = f;
    return true;
}

//==============================================================================
// JSON helpers

juce::var property (const juce::var& object, const char* key)
{
    if (! object.isObject())
        return {};
    return object.getProperty (juce::Identifier (key), juce::var());
}

bool wantString (const juce::var& parent, const char* key, bool required, int maxChars,
                 juce::String& out, juce::String& why)
{
    const auto v = property (parent, key);
    if (v.isVoid() || v.isUndefined())
    {
        if (required)
        {
            why = juce::String ("missing field: ") + key;
            return false;
        }
        out = juce::var();
        return true;
    }
    if (! v.isString())
    {
        why = juce::String ("field must be a string: ") + key;
        return false;
    }
    const auto s = v.toString();
    if (s.isEmpty())
    {
        if (required)
        {
            why = juce::String ("field must not be empty: ") + key;
            return false;
        }
        out = juce::var();
        return true;
    }
    if (s.length() > maxChars)
    {
        why = juce::String ("field is too long: ") + key;
        return false;
    }
    if (! isAsciiPrintable (s))
    {
        why = juce::String ("field must be printable ASCII: ") + key;
        return false;
    }
    out = s;
    return true;
}

bool wantNumber (const juce::var& parent, const char* key, bool required,
                 double& out, juce::String& why)
{
    const auto v = property (parent, key);
    if (v.isVoid() || v.isUndefined())
    {
        if (required)
        {
            why = juce::String ("missing field: ") + key;
            return false;
        }
        out = 0.0;
        return true;
    }
    // JUCE 9 keeps separate flags for int, int64 and double: a JSON integer
    // arrives as an int and must not be mistaken for a string.
    if (! (v.isInt() || v.isInt64() || v.isDouble()))
    {
        why = juce::String ("field must be a number: ") + key;
        return false;
    }
    const double d = (double) v;
    if (! std::isfinite (d) || std::abs (d) > 1.0e9)
    {
        why = juce::String ("field must be a finite number: ") + key;
        return false;
    }
    out = d;
    return true;
}

bool wantObject (const juce::var& parent, const char* key, bool required,
                 juce::var& out, juce::String& why)
{
    const auto v = property (parent, key);
    if (v.isVoid() || v.isUndefined())
    {
        if (required)
        {
            why = juce::String ("missing object: ") + key;
            return false;
        }
        out = juce::var();
        return true;
    }
    if (! v.isObject())
    {
        why = juce::String ("field must be an object: ") + key;
        return false;
    }
    out = v;
    return true;
}

bool parseJsonDocument (const juce::String& text, juce::var& out, juce::String& why)
{
    out = juce::JSON::parse (text);
    if (! out.isObject())
    {
        why = "document is not a JSON object";
        return false;
    }
    return true;
}

//==============================================================================
// Package documents

struct ParsedManifest
{
    ModulePackage package;
    juce::String patchRel;
    juce::String uiRel;          // declared by the manifest, empty when absent
    bool hasUi = false;
    bool hasMainProcessor = false;
    std::vector<std::pair<juce::String, juce::String>> resources; // path, sha256
};

bool parseManifest (const juce::MemoryBlock& bytes, ParsedManifest& out, juce::String& why)
{
    juce::String text;
    if (! blockToUtf8 (bytes, text, why))
        return false;

    juce::var root;
    if (! parseJsonDocument (text, root, why))
        return false;

    juce::String schema;
    if (! wantString (root, "schema", true, 64, schema, why))
        return false;
    if (schema != kPackageSchema)
    {
        why = "unsupported manifest schema";
        return false;
    }

    juce::var module;
    if (! wantObject (root, "module", true, module, why))
        return false;

    auto& pkg = out.package;
    if (! wantString (module, "id", true, kMaxIdChars, pkg.id, why))
        return false;
    if (! isReverseDnsId (pkg.id))
    {
        why = "module.id is not a lowercase reverse-DNS identifier";
        return false;
    }
    if (! wantString (module, "version", true, kMaxVersionChars, pkg.version, why))
        return false;
    if (! isSemVer (pkg.version))
    {
        why = "module.version is not SemVer";
        return false;
    }
    if (! wantString (module, "name", true, kMaxNameChars, pkg.name, why))
        return false;
    if (! wantString (module, "kind", true, 32, pkg.kind, why))
        return false;
    if (pkg.kind != "effect" && pkg.kind != "instrument")
    {
        why = "module.kind must be effect or instrument";
        return false;
    }
    if (! wantString (module, "vendor", false, kMaxNameChars, pkg.vendor, why))
        return false;
    if (! wantString (module, "license", false, kMaxShortFieldChars, pkg.license, why))
        return false;

    juce::var runtime;
    if (! wantObject (root, "runtime", true, runtime, why))
        return false;

    juce::String engine;
    if (! wantString (runtime, "engine", true, 32, engine, why))
        return false;
    if (engine != "cmajor")
    {
        why = "runtime.engine must be cmajor";
        return false;
    }

    if (! wantString (runtime, "patch", true, kMaxPathChars, out.patchRel, why))
        return false;
    juce::String pathWhy;
    if (! isSafeRelativePath (out.patchRel, pathWhy))
    {
        why = "runtime.patch is not a safe relative path (" + pathWhy + ")";
        return false;
    }
    if (! out.patchRel.endsWithIgnoreCase (".cmajorpatch"))
    {
        why = "runtime.patch must name a .cmajorpatch file";
        return false;
    }
    if (! out.patchRel.startsWith ("patch/"))
    {
        why = "runtime.patch must live under patch/";
        return false;
    }

    const auto patchFormat = property (runtime, "patch_format");
    if (! patchFormat.isVoid())
    {
        double format = 0.0;
        if (! wantNumber (runtime, "patch_format", true, format, why))
            return false;
        if (format != 1.0)
        {
            why = "unsupported patch_format";
            return false;
        }
    }

    juce::String mainProcessor;
    if (! wantString (runtime, "mainProcessor", false, kMaxShortFieldChars, mainProcessor, why))
        return false;
    out.hasMainProcessor = mainProcessor.isNotEmpty();
    pkg.mainProcessor = mainProcessor;

    const auto params = property (root, "parameters");
    if (! params.isVoid())
    {
        if (! params.isArray())
        {
            why = "parameters must be an array";
            return false;
        }
        const auto* arr = params.getArray();
        if (arr->size() > kMaxParams)
        {
            why = "too many parameters";
            return false;
        }
        for (const auto& item : *arr)
        {
            if (! item.isObject())
            {
                why = "parameter entry must be an object";
                return false;
            }
            ModuleParamSpec spec;
            if (! wantString (item, "id", true, kMaxShortFieldChars, spec.id, why))
                return false;
            if (! wantString (item, "name", false, kMaxShortFieldChars, spec.name, why))
                return false;
            if (spec.name.isEmpty())
                spec.name = spec.id;
            if (! wantString (item, "endpoint", false, kMaxShortFieldChars, spec.endpoint, why))
                return false;
            if (spec.endpoint.isEmpty())
                spec.endpoint = spec.id;
            if (! wantString (item, "unit", false, kMaxUnitChars, spec.unit, why))
                return false;

            double minV = 0.0, maxV = 1.0, defV = 0.0;
            if (! wantNumber (item, "min", true, minV, why))
                return false;
            if (! wantNumber (item, "max", true, maxV, why))
                return false;
            if (! wantNumber (item, "default", false, defV, why))
                return false;
            if (! (minV < maxV))
            {
                why = "parameter range is inverted or empty";
                return false;
            }
            if (defV < minV || defV > maxV)
            {
                why = "parameter default is outside its range";
                return false;
            }
            spec.min = (float) minV;
            spec.max = (float) maxV;
            spec.def = (float) defV;

            for (const auto& other : pkg.params)
            {
                if (other.id == spec.id)
                {
                    why = "duplicate parameter id";
                    return false;
                }
                if (other.endpoint == spec.endpoint)
                {
                    why = "duplicate parameter endpoint";
                    return false;
                }
            }
            pkg.params.push_back (spec);
        }
    }

    juce::String declaredUi;
    if (! wantString (root, "ui", false, kMaxPathChars, declaredUi, why))
        return false;
    if (declaredUi.isNotEmpty())
    {
        juce::String uiWhy;
        if (! isSafeRelativePath (declaredUi, uiWhy))
        {
            why = "ui is not a safe relative path (" + uiWhy + ")";
            return false;
        }
        if (! declaredUi.endsWithIgnoreCase (".json"))
        {
            why = "ui must point to a .json file";
            return false;
        }
        if (declaredUi == kManifestName)
        {
            why = "ui must not point to the manifest";
            return false;
        }
        out.uiRel = declaredUi;
        out.hasUi = true;
    }

    const auto resources = property (root, "resources");
    if (! resources.isVoid())
    {
        if (! resources.isArray())
        {
            why = "resources must be an array";
            return false;
        }
        const auto* arr = resources.getArray();
        if (arr->size() > kMaxResources)
        {
            why = "too many resources";
            return false;
        }
        for (const auto& item : *arr)
        {
            if (! item.isObject())
            {
                why = "resource entry must be an object";
                return false;
            }
            juce::String rel, digest;
            if (! wantString (item, "path", true, kMaxPathChars, rel, why))
                return false;
            if (! isSafeRelativePath (rel, pathWhy))
            {
                why = "resource path is not safe (" + pathWhy + ")";
                return false;
            }
            if (! wantString (item, "sha256", true, 64, digest, why))
                return false;
            if (! isHex64 (digest))
            {
                why = "resource sha256 is not a hex digest";
                return false;
            }
            out.resources.emplace_back (rel, digest.toLowerCase());
        }
    }

    return true;
}

// Contract v0: a patch is a flat document with a closed set of keys. A worker,
// a view or any unknown key is refused rather than ignored.
bool parsePatch (const juce::MemoryBlock& bytes, const ParsedManifest& manifest,
                 juce::String& mainProcessorOut, juce::String& sourceOut, juce::String& why)
{
    juce::String text;
    if (! blockToUtf8 (bytes, text, why))
        return false;

    juce::var root;
    if (! parseJsonDocument (text, root, why))
        return false;

    static const char* const allowed[] = {
        "CmajorVersion", "ID", "version", "name", "isInstrument",
        "source", "mainProcessor"
    };

    if (auto* object = root.getDynamicObject())
    {
        for (const auto& prop : object->getProperties())
        {
            const auto key = prop.name.toString();
            bool known = false;
            for (auto* a : allowed)
                if (key == a)
                    known = true;
            if (! known)
            {
                why = "patch field is not allowed in contract v0: " + describe (key);
                return false;
            }
        }
    }

    juce::String id, version;
    if (! wantString (root, "ID", true, kMaxIdChars, id, why))
        return false;
    if (id != manifest.package.id)
    {
        why = "patch ID does not match the manifest";
        return false;
    }
    if (! wantString (root, "version", true, kMaxVersionChars, version, why))
        return false;
    if (version != manifest.package.version)
    {
        why = "patch version does not match the manifest";
        return false;
    }

    juce::String source;
    if (! wantString (root, "source", true, kMaxPathChars, source, why))
        return false;
    juce::String pathWhy;
    if (! isSafeRelativePath (source, pathWhy))
    {
        why = "patch source is not a safe relative path (" + pathWhy + ")";
        return false;
    }
    if (! source.endsWithIgnoreCase (".cmajor"))
    {
        why = "patch source must name a .cmajor file";
        return false;
    }
    sourceOut = source;

    juce::String patchMain;
    if (! wantString (root, "mainProcessor", false, kMaxShortFieldChars, patchMain, why))
        return false;

    if (manifest.hasMainProcessor)
    {
        if (patchMain.isNotEmpty() && patchMain != manifest.package.mainProcessor)
        {
            why = "patch mainProcessor does not match the manifest";
            return false;
        }
        mainProcessorOut = manifest.package.mainProcessor;
    }
    else
    {
        mainProcessorOut = patchMain;
    }

    const auto isInstrument = property (root, "isInstrument");
    if (! isInstrument.isVoid())
    {
        if (! isInstrument.isBool())
        {
            why = "patch isInstrument must be a boolean";
            return false;
        }
        if ((bool) isInstrument != (manifest.package.kind == "instrument"))
        {
            why = "patch isInstrument contradicts the manifest";
            return false;
        }
    }

    return true;
}

//==============================================================================
// install.json

juce::var makeInstallJson (const ModulePackage& p, const juce::String& installedAt,
                           const juce::String& uiSHA256)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("schema", kInstallSchema);

    auto* module = new juce::DynamicObject();
    module->setProperty ("id", p.id);
    module->setProperty ("version", p.version);
    module->setProperty ("name", p.name);
    module->setProperty ("kind", p.kind);
    module->setProperty ("vendor", p.vendor);
    module->setProperty ("license", p.license);
    root->setProperty ("module", juce::var (module));

    auto* pkg = new juce::DynamicObject();
    pkg->setProperty ("sha256", p.packageSHA256);
    pkg->setProperty ("size", p.packageSize);
    root->setProperty ("package", juce::var (pkg));

    auto* source = new juce::DynamicObject();
    source->setProperty ("path", p.sourceRel);
    source->setProperty ("sha256", p.sourceSHA256);
    root->setProperty ("source", juce::var (source));

    auto* manifest = new juce::DynamicObject();
    manifest->setProperty ("path", p.manifestRel);
    manifest->setProperty ("sha256", p.manifestSHA256);
    root->setProperty ("manifest", juce::var (manifest));

    auto* patch = new juce::DynamicObject();
    patch->setProperty ("path", p.patchRel);
    root->setProperty ("patch", juce::var (patch));

    if (p.uiRel.isNotEmpty())
    {
        auto* ui = new juce::DynamicObject();
        ui->setProperty ("path", p.uiRel);
        ui->setProperty ("sha256", uiSHA256);
        root->setProperty ("ui", juce::var (ui));
    }

    root->setProperty ("mainProcessor", p.mainProcessor);

    juce::Array<juce::var> params;
    for (const auto& spec : p.params)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", spec.id);
        o->setProperty ("name", spec.name);
        o->setProperty ("endpoint", spec.endpoint);
        o->setProperty ("min", (double) spec.min);
        o->setProperty ("max", (double) spec.max);
        o->setProperty ("def", (double) spec.def);
        o->setProperty ("unit", spec.unit);
        params.add (juce::var (o));
    }
    root->setProperty ("parameters", params);

    root->setProperty ("installedAt", installedAt);

    return juce::var (root);
}

bool parseInstallJson (const juce::File& dir, const juce::String& expectedId,
                       const juce::String& expectedVersion, const juce::String& expectedDigest,
                       InstalledModule& out, juce::String& why)
{
    juce::MemoryBlock bytes;
    if (! readFileBounded (dir.getChildFile ("install.json"), kMaxInstallJsonBytes, bytes, why))
        return false;

    juce::String text;
    if (! blockToUtf8 (bytes, text, why))
        return false;

    juce::var root;
    if (! parseJsonDocument (text, root, why))
        return false;

    juce::String schema;
    if (! wantString (root, "schema", true, 64, schema, why))
        return false;
    if (schema != kInstallSchema)
    {
        why = "unsupported install.json schema";
        return false;
    }

    juce::var module;
    if (! wantObject (root, "module", true, module, why))
        return false;

    auto& pkg = out.package;
    if (! wantString (module, "id", true, kMaxIdChars, pkg.id, why))
        return false;
    if (! wantString (module, "version", true, kMaxVersionChars, pkg.version, why))
        return false;
    if (! wantString (module, "name", true, kMaxNameChars, pkg.name, why))
        return false;
    if (! wantString (module, "kind", true, 32, pkg.kind, why))
        return false;
    if (! wantString (module, "vendor", false, kMaxNameChars, pkg.vendor, why))
        return false;
    if (! wantString (module, "license", false, kMaxShortFieldChars, pkg.license, why))
        return false;

    if (pkg.id != expectedId)
    {
        why = "install.json module.id does not match the directory";
        return false;
    }
    if (pkg.version != expectedVersion)
    {
        why = "install.json version does not match the directory";
        return false;
    }
    if (pkg.kind != "effect" && pkg.kind != "instrument")
    {
        why = "install.json module.kind is invalid";
        return false;
    }

    juce::var pkgVar;
    if (! wantObject (root, "package", true, pkgVar, why))
        return false;
    if (! wantString (pkgVar, "sha256", true, 64, pkg.packageSHA256, why))
        return false;
    if (! isCanonicalHex64 (pkg.packageSHA256))
    {
        why = "install.json package.sha256 is not a canonical digest";
        return false;
    }
    if (pkg.packageSHA256 != expectedDigest)
    {
        why = "install.json digest does not match the directory name";
        return false;
    }
    double packageSize = 0.0;
    if (! wantNumber (pkgVar, "size", true, packageSize, why))
        return false;
    if (packageSize <= 0.0 || packageSize > (double) kMaxPackageBytes)
    {
        why = "install.json package.size is out of bounds";
        return false;
    }
    pkg.packageSize = (juce::int64) packageSize;

    auto pathField = [&] (const char* key, bool required, juce::String& rel, juce::File& file) -> bool
    {
        juce::var obj;
        if (! wantObject (root, key, required, obj, why))
            return false;
        if (obj.isVoid())
        {
            rel = juce::String();
            file = juce::File();
            return true;
        }
        juce::String stored;
        if (! wantString (obj, "path", true, kMaxPathChars, rel, why))
            return false;
        juce::String pathWhy;
        if (! isSafeRelativePath (rel, pathWhy))
        {
            why = juce::String (key) + " path is not safe (" + pathWhy + ")";
            return false;
        }
        if (! resolveInside (dir, rel, file))
        {
            why = juce::String (key) + " path does not resolve inside the install root";
            return false;
        }
        if (! file.existsAsFile())
        {
            why = juce::String (key) + " file is missing";
            return false;
        }
        if (std::strcmp (key, "ui") == 0)
        {
            if (! wantString (obj, "sha256", true, 64, stored, why))
                return false;
            if (! isCanonicalHex64 (stored))
            {
                why = "ui sha256 is not a canonical digest";
                return false;
            }
        }
        else if (std::strcmp (key, "manifest") == 0)
        {
            if (! wantString (obj, "sha256", true, 64, stored, why))
                return false;
            pkg.manifestSHA256 = stored;
        }
        else if (std::strcmp (key, "source") == 0)
        {
            if (! wantString (obj, "sha256", true, 64, stored, why))
                return false;
            pkg.sourceSHA256 = stored;
        }
        return true;
    };

    if (! pathField ("source", true, pkg.sourceRel, out.sourceFile))
        return false;
    if (! pathField ("manifest", true, pkg.manifestRel, out.manifestFile))
        return false;
    if (! pathField ("patch", true, pkg.patchRel, out.patchFile))
        return false;
    if (! pathField ("ui", false, pkg.uiRel, out.uiFile))
        return false;

    if (! isCanonicalHex64 (pkg.sourceSHA256) || ! isCanonicalHex64 (pkg.manifestSHA256))
    {
        why = "install.json source or manifest digest is malformed";
        return false;
    }

    if (! wantString (root, "mainProcessor", false, kMaxShortFieldChars, pkg.mainProcessor, why))
        return false;

    pkg.params.clear();
    const auto params = property (root, "parameters");
    if (! params.isVoid())
    {
        if (! params.isArray())
        {
            why = "install.json parameters must be an array";
            return false;
        }
        const auto* arr = params.getArray();
        if (arr->size() > kMaxParams)
        {
            why = "install.json declares too many parameters";
            return false;
        }
        for (const auto& item : *arr)
        {
            if (! item.isObject())
            {
                why = "install.json parameter entry must be an object";
                return false;
            }
            ModuleParamSpec spec;
            if (! wantString (item, "id", true, kMaxShortFieldChars, spec.id, why))
                return false;
            if (! wantString (item, "name", false, kMaxShortFieldChars, spec.name, why))
                return false;
            if (! wantString (item, "endpoint", false, kMaxShortFieldChars, spec.endpoint, why))
                return false;
            if (! wantString (item, "unit", false, kMaxUnitChars, spec.unit, why))
                return false;
            double minV = 0.0, maxV = 1.0, defV = 0.0;
            if (! wantNumber (item, "min", true, minV, why))
                return false;
            if (! wantNumber (item, "max", true, maxV, why))
                return false;
            if (! wantNumber (item, "def", true, defV, why))
                return false;
            if (! (minV < maxV) || defV < minV || defV > maxV)
            {
                why = "install.json parameter range is invalid";
                return false;
            }
            spec.min = (float) minV;
            spec.max = (float) maxV;
            spec.def = (float) defV;
            pkg.params.push_back (spec);
        }
    }

    if (! wantString (root, "installedAt", false, kMaxInstalledAtChars, out.installedAt, why))
        return false;

    out.root = dir;
    return true;
}

//==============================================================================
// Archive entry model

struct ArchiveEntry
{
    juce::String path;
    int index = -1;
    juce::int64 size = 0;
};

struct Inventory
{
    std::vector<ArchiveEntry> files;
    std::map<std::string, int> indexByPath;   // path -> zip entry index
    std::set<std::string> fileKeys;           // case-folded file paths
    std::set<std::string> dirKeys;            // case-folded directory paths
    int manifestIndex = -1;
    juce::String manifestPath;
    juce::String uiPath;
    bool hasUi = false;
    juce::int64 totalBytes = 0;
};

bool collectInventory (juce::ZipFile& zip, Inventory& inv, juce::String& why)
{
    const int count = zip.getNumEntries();
    if (count <= 0)
    {
        why = "archive holds no entries";
        return false;
    }
    if (count > kMaxEntries)
    {
        why = "archive holds too many entries";
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        const auto* entry = zip.getEntry (i);
        if (entry == nullptr)
        {
            why = "archive entry metadata is missing";
            return false;
        }
        if (entry->isSymbolicLink)
        {
            why = "symbolic link entry refused: " + describe (entry->filename);
            return false;
        }

        const bool isDirectory = entry->filename.endsWithChar ('/');
        const auto path = isDirectory ? entry->filename.dropLastCharacters (1) : entry->filename;

        juce::String pathWhy;
        if (! isSafeRelativePath (path, pathWhy))
        {
            why = "unsafe entry path " + describe (entry->filename) + " (" + pathWhy + ")";
            return false;
        }

        if (entry->uncompressedSize < 0 || entry->uncompressedSize > kMaxEntryBytes)
        {
            why = "entry exceeds 256 MiB: " + describe (path);
            return false;
        }
        if (isDirectory && entry->uncompressedSize != 0)
        {
            why = "directory entry carries data: " + describe (path);
            return false;
        }

        inv.totalBytes += entry->uncompressedSize;
        if (inv.totalBytes > kMaxTotalExtracted)
        {
            why = "archive expands beyond 2 GiB";
            return false;
        }

        const auto key = foldCase (path).toStdString();
        if (inv.fileKeys.count (key) != 0 || inv.dirKeys.count (key) != 0)
        {
            why = "duplicate archive path after case folding: " + describe (path);
            return false;
        }

        if (isDirectory)
        {
            inv.dirKeys.insert (key);
            continue;
        }

        if (! isAllowedSuffix (path))
        {
            why = "file type is not allowed: " + describe (path);
            return false;
        }

        inv.fileKeys.insert (key);
        inv.indexByPath.emplace (path.toStdString(), i);
        inv.files.push_back ({ path, i, entry->uncompressedSize });

        const auto base = path.fromLastOccurrenceOf ("/", false, false);
        if (base.equalsIgnoreCase (kManifestName))
        {
            if (path != kManifestName)
            {
                why = "openvst.json must sit at the archive root";
                return false;
            }
            if (inv.manifestIndex >= 0)
            {
                why = "archive holds more than one openvst.json";
                return false;
            }
            inv.manifestIndex = i;
            inv.manifestPath = path;
        }
        if (base.equalsIgnoreCase ("ui.json"))
        {
            if (inv.hasUi)
            {
                why = "archive holds more than one ui.json";
                return false;
            }
            inv.hasUi = true;
            inv.uiPath = path;
        }
    }

    if (inv.manifestIndex < 0)
    {
        why = "archive has no openvst.json at its root";
        return false;
    }

    // A file entry cannot also be a directory prefix of another entry: the
    // extraction would fail halfway and leave a partial tree.
    for (const auto& f : inv.files)
    {
        const auto parts = splitPath (f.path);
        juce::String prefix;
        for (int i = 0; i < parts.size() - 1; ++i)
        {
            prefix = prefix.isEmpty() ? parts[i] : prefix + "/" + parts[i];
            if (inv.fileKeys.count (foldCase (prefix).toStdString()) != 0)
            {
                why = "path is both a file and a directory: " + describe (prefix);
                return false;
            }
        }
    }

    return true;
}

//==============================================================================
// Extraction

bool extractEntry (juce::ZipFile& zip, const ArchiveEntry& entry,
                   const juce::File& stagingRoot, juce::String& shaOut, juce::String& why)
{
    juce::File dest;
    if (! resolveInside (stagingRoot, entry.path, dest))
    {
        why = "entry path does not resolve inside the staging root: " + describe (entry.path);
        return false;
    }
    if (dest.exists())
    {
        why = "duplicate output path: " + describe (entry.path);
        return false;
    }
    if (! ensureDirectory (dest.getParentDirectory()))
    {
        why = "cannot create directory for: " + describe (entry.path);
        return false;
    }

    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (entry.index));
    if (in == nullptr)
    {
        why = "entry stream cannot be opened: " + describe (entry.path);
        return false;
    }

    {
        juce::FileOutputStream out (dest, (size_t) kChunkSize);
        if (out.failedToOpen())
        {
            why = "cannot write entry: " + describe (entry.path);
            return false;
        }

        juce::HeapBlock<char> buffer ((size_t) kChunkSize);
        juce::int64 written = 0;
        for (;;)
        {
            const int n = in->read (buffer.getData(), kChunkSize);
            if (n < 0)
            {
                why = "entry stream failed: " + describe (entry.path);
                dest.deleteFile();
                return false;
            }
            if (n == 0)
                break;

            written += n;
            // The declared size is the only budget: a stream that keeps
            // producing data is refused before it can fill the disk.
            if (written > entry.size)
            {
                why = "entry holds more than its declared size: " + describe (entry.path)
                    + " declared=" + juce::String (entry.size) + " got=" + juce::String (written)
                    + " chunk=" + juce::String (n);
                out.flush();
                dest.deleteFile();
                return false;
            }
            if (! out.write (buffer.getData(), n))
            {
                why = "write failed: " + describe (entry.path);
                dest.deleteFile();
                return false;
            }
        }

        out.flush();
        if (written != entry.size)
        {
            why = "entry ended before its declared size: " + describe (entry.path);
            dest.deleteFile();
            return false;
        }
    }

    syncPath (dest, false);

    if (dest.getSize() != entry.size)
    {
        why = "extracted size differs from the declared size: " + describe (entry.path);
        dest.deleteFile();
        return false;
    }
    shaOut = sha256FileImpl (dest);
    if (shaOut.length() != 64)
    {
        why = "cannot hash extracted entry: " + describe (entry.path);
        return false;
    }
    return true;
}

//==============================================================================
// Staging cleanup

void removeStaleStaging (const juce::File& stagingDir)
{
    if (! stagingDir.isDirectory() || stagingDir.isSymbolicLink())
        return;

    const auto now = juce::Time::getCurrentTime().toMilliseconds();
    for (const auto& child : stagingDir.findChildFiles (juce::File::findDirectories, false, "*"))
    {
        if (child.isSymbolicLink())
            continue;
        const auto modified = child.getLastModificationTime().toMilliseconds();
        if (modified > 0 && now - modified > kStagingStaleMs)
            child.deleteRecursively();
    }
}

bool splitVersionDirectory (const juce::String& name, juce::String& version, juce::String& digest)
{
    if (name.length() < 66)
        return false;
    const auto candidate = name.getLastCharacters (64);
    if (! isCanonicalHex64 (candidate))
        return false;
    if (name[name.length() - 65] != '-')
        return false;
    version = name.dropLastCharacters (65);
    if (! isSemVer (version))
        return false;
    digest = candidate;
    return true;
}

//==============================================================================
// Download helpers

bool isHttpUrl (const juce::URL& url)
{
    const auto scheme = url.getScheme().toLowerCase();
    return scheme == "http" || scheme == "https";
}

} // namespace

//==============================================================================
// ModuleStore

ModuleStore::ModuleStore (juce::File root)
    : root_ (std::move (root))
{
}

juce::String ModuleStore::sha256File (const juce::File& file)
{
    return sha256FileImpl (file);
}

ModuleStore::InstallResult ModuleStore::install (const juce::File& zipFile,
                                                 const juce::String& expectedSHA256,
                                                 std::function<bool(double)> progress)
{
    InstallResult result;
    const auto fail = [&result] (const juce::String& why)
    {
        result.ok = false;
        result.error = why;
        return result;
    };

    const juce::File store (root_.getFullPathName());
    if (! ensureDirectory (store))
        return fail ("cannot create the module store directory");

    const auto stagingParent = store.getChildFile (kStagingDirName);
    if (! ensureDirectory (stagingParent))
        return fail ("cannot create the staging directory");

    // Best effort, bounded by age: a crashed install leaves a directory that
    // must not accumulate.
    removeStaleStaging (stagingParent);

    if (! zipFile.existsAsFile() || zipFile.isSymbolicLink())
        return fail ("package file is missing");
    const auto zipSize = zipFile.getSize();
    if (zipSize <= 0)
        return fail ("package file is empty");
    if (zipSize > kMaxPackageBytes)
        return fail ("package exceeds 512 MiB");

    const auto digest = sha256FileImpl (zipFile);
    if (digest.length() != 64)
        return fail ("cannot hash the package file");
    if (expectedSHA256.isNotEmpty())
    {
        if (! isHex64 (expectedSHA256))
            return fail ("expected digest is not a SHA-256 hex string");
        if (! digest.equalsIgnoreCase (expectedSHA256))
            return fail ("package digest does not match the expected SHA-256");
    }

    juce::ZipFile zip (zipFile);
    Inventory inventory;
    juce::String why;
    if (! collectInventory (zip, inventory, why))
        return fail (why);

    juce::MemoryBlock manifestBytes;
    if (! readEntryBounded (zip, inventory.manifestIndex, kMaxManifestBytes, manifestBytes, why))
        return fail ("openvst.json: " + why);
    const auto manifestDigest = sha256Of (manifestBytes);

    ParsedManifest manifest;
    if (! parseManifest (manifestBytes, manifest, why))
        return fail ("openvst.json: " + why);

    auto& package = manifest.package;
    package.packageSHA256 = digest;
    package.packageSize = zipSize;
    package.manifestRel = inventory.manifestPath;
    package.manifestSHA256 = manifestDigest;

    const auto patchIt = inventory.indexByPath.find (manifest.patchRel.toStdString());
    if (patchIt == inventory.indexByPath.end())
        return fail ("runtime.patch is not present in the archive");

    juce::MemoryBlock patchBytes;
    if (! readEntryBounded (zip, patchIt->second, kMaxPatchBytes, patchBytes, why))
        return fail ("patch: " + why);

    juce::String mainProcessor, declaredSource;
    if (! parsePatch (patchBytes, manifest, mainProcessor, declaredSource, why))
        return fail ("patch: " + why);
    package.mainProcessor = mainProcessor;
    package.patchRel = manifest.patchRel;

    // The patch names its source relative to the patch file, and the package
    // validator enforces the same rule. Resolve it here, once, into the
    // archive-relative path that is extracted and recorded. Nothing is
    // normalised away: a source that does not land under patch/ is refused.
    const int patchSlash = manifest.patchRel.lastIndexOfChar ('/');
    const juce::String patchDir = patchSlash >= 0 ? manifest.patchRel.substring (0, patchSlash + 1)
                                                  : juce::String();
    const juce::String sourceRel = patchDir + declaredSource;
    if (! sourceRel.startsWith ("patch/"))
        return fail ("patch source must live under patch/");

    const auto sourceIt = inventory.indexByPath.find (sourceRel.toStdString());
    if (sourceIt == inventory.indexByPath.end())
        return fail ("patch source is not present in the archive");
    package.sourceRel = sourceRel;

    // Hashed before publication so the idempotency check below can compare
    // identities without extracting anything, and compared again after
    // extraction so the published bytes cannot drift from the recorded digest.
    package.sourceSHA256 = hashZipEntry (zip, sourceIt->second);
    if (! isCanonicalHex64 (package.sourceSHA256))
        return fail ("cannot hash the module source");

    for (const auto& resource : manifest.resources)
        if (inventory.indexByPath.find (resource.first.toStdString()) == inventory.indexByPath.end())
            return fail ("declared resource is not present in the archive: " + describe (resource.first));

    // The manifest may declare the interface document; a package that predates
    // the declaration is recognised by its single ui.json. Two different
    // answers are an ambiguity, never a choice.
    if (manifest.hasUi)
    {
        const auto uiIt = inventory.indexByPath.find (manifest.uiRel.toStdString());
        if (uiIt == inventory.indexByPath.end())
            return fail ("declared ui document is not present in the archive");
        if (inventory.hasUi && inventory.uiPath != manifest.uiRel)
            return fail ("archive holds more than one ui document");
        if (zip.getEntry (uiIt->second)->uncompressedSize > kMaxUiBytes)
            return fail ("ui document exceeds 64 KiB");
        package.uiRel = manifest.uiRel;
    }
    else if (inventory.hasUi)
    {
        const auto uiIt = inventory.indexByPath.find (inventory.uiPath.toStdString());
        if (uiIt == inventory.indexByPath.end())
            return fail ("ui.json is not present in the archive");
        if (zip.getEntry (uiIt->second)->uncompressedSize > kMaxUiBytes)
            return fail ("ui.json exceeds 64 KiB");
        package.uiRel = inventory.uiPath;
    }

    ModuleLock lock;
    if (! acquireModuleLock (store.getChildFile (kLocksDirName), package.id, lock, why))
        return fail (why);

    const auto finalDir = store.getChildFile (package.id)
                               .getChildFile (package.version + "-" + digest);

    // A version is immutable: two different byte streams must never share one
    // (id, version), or nothing could say which one a project refers to. A
    // changed package is a new version, published beside the old one.
    if (const auto idDir = finalDir.getParentDirectory(); idDir.isDirectory())
    {
        for (const auto& sibling : idDir.findChildFiles (juce::File::findDirectories, false, "*"))
        {
            if (sibling.isSymbolicLink())
                continue;
            juce::String siblingVersion, siblingDigest;
            if (! splitVersionDirectory (sibling.getFileName(), siblingVersion, siblingDigest))
                continue;
            if (siblingVersion == package.version && siblingDigest != digest)
                return fail ("version " + package.version + " is already installed with different bytes");
        }
    }

    // Idempotent republish of exactly the same bytes.
    if (finalDir.exists())
    {
        InstalledModule existing;
        if (! parseInstallJson (finalDir, package.id, package.version, digest, existing, why))
            return fail ("existing install directory is invalid: " + why);
        if (! existing.package.sourceSHA256.equalsIgnoreCase (package.sourceSHA256))
            return fail ("existing install directory holds different source bytes");
        result.ok = true;
        result.installed = existing;
        return result;
    }

    const auto stagingDir = stagingParent.getChildFile (uniqueStagingName (digest));
    if (stagingDir.exists())
        return fail ("staging directory already exists");
    if (! stagingDir.createDirectory())
        return fail ("cannot create the staging directory");
    DirectoryGuard stagingGuard (stagingDir);

    std::map<std::string, juce::String> hashes;
    const auto total = (double) juce::jmax ((size_t) 1, inventory.files.size());
    size_t done = 0;

    for (const auto& entry : inventory.files)
    {
        if (progress && ! progress (0.05 + 0.85 * (double) done / total))
            return fail ("install cancelled");

        juce::String sha;
        if (! extractEntry (zip, entry, stagingDir, sha, why))
            return fail (why);
        hashes.emplace (entry.path.toStdString(), sha);
        ++done;
    }

    const auto sourceHash = hashes.find (package.sourceRel.toStdString());
    if (sourceHash == hashes.end())
        return fail ("source file was not extracted");
    if (sourceHash->second != package.sourceSHA256)
        return fail ("source bytes changed during extraction");

    const auto manifestHash = hashes.find (package.manifestRel.toStdString());
    if (manifestHash == hashes.end())
        return fail ("manifest was not extracted");
    if (manifestHash->second != package.manifestSHA256)
        return fail ("manifest bytes changed during extraction");

    for (const auto& resource : manifest.resources)
    {
        const auto it = hashes.find (resource.first.toStdString());
        if (it == hashes.end())
            return fail ("declared resource was not extracted");
        if (! it->second.equalsIgnoreCase (resource.second))
            return fail ("resource digest mismatch: " + describe (resource.first));
    }

    juce::String uiHash;
    if (package.uiRel.isNotEmpty())
    {
        const auto uiIt = hashes.find (package.uiRel.toStdString());
        if (uiIt == hashes.end())
            return fail ("ui.json was not extracted");
        uiHash = uiIt->second;
    }

    // install.json goes in last so a directory without it is recognisably
    // incomplete.
    {
        const auto text = juce::JSON::toString (makeInstallJson (package,
                                                                 juce::Time::getCurrentTime().toISO8601 (true),
                                                                 uiHash),
                                                false) + "\n";
        if (text.length() > kMaxInstallJsonBytes)
            return fail ("install.json would exceed 64 KiB");

        const auto file = stagingDir.getChildFile ("install.json");
        juce::FileOutputStream out (file, 4096);
        if (out.failedToOpen())
            return fail ("cannot write install.json");
        const auto utf8 = text.toRawUTF8();
        if (! out.write (utf8, (int) std::strlen (utf8)))
            return fail ("cannot write install.json");
        out.flush();
    }

    syncPath (stagingDir.getChildFile ("install.json"), false);
    syncPath (stagingDir, true);

    if (progress && ! progress (0.95))
        return fail ("install cancelled");

    if (! ensureDirectory (finalDir.getParentDirectory()))
        return fail ("cannot create the module directory");

    if (finalDir.exists())
    {
        // Another writer published while this install was working. Accept it
        // only if it is byte-identical, exactly as above.
        InstalledModule existing;
        if (! parseInstallJson (finalDir, package.id, package.version, digest, existing, why))
            return fail ("existing install directory is invalid: " + why);
        if (! existing.package.sourceSHA256.equalsIgnoreCase (package.sourceSHA256))
            return fail ("existing install directory holds different source bytes");
        result.ok = true;
        result.installed = existing;
        return result;
    }

    if (! stagingDir.moveFileTo (finalDir))
        return fail ("cannot publish the installed module");

    stagingGuard.release();
    syncPath (finalDir.getParentDirectory(), true);

    InstalledModule installed;
    if (! parseInstallJson (finalDir, package.id, package.version, digest, installed, why))
        return fail ("published install directory failed re-reading: " + why);

    if (progress && ! progress (1.0))
        return fail ("install cancelled");

    result.ok = true;
    result.installed = installed;
    return result;
}

std::vector<InstalledModule> ModuleStore::scan() const
{
    problems_.clear();

    const juce::File store (root_.getFullPathName());
    if (! store.isDirectory() || store.isSymbolicLink())
        return {};

    removeStaleStaging (store.getChildFile (kStagingDirName));

    std::vector<InstalledModule> found;

    for (const auto& idDir : store.findChildFiles (juce::File::findDirectories, false, "*"))
    {
        const auto idName = idDir.getFileName();
        if (idName == kStagingDirName || idName == kLocksDirName)
            continue;
        if (idDir.isSymbolicLink())
        {
            problems_.add ("skipped symlinked module directory: " + describe (idName));
            continue;
        }
        if (! isReverseDnsId (idName))
        {
            problems_.add ("skipped unrecognised store entry: " + describe (idName));
            continue;
        }

        for (const auto& versionDir : idDir.findChildFiles (juce::File::findDirectories, false, "*"))
        {
            const auto dirName = versionDir.getFileName();
            if (versionDir.isSymbolicLink())
            {
                problems_.add ("skipped symlinked version directory: " + describe (dirName));
                continue;
            }

            juce::String version, versionDigest;
            if (! splitVersionDirectory (dirName, version, versionDigest))
            {
                problems_.add ("skipped malformed version directory: " + describe (dirName));
                continue;
            }

            InstalledModule module;
            juce::String why;
            if (! parseInstallJson (versionDir, idName, version, versionDigest, module, why))
            {
                problems_.add ("skipped unusable install: " + describe (idName + "/" + dirName)
                               + " (" + why + ")");
                continue;
            }

            if (! module.sourceFile.existsAsFile())
            {
                problems_.add ("skipped install with a missing source: " + describe (dirName));
                continue;
            }

            found.push_back (std::move (module));
        }
    }

    std::sort (found.begin(), found.end(),
               [] (const InstalledModule& a, const InstalledModule& b)
               {
                   if (a.package.id != b.package.id) return a.package.id < b.package.id;
                   if (a.package.version != b.package.version) return a.package.version < b.package.version;
                   return a.package.packageSHA256 < b.package.packageSHA256;
               });

    return found;
}

bool ModuleStore::remove (const InstalledModule& module)
{
    const auto& pkg = module.package;
    if (! isReverseDnsId (pkg.id) || ! isSemVer (pkg.version) || ! isCanonicalHex64 (pkg.packageSHA256))
        return false;

    const juce::File store (root_.getFullPathName());
    if (! store.isDirectory() || store.isSymbolicLink())
        return false;

    // The path is rebuilt from the identity fields, never taken from the
    // caller's File, and it must equal what the caller pointed at.
    const auto expected = store.getChildFile (pkg.id).getChildFile (pkg.version + "-" + pkg.packageSHA256);
    if (module.root.getFullPathName() != expected.getFullPathName())
        return false;
    if (! expected.isAChildOf (store) || expected == store)
        return false;
    if (expected.isSymbolicLink())
        return false;

    InstalledModule recorded;
    juce::String why;
    if (! parseInstallJson (expected, pkg.id, pkg.version, pkg.packageSHA256, recorded, why))
        return false;

    if (! expected.deleteRecursively())
        return false;

    // The id directory is removed only when empty: another version may live
    // beside this one.
    auto idDir = expected.getParentDirectory();
    if (idDir.isDirectory() && idDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
        idDir.deleteFile();

    return true;
}

ModuleStore::DownloadResult ModuleStore::download (const juce::URL& url, const juce::File& partFile,
                                                   const juce::String& expectedSHA256,
                                                   std::function<bool(double)> progress)
{
    DownloadResult result;
    const auto fail = [&result] (const juce::String& why)
    {
        result.ok = false;
        result.error = why;
        return result;
    };

    if (! isHttpUrl (url))
        return fail ("download URL must be http or https");
    if (! isHex64 (expectedSHA256))
        return fail ("expected digest is not a SHA-256 hex string");
    if (partFile.getFileName().isEmpty())
        return fail ("part file path is empty");

    if (! ensureDirectory (partFile.getParentDirectory()))
        return fail ("cannot create the download directory");

    juce::int64 existing = 0;
    if (partFile.existsAsFile() && ! partFile.isSymbolicLink())
        existing = partFile.getSize();
    if (existing < 0 || existing > kMaxPackageBytes)
    {
        partFile.deleteFile();
        existing = 0;
    }

    bool restartedAfterRangeError = false;
    juce::int64 finalSize = 0;

    for (;;)
    {
        juce::String headers;
        if (existing > 0)
            headers = "Range: bytes=" + juce::String (existing) + "-\r\n";

        int statusCode = 0;
        // InputStreamOptions holds a const member, so it is built once and
        // never assigned; an empty extra-headers string appends nothing.
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (15000)
                                 .withNumRedirectsToFollow (5)
                                 .withStatusCode (&statusCode)
                                 .withExtraHeaders (headers);

        auto stream = url.createInputStream (options);
        if (stream == nullptr)
            return fail ("network request failed");

        if (existing > 0 && statusCode == 206)
        {
            // Resume accepted: append to the verified prefix.
        }
        else if (statusCode == 200)
        {
            existing = 0;
        }
        else if (statusCode == 416 && ! restartedAfterRangeError)
        {
            // The server no longer agrees with the prefix; start over once.
            restartedAfterRangeError = true;
            existing = 0;
            partFile.deleteFile();
            continue;
        }
        else
        {
            return fail ("unexpected HTTP status " + juce::String (statusCode));
        }

        const auto remaining = stream->getTotalLength();
        juce::int64 expectedTotal = remaining > 0 ? existing + remaining : -1;

        juce::FileOutputStream out (partFile, (size_t) kChunkSize);
        if (out.failedToOpen())
            return fail ("cannot open the part file for writing");

        if (existing > 0)
        {
            out.setPosition (existing);
        }
        else
        {
            out.setPosition (0);
            out.truncate();
        }

        juce::HeapBlock<char> buffer ((size_t) kChunkSize);
        juce::int64 written = existing;

        for (;;)
        {
            const int n = stream->read (buffer.getData(), kChunkSize);
            if (n < 0)
            {
                out.flush();
                return fail ("download stream failed");
            }
            if (n == 0)
                break;

            written += n;
            if (written > kMaxPackageBytes)
            {
                out.flush();
                partFile.deleteFile();
                return fail ("package exceeds 512 MiB");
            }
            if (! out.write (buffer.getData(), n))
            {
                out.flush();
                return fail ("write to the part file failed");
            }

            if (progress)
            {
                const double fraction = expectedTotal > 0
                                          ? juce::jlimit (0.0, 1.0, (double) written / (double) expectedTotal)
                                          : 0.0;
                if (! progress (fraction))
                {
                    out.flush();
                    return fail ("download cancelled");
                }
            }
        }

        out.flush();
        out.setPosition (written);
        finalSize = written;
        break;
    }

    if (finalSize <= 0)
        return fail ("downloaded file is empty");

    const auto digest = sha256FileImpl (partFile);
    if (! digest.equalsIgnoreCase (expectedSHA256))
    {
        partFile.deleteFile();
        return fail ("downloaded package digest does not match the expected SHA-256");
    }

    result.ok = true;
    result.bytes = finalSize;
    return result;
}

} // namespace openvst
