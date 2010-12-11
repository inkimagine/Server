#include "..\StdAfx.h"

#include "frame_producer_device.h"

#include "layer.h"

#include "../format/video_format.h"
#include "../processor/composite_frame.h"
#include "../processor/producer_frame.h"

#include "../../common/utility/scope_exit.h"
#include "../../common/concurrency/executor.h"

#include <boost/thread.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/foreach.hpp>

#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/mutex.h>
	
namespace caspar { namespace core {
	
std::vector<producer_frame> receive(std::map<int, layer>& layers)
{	
	std::vector<producer_frame> frames(layers.size(), producer_frame::empty());
	tbb::parallel_for(tbb::blocked_range<size_t>(0, frames.size()), 
	[&](const tbb::blocked_range<size_t>& r)
	{
		auto it = layers.begin();
		std::advance(it, r.begin());
		for(size_t i = r.begin(); i != r.end(); ++i, ++it)
			frames[i] = it->second.receive();
	});		
	return frames;
}

struct frame_producer_device::implementation : boost::noncopyable
{	
	implementation(const frame_processor_device_ptr& frame_processor)  : frame_processor_(frame_processor)
	{
		executor_.start();
		executor_.begin_invoke([=]{tick();});
	}
					
	void tick()
	{		
		try
		{	
			frame_processor_->send(composite_frame(receive(layers_)));
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			layers_.clear();
			CASPAR_LOG(error) << "Unexpected exception. Cleared layers in render-device";
		}
		executor_.begin_invoke([=]{tick();});
	}

	void load(int render_layer, const frame_producer_ptr& producer, load_option::type option)
	{
		producer->initialize(frame_processor_);
		executor_.begin_invoke([=]
		{
			layers_[render_layer].load(producer, option);
		});
	}
			
	void pause(int render_layer)
	{		
		executor_.begin_invoke([=]
		{			
			auto it = layers_.find(render_layer);
			if(it != layers_.end())
				it->second.pause();		
		});
	}

	void play(int render_layer)
	{		
		executor_.begin_invoke([=]
		{
			auto it = layers_.find(render_layer);
			if(it != layers_.end())
				it->second.play();		
		});
	}

	void stop(int render_layer)
	{		
		executor_.begin_invoke([=]
		{
			auto it = layers_.find(render_layer);
			if(it != layers_.end())
			{
				it->second.stop();
				if(!it->second.background())
					layers_.erase(it);
			}
		});
	}

	void clear(int render_layer)
	{
		executor_.begin_invoke([=]
		{			
			auto it = layers_.find(render_layer);
			if(it != layers_.end())
			{
				it->second.clear();		
				layers_.erase(it);
			}
		});
	}
		
	void clear()
	{
		executor_.begin_invoke([=]
		{			
			layers_.clear();
		});
	}		

	boost::unique_future<frame_producer_ptr> foreground(int render_layer) const
	{
		return executor_.begin_invoke([=]() -> frame_producer_ptr
		{			
			auto it = layers_.find(render_layer);
			return it != layers_.end() ? it->second.foreground() : nullptr;
		});
	}
	
	boost::unique_future<frame_producer_ptr> background(int render_layer) const
	{
		return executor_.begin_invoke([=]() -> frame_producer_ptr
		{
			auto it = layers_.find(render_layer);
			return it != layers_.end() ? it->second.background() : nullptr;
		});
	}
	mutable common::executor executor_;
				
	frame_processor_device_ptr frame_processor_;
						
	std::map<int, layer> layers_;		
};

frame_producer_device::frame_producer_device(const frame_processor_device_ptr& frame_processor) : impl_(new implementation(frame_processor)){}
void frame_producer_device::load(int render_layer, const frame_producer_ptr& producer, load_option::type option){impl_->load(render_layer, producer, option);}
void frame_producer_device::pause(int render_layer){impl_->pause(render_layer);}
void frame_producer_device::play(int render_layer){impl_->play(render_layer);}
void frame_producer_device::stop(int render_layer){impl_->stop(render_layer);}
void frame_producer_device::clear(int render_layer){impl_->clear(render_layer);}
void frame_producer_device::clear(){impl_->clear();}
boost::unique_future<frame_producer_ptr> frame_producer_device::foreground(int render_layer) const {return impl_->foreground(render_layer);}
boost::unique_future<frame_producer_ptr> frame_producer_device::background(int render_layer) const {return impl_->background(render_layer);}
}}