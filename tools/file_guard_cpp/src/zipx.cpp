#include "file_guard/zipx.h"

#include "file_guard/errors.h"
#include "file_guard/util.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace file_guard::zipx {

namespace {

class BitReader {
public:
    BitReader(const std::uint8_t *data, std::size_t size)
        : m_data(data), m_size(size) {}

    int bits(int need)
    {
        int value = 0;
        for (int i = 0; i < need; ++i)
            value |= nextBit() << i;
        return value;
    }

    void align() { m_bitBuffer = 0; m_bitCount = 0; }

    bool byteAt(std::size_t offset, std::uint8_t *out) const
    {
        if (offset >= m_size)
            return false;
        *out = m_data[offset];
        return true;
    }

    std::size_t position() const { return m_position; }
    bool exhausted() const { return m_position >= m_size; }

private:
    int nextBit()
    {
        if (m_bitCount == 0) {
            if (m_position >= m_size)
                throw SecurityError(ErrorCode::Corrupt, "压缩数据提前结束");
            m_bitBuffer = m_data[m_position++];
            m_bitCount = 8;
        }
        const int bit = m_bitBuffer & 1;
        m_bitBuffer >>= 1;
        --m_bitCount;
        return bit;
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_position = 0;
    int m_bitBuffer = 0;
    int m_bitCount = 0;
};

struct Huffman {
    std::uint16_t counts[16] = {};
    std::uint16_t symbols[288] = {};
};

void buildHuffman(Huffman &h, const std::uint8_t *lengths, int n)
{
    std::memset(&h, 0, sizeof(h));
    for (int i = 0; i < n; ++i)
        ++h.counts[lengths[i]];
    h.counts[0] = 0;

    std::uint16_t offsets[16] = {};
    for (int len = 1; len < 16; ++len)
        offsets[len] = static_cast<std::uint16_t>(offsets[len - 1] + h.counts[len - 1]);

    for (int symbol = 0; symbol < n; ++symbol) {
        if (lengths[symbol] != 0)
            h.symbols[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
    }
}

int decodeSymbol(BitReader &reader, const Huffman &h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= reader.bits(1);
        const int count = h.counts[len];
        if (code - first < count)
            return h.symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    throw SecurityError(ErrorCode::Corrupt, "无效的 Huffman 编码");
}

const std::uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint16_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                     6145, 8193, 12289, 16385, 24577};
const std::uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflateBlock(BitReader &reader, const Huffman &literal, const Huffman &distance,
                  crypto::Bytes &out, std::size_t limit)
{
    while (true) {
        const int symbol = decodeSymbol(reader, literal);
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
            if (out.size() > limit)
                throw SecurityError(ErrorCode::Corrupt, "解压后大小超出上限");
            continue;
        }
        if (symbol == 256)
            return;
        const int lengthIndex = symbol - 257;
        if (lengthIndex < 0 || lengthIndex >= 29)
            throw SecurityError(ErrorCode::Corrupt, "无效的长度码");
        const int length = kLengthBase[lengthIndex] + reader.bits(kLengthExtra[lengthIndex]);

        const int distSymbol = decodeSymbol(reader, distance);
        if (distSymbol >= 30)
            throw SecurityError(ErrorCode::Corrupt, "无效的距离码");
        const int dist = kDistBase[distSymbol] + reader.bits(kDistExtra[distSymbol]);
        if (dist == 0 || static_cast<std::size_t>(dist) > out.size())
            throw SecurityError(ErrorCode::Corrupt, "无效的距离");

        const std::size_t start = out.size();
        out.resize(start + static_cast<std::size_t>(length));
        if (out.size() > limit)
            throw SecurityError(ErrorCode::Corrupt, "解压后大小超出上限");
        for (int i = 0; i < length; ++i)
            out[start + static_cast<std::size_t>(i)] = out[out.size() - static_cast<std::size_t>(dist) - static_cast<std::size_t>(length) + static_cast<std::size_t>(i)];
    }
}

} // namespace

crypto::Bytes inflateRaw(const std::uint8_t *data, std::size_t size, std::size_t expectedOut)
{
    const std::size_t limit = std::max<std::size_t>(expectedOut, 4u << 20) + 64 * 1024;
    crypto::Bytes out;
    out.reserve(std::min(expectedOut, limit));
    BitReader reader(data, size);

    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = reader.bits(1) != 0;
        const int type = reader.bits(2);
        if (type == 0) {
            reader.align();
            const int length = reader.bits(16);
            const int inverse = reader.bits(16);
            if ((length ^ 0xFFFF) != inverse)
                throw SecurityError(ErrorCode::Corrupt, "存储块长度校验失败");
            for (int i = 0; i < length; ++i)
                out.push_back(static_cast<std::uint8_t>(reader.bits(8)));
        } else if (type == 1) {
            std::uint8_t lengths[288];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            Huffman literal;
            buildHuffman(literal, lengths, 288);
            std::uint8_t distLengths[30];
            std::fill(std::begin(distLengths), std::end(distLengths), 5);
            Huffman distance;
            buildHuffman(distance, distLengths, 30);
            inflateBlock(reader, literal, distance, out, limit);
        } else if (type == 2) {
            const int literalCount = reader.bits(5) + 257;
            const int distanceCount = reader.bits(5) + 1;
            const int codeCount = reader.bits(4) + 4;
            static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                   11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t codeLengths[19] = {};
            for (int i = 0; i < codeCount; ++i)
                codeLengths[order[i]] = static_cast<std::uint8_t>(reader.bits(3));
            Huffman codeHuffman;
            buildHuffman(codeHuffman, codeLengths, 19);

            std::uint8_t lengths[320] = {};
            int index = 0;
            while (index < literalCount + distanceCount) {
                const int symbol = decodeSymbol(reader, codeHuffman);
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                } else if (symbol == 16) {
                    if (index == 0)
                        throw SecurityError(ErrorCode::Corrupt, "重复长度码没有前值");
                    const int repeat = 3 + reader.bits(2);
                    const std::uint8_t value = lengths[index - 1];
                    for (int i = 0; i < repeat; ++i) lengths[index++] = value;
                } else if (symbol == 17) {
                    const int repeat = 3 + reader.bits(3);
                    for (int i = 0; i < repeat; ++i) lengths[index++] = 0;
                } else if (symbol == 18) {
                    const int repeat = 11 + reader.bits(7);
                    for (int i = 0; i < repeat; ++i) lengths[index++] = 0;
                } else {
                    throw SecurityError(ErrorCode::Corrupt, "无效的码长码");
                }
            }
            Huffman literal;
            buildHuffman(literal, lengths, literalCount);
            Huffman distance;
            buildHuffman(distance, lengths + literalCount, distanceCount);
            inflateBlock(reader, literal, distance, out, limit);
        } else {
            throw SecurityError(ErrorCode::Corrupt, "无效的压缩块类型");
        }
        if (out.size() > limit)
            throw SecurityError(ErrorCode::Corrupt, "解压后大小超出上限");
    }
    return out;
}

std::vector<Entry> list(const std::filesystem::path &zipPath)
{
    const crypto::Bytes raw = util::readAll(zipPath);
    if (raw.size() < 22)
        throw SecurityError(ErrorCode::Corrupt, "不是有效的 ZIP 文件");
    if (raw.size() > 256u << 20)
        throw SecurityError(ErrorCode::InvalidArgument, "ZIP 文件过大");

    std::size_t eocd = std::string::npos;
    const std::size_t searchEnd = raw.size();
    for (std::size_t i = searchEnd - 22 + 1; i-- > 0;) {
        if (raw[i] == 0x50 && raw[i + 1] == 0x4B && raw[i + 2] == 0x05 && raw[i + 3] == 0x06) {
            eocd = i;
            break;
        }
        if (searchEnd - i > 66000)
            break;
    }
    if (eocd == std::string::npos)
        throw SecurityError(ErrorCode::Corrupt, "ZIP 缺少目录结尾记录");

    const auto u16 = [&raw](std::size_t offset) -> std::uint16_t {
        return static_cast<std::uint16_t>(raw[offset] | (raw[offset + 1] << 8));
    };
    const auto u32 = [&raw](std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(raw[offset]) |
               (static_cast<std::uint32_t>(raw[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(raw[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(raw[offset + 3]) << 24);
    };

    const std::uint16_t count = u16(eocd + 10);
    std::size_t cursor = u32(eocd + 16);
    std::vector<Entry> entries;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (cursor + 46 > raw.size() || u32(cursor) != 0x02014B50)
            break;
        Entry entry;
        entry.method = u16(cursor + 10);
        entry.compSize = u32(cursor + 20);
        entry.uncompSize = u32(cursor + 24);
        const std::uint16_t nameLength = u16(cursor + 28);
        const std::uint16_t extraLength = u16(cursor + 30);
        const std::uint16_t commentLength = u16(cursor + 32);
        entry.localOffset = u32(cursor + 42);
        if (cursor + 46 + nameLength > raw.size())
            break;
        entry.name.assign(reinterpret_cast<const char *>(raw.data() + cursor + 46), nameLength);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        if (!entry.name.empty() && entry.name.back() != '/')
            entries.push_back(entry);
        cursor += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}

crypto::Bytes readEntry(const std::filesystem::path &zipPath, const Entry &entry,
                        std::size_t maxOut)
{
    const crypto::Bytes raw = util::readAll(zipPath);
    const std::size_t offset = entry.localOffset;
    if (offset + 30 > raw.size())
        throw SecurityError(ErrorCode::Corrupt, "ZIP 本地头越界");
    const auto u16 = [&raw](std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>(raw[at] | (raw[at + 1] << 8));
    };
    if (raw[offset] != 0x50 || raw[offset + 1] != 0x4B || raw[offset + 2] != 0x03 ||
        raw[offset + 3] != 0x04)
        throw SecurityError(ErrorCode::Corrupt, "ZIP 本地头签名错误");
    const std::uint16_t nameLength = u16(offset + 26);
    const std::uint16_t extraLength = u16(offset + 28);
    const std::size_t dataOffset = offset + 30 + nameLength + extraLength;
    if (dataOffset + entry.compSize > raw.size())
        throw SecurityError(ErrorCode::Corrupt, "ZIP 数据越界");
    if (entry.uncompSize > maxOut)
        throw SecurityError(ErrorCode::InvalidArgument, "ZIP 条目解压后过大");

    if (entry.method == 0) {
        if (entry.compSize > maxOut)
            throw SecurityError(ErrorCode::InvalidArgument, "ZIP 条目不合法");
        return crypto::Bytes(raw.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                             raw.begin() + static_cast<std::ptrdiff_t>(dataOffset + entry.compSize));
    }
    if (entry.method != 8)
        throw SecurityError(ErrorCode::InvalidArgument, "不支持的 ZIP 压缩方法");
    crypto::Bytes out = inflateRaw(raw.data() + dataOffset, entry.compSize, entry.uncompSize);
    if (out.size() > maxOut)
        throw SecurityError(ErrorCode::InvalidArgument, "ZIP 条目解压后过大");
    return out;
}

} // namespace file_guard::zipx
