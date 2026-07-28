#ifndef SPROUT_RUNTIME_RSB_INDEX_H
#define SPROUT_RUNTIME_RSB_INDEX_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace sprout {

/* Index of every file packed inside the game's main RSB (the .obb).
 *
 * PvZ2 reaches its resources through an "ASSET:<path>" scheme that, on a real
 * device, AndroidGameApp.Resources_GetAssetFileInfo answers by calling
 * AssetManager.openFd() and handing native code back
 * (APK path, startOffset, length) -- assets are stored uncompressed in the
 * zip, so native just opens the APK and reads at that offset. This APK ships
 * no assets/ folder at all, so that lookup can never succeed here; everything
 * lives in the .obb instead. Because non-image entries are stored raw inside
 * the RSB, we can answer the exact same contract with (obb path, offset,
 * length) and no extraction step.
 *
 * RSB v4 layout: 0x28 rsgNumber, 0x2C rsgInfoBegin, 0x30 rsgInfoEachLength.
 * Each RSG info record holds the package name at +0, its absolute offset at
 * +0x80 and its size at +0x84. Each RSG is a self-contained "pgsr" package
 * whose payload starts at header +0x14, with a name list at +0x4C of length
 * +0x48. Names in that list use the PopCap 4-byte-per-node trie (byte0 =
 * character, 0 terminates a name; bytes 1..3 = uint24 sibling-branch offset
 * in 4-byte units), and each terminator is followed by
 * [flag][offset][size], offset being relative to the payload start. flag == 1
 * marks a texture, which carries 20 extra bytes of image info and whose
 * payload is zlib-compressed -- those are indexed but must not be served
 * raw. */
class RsbIndex {
public:
    struct Entry {
        uint64_t offset = 0; /* absolute byte offset inside the .obb */
        uint32_t size = 0;
        bool compressed = false; /* texture payloads (flag == 1) are zlib streams */
    };

    /* Parses the whole index once. Returns false (and leaves the index empty)
     * if the file is missing or is not an RSB. */
    bool load(const std::string &obb_path);

    bool loaded() const { return loaded_; }
    size_t size() const { return entries_.size(); }
    const std::string &path() const { return obb_path_; }

    /* Looks up a guest resource path (any case, '/' or '\\' separators, with
     * or without the "ASSET:" prefix). Returns nullptr if absent. */
    const Entry *find(const std::string &guest_path) const;

    /* The exact normalisation `find` applies, exposed for logging: strips
     * "ASSET:", uppercases, and converts '/' to '\\' to match how names are
     * stored in the RSB. Mirrors AndroidAssetUtils.GetAssetNameFromFileName
     * except for its ".smf" suffixing, which only applies to APK assets. */
    static std::string normalize(const std::string &guest_path);

private:
    bool loaded_ = false;
    std::string obb_path_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace sprout

#endif
