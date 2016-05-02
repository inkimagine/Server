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

#include "../../stdafx.h"

#include "video_decoder.h"

#include "../util/util.h"

#include "../../ffmpeg_error.h"

#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/frame_factory.h>

#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/filesystem.hpp>

#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libavutil/imgutils.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct video_decoder::implementation : boost::noncopyable
{
	input 									input_;
	const safe_ptr<AVCodecContext>			codec_context_;
	const AVCodec*							codec_;
	int										stream_index_;
	const AVStream*							stream_;
	const uint32_t							nb_frames_;
	const size_t							width_;
	const size_t							height_;
	bool									is_progressive_;
	const int64_t							stream_start_pts_;
	tbb::atomic<int64_t>					seek_pts_;

public:
	explicit implementation(input input)
		: input_(input)
		, codec_context_(input.open_video_codec(stream_index_))
		, codec_(codec_context_->codec)
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, stream_(input_.format_context()->streams[stream_index_])
		, stream_start_pts_(stream_->start_time)
		, nb_frames_(static_cast<uint32_t>(stream_->nb_frames))
	{
		seek_pts_ = 0;
		CASPAR_LOG(trace) << "Codec: " << codec_->long_name;
	}

	std::shared_ptr<AVFrame> poll()
	{		
		std::shared_ptr<AVFrame> video = nullptr;
		std::shared_ptr<AVPacket> packet = nullptr;
		while (!video && input_.try_pop_video(packet))
			video = decode(packet);
		return video;
	}

	std::shared_ptr<AVFrame> decode(std::shared_ptr<AVPacket> pkt)
	{

		std::shared_ptr<AVFrame> decoded_frame = create_frame();

		int got_picture_ptr = 0;
		int bytes_consumed = avcodec_decode_video2(codec_context_.get(), decoded_frame.get(), &got_picture_ptr, pkt.get());//), "[video_decoder]");
		
		if(got_picture_ptr == 0 || bytes_consumed < 0)	
			return nullptr;

		is_progressive_ = !decoded_frame->interlaced_frame;

		if(decoded_frame->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
		int64_t frame_time_stamp = av_frame_get_best_effort_timestamp(decoded_frame.get());
		if (frame_time_stamp < seek_pts_)
			return nullptr;

		return fix_IMX_frame(decoded_frame);
	}

	// remove VBI lines from IMX frame
	std::shared_ptr<AVFrame> fix_IMX_frame(std::shared_ptr<AVFrame> frame)
	{
		if (codec_context_->codec_id == AV_CODEC_ID_MPEG2VIDEO && frame->width == 720 && frame->height == 608)
		{
			auto duplicate = create_frame();
			duplicate->width = frame->width;
			duplicate->interlaced_frame = frame->interlaced_frame;
			duplicate->top_field_first = frame->top_field_first;
			duplicate->format = frame->format;
			duplicate->height = 576;
			duplicate->flags = frame->flags;
			for (int i = 0; i < 4; i++)
				duplicate->linesize[i] = frame->linesize[i];
			if (av_frame_get_buffer(duplicate.get(), 1) < 0) goto error;
			for (int i = 0; i < 4; i++)
				memcpy(duplicate->data[i], frame->data[i] + ((32)*duplicate->linesize[i]), duplicate->linesize[i] * duplicate->height);
			return duplicate;
		}
	error:
		return frame;
	}
	
	void seek(uint64_t time)
	{
		avcodec_flush_buffers(codec_context_.get());
		seek_pts_ = stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_
			+ (time * stream_->time_base.den / (AV_TIME_BASE * stream_->time_base.num));
	}

	std::wstring print() const
	{		
		return L"[video-decoder] " + widen(codec_context_->codec->long_name);
	}
};

video_decoder::video_decoder(input input) : impl_(new implementation(input)){}
std::shared_ptr<AVFrame> video_decoder::poll(){return impl_->poll();}
size_t video_decoder::width() const{return impl_->width_;}
size_t video_decoder::height() const{return impl_->height_;}
uint32_t video_decoder::nb_frames() const{return impl_->nb_frames_;}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}
void video_decoder::seek(uint64_t time) { impl_->seek(time); }

}}