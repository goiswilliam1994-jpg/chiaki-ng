// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_PASSTHROUGH_H
#define CHIAKI_PASSTHROUGH_H

#include <chiaki/audioreceiver.h>
#include <chiaki/ffmpegdecoder.h>

#include <cstdint>
#include <mutex>
#include <vector>

struct AVFormatContext;
struct AVPacket;

/**
 * Sends the console's encoded video and Opus sound, untouched, as MPEG-TS over UDP to 127.0.0.1:port, so a local
 * media server can hand them to a browser without decoding and encoding them again.
 * It sits in front of the decoders: each frame goes out first, then to the decoder as before.
 * H.264 only: the session asks the console for H.264 while passthrough is on, since browsers decode it.
 */
class Passthrough
{
	public:
		Passthrough(int port, ChiakiFfmpegDecoder *decoder, ChiakiLog *log);
		~Passthrough();
		static bool VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);
		/** A sink that copies each Opus packet out, then hands it to inner (the session's Opus decoder). */
		ChiakiAudioSink WrapAudioSink(ChiakiAudioSink inner);

	private:
		static void AudioHeaderCb(ChiakiAudioHeader *header, void *user);
		static void AudioFrameCb(uint8_t *buf, size_t buf_size, void *user);
		void Write(const uint8_t *buf, size_t size);
		void WriteAudio(const uint8_t *buf, size_t size);
		int64_t Now90k();
		void Send(int stream, const uint8_t *buf, size_t size, int64_t pts, bool key);

		ChiakiFfmpegDecoder *decoder;
		ChiakiLog *log;
		ChiakiAudioSink audio_inner = {};
		AVFormatContext *mux = nullptr;
		AVPacket *packet = nullptr;
		bool open = false;
		std::mutex mux_mutex;
		int64_t start_us = -1;
		int64_t last_pts[2] = { -1, -1 };
		int64_t audio_next_pts = -1;
		int64_t audio_frame_90k = 900; // 480 samples at 48 kHz, until the stream's audio header says otherwise
		std::vector<uint8_t> parameter_sets; // SPS/PPS from the stream header, put in front of key frames that lack them
		std::vector<uint8_t> joined;
		bool write_error_logged = false;
};

#endif // CHIAKI_PASSTHROUGH_H
