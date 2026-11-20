// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <passthrough.h>

#include <chiaki/log.h>

#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

Passthrough::Passthrough(int port, ChiakiFfmpegDecoder *decoder, ChiakiLog *log)
	: decoder(decoder), log(log)
{
	// pkt_size: seven whole 188-byte TS packets per datagram
	std::string url = "udp://127.0.0.1:" + std::to_string(port) + "?pkt_size=1316";
	if(avformat_alloc_output_context2(&mux, nullptr, "mpegts", url.c_str()) < 0)
	{
		CHIAKI_LOGE(log, "Passthrough: no MPEG-TS muxer");
		return;
	}
	AVStream *stream = avformat_new_stream(mux, nullptr);
	packet = av_packet_alloc();
	if(!stream || !packet)
	{
		CHIAKI_LOGE(log, "Passthrough: out of memory");
		return;
	}
	stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	stream->codecpar->codec_id = AV_CODEC_ID_H264;
	stream->time_base = AVRational{1, 90000};

	AVDictionary *options = nullptr;
	av_dict_set(&options, "flush_packets", "1", 0); // each frame leaves right away
	bool ok = avio_open(&mux->pb, url.c_str(), AVIO_FLAG_WRITE) >= 0
		&& avformat_write_header(mux, &options) >= 0;
	av_dict_free(&options);
	if(!ok)
	{
		CHIAKI_LOGE(log, "Passthrough: can't send to %s", url.c_str());
		return;
	}
	open = true;
	CHIAKI_LOGI(log, "Passthrough: sending the console's video to %s", url.c_str());
}

Passthrough::~Passthrough()
{
	if(open)
		av_write_trailer(mux);
	if(mux)
	{
		avio_closep(&mux->pb);
		avformat_free_context(mux);
	}
	av_packet_free(&packet);
}

bool Passthrough::VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	auto passthrough = static_cast<Passthrough *>(user);
	if(passthrough->open)
		passthrough->Write(buf, buf_size);
	return chiaki_ffmpeg_decoder_video_sample_cb(buf, buf_size, frames_lost, frame_recovered, passthrough->decoder);
}

void Passthrough::Write(const uint8_t *buf, size_t size)
{
	while(size && !buf[size - 1]) // chiaki keeps zero padding after the data
		size--;

	// NAL units up to the first slice: SPS (7) and PPS (8), then an IDR slice (5) or another slice (1)
	bool has_parameter_sets = false;
	int slice_type = 0;
	for(size_t i = 0; i + 3 < size && !slice_type; i++)
	{
		if(buf[i] || buf[i + 1] || buf[i + 2] != 1)
			continue;
		int type = buf[i + 3] & 0x1f;
		if(type == 7 || type == 8)
			has_parameter_sets = true;
		else if(type == 1 || type == 5)
			slice_type = type;
		i += 2;
	}
	if(!slice_type)
	{
		// The stream header: kept so every later key frame carries it, for viewers who join mid-stream
		if(has_parameter_sets)
			parameter_sets.assign(buf, buf + size);
		return;
	}
	bool key = slice_type == 5;
	if(key && !has_parameter_sets && !parameter_sets.empty())
	{
		joined.assign(parameter_sets.begin(), parameter_sets.end());
		joined.insert(joined.end(), buf, buf + size);
		buf = joined.data();
		size = joined.size();
	}

	// chiaki gives no timestamps; arrival time is what a live viewer needs anyway
	int64_t now_us = av_gettime_relative();
	if(start_us < 0)
		start_us = now_us;
	int64_t pts = av_rescale_q(now_us - start_us, AVRational{1, 1000000}, mux->streams[0]->time_base);
	if(pts <= last_pts)
		pts = last_pts + 1;
	last_pts = pts;

	packet->data = const_cast<uint8_t *>(buf);
	packet->size = (int)size;
	packet->stream_index = 0;
	packet->pts = packet->dts = pts;
	packet->flags = key ? AV_PKT_FLAG_KEY : 0;
	int err = av_write_frame(mux, packet);
	mux->pb->error = 0; // one failed send would otherwise silence every later one
	if(err < 0 && !write_error_logged)
	{
		char msg[AV_ERROR_MAX_STRING_SIZE];
		CHIAKI_LOGW(log, "Passthrough: send failed (%s), still trying", av_make_error_string(msg, sizeof(msg), err));
		write_error_logged = true;
	}
}
