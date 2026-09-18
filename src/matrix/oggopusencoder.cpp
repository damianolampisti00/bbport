#include "oggopusencoder.hpp"

// Same stdint-before-opus.h ordering as OggOpusDecoder.cpp, and for the
// same reason: opus_types.h's own stdint-detection macro needs <stdint.h>
// already processed to pick the exact-width typedef path for this
// statically-linked C99 build, rather than guessing plain int/short for a
// C++ translation unit.
#include <stdint.h>
#include <opus/opus.h>

#include <QVector>
#include <QtCore/qendian.h>
#include <QtGlobal>
#include <string.h>

namespace {

const quint32 kSampleRate = 48000; // Opus's own native rate; caller (ffmpeg) resamples to this
const int kFrameSamples = 960; // 20ms @ 48kHz, same frame duration ffmpeg's own -c:a libopus call used before this replaced it
const int kBitrate = 32000; // matches the previous ffmpeg -b:a 32k
const int kMaxPacketBytes = 4000; // generous upper bound for one 20ms VOIP frame at this bitrate
const int kPacketsPerPage = 50; // ~1s of audio per page; keeps segment-table size (1 segment/packet here) comfortably under the 255 lacing-value limit

// Standard Ogg CRC-32: polynomial 0x04c11db7, MSB-first, no reflection, no
// final XOR -- the exact variant libogg itself uses for page checksums (a
// different, incompatible variant from the far more common zlib/PNG CRC-32,
// so this can't reuse Qt's qChecksum() or similar).
quint32 oggCrcTable[256];
bool oggCrcTableReady = false;

void ensureOggCrcTable()
{
    if (oggCrcTableReady) return;
    for (quint32 i = 0; i < 256; ++i) {
        quint32 r = i << 24;
        for (int j = 0; j < 8; ++j) {
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
        }
        oggCrcTable[i] = r;
    }
    oggCrcTableReady = true;
}

quint32 oggCrc32(const uchar *data, int len)
{
    ensureOggCrcTable();
    quint32 crc = 0;
    for (int i = 0; i < len; ++i) {
        crc = (crc << 8) ^ oggCrcTable[((crc >> 24) & 0xff) ^ data[i]];
    }
    return crc;
}

void appendLE32(QByteArray *out, quint32 v) { quint32 le = qToLittleEndian(v); out->append((const char*)&le, 4); }
void appendLE16(QByteArray *out, quint16 v) { quint16 le = qToLittleEndian(v); out->append((const char*)&le, 2); }

QByteArray buildOpusHead(int channels, quint16 preSkip)
{
    QByteArray head;
    head.append("OpusHead", 8);
    head.append(char(1)); // version
    head.append(char(channels));
    appendLE16(&head, preSkip);
    appendLE32(&head, kSampleRate); // "input" sample rate -- informational only
    appendLE16(&head, 0); // output gain
    head.append(char(0)); // channel mapping family 0: mono/stereo, single stream
    return head;
}

QByteArray buildOpusTags()
{
    QByteArray vendor = "BBport";
    QByteArray tags;
    tags.append("OpusTags", 8);
    appendLE32(&tags, quint32(vendor.size()));
    tags.append(vendor);
    appendLE32(&tags, 0); // no user comments
    return tags;
}

// Writes one Ogg page containing every packet in `packets` back-to-back,
// each terminated by its own lacing value (no packet here ever spans a page
// boundary -- every packet involved, header or audio, is far under the
// 255*255 byte single-page ceiling that would require that).
void writePage(QByteArray *out, quint32 serial, quint32 pageSeq, quint8 headerType,
        quint64 granulePos, const QList<QByteArray> &packets)
{
    QByteArray segTable;
    QByteArray body;
    foreach (const QByteArray &pkt, packets) {
        int remaining = pkt.size();
        while (remaining >= 255) {
            segTable.append(char(255));
            remaining -= 255;
        }
        segTable.append(char(remaining)); // 0..254, always terminates the packet
        body.append(pkt);
    }

    QByteArray page;
    page.append("OggS", 4);
    page.append(char(0)); // stream structure version
    page.append(char(headerType));
    for (int i = 0; i < 8; ++i) page.append(char((granulePos >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i) page.append(char((serial >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i) page.append(char((pageSeq >> (8 * i)) & 0xff));
    int crcFieldOffset = page.size();
    page.append(char(0)); page.append(char(0)); page.append(char(0)); page.append(char(0)); // CRC placeholder
    page.append(char(segTable.size()));
    page.append(segTable);
    page.append(body);

    quint32 crc = oggCrc32(reinterpret_cast<const uchar*>(page.constData()), page.size());
    page[crcFieldOffset + 0] = char(crc & 0xff);
    page[crcFieldOffset + 1] = char((crc >> 8) & 0xff);
    page[crcFieldOffset + 2] = char((crc >> 16) & 0xff);
    page[crcFieldOffset + 3] = char((crc >> 24) & 0xff);

    out->append(page);
}

} // namespace

bool OggOpusEncoder::encodeFromPcm(const QByteArray &pcm, int channels, QByteArray *oggOut)
{
    if (pcm.isEmpty() || (channels != 1 && channels != 2)) return false;

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(kSampleRate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) return false;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    int lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    quint16 preSkip = quint16(lookahead);

    // Fixed, arbitrary-but-consistent serial number -- this file is always
    // exactly one logical bitstream on its own, never muxed with anything
    // else, so there's no real multiplexing collision to avoid here.
    const quint32 kSerial = 0x424f5054; // "BOPT"

    QByteArray body;
    quint32 pageSeq = 0;
    writePage(&body, kSerial, pageSeq++, 0x02 /* BOS */, 0, QList<QByteArray>() << buildOpusHead(channels, preSkip));
    writePage(&body, kSerial, pageSeq++, 0x00, 0, QList<QByteArray>() << buildOpusTags());

    const opus_int16 *samples = reinterpret_cast<const opus_int16*>(pcm.constData());
    int totalSamples = pcm.size() / (channels * int(sizeof(opus_int16)));

    QVector<unsigned char> packetBuf(kMaxPacketBytes);
    QList<QByteArray> pending;
    quint64 samplesEncoded = 0;
    bool anyPacket = false;

    for (int pos = 0; pos < totalSamples; pos += kFrameSamples) {
        int framesHere = qMin(kFrameSamples, totalSamples - pos);
        QVector<opus_int16> frameBuf;
        const opus_int16 *frameData = samples + (qint64(pos) * channels);
        if (framesHere < kFrameSamples) {
            // Opus needs one of its fixed frame sizes -- zero-pad the final,
            // short frame rather than dropping this last fraction of a
            // second of audio.
            frameBuf.fill(0, kFrameSamples * channels);
            memcpy(frameBuf.data(), frameData, size_t(framesHere) * channels * sizeof(opus_int16));
            frameData = frameBuf.constData();
        }

        opus_int32 n = opus_encode(enc, frameData, kFrameSamples, packetBuf.data(), kMaxPacketBytes);
        if (n < 0) continue; // skip a frame opus itself rejects rather than aborting the whole recording

        pending.append(QByteArray(reinterpret_cast<const char*>(packetBuf.constData()), n));
        samplesEncoded += quint64(framesHere);
        anyPacket = true;

        if (pending.size() >= kPacketsPerPage) {
            writePage(&body, kSerial, pageSeq++, 0x00, preSkip + samplesEncoded, pending);
            pending.clear();
        }
    }

    if (!pending.isEmpty()) {
        writePage(&body, kSerial, pageSeq++, 0x04 /* EOS */, preSkip + samplesEncoded, pending);
    } else if (anyPacket) {
        // The last flush above already emptied `pending` right at a page
        // boundary -- still need an explicit (empty) EOS page so players
        // that key off the EOS flag rather than just running out of data
        // recognize the stream as cleanly terminated.
        writePage(&body, kSerial, pageSeq++, 0x04, preSkip + samplesEncoded, QList<QByteArray>());
    }

    opus_encoder_destroy(enc);
    if (!anyPacket) return false;

    *oggOut = body;
    return true;
}
