#include <sprout/runtime/rsb_index.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sprout {

namespace {

uint32_t rd32(const std::vector<uint8_t> &b, size_t off) {
    if (off + 4 > b.size()) return 0;
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

uint32_t rd24(const std::vector<uint8_t> &b, size_t off) {
    if (off + 3 > b.size()) return 0;
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16);
}

}  // namespace

std::string RsbIndex::normalize(const std::string &guest_path) {
    std::string s = guest_path;
    static const char kScheme[] = "ASSET:";
    if (s.compare(0, sizeof(kScheme) - 1, kScheme) == 0) s.erase(0, sizeof(kScheme) - 1);
    for (char &c : s) {
        if (c == '/') c = '\\';
        c = (char)std::toupper((unsigned char)c);
    }
    return s;
}

bool RsbIndex::load(const std::string &obb_path) {
    entries_.clear();
    loaded_ = false;
    obb_path_ = obb_path;

    FILE *f = std::fopen(obb_path.c_str(), "rb");
    if (!f) {
        std::printf("pvz2: [rsb-index] cannot open '%s'\n", obb_path.c_str());
        return false;
    }

    std::vector<uint8_t> head(0x100);
    if (std::fread(head.data(), 1, head.size(), f) != head.size() ||
        std::memcmp(head.data(), "1bsr", 4) != 0) {
        std::printf("pvz2: [rsb-index] '%s' is not an RSB\n", obb_path.c_str());
        std::fclose(f);
        return false;
    }

    const uint32_t rsg_number = rd32(head, 0x28);
    const uint32_t rsg_info_begin = rd32(head, 0x2C);
    const uint32_t rsg_info_each = rd32(head, 0x30);
    if (rsg_number == 0 || rsg_info_each < 0x88) {
        std::printf("pvz2: [rsb-index] implausible RSG table (n=%u each=%u)\n", rsg_number, rsg_info_each);
        std::fclose(f);
        return false;
    }

    std::vector<uint8_t> info((size_t)rsg_number * rsg_info_each);
    if (std::fseek(f, (long)rsg_info_begin, SEEK_SET) != 0 ||
        std::fread(info.data(), 1, info.size(), f) != info.size()) {
        std::printf("pvz2: [rsb-index] truncated RSG info table\n");
        std::fclose(f);
        return false;
    }

    uint32_t skipped = 0;
    std::vector<uint8_t> rsg;
    for (uint32_t i = 0; i < rsg_number; ++i) {
        const size_t rec = (size_t)i * rsg_info_each;
        const uint32_t rsg_off = rd32(info, rec + 0x80);
        const uint32_t rsg_size = rd32(info, rec + 0x84);
        if (rsg_off == 0 || rsg_size == 0) continue;

        /* Only the header and the name list are needed, never the payload --
         * but the list sits at an arbitrary offset, so read up to its end. */
        std::vector<uint8_t> hdr(0x60);
        if (std::fseek(f, (long)rsg_off, SEEK_SET) != 0 ||
            std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size() ||
            std::memcmp(hdr.data(), "pgsr", 4) != 0) {
            ++skipped;
            continue;
        }
        const uint32_t data_off = rd32(hdr, 0x14);
        const uint32_t list_len = rd32(hdr, 0x48);
        const uint32_t list_beg = rd32(hdr, 0x4C);
        if (list_len == 0 || (uint64_t)list_beg + list_len > rsg_size) {
            ++skipped;
            continue;
        }

        rsg.assign(list_len, 0);
        if (std::fseek(f, (long)(rsg_off + list_beg), SEEK_SET) != 0 ||
            std::fread(rsg.data(), 1, rsg.size(), f) != rsg.size()) {
            ++skipped;
            continue;
        }

        /* Walk the trie. `stack` holds (name-so-far, position) pairs for the
         * sibling branches still to explore. */
        std::vector<std::pair<std::string, uint32_t>> stack;
        std::string name;
        uint32_t pos = 0;
        while (pos + 4 <= list_len) {
            const uint8_t ch = rsg[pos];
            const uint32_t branch = rd24(rsg, pos + 1);
            if (ch == 0) {
                const uint32_t flag = rd32(rsg, pos + 4);
                const uint32_t off = rd32(rsg, pos + 8);
                const uint32_t size = rd32(rsg, pos + 12);
                if (!name.empty()) {
                    Entry e;
                    e.offset = (uint64_t)rsg_off + data_off + off;
                    e.size = size;
                    e.compressed = (flag == 1);
                    entries_.emplace(name, e);
                }
                pos += 16 + (flag == 1 ? 20 : 0); /* textures carry image info */
                if (stack.empty()) {
                    if (name.empty()) break;
                    name.clear();
                    continue;
                }
                name = stack.back().first;
                pos = stack.back().second;
                stack.pop_back();
            } else {
                if (branch != 0) stack.emplace_back(name, branch * 4);
                name.push_back((char)ch);
                pos += 4;
            }
        }
    }
    std::fclose(f);

    loaded_ = !entries_.empty();
    std::printf("pvz2: [rsb-index] %zu files indexed from %u RSGs (%u skipped) in '%s'\n",
                entries_.size(), rsg_number, skipped, obb_path.c_str());
    return loaded_;
}

const RsbIndex::Entry *RsbIndex::find(const std::string &guest_path) const {
    auto it = entries_.find(normalize(guest_path));
    return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace sprout
