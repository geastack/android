#include "services/frame_scheduler.h"

#include <chrono>
#include <cstdint>

namespace {

int millisSinceBoot()
{
	using Clock = std::chrono::steady_clock;
	return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

int g_frame_interval_ms = gea::framework::services::FrameScheduler::kDefaultFrameIntervalMs;

}  // namespace

extern "C" int gea_embedded_now_ms(void)
{
	return millisSinceBoot();
}

namespace gea::framework::services {

EventQueue FrameScheduler::createEventQueue() { return EventQueue{}; }
EventQueue FrameScheduler::eventQueue() { return EventQueue{}; }
bool FrameScheduler::sendEvent(const gea::framework::events::Event &, int) { return true; }
bool FrameScheduler::receiveEvent(gea::framework::events::Event *) { return false; }
void FrameScheduler::start(EventQueue) {}
void FrameScheduler::runFrame(const FrameCallbacks &callbacks)
{
	if (callbacks.frame) callbacks.frame(millisSinceBoot(), callbacks.context);
}
void FrameScheduler::setFrameIntervalMs(int intervalMs)
{
	if (intervalMs < kMinFrameIntervalMs) intervalMs = kMinFrameIntervalMs;
	if (intervalMs > kMaxFrameIntervalMs) intervalMs = kMaxFrameIntervalMs;
	g_frame_interval_ms = intervalMs;
}
int FrameScheduler::frameIntervalMs() { return g_frame_interval_ms; }
void FrameScheduler::setFrameRate(double fps)
{
	if (fps > 0.0) setFrameIntervalMs(static_cast<int>(1000.0 / fps + 0.5));
}
double FrameScheduler::frameRate() { return 1000.0 / static_cast<double>(g_frame_interval_ms); }
int FrameScheduler::nowMs() { return millisSinceBoot(); }

}  // namespace gea::framework::services
