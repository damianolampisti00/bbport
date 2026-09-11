#ifndef OGGOPUSDECODER_HPP_
#define OGGOPUSDECODER_HPP_

#include <QByteArray>

// Decodes an Ogg-Opus file (the format WhatsApp/Signal voice notes use,
// received via the Beeper bridge) into a plain 16-bit PCM WAV file, which
// BB10's own MediaPlayer can play -- BB10 has no Opus codec support at all,
// so files in this format otherwise fail with MediaError::UnsupportedType.
//
// No dependency on libogg: the Ogg page/packet framing needed here is
// simple enough to parse directly (see .cpp), avoiding a second vendored
// third-party library just for container demuxing.
namespace OggOpusDecoder
{
    // Returns false (leaving *wavOut untouched) if oggData isn't a
    // recognizable Ogg-Opus stream, or decoding otherwise fails.
    bool decodeToWav(const QByteArray &oggData, QByteArray *wavOut);
}

#endif /* OGGOPUSDECODER_HPP_ */
