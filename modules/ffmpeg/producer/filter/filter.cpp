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

#include "filter.h"

//#include "parallel_yadif.h"

#include "../../ffmpeg_error.h"
#include "../util/util.h"

#include <common/exception/exceptions.h>

#include <boost/assign.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/assign.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/rational.hpp>

#include <cstdio>
#include <sstream>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavutil/avutil.h>
	#include <libavutil/imgutils.h>
	#include <libavfilter/avfilter.h>
	#include <libavcodec/avcodec.h>
	#include <libavfilter/avfiltergraph.h>
	#include <libavfilter/buffersink.h>
	#include <libavfilter/buffersrc.h>
	#include <libavutil/opt.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#if defined(FF)
#undef FF
#endif

#define FF(call) THROW_ON_ERROR2(call, "[filter]")

namespace caspar { namespace ffmpeg {

struct filter::implementation
{
	std::string						filtergraph_;
	std::shared_ptr<AVFilterGraph>	video_graph_;	
    AVFilterContext*				video_graph_in_;
    AVFilterContext*				video_graph_out_;
	std::vector<AVPixelFormat>		pix_fmts_;
	const AVPixelFormat				pix_format_;
	const int						width_;
	const int						height_;
	const boost::rational<int>		in_time_base_;
	const boost::rational<int>		in_frame_rate_;
	const boost::rational<int>		in_sample_aspect_ratio_;
	std::queue<std::shared_ptr<AVFrame>>	fast_path_;

	implementation(
		int in_width,
		int in_height,
		boost::rational<int> in_time_base,
		boost::rational<int> in_frame_rate,
		boost::rational<int> in_sample_aspect_ratio,
		AVPixelFormat in_pix_fmt,
		std::vector<AVPixelFormat> out_pix_fmts,
		const std::string& filtergraph
		) 
		: filtergraph_(boost::to_lower_copy(filtergraph))
		, pix_format_(in_pix_fmt)
		, width_(in_width)
		, height_(in_height)
		, in_time_base_(in_time_base)
		, in_frame_rate_(in_frame_rate)
		, in_sample_aspect_ratio_(in_sample_aspect_ratio)
	{
		if(pix_fmts_.empty())
		{
			pix_fmts_ = boost::assign::list_of
				(AV_PIX_FMT_YUVA420P)
				(AV_PIX_FMT_YUV444P)
				(AV_PIX_FMT_YUV422P)
				(AV_PIX_FMT_YUV420P)
				(AV_PIX_FMT_YUV411P)
				(AV_PIX_FMT_BGRA)
				(AV_PIX_FMT_ARGB)
				(AV_PIX_FMT_RGBA)
				(AV_PIX_FMT_ABGR)
				(AV_PIX_FMT_GRAY8);
		}		
		pix_fmts_.push_back(AV_PIX_FMT_NONE);

		configure_filtergraph();
	}
	void configure_filtergraph()
	{
		if (filtergraph_.empty())
		{
			video_graph_.reset();
			return;
		}

		video_graph_.reset(
			avfilter_graph_alloc(), 
			[](AVFilterGraph* p)
			{
				avfilter_graph_free(&p);
			});
		video_graph_->nb_threads  = 0;
		video_graph_->thread_type = AVFILTER_THREAD_SLICE;
				
		const auto vsrc_options = (boost::format("video_size=%1%x%2%:pix_fmt=%3%:time_base=%4%/%5%:pixel_aspect=%6%/%7%:frame_rate=%8%/%9%")
			% width_ % height_
			% pix_format_
			% in_time_base_.numerator() % in_time_base_.denominator()
			% in_sample_aspect_ratio_.numerator() % in_sample_aspect_ratio_.denominator()
			% in_frame_rate_.numerator() % in_frame_rate_.denominator()).str();

		AVFilterContext* filt_vsrc = nullptr;			
		FF(avfilter_graph_create_filter(
			&filt_vsrc,
			avfilter_get_by_name("buffer"), 
			"filter_buffer",
			vsrc_options.c_str(), 
			nullptr, 
			video_graph_.get()));
				
		AVFilterContext* filt_vsink = nullptr;
		FF(avfilter_graph_create_filter(
			&filt_vsink,
			avfilter_get_by_name("buffersink"), 
			"filter_buffersink",
			nullptr, 
			nullptr, 
			video_graph_.get()));
		
#pragma warning (push)
#pragma warning (disable : 4245)

		FF(av_opt_set_int_list(
			filt_vsink, 
			"pix_fmts", 
			pix_fmts_.data(), 
			-1,
			AV_OPT_SEARCH_CHILDREN));

#pragma warning (pop)
		try
		{
			configure_filtergraph(
				*video_graph_,
				filtergraph_,
				*filt_vsrc,
				*filt_vsink);
			video_graph_in_ = filt_vsrc;
			video_graph_out_ = filt_vsink;

			CASPAR_LOG(trace) << L"Filter configured:\n" << avfilter_graph_dump(video_graph_.get(), nullptr);
		}
		catch (...)
		{
			CASPAR_LOG(error) << L"Cannot configure filtergraph: " << filtergraph_.c_str();
			filtergraph_.clear(); // disable filtering on configure_filtergraph  failure
		}
	}
	
	void configure_filtergraph(
		AVFilterGraph& graph, 
		const std::string& filtergraph, 
		AVFilterContext& source_ctx, 
		AVFilterContext& sink_ctx)
	{
		AVFilterInOut* outputs = nullptr;
		AVFilterInOut* inputs = nullptr;

		try
		{
			if(!filtergraph.empty()) 
			{
				outputs = avfilter_inout_alloc();
				inputs  = avfilter_inout_alloc();

				CASPAR_VERIFY(outputs && inputs);

				outputs->name       = av_strdup("in");
				outputs->filter_ctx = &source_ctx;
				outputs->pad_idx    = 0;
				outputs->next       = nullptr;

				inputs->name        = av_strdup("out");
				inputs->filter_ctx  = &sink_ctx;
				inputs->pad_idx     = 0;
				inputs->next        = nullptr;

				FF(avfilter_graph_parse(
					&graph, 
					filtergraph.c_str(), 
					inputs,
					outputs,
					nullptr));
			} 
			else 
			{
				FF(avfilter_link(
					&source_ctx, 
					0, 
					&sink_ctx, 
					0));
			}

			FF(avfilter_graph_config(
				&graph, 
				nullptr));
		}
		catch(...)
		{
			avfilter_inout_free(&outputs);
			avfilter_inout_free(&inputs);
			throw;
		}
	}


	bool fast_path() const
	{
		return filtergraph_.empty();
	}

	void push(const std::shared_ptr<AVFrame>& frame)
	{		
		if (fast_path())
			fast_path_.push(frame);
		else
			FF(av_buffersrc_add_frame(
				video_graph_in_, 
				frame.get()));
	}

	std::shared_ptr<AVFrame> poll()
	{
		if (fast_path())
		{
			if (fast_path_.empty())
				return nullptr;

			auto result = fast_path_.front();
			fast_path_.pop();
			return result;
		}

		auto filt_frame = ffmpeg::create_frame();
		
		const auto ret = av_buffersink_get_frame(
			video_graph_out_, 
			filt_frame.get());
				
		if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
			return nullptr;
					
		FF_RET(ret, "poll");

		return filt_frame;	
	}

	void clear()
	{
		do {} while (poll() != nullptr);
	}

	bool is_frame_format_changed(const std::shared_ptr<AVFrame>& frame)
	{
		return pix_format_ != frame->format || width_ != frame->width || height_ != frame->height;
	}

};

filter::filter(
		int in_width,
		int in_height,
		boost::rational<int> in_time_base,
		boost::rational<int> in_frame_rate,
		boost::rational<int> in_sample_aspect_ratio,
		AVPixelFormat in_pix_fmt,
		std::vector<AVPixelFormat> out_pix_fmts,
		const std::string& filtergraph) 
		: impl_(new implementation(
			in_width,
			in_height,
			in_time_base,
			in_frame_rate,
			in_sample_aspect_ratio,
			in_pix_fmt,
			out_pix_fmts,
			filtergraph)){}
filter::filter(filter&& other) : impl_(std::move(other.impl_)){}
filter& filter::operator=(filter&& other){impl_ = std::move(other.impl_); return *this;}
void filter::push(const std::shared_ptr<AVFrame>& frame){impl_->push(frame);}
std::shared_ptr<AVFrame> filter::poll(){return impl_->poll();}
std::string filter::filter_str() const{return impl_->filtergraph_;}
std::vector<safe_ptr<AVFrame>> filter::poll_all()
{	
	std::vector<safe_ptr<AVFrame>> frames;
	for(auto frame = poll(); frame; frame = poll())
		frames.push_back(make_safe_ptr(frame));
	return frames;
}
void filter::clear() { impl_->clear(); }
bool filter::is_frame_format_changed(const std::shared_ptr<AVFrame>& frame) { return impl_->is_frame_format_changed(frame);}

}}