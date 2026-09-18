#ifndef OGGOPUSENCODER_HPP_
#define OGGOPUSENCODER_HPP_

#include <QByteArray>

// Encodes 16-bit PCM audio into an Ogg-Opus file -- the format WhatsApp/
// Signal voice notes actually use, and the only shape a receiving Matrix
// client will render as an inline voice-message bubble (see
// MessageListModel::sendAudio()'s isVoiceMessage check).
//
// Exists because this device's BerryCore ffmpeg build was compiled with
// --disable-libopus (confirmed via a real device's `ffmpeg -encoders`, no
// external encoder libraries at all: no libx264, libopus, libmp3lame...),
// so ffmpeg itself can never produce Opus output here -- but it CAN still
// decode the AAC/m4a bb::multimedia::AudioRecorder actually records (AAC
// decode and the WAV muxer are both native to ffmpeg, not on that disabled
// list), so MediaManager::uploadAudioAsOgg() uses ffmpeg only for that AAC
// decode step, then hands the resulting 48kHz PCM here to encode with the
// same libopus already statically linked into BBport for OggOpusDecoder's
// playback path -- no new cross-compiled dependency needed.
//
// No dependency on libogg, matching OggOpusDecoder's own reasoning: the Ogg
// page framing this needs to WRITE is exactly as simple as the framing that
// class already parses, so hand-rolling it here avoids a second vendored
// container library for a job just as small in the other direction.
namespace OggOpusEncoder
{
    // pcm must be raw interleaved signed 16-bit little-endian samples at
    // 48kHz (Opus's own native rate -- have ffmpeg resample to this rate
    // directly during the AAC decode step, e.g. `-ar 48000`, so no resampler
    // is needed on this side). channels must be 1 or 2. Returns false
    // (leaving *oggOut untouched) on any encoder failure or empty input.
    bool encodeFromPcm(const QByteArray &pcm, int channels, QByteArray *oggOut);
}

#endif /* OGGOPUSENCODER_HPP_ */
