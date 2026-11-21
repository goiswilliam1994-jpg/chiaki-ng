// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <passthrough.h>

#include <chiaki/log.h>

#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
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
	AVStream *video = avformat_new_stream(mux, nullptr);
	AVStream *sound = avformat_new_stream(mux, nullptr);
	packet = av_packet_alloc();
	// OpusHead: version 1, 2 channels, no pre-skip, 48000 Hz, no gain, mapping family 0 (the TS muxer reads the channels from it)
	static const uint8_t opus_head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0 };
	uint8_t *extradata = (uint8_t *)av_mallocz(sizeof(opus_head) + AV_INPUT_BUFFER_PADDING_SIZE);
	if(!video || !sound || !packet || !extradata)
	{
		av_free(extradata);
		CHIAKI_LOGE(log, "Passthrough: out of memory");
		return;
	}
	video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	video->codecpar->codec_id = AV_CODEC_ID_H264;
	video->time_base = AVRational{1, 90000};
	sound->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	sound->codecpar->codec_id = AV_CODEC_ID_OPUS;
	sound->codecpar->sample_rate = 48000;
	av_channel_layout_default(&sound->codecpar->ch_layout, 2);
	memcpy(extradata, opus_head, sizeof(opus_head));
	sound->codecpar->extradata = extradata;
	sound->codecpar->extradata_size = sizeof(opus_head);
	sound->time_base = AVRational{1, 90000};

	AVDictionary *options = nullptr;
	av_dict_set(&options, "flush_packets", "1", 0); // each frame leaves right away
	av_dict_set(&options, "pes_payload_size", "0", 0); // the muxer would hold 10 ms Opus packets until ~3 KB piled up
	bool ok = avio_open(&mux->pb, url.c_str(), AVIO_FLAG_WRITE) >= 0
		&& avformat_write_header(mux, &options) >= 0;
	av_dict_free(&options);
	if(!ok)
	{
		CHIAKI_LOGE(log, "Passthrough: can't send to %s", url.c_str());
		return;
	}
	open = true;
	CHIAKI_LOGI(log, "Passthrough: sending the console's video and sound to %s", url.c_str());
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

ChiakiAudioSink Passthrough::WrapAudioSink(ChiakiAudioSink inner)
{
	audio_inner = inner;
	ChiakiAudioSink sink = {};
	sink.user = this;
	sink.header_cb = AudioHeaderCb;
	sink.frame_cb = AudioFrameCb;
	return sink;
}

void Passthrough::AudioHeaderCb(ChiakiAudioHeader *header, void *user)
{
	auto passthrough = static_cast<Passthrough *>(user);
	if(header->rate && header->frame_size)
		passthrough->audio_frame_90k = (int64_t)header->frame_size * 90000 / header->rate;
	if(passthrough->audio_inner.header_cb)
		passthrough->audio_inner.header_cb(header, passthrough->audio_inner.user);
}

void Passthrough::AudioFrameCb(uint8_t *buf, size_t buf_size, void *user)
{
	auto passthrough = static_cast<Passthrough *>(user);
	if(passthrough->open)
		passthrough->WriteAudio(buf, buf_size);
	if(passthrough->audio_inner.frame_cb)
		passthrough->audio_inner.frame_cb(buf, buf_size, passthrough->audio_inner.user);
}

int64_t Passthrough::Now90k()
{
	// chiaki gives no timestamps; arrival time is what a live viewer needs, and one clock keeps picture and sound together
	int64_t now_us = av_gettime_relative();
	if(start_us < 0)
		start_us = now_us;
	return (now_us - start_us) * 9 / 100;
}

void Passthrough::Write(const uint8_t *buf, size_t size)
{
	std::lock_guard<std::mutex> lock(mux_mutex);
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
	Send(0, buf, size, Now90k(), key);
}

void Passthrough::WriteAudio(const uint8_t *buf, size_t size)
{
	std::lock_guard<std::mutex> lock(mux_mutex);
	// Packets come in bursts: count their length so the sound plays smooth, and follow the wall clock again
	// whenever the count drifts 60 ms from it (lost packets, a stall), so it stays with the picture
	int64_t now = Now90k();
	if(audio_next_pts < 0 || now - audio_next_pts > 5400 || audio_next_pts - now > 5400)
		audio_next_pts = now;
	Send(1, buf, size, audio_next_pts, true);
	audio_next_pts += audio_frame_90k;
}

void Passthrough::Send(int stream, const uint8_t *buf, size_t size, int64_t pts, bool key)
{
	if(!size)
		return;
	if(pts <= last_pts[stream])
		pts = last_pts[stream] + 1;
	last_pts[stream] = pts;
	packet->data = const_cast<uint8_t *>(buf);
	packet->size = (int)size;
	packet->stream_index = stream;
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
