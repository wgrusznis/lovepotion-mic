#pragma once

#include <common/object.hpp>
#include <objects/data/sounddata/sounddata.hpp>

#include <3ds.h>
#include <cstdlib> // memalign, free

namespace love
{
    /**
     * RecordingDevice3DS
     *
     * 3DS-specific audio input device backed by the libctru MIC service.
     * Exposes the built-in microphone through the LÖVE RecordingDevice interface
     * so that love.audio.getRecordingDevices() returns it on the forked build.
     *
     * The MIC buffer must be 0x1000-byte aligned regular heap memory allocated
     * with memalign(0x1000, size). Do not use linearAlloc — the kernel rejects
     * linear-heap pointers for the MIC shared memory mapping.
     *
     * libctru writes PCM samples into the buffer as a ring. When the write
     * offset wraps past the end of the buffer, getData() copies two segments
     * to produce contiguous output.
     */
    class RecordingDevice3DS : public Object
    {
      public:
        static Type type;

        RecordingDevice3DS();
        virtual ~RecordingDevice3DS();

        /**
         * Start recording.
         *
         * @param samples    Buffer size in samples (e.g. 8192).
         * @param sampleRate Sample rate in Hz (8000 or 16000).
         * @param bitDepth   Bit depth — only 16-bit PCM is supported on 3DS.
         * @param channels   Channel count — only mono (1) is supported on 3DS.
         * @return true on success, false if micInit or MICU_StartSampling fails.
         */
        bool start(int samples, int sampleRate, int bitDepth, int channels);

        /**
         * Stop an active recording session.
         * Safe to call when not recording (no-op).
         * @return true always.
         */
        bool stop();

        /**
         * Retrieve captured PCM audio as a SoundData object.
         * @return A new SoundData containing the captured samples, or nullptr.
         */
        SoundData* getData();

        /** @return true while a recording session is active. */
        bool isRecording() const;

        /** @return Human-readable device name. */
        const char* getName() const;

        /** @return Number of samples currently captured. */
        int getSampleCount() const;

        /** @return Sample rate used for the current/last recording (Hz). */
        int getSampleRate() const;

        /** @return Bit depth (always 16 on 3DS). */
        int getBitDepth() const;

        /** @return Channel count (always 1 on 3DS). */
        int getChannelCount() const;

      private:
        /**
         * Map a Hz sample rate to the nearest MICU_SampleRate enum value.
         */
        static MICU_SampleRate toMicuSampleRate(int hz);

        u8*  micBuffer;      ///< DSP-aligned sample buffer (memalign(0x1000, size)).
        u32  micBufferSize;  ///< Size of micBuffer in bytes.
        u32  dataSize;       ///< Usable ring-buffer size as reported by libctru.
        u32  startOffset;    ///< Byte offset where the current recording started.
        u32  savedEndOffset; ///< Write position saved by stop() for post-stop getData() calls.
        bool stopped;        ///< True once stop() has been called and savedEndOffset is valid.
        u32  sampleRate;     ///< Sample rate passed to start() (Hz).
        int  bitDepth;       ///< Bit depth passed to start() (always 16 on 3DS).
        int  channels;       ///< Channel count passed to start() (always 1 on 3DS).
        bool recording;      ///< True while a recording session is active.
    };
} // namespace love
