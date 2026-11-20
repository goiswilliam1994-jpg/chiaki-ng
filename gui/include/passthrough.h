// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_PASSTHROUGH_H
#define CHIAKI_PASSTHROUGH_H

#include <chiaki/ffmpegdecoder.h>

#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVPacket;

/**
 * Sends the console's encoded video, untouched, as MPEG-TS over UDP to 127.0.0.1:port, so a local
 * media server can hand it to a browser without decoding and encoding it again.
 * It sits in front of the decoder: each frame goes out first, then to the decoder as before.
 * H.264 only: the session asks the console for H.264 while passthrough is on, since browsers decode it.
 */
class Passthrough
{
	public:
		Passthrough(int port, ChiakiFfmpegDecoder *decoder, ChiakiLog *log);
		~Passthrough();
		static bool VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);

	private:
		void Write(const uint8_t *buf, size_t size);

		ChiakiFfmpegDecoder *decoder;
		ChiakiLog *log;
		AVFormatContext *mux = nullptr;
		AVPacket *packet = nullptr;
		bool open = false;
		int64_t start_us = -1;
		int64_t last_pts = -1;
		std::vector<uint8_t> parameter_sets; // SPS/PPS from the stream header, put in front of key frames that lack them
		std::vector<uint8_t> joined;
		bool write_error_logged = false;
};

#endif // CHIAKI_PASSTHROUGH_H
