#include "oggopusdecoder.hpp"

// Included before opus.h so opus_types.h's own stdint-detection macro (it
// checks whether <stdint.h> was already processed) takes the exact-width
// typedef path -- matching the C99 build used for the vendored static lib
// (third_party/opus) rather than falling back to its generic "int"/"short"
// guess, which this being a C++ (not C99) translation unit would otherwise
// trigger.
#include <stdint.h>
#include <opus/opus.h>

#include <QVector>
#include <QtCore/qendian.h>

namespace {

// One decoded Opus packet's raw bytes, reassembled from one or more Ogg
// page segments (a packet can span page boundaries -- see the lacing-value
// loop below).
struct OggPage {
    quint32 serial;
    quint8 headerType;
    QVector<int> segmentLengths;
    int dataStart; // offset into the original buffer
};

bool readPageAt(const QByteArray &data, int pos, OggPage *page, int *pageEnd)
{
    if (pos + 27 > data.size()) return false;
    const uchar *p = reinterpret_cast<const uchar*>(data.constData()) + pos;
    if (p[0] != 'O' || p[1] != 'g' || p[2] != 'g' || p[3] != 'S') return false;
    page->headerType = p[5];
    page->serial = qFromLittleEndian<quint32>(p + 14);
    int numSegments = p[26];
    int segTableStart = pos + 27;
    if (segTableStart + numSegments > data.size()) return false;

    page->segmentLengths.clear();
    page->segmentLengths.reserve(numSegments);
    int totalDataLen = 0;
    const uchar *segTable = reinterpret_cast<const uchar*>(data.constData()) + segTableStart;
    for (int i = 0; i < numSegments; ++i) {
        page->segmentLengths.append(segTable[i]);
        totalDataLen += segTable[i];
    }
    page->dataStart = segTableStart + numSegments;
    if (page->dataStart + totalDataLen > data.size()) return false;

    *pageEnd = page->dataStart + totalDataLen;
    return true;
}

// Walks every Ogg page belonging to the first logical bitstream found (its
// serial number), reassembling lacing-value segments into whole packets.
bool extractPackets(const QByteArray &data, QList<QByteArray> *packets)
{
    int pos = 0;
    bool haveSerial = false;
    quint32 streamSerial = 0;
    QByteArray currentPacket;

    while (pos < data.size()) {
        OggPage page;
        int pageEnd = 0;
        if (!readPageAt(data, pos, &page, &pageEnd)) break;

        if (!haveSerial) {
            streamSerial = page.serial;
            haveSerial = true;
        }

        if (page.serial == streamSerial) {
            int offset = page.dataStart;
            for (int i = 0; i < page.segmentLengths.size(); ++i) {
                int len = page.segmentLengths.at(i);
                currentPacket.append(data.constData() + offset, len);
                offset += len;
                if (len < 255) {
                    packets->append(currentPacket);
                    currentPacket.clear();
                }
            }
        }

        pos = pageEnd;
    }

    return !packets->isEmpty();
}

void appendWavHeader(QByteArray *out, int dataSize, int sampleRate, int channels)
{
    int byteRate = sampleRate * channels * 2;
    int blockAlign = channels * 2;
    int riffSize = 36 + dataSize;

    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    quint32 le;
    le = qToLittleEndian<quint32>(riffSize); header.append((const char*)&le, 4);
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    le = qToLittleEndian<quint32>(16); header.append((const char*)&le, 4);
    quint16 le16;
    le16 = qToLittleEndian<quint16>(1); header.append((const char*)&le16, 2); // PCM
    le16 = qToLittleEndian<quint16>(channels); header.append((const char*)&le16, 2);
    le = qToLittleEndian<quint32>(sampleRate); header.append((const char*)&le, 4);
    le = qToLittleEndian<quint32>(byteRate); header.append((const char*)&le, 4);
    le16 = qToLittleEndian<quint16>(blockAlign); header.append((const char*)&le16, 2);
    le16 = qToLittleEndian<quint16>(16); header.append((const char*)&le16, 2); // bits/sample
    header.append("data", 4);
    le = qToLittleEndian<quint32>(dataSize); header.append((const char*)&le, 4);

    out->prepend(header);
}

} // namespace

bool OggOpusDecoder::decodeToWav(const QByteArray &oggData, QByteArray *wavOut)
{
    if (oggData.size() < 4 || !oggData.startsWith("OggS")) return false;

    QList<QByteArray> packets;
    if (!extractPackets(oggData, &packets)) return false;
    if (packets.size() < 3) return false; // need OpusHead + OpusTags + >=1 audio packet

    const QByteArray &head = packets.at(0);
    if (head.size() < 19 || !head.startsWith("OpusHead")) return false;

    const uchar *h = reinterpret_cast<const uchar*>(head.constData());
    int channels = h[9];
    quint16 preSkip = qFromLittleEndian<quint16>(h + 10);
    // Channel mapping family 0 = single Opus stream, mono or stereo, which
    // covers every real-world voice-note case; anything else (multistream
    // surround) is out of scope here.
    quint8 channelMappingFamily = h[18];
    if (channels < 1 || channels > 2 || channelMappingFamily != 0) return false;

    const int kDecodeRate = 48000; // pre-skip is always specified at 48kHz
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(kDecodeRate, channels, &err);
    if (err != OPUS_OK || !dec) return false;

    const int kMaxFrameSamples = 5760; // 120ms @ 48kHz, the largest Opus frame
    QVector<opus_int16> pcmBuf(kMaxFrameSamples * channels);
    QByteArray pcmOut;
    int samplesToSkip = preSkip;

    // packets[0]=OpusHead, packets[1]=OpusTags, packets[2..]=audio frames.
    for (int i = 2; i < packets.size(); ++i) {
        const QByteArray &pkt = packets.at(i);
        int n = opus_decode(dec, reinterpret_cast<const unsigned char*>(pkt.constData()), pkt.size(),
                pcmBuf.data(), kMaxFrameSamples, 0);
        if (n < 0) continue; // skip a corrupt frame rather than aborting the whole clip

        int startSample = 0;
        if (samplesToSkip > 0) {
            if (n <= samplesToSkip) {
                samplesToSkip -= n;
                continue;
            }
            startSample = samplesToSkip;
            samplesToSkip = 0;
        }
        int bytesOffset = startSample * channels * (int)sizeof(opus_int16);
        int bytesLen = (n - startSample) * channels * (int)sizeof(opus_int16);
        pcmOut.append(reinterpret_cast<const char*>(pcmBuf.constData()) + bytesOffset, bytesLen);
    }

    opus_decoder_destroy(dec);
    if (pcmOut.isEmpty()) return false;

    appendWavHeader(&pcmOut, pcmOut.size(), kDecodeRate, channels);
    *wavOut = pcmOut;
    return true;
}
