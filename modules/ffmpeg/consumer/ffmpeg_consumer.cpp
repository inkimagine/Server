/*
* Copyright 2013 Sveriges Television AB http://casparcg.com/
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/
 
#include "../StdAfx.h"

#include "../ffmpeg_error.h"
#include "../tbb_avcodec.h"
#include "../ffmpeg.h"

#include "ffmpeg_consumer.h"

#include <core/parameters/parameters.h>
#include <core/mixer/read_frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_format.h>
#include <core/recorder.h>

#include <common/concurrency/executor.h>
#include <common/concurrency/future_util.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/utility/string.h>
#include <common/memory/memshfl.h>

#include <boost/algorithm/string.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#include <boost/crc.hpp>
#pragma warning(pop)

#include <tbb/cache_aligned_allocator.h>
#include <tbb/parallel_invoke.h>
#include <tbb/atomic.h>

#include <boost/range/algorithm.hpp>
#include <boost/range/algorithm_ext.hpp>
#include <boost/lexical_cast.hpp>

#include <string>

namespace caspar { namespace ffmpeg {
	
AVFormatContext * alloc_output_format_context(const std::string filename, AVOutputFormat * output_format)
{
	AVFormatContext * ctx = nullptr;
	if (avformat_alloc_output_context2(&ctx, output_format, NULL, filename.c_str()) >= 0)
		return ctx;
	else
		return nullptr;
}

int crc16(const std::wstring& str)
{
	boost::crc_16_type result;
	result.process_bytes(str.data(), str.length());
	return result.checksum();
}

static const std::string			MXF = ".MXF";

struct output_format
{
	AVCodec *									video_codec;
	AVCodec *									audio_codec;
	AVOutputFormat*								format;
	const bool									is_mxf;
	const bool									is_widescreen;
	const int64_t								audio_bitrate;
	const int64_t								video_bitrate;

	output_format(const std::string& filename, AVCodec * acodec, AVCodec * vcodec, const bool is_stream, const bool is_wide, const int64_t a_rate, const int64_t v_rate)
		: format(av_guess_format(NULL, filename.c_str(), NULL))
		, video_codec(vcodec)
		, audio_codec(acodec)
		, is_widescreen(is_wide)
		, is_mxf(std::equal(MXF.rbegin(), MXF.rend(), boost::to_upper_copy(filename).rbegin()))
		, audio_bitrate(a_rate)
		, video_bitrate(v_rate)
	{
		if (is_mxf) // is MXF
			format = av_guess_format("mxf_d10", filename.c_str(), NULL);
		if (is_stream && !format)
			format = av_guess_format("mpegts", NULL, NULL);
		
		if (format)
		{
			if (!video_codec)
				video_codec = avcodec_find_encoder(format->video_codec);
			if (!audio_codec)
				audio_codec = avcodec_find_encoder(format->audio_codec);
		}
		if (!video_codec)
			video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!audio_codec)
			audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	}
};

AVDictionary ** read_parameters(std::string options) {
	AVDictionary ** result = new AVDictionary *;
	LOG_ON_ERROR2(av_dict_parse_string(result, options.c_str(), "=", ",", 0), L"Parameters unrecognized");
	return result;
}

typedef std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>	byte_vector;

struct ffmpeg_consumer : boost::noncopyable
{
	tbb::atomic<bool>						ready_;
	const std::string						filename_;
	AVDictionary **							options_;
	output_format							output_format_;
	const core::video_format_desc			format_desc_;

	const safe_ptr<diagnostics::graph>		graph_;

	executor								encode_executor_;

	AVFormatContext *						format_context_;

	AVStream *								audio_st_;
	AVStream *								video_st_;

	SwrContext *							swr_;
	SwsContext *							sws_;

	byte_vector								audio_bufers_[AV_NUM_DATA_POINTERS];
	byte_vector								key_picture_buf_;
	byte_vector								picture_buf_;

	int64_t									out_frame_number_;
	int64_t									out_audio_sample_number_;

	bool									key_only_;
	bool									audio_is_planar;
	tbb::atomic<int64_t>					current_encoding_delay_;

public:
	ffmpeg_consumer(const std::string& filename, const core::video_format_desc& format_desc, bool key_only, output_format format, const std::string options)
		: filename_(filename)
		, format_desc_(format_desc)
		, encode_executor_(print())
		, out_frame_number_(0)
		, out_audio_sample_number_(0)
		, output_format_(format)
		, key_only_(key_only)
		, options_(read_parameters(options))
		, sws_(nullptr)
		, swr_(nullptr)
		, format_context_(nullptr)
		, audio_st_(nullptr)
		, video_st_(nullptr)
	{
		current_encoding_delay_ = 0;
		ready_ = false;
		// TODO: Ask stakeholders about case where file already exists.
		boost::filesystem2::remove(filename); // Delete the file if it exists

		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		encode_executor_.set_capacity(8);

		try
		{
			format_context_ = alloc_output_format_context(filename_, output_format_.format);

			//  Add the audio and video streams using the default format codecs	and initialize the codecs.

			video_st_ = add_video_stream();
			if (!key_only_)
				audio_st_ = add_audio_stream();

			av_dump_format(format_context_, 0, filename_.c_str(), 1);

			// Open the output
			if (!(format_context_->oformat->flags & AVFMT_NOFILE))
				avio_open(&format_context_->pb, filename_.c_str(), AVIO_FLAG_WRITE | AVIO_FLAG_NONBLOCK);
			THROW_ON_ERROR2(avformat_write_header(format_context_, options_), "[ffmpeg_consumer]");
			char * unused_options;
			if (options_ 
				&& av_dict_count(*options_) > 0
				&& av_dict_get_string(*options_, &unused_options, '=', ',') >= 0)
				CASPAR_LOG(warning) << print() << L" Unrecognized FFMpeg options: " << widen(std::string(unused_options));
		}
		catch (...)
		{
			cleanup();
			boost::filesystem2::remove(filename_); // Delete the file if exists and consumer not fully initialized
			throw;
		}
		ready_ = true;
		CASPAR_LOG(info) << print() << L" Successfully Initialized.";
	}

	~ffmpeg_consumer()
	{
		ready_ = false;
		encode_executor_.stop();
		encode_executor_.join();
		
		if ((video_st_ && (video_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY))
			|| (!key_only_ && (audio_st_ && (audio_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY))))
			flush_encoders();
		LOG_ON_ERROR2(av_write_trailer(format_context_), "[ffmpeg_consumer]");
		cleanup();

		CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
	}

	void cleanup() 
	{
		if (video_st_)
			avcodec_close(video_st_->codec);
		if (audio_st_)
			avcodec_close(audio_st_->codec);
		if (swr_)
			swr_free(&swr_);
		sws_freeContext(sws_); //if it's null, it does nothing

		if (format_context_)
		{
			if (!(format_context_->oformat->flags & AVFMT_NOFILE))
				LOG_ON_ERROR2(avio_close(format_context_->pb), "[ffmpeg_consumer]"); // Close the output ffmpeg.
			avformat_free_context(format_context_);
		}
		if (options_)
			av_dict_free(options_);
	}

	std::wstring print() const
	{
		return L"ffmpeg_consumer[" + widen(filename_) + L"]";
	}

	AVStream* add_video_stream()
	{
		if (!output_format_.video_codec)
			return NULL;
		AVCodec * encoder = output_format_.video_codec;

		if (!encoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Codec not found."));

		AVStream * st = avformat_new_stream(format_context_, encoder);
		if (!st)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate video-stream.") << boost::errinfo_api_function("av_new_stream"));

		st->id = 0;
		st->time_base = av_make_q(format_desc_.duration, format_desc_.time_scale);

		AVCodecContext * c = st->codec;

		c->refcounted_frames = 0;
		c->codec_id = encoder->id;
		c->codec_type = AVMEDIA_TYPE_VIDEO;
		c->width = format_desc_.width;
		c->height = format_desc_.height;
		c->gop_size = 25;
		c->time_base = st->time_base;
		c->flags |= format_desc_.field_mode == core::field_mode::progressive ? 0 : (CODEC_FLAG_INTERLACED_ME | CODEC_FLAG_INTERLACED_DCT);
		if (c->pix_fmt == AV_PIX_FMT_NONE)
			c->pix_fmt = AV_PIX_FMT_YUV420P;

		if (c->codec_id == AV_CODEC_ID_PRORES)
		{
			c->bit_rate = c->width < 1280 ? 63 * 1000000 : 220 * 1000000;
			c->pix_fmt = AV_PIX_FMT_YUV422P10;
		}
		else if (c->codec_id == AV_CODEC_ID_DNXHD)
		{
			if (c->width < 1280 || c->height < 720)
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Unsupported video dimensions."));

			c->bit_rate = 220 * 1000000;
			c->pix_fmt = AV_PIX_FMT_YUV422P;
		}
		else if (c->codec_id == AV_CODEC_ID_DVVIDEO)
		{
			c->width = c->height == 1280 ? 960 : c->width;

			if (format_desc_.format == core::video_format::ntsc)
				c->pix_fmt = AV_PIX_FMT_YUV411P;
			else if (format_desc_.format == core::video_format::pal)
				c->pix_fmt = AV_PIX_FMT_YUV420P;
			else // dv50
				c->pix_fmt = AV_PIX_FMT_YUV422P;

			if (format_desc_.duration == 1001)
				c->width = c->height == 1080 ? 1280 : c->width;
			else
				c->width = c->height == 1080 ? 1440 : c->width;
		}
		else if (c->codec_id == AV_CODEC_ID_H264)
		{
			c->pix_fmt = AV_PIX_FMT_YUV420P;
			c->bit_rate = format_desc_.height * 14 * 1000; // about 8Mbps for SD, 14 for HD
			LOG_ON_ERROR2(av_opt_set(c->priv_data, "preset", "veryfast", NULL), "[ffmpeg_consumer]");
		}
		else if (c->codec_id == AV_CODEC_ID_QTRLE)
		{
			c->pix_fmt = AV_PIX_FMT_ARGB;
		}
		else if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
		{
			if (output_format_.is_mxf && format_desc_.format == core::video_format::pal)
			{
				// IMX50 encoding parameters
				c->pix_fmt = AV_PIX_FMT_YUV422P;
				c->bit_rate = 50 * 1000000;
				c->rc_max_rate = c->bit_rate;
				c->rc_min_rate = c->bit_rate;
				c->rc_buffer_size = 2000000;
				c->rc_initial_buffer_occupancy = 2000000;
				c->rc_buffer_aggressivity = 0.25;
				c->gop_size = 1;
			}
			else
			{
				c->pix_fmt = AV_PIX_FMT_YUV422P;
				c->bit_rate = 15 * 1000000;
			}
		}
		if (output_format_.video_bitrate != 0)
			c->bit_rate = output_format_.video_bitrate * 1024;

		c->max_b_frames = 0; // b-frames not supported.

		AVRational sample_aspect_ratio;
		switch (format_desc_.format) {
		case caspar::core::video_format::pal:
			sample_aspect_ratio = output_format_.is_widescreen ? av_make_q(64, 45) : av_make_q(16, 15);
			break;
		case caspar::core::video_format::ntsc:
			sample_aspect_ratio = output_format_.is_widescreen ? av_make_q(32, 27) : av_make_q(8, 9);
			break;
		default:
			sample_aspect_ratio = av_make_q(1, 1);
			break;
		}


		if (output_format_.format->flags & AVFMT_GLOBALHEADER)
			c->flags |= CODEC_FLAG_GLOBAL_HEADER;
		c->sample_aspect_ratio = sample_aspect_ratio;

		if (tbb_avcodec_open(c, encoder, options_, true) < 0)
		{
			CASPAR_LOG(debug) << print() << L" Multithreaded avcodec_open2 failed";
			c->thread_count = 1;
			THROW_ON_ERROR2(avcodec_open2(c, encoder, options_), "[ffmpeg_consumer]");
		}
		return st;
	}

	AVStream * add_audio_stream()
	{
		if (!output_format_.audio_codec)
			return NULL;

		AVCodec * encoder = output_format_.audio_codec;
		if (!encoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("codec not found") << boost::errinfo_api_function("avcodec_find_encoder"));

		auto st = avformat_new_stream(format_context_, encoder);
		if (!st)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate audio-stream") << boost::errinfo_api_function("av_new_stream"));
		st->id = 1;


		AVCodecContext * c = st->codec;
		c->refcounted_frames = 0;
		c->codec_id = encoder->id;
		c->codec_type = AVMEDIA_TYPE_AUDIO;
		c->sample_rate = format_desc_.audio_sample_rate; 
		c->channels = 2;
		c->channel_layout = av_get_default_channel_layout(c->channels);
		c->profile = FF_PROFILE_UNKNOWN;
		c->sample_fmt = encoder->sample_fmts[0];
		if (encoder->id == AV_CODEC_ID_FLV1)
			c->sample_rate = 44100;

		if (encoder->id == AV_CODEC_ID_AAC)
		{
			c->sample_fmt = AV_SAMPLE_FMT_FLTP;
			c->profile = FF_PROFILE_AAC_MAIN;
			c->bit_rate = 160 * 1024;
		}
		if (output_format_.is_mxf)
		{
			c->channels = 4;
			c->channel_layout = AV_CH_LAYOUT_4POINT0;
			c->sample_fmt = AV_SAMPLE_FMT_S16;
			c->bit_rate_tolerance = 0;
		}

		if (output_format_.format->flags & AVFMT_GLOBALHEADER)
			c->flags |= CODEC_FLAG_GLOBAL_HEADER;
		
		if (output_format_.audio_bitrate != 0)
			c->bit_rate = output_format_.audio_bitrate * 1024;

		audio_is_planar = av_sample_fmt_is_planar(c->sample_fmt) != 0;

		THROW_ON_ERROR2(tbb_avcodec_open(c, encoder, options_, true), "[ffmpeg_consumer]");

		return st;
	}

	std::shared_ptr<AVFrame> convert_video(core::read_frame& frame, AVCodecContext* c)
	{
		if (!sws_)
		{
			sws_ = sws_getContext(format_desc_.width, format_desc_.height, AV_PIX_FMT_BGRA, c->width, c->height, c->pix_fmt, SWS_BICUBIC, nullptr, nullptr, NULL);
			if (sws_ == nullptr)
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Cannot initialize the conversion context"));
		}
		std::shared_ptr<AVFrame> in_frame(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });
		auto in_picture = reinterpret_cast<AVPicture*>(in_frame.get());

		if (key_only_)
		{
			key_picture_buf_.resize(frame.image_data().size());
			in_picture->linesize[0] = format_desc_.width * 4; //AV_PIX_FMT_BGRA
			in_picture->data[0] = key_picture_buf_.data();

			fast_memshfl(in_picture->data[0], frame.image_data().begin(), frame.image_data().size(), 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);
		}
		else
		{
			avpicture_fill(in_picture, const_cast<uint8_t*>(frame.image_data().begin()), AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height);
		}

		std::shared_ptr<AVFrame> out_frame(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });
		picture_buf_.resize(av_image_get_buffer_size(c->pix_fmt, c->width, c->height, 16));
		av_image_fill_arrays(out_frame->data, out_frame->linesize, picture_buf_.data(), c->pix_fmt, c->width, c->height, 16);

		sws_scale(sws_, in_frame->data, in_frame->linesize, 0, format_desc_.height, out_frame->data, out_frame->linesize);
		out_frame->width = format_desc_.width;
		out_frame->height = format_desc_.height;
		out_frame->format = c->pix_fmt;

		return out_frame;
	}

	void encode_video_frame(core::read_frame& frame)
	{
		AVCodecContext * codec_context = video_st_->codec;

		auto av_frame = convert_video(frame, codec_context);
		av_frame->interlaced_frame = format_desc_.field_mode != core::field_mode::progressive;
		av_frame->top_field_first = format_desc_.field_mode == core::field_mode::upper;
		av_frame->pts = out_frame_number_++;

		std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });

		av_init_packet(pkt.get());
		int got_packet;

		avcodec_encode_video2(codec_context, pkt.get(), av_frame.get(), &got_packet);// , "[video_encoder]");

		if (got_packet == 0)
			return;

		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts = av_rescale_q(pkt->pts, codec_context->time_base, video_st_->time_base);
		if (pkt->dts != AV_NOPTS_VALUE)
			pkt->dts = av_rescale_q(pkt->dts, codec_context->time_base, video_st_->time_base);

		if (codec_context->coded_frame->key_frame)
			pkt->flags |= AV_PKT_FLAG_KEY;

		pkt->stream_index = video_st_->index;
		THROW_ON_ERROR2(av_interleaved_write_frame(format_context_, pkt.get()), "[video_encoder]");
	}
	
	void resample_audio(core::read_frame& frame, AVCodecContext* ctx)
	{
		if (!swr_)
		{
			uint64_t out_channel_layout = av_get_default_channel_layout(ctx->channels);
			uint64_t in_channel_layout = create_channel_layout_bitmask(frame.num_channels());
			swr_ = swr_alloc_set_opts(nullptr,
				out_channel_layout,
				ctx->sample_fmt,
				ctx->sample_rate,
				in_channel_layout,
				AV_SAMPLE_FMT_S32,
				format_desc_.audio_sample_rate,
				0, nullptr);
			if (!swr_)
				BOOST_THROW_EXCEPTION(caspar_exception()
					<< msg_info("Cannot alloc audio resampler"));
			THROW_ON_ERROR2(swr_init(swr_), "[audio_encoder]");
		}
		byte_vector out_buffers[AV_NUM_DATA_POINTERS];
		const int in_samples_count = frame.audio_data().size() / frame.num_channels();
		const int out_samples_count = static_cast<int>(av_rescale_rnd(in_samples_count, ctx->sample_rate, format_desc_.audio_sample_rate, AV_ROUND_UP));
		if (audio_is_planar)
			for (char i = 0; i < ctx->channels; i++)
				out_buffers[i].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32));
		else
			out_buffers[0].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32) *ctx->channels);

		const uint8_t* in[] = { reinterpret_cast<const uint8_t*>(frame.audio_data().begin()) };
		uint8_t*       out[AV_NUM_DATA_POINTERS];
		for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
			out[i] = out_buffers[i].data();

		int converted_sample_count = swr_convert(swr_,
			out, out_samples_count,
			in, in_samples_count);
		if (audio_is_planar)
			for (char i = 0; i < ctx->channels; i++)
			{
				out_buffers[i].resize(converted_sample_count * av_get_bytes_per_sample(ctx->sample_fmt));
				boost::range::push_back(audio_bufers_[i], out_buffers[i]);
			}
		else
		{
			out_buffers[0].resize(converted_sample_count * av_get_bytes_per_sample(ctx->sample_fmt) * ctx->channels);
			boost::range::push_back(audio_bufers_[0], out_buffers[0]);
		}
	}

	std::int64_t create_channel_layout_bitmask(int num_channels)
	{
		if (num_channels > 63)
			BOOST_THROW_EXCEPTION(caspar_exception("FFMpeg cannot handle more than 63 audio channels"));
		const auto ALL_63_CHANNELS = 0x7FFFFFFFFFFFFFFFULL;
		auto to_shift = 63 - num_channels;
		auto result = ALL_63_CHANNELS >> to_shift;
		return static_cast<std::int64_t>(result);
	}
	
	void encode_audio_frame(core::read_frame& frame)
	{			
		AVCodecContext * enc = audio_st_->codec;
		resample_audio(frame, enc);
		size_t input_audio_size = enc->frame_size == 0 ? 
			audio_bufers_[0].size() :
			enc->frame_size * av_get_bytes_per_sample(enc->sample_fmt) * enc->channels;
		int frame_size = input_audio_size / (av_get_bytes_per_sample(enc->sample_fmt) * enc->channels);
		while (audio_bufers_[0].size() >= input_audio_size)
		{
			std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });
			std::shared_ptr<AVFrame> in_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
			in_frame->nb_samples = frame_size;
			in_frame->pts = out_audio_sample_number_;
			out_audio_sample_number_ += frame_size;
			uint8_t* out_buffers[AV_NUM_DATA_POINTERS];
			for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
				out_buffers[i] = audio_bufers_[i].data();
			THROW_ON_ERROR2(avcodec_fill_audio_frame(in_frame.get(), enc->channels, enc->sample_fmt, (const uint8_t *)out_buffers[0], input_audio_size, 0), "[audio_encoder]");
			if (audio_is_planar)
				for (char i = 0; i < enc->channels; i++)
					in_frame->data[i] = audio_bufers_[i].data();
			int got_packet;
			THROW_ON_ERROR2(avcodec_encode_audio2(enc, pkt.get(), in_frame.get(), &got_packet), "[audio_encoder]");
			if (audio_is_planar)
				for (char i = 0; i < enc->channels; i++)
					audio_bufers_[i].erase(audio_bufers_[i].begin(), audio_bufers_[i].begin() + (enc->frame_size * av_get_bytes_per_sample(enc->sample_fmt)));
			else
				audio_bufers_[0].erase(audio_bufers_[0].begin(), audio_bufers_[0].begin() + input_audio_size);
			if (!got_packet)
				return;
			if (pkt->pts != AV_NOPTS_VALUE)
				pkt->pts = av_rescale_q(pkt->pts, enc->time_base, audio_st_->time_base);
			if (pkt->dts != AV_NOPTS_VALUE)
				pkt->dts = av_rescale_q(pkt->dts, enc->time_base, audio_st_->time_base);
			pkt->stream_index = audio_st_->index;
			THROW_ON_ERROR2(av_interleaved_write_frame(format_context_, pkt.get()), "[audio_encoder]");
		}
	}
		 
	void send(const safe_ptr<core::read_frame>& frame)
	{
		encode_executor_.begin_invoke([=] {		
			boost::timer frame_timer;

			encode_video_frame(*frame);

			if (!key_only_)
				encode_audio_frame(*frame);

			graph_->set_value("frame-time", frame_timer.elapsed()*format_desc_.fps*0.5);
			current_encoding_delay_ = frame->get_age_millis();
		});
	}

	bool ready_for_frame()
	{
		return ready_ && (encode_executor_.size() < encode_executor_.capacity());
	}

	void mark_dropped()
	{
		graph_->set_tag("dropped-frame");

		// TODO: adjust PTS accordingly to make dropped frames contribute
		//       to the total playing time
	}

	void flush_encoders()
	{
		bool audio_full = false;
		bool video_full = false;
		bool need_flush_audio = !key_only_ && audio_st_ && (audio_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY) != 0;
		bool need_flush_video = video_st_ && (video_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY) != 0;
		do
		{
			audio_full |= !need_flush_audio || flush_stream(false);
			video_full |= !need_flush_video || flush_stream(true);
		} while (!audio_full || !video_full);
	}

	
	bool flush_stream(bool video)
	{
		std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });

		av_init_packet(pkt.get());
		auto stream = video ? video_st_ : audio_st_;
		int got_packet;
		if (video)
			THROW_ON_ERROR2(avcodec_encode_video2(stream->codec, pkt.get(), NULL, &got_packet), "[flush_video]");
		else
			THROW_ON_ERROR2(avcodec_encode_audio2(stream->codec, pkt.get(), NULL, &got_packet), "[flush_audio]");

		if (got_packet == 0)
			return true;

		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts = av_rescale_q(pkt->pts, stream->codec->time_base, stream->time_base);
		if (pkt->dts != AV_NOPTS_VALUE)
			pkt->dts = av_rescale_q(pkt->dts, stream->codec->time_base, stream->time_base);

		if (stream->codec->coded_frame->key_frame)
			pkt->flags |= AV_PKT_FLAG_KEY;

		pkt->stream_index = stream->index;
		THROW_ON_ERROR2(av_interleaved_write_frame(format_context_, pkt.get()), "[flush_stream]");
		return false;
	}

};

struct ffmpeg_consumer_proxy : public core::frame_consumer
{
	int								index_;
	const std::wstring				filename_;
	const bool						separate_key_;
	output_format					output_format_;
	core::video_format_desc			format_desc_;
	const std::string				options_;
	const int						tc_in_;
	const int						tc_out_;
	core::recorder*					recorder_;

	std::unique_ptr<ffmpeg_consumer> consumer_;
	std::unique_ptr<ffmpeg_consumer> key_only_consumer_;

public:

	ffmpeg_consumer_proxy(const std::wstring& filename, output_format format, const std::string options, const bool separate_key, core::recorder* recorder = nullptr, const int tc_in = 0, const int tc_out = std::numeric_limits<int>().max())
		: filename_(filename)
		, separate_key_(separate_key)
		, output_format_(format)
		, options_(options)
		, index_(100000 + crc16(boost::to_lower_copy(filename)))
		, tc_in_(tc_in)
		, tc_out_(tc_out)
		, recorder_(recorder)
	{
	}
	
	virtual void initialize(const core::video_format_desc& format_desc, int)
	{	
		format_desc_ = format_desc;
		consumer_.reset();
		key_only_consumer_.reset();
		consumer_.reset(new ffmpeg_consumer(
			narrow(filename_),
			format_desc_,
			false,
			output_format_,
			options_
			));

		if (separate_key_)
		{
			boost::filesystem::wpath fill_file(filename_);
			auto without_extension = fill_file.stem();
			auto key_file = env::media_folder() + without_extension + L"_A" + fill_file.extension();

			key_only_consumer_.reset(new ffmpeg_consumer(
				narrow(key_file),
				format_desc_,
				true, 
				output_format_,
				options_
			));
		}
	}

	virtual int64_t presentation_frame_age_millis() const override
	{
		return consumer_ ? consumer_->current_encoding_delay_ : 0;
	}
	
	virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
	{
		bool ready_for_frame = consumer_->ready_for_frame();

		if (ready_for_frame && separate_key_)
			ready_for_frame = ready_for_frame && key_only_consumer_->ready_for_frame();

		if (ready_for_frame)
		{
			if (recorder_)
			{
				int timecode = frame->get_timecode();
				if (timecode == std::numeric_limits<int>().max())
					timecode = recorder_->GetTimecode();
				if (timecode == std::numeric_limits<int>().max() 
					|| (timecode >= tc_in_ && timecode < tc_out_))
				{
					consumer_->send(frame);
					if (separate_key_)
						key_only_consumer_->send(frame);
				}
			}
			else
			{
				consumer_->send(frame);
				if (separate_key_)
					key_only_consumer_->send(frame);
			}
		}
		else
		{
			consumer_->mark_dropped();

			if (separate_key_)
				key_only_consumer_->mark_dropped();
		}
		return caspar::wrap_as_future(true);
	}
	
	virtual std::wstring print() const override
	{
		return consumer_ ? consumer_->print() : L"[ffmpeg_consumer]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"ffmpeg_consumer");
		info.add(L"filename", filename_);
		info.add(L"separate_key", separate_key_);
		return info;
	}
		
	virtual bool has_synchronization_clock() const override
	{
		return false;
	}

	virtual size_t buffer_depth() const override
	{
		return 1;
	}

	virtual int index() const override
	{
		return index_;
	}

};	

safe_ptr<core::frame_consumer> create_recorder_consumer(const std::wstring filename, const core::parameters& params, const int tc_in, const int tc_out, core::recorder* recorder)
{
	std::wstring acodec = params.get_original(L"ACODEC");
	std::wstring vcodec = params.get_original(L"VCODEC");
	std::wstring options = params.get_original(L"OPTIONS");
	int64_t		 arate = params.get(L"ARATE", 0LL);
	int64_t		 vrate = params.get(L"VRATE", 0LL);
	output_format format(
		narrow(filename),
		avcodec_find_encoder_by_name(narrow(acodec).c_str()),
		avcodec_find_encoder_by_name(narrow(vcodec).c_str()),
		false,
		!params.has(L"NARROW"),
		arate,
		vrate
	);
	return make_safe<ffmpeg_consumer_proxy>(env::media_folder() + filename, format, narrow(options), false, recorder, tc_in, tc_out);
}

safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params)
{
	if(params.size() < 1 || (params[0] != L"FILE" && params[0] != L"STREAM"))
		return core::frame_consumer::empty();

	auto params2 = params;
	
	std::wstring filename	= (params2.size() > 1 ? params2.at_original(1) : L"");
	bool separate_key = params2.remove_if_exists(L"SEPARATE_KEY");
	bool is_stream = params2[0] == L"STREAM";
	std::wstring acodec = params2.get_original(L"ACODEC");
	std::wstring vcodec = params2.get_original(L"VCODEC");
	std::wstring options = params2.get_original(L"OPTIONS");
	int64_t		 arate  = params2.get(L"ARATE", 0LL);
	int64_t		 vrate	= params2.get(L"VRATE", 0LL);
	output_format format(
		narrow(filename),
		avcodec_find_encoder_by_name(narrow(acodec).c_str()),
		avcodec_find_encoder_by_name(narrow(vcodec).c_str()),
		is_stream,
		!params2.remove_if_exists(L"NARROW"),
		arate,
		vrate
	);
	
	return make_safe<ffmpeg_consumer_proxy>(is_stream ? filename : env::media_folder() + filename, format, narrow(options), separate_key);
}

safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree)
{
	auto filename		= ptree.get<std::wstring>(L"path");
	auto vcodec			= ptree.get(L"vcodec", L"libx264");
	auto acodec			= ptree.get(L"acodec", L"aac");
	auto separate_key	= ptree.get(L"separate-key", false);
	auto vrate			= ptree.get(L"vrate", 0LL);
	auto arate			= ptree.get(L"arate", 0LL);
	auto options = ptree.get(L"options", L"");
	output_format format(
		narrow(filename),
		avcodec_find_encoder_by_name(narrow(acodec).c_str()),
		avcodec_find_encoder_by_name(narrow(vcodec).c_str()),
		true,
		!ptree.get(L"narrow", true),
		arate,
		vrate
	);

	return make_safe<ffmpeg_consumer_proxy>(filename, format, narrow(options), separate_key);
}

}}
