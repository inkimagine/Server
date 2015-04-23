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
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct video_decoder::implementation : boost::noncopyable
{
	int										index_;
	const safe_ptr<AVCodecContext>			codec_context_;
	const safe_ptr<AVFormatContext>			context_;
	const AVStream*							stream_;

	std::queue<safe_ptr<AVPacket>>			packets_;
	
	const uint32_t							nb_frames_;

	const size_t							width_;
	const size_t							height_;
	bool									is_progressive_;

	tbb::atomic<size_t>						file_frame_number_;
	tbb::atomic<int64_t>					packet_time_;
	const int64_t							stream_start_pts_;

public:
	explicit implementation(const safe_ptr<AVFormatContext>& context) 
		: codec_context_(open_codec(*context, AVMEDIA_TYPE_VIDEO, index_))
		, nb_frames_(static_cast<uint32_t>(context->streams[index_]->nb_frames))
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, context_(context)
		, stream_(context->streams[index_])
		, stream_start_pts_(stream_->start_time) 
	{
		file_frame_number_ = 0;
		packet_time_ = - ((stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_) * AV_TIME_BASE * stream_->time_base.num)/stream_->time_base.den;
	}

	void push(const std::shared_ptr<AVPacket>& packet)
	{
		if(!packet)
			return;

		if(packet->stream_index == index_ || packet->data == nullptr)
			packets_.push(make_safe_ptr(packet));
	}

	std::shared_ptr<AVFrame> poll()
	{		
		if(packets_.empty())
			return nullptr;
		
		auto packet = packets_.front();

		if(packet->data == nullptr)
		{			
			if(codec_context_->codec->capabilities & CODEC_CAP_DELAY)
			{
				auto video = decode(packet);
				if(video)
					return video;
			}
					
			packets_.pop();
			file_frame_number_ = static_cast<size_t>(packet->pos);
			avcodec_flush_buffers(codec_context_.get());
			return flush_video();	
		}
			
		packets_.pop();
		return decode(packet);
	}

	std::shared_ptr<AVFrame> decode(safe_ptr<AVPacket> pkt)
	{
		//int64_t packet_time = ((pkt->pts - (stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_)) * AV_TIME_BASE * stream_->time_base.num)/stream_->time_base.den;
		//CASPAR_LOG(info) << "Begin decode packet of time: " << packet_time;

		std::shared_ptr<AVFrame> decoded_frame(avcodec_alloc_frame(), av_free);

		int frame_finished = 0;
		THROW_ON_ERROR2(avcodec_decode_video2(codec_context_.get(), decoded_frame.get(), &frame_finished, pkt.get()), "[video_decoder]");
		
		// If a decoder consumes less then the whole packet then something is wrong
		// that might be just harmless padding at the end, or a problem with the
		// AVParser or demuxer which puted more then one frame in a AVPacket.

		if(frame_finished == 0)	
			return nullptr;

		is_progressive_ = !decoded_frame->interlaced_frame;

		if(decoded_frame->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
		
		if (!(stream_->avg_frame_rate.den == 0 || decoded_frame->pkt_pts == AV_NOPTS_VALUE))
			file_frame_number_ = static_cast<size_t>((decoded_frame->pkt_pts * stream_->time_base.num * stream_->avg_frame_rate.num) / (stream_->time_base.den*stream_->avg_frame_rate.den));
		else
			++file_frame_number_;

		if (decoded_frame->pkt_pts == AV_NOPTS_VALUE)
			if (stream_->avg_frame_rate.num > 0)
				packet_time_ = (AV_TIME_BASE * static_cast<int64_t>(file_frame_number_) * stream_->avg_frame_rate.den)/stream_->avg_frame_rate.num;
			else
				packet_time_ = std::numeric_limits<int64_t>().max();
		else
			packet_time_ = ((decoded_frame->pkt_pts - (stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_)) * AV_TIME_BASE * stream_->time_base.num)/stream_->time_base.den;
		//packet_time = ((pkt->pts - (stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_)) * AV_TIME_BASE * stream_->time_base.num)/stream_->time_base.den;
		//CASPAR_LOG(info) << "End decode packet of time: " << packet_time << " decoded frame time " << packet_time_;

		// This ties the life of the decoded_frame to the packet that it came from. For the
		// current version of ffmpeg (0.8 or c17808c) the RAW_VIDEO codec returns frame data
		// owned by the packet.
		return std::shared_ptr<AVFrame>(decoded_frame.get(), [decoded_frame, pkt](AVFrame*){});
	}
	
	bool ready() const
	{
		return packets_.size() >= 8;
	}

	bool empty() const
	{
		return packets_.size() == 0;
	}

	uint32_t nb_frames() const
	{
		return std::max<uint32_t>(nb_frames_, file_frame_number_);
	}

	std::wstring print() const
	{		
		return L"[video-decoder] " + widen(codec_context_->codec->long_name);
	}
};

video_decoder::video_decoder(const safe_ptr<AVFormatContext>& context) : impl_(new implementation(context)){}
void video_decoder::push(const std::shared_ptr<AVPacket>& packet){impl_->push(packet);}
std::shared_ptr<AVFrame> video_decoder::poll(){return impl_->poll();}
bool video_decoder::ready() const{return impl_->ready();}
bool video_decoder::empty() const{return impl_->empty();}
size_t video_decoder::width() const{return impl_->width_;}
size_t video_decoder::height() const{return impl_->height_;}
uint32_t video_decoder::nb_frames() const{return impl_->nb_frames();}
uint32_t video_decoder::file_frame_number() const{return impl_->file_frame_number_;}
int64_t video_decoder::packet_time() const{return impl_->packet_time_;}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}

}}