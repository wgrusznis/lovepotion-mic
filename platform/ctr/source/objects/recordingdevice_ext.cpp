#include <objects/recordingdevice_ext.hpp>

#include <3ds.h>
#include <cstdlib>  // free
#include <cstring>  // memcpy, memset
#include <malloc.h> // memalign

using namespace love;

Type RecordingDevice3DS::type("RecordingDevice", &Object::type);

RecordingDevice3DS::RecordingDevice3DS()
    : micBuffer(nullptr)
    , micBufferSize(0)
    , dataSize(0)
    , startOffset(0)
    , savedEndOffset(0)
    , stopped(false)
    , sampleRate(16000)
    , bitDepth(16)
    , channels(1)
    , recording(false)
{
}

RecordingDevice3DS::~RecordingDevice3DS()
{
    // Clean up gracefully if the caller forgot to call stop().
    if (recording)
    {
        MICU_StopSampling();
        micExit();
        recording = false;
    }

    if (micBuffer != nullptr)
    {
        free(micBuffer);
        micBuffer = nullptr;
    }
}

bool RecordingDevice3DS::start(int samples, int sampleRate_, int bitDepth_, int channels_)
{
    if (recording)
        return false;

    // 3DS MIC hardware only supports 16-bit signed PCM, mono.
    // Accept the parameters for API compatibility but clamp to hardware limits.
    bitDepth   = 16;
    channels   = 1;
    sampleRate = static_cast<u32>(sampleRate_);

    const u32 bytesPerSample = 2u; // 16-bit PCM
    micBufferSize            = static_cast<u32>(samples) * bytesPerSample;

    // libctru requires a minimum buffer size of 0x30000 (192 KB).
    if (micBufferSize < 0x30000)
        micBufferSize = 0x30000;

    if (micBuffer != nullptr)
    {
        free(micBuffer);
        micBuffer = nullptr;
    }

    // The MIC buffer must be 0x1000-aligned regular heap memory (not linear memory).
    // The kernel rejects linear-heap pointers for the MIC shared memory mapping.
    micBuffer = static_cast<u8*>(memalign(0x1000, micBufferSize));
    if (micBuffer == nullptr)
        return false;

    memset(micBuffer, 0, micBufferSize);

    Result res = micInit(micBuffer, micBufferSize);
    if (R_FAILED(res))
    {
        free(micBuffer);
        micBuffer = nullptr;
        return false;
    }

    dataSize = micGetSampleDataSize();

    // Begin PCM sampling with looping enabled so recordings can exceed one buffer fill.
    res = MICU_StartSampling(
        MICU_ENCODING_PCM16_SIGNED,
        toMicuSampleRate(static_cast<int>(sampleRate)),
        0,
        dataSize,
        true
    );

    if (R_FAILED(res))
    {
        micExit();
        free(micBuffer);
        micBuffer = nullptr;
        return false;
    }

    startOffset    = 0;
    savedEndOffset = 0;
    stopped        = false;
    recording      = true;
    return true;
}

bool RecordingDevice3DS::stop()
{
    if (!recording)
        return true;

    // Save the final write position before closing the MIC service so that
    // getData() can still be called after stop().
    savedEndOffset = micGetLastSampleOffset();
    stopped        = true;

    MICU_StopSampling();
    micExit();
    recording = false;
    return true;
}

SoundData* RecordingDevice3DS::getData()
{
    if (micBuffer == nullptr)
        return nullptr;

    // libctru writes samples into micBuffer as a ring.
    // Use the offset saved by stop() if the MIC service has been closed;
    // otherwise read the live position from libctru.
    const u32 endOffset = stopped ? savedEndOffset : micGetLastSampleOffset();

    u32 capturedBytes = 0;

    if (endOffset >= startOffset)
    {
        // Contiguous segment — no wrap.
        capturedBytes = endOffset - startOffset;
    }
    else
    {
        // Two segments spanning the ring-buffer wrap boundary.
        capturedBytes = (dataSize - startOffset) + endOffset;
    }

    if (capturedBytes == 0)
        return nullptr;

    u8* pcmOut = static_cast<u8*>(malloc(capturedBytes));
    if (pcmOut == nullptr)
        return nullptr;

    if (endOffset >= startOffset)
    {
        memcpy(pcmOut, micBuffer + startOffset, capturedBytes);
    }
    else
    {
        const u32 part1Size = dataSize - startOffset;
        const u32 part2Size = endOffset;

        memcpy(pcmOut,             micBuffer + startOffset, part1Size);
        memcpy(pcmOut + part1Size, micBuffer,               part2Size);
    }

    const int bytesPerSample = (bitDepth / 8) * channels;
    const int sampleCount    = (bytesPerSample > 0)
        ? static_cast<int>(capturedBytes / static_cast<u32>(bytesPerSample))
        : 0;

    if (sampleCount == 0)
    {
        free(pcmOut);
        return nullptr;
    }

    SoundData* soundData = nullptr;
    try
    {
        soundData = new SoundData(
            pcmOut,
            sampleCount,
            static_cast<int>(sampleRate),
            bitDepth,
            channels
        );
    }
    catch (...)
    {
        free(pcmOut);
        return nullptr;
    }

    free(pcmOut);

    // Advance startOffset so the next getData() call returns only new samples.
    startOffset = endOffset;

    return soundData;
}

bool RecordingDevice3DS::isRecording() const
{
    return recording;
}

const char* RecordingDevice3DS::getName() const
{
    return "3DS Built-in Microphone";
}

int RecordingDevice3DS::getSampleCount() const
{
    if (micBuffer == nullptr)
        return 0;

    const u32 endOffset = stopped ? savedEndOffset : micGetLastSampleOffset();

    u32 capturedBytes = 0;
    if (endOffset >= startOffset)
        capturedBytes = endOffset - startOffset;
    else
        capturedBytes = (dataSize - startOffset) + endOffset;

    const int bytesPerSample = (bitDepth / 8) * channels;
    if (bytesPerSample == 0)
        return 0;

    return static_cast<int>(capturedBytes / static_cast<u32>(bytesPerSample));
}

int RecordingDevice3DS::getSampleRate() const
{
    return static_cast<int>(sampleRate);
}

int RecordingDevice3DS::getBitDepth() const
{
    return bitDepth;
}

int RecordingDevice3DS::getChannelCount() const
{
    return channels;
}

MICU_SampleRate RecordingDevice3DS::toMicuSampleRate(int hz)
{
    // libctru exposes two practical rates: ~16.36 kHz and ~8.18 kHz.
    if (hz >= 12000)
        return MICU_SAMPLE_RATE_16360;
    else
        return MICU_SAMPLE_RATE_8180;
}
