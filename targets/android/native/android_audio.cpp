#include "audio.h"

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace gea::platform::audio {

namespace {

constexpr int kMaxOscillators = 16;
constexpr int kMaxToneMs = 2500;

JavaVM *g_java_vm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_play_tone_method = nullptr;
std::mutex g_jni_mutex;
int g_volume = 80;

struct OscillatorState {
	bool inUse = false;
	OscillatorType type = OscillatorType::Sine;
	double frequencyHz = 440.0;
	bool connected = false;
	bool started = false;
	double startTime = 0.0;
};

OscillatorState g_oscillators[kMaxOscillators]{};
int g_next_oscillator = 0;

double steadyNowSeconds()
{
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

OscillatorType normalizeType(OscillatorType type)
{
	switch (type) {
	case OscillatorType::Sine:
	case OscillatorType::Square:
	case OscillatorType::Sawtooth:
	case OscillatorType::Triangle:
		return type;
	}
	return OscillatorType::Sine;
}

NativeAudioHandle nativeHandle(OscillatorState &oscillator)
{
	return reinterpret_cast<NativeAudioHandle>(&oscillator);
}

bool owns(const OscillatorState *oscillator)
{
	const auto address = reinterpret_cast<NativeAudioHandle>(oscillator);
	const auto begin = reinterpret_cast<NativeAudioHandle>(g_oscillators);
	const auto end = reinterpret_cast<NativeAudioHandle>(g_oscillators + kMaxOscillators);
	return address >= begin && address < end;
}

OscillatorState *oscillator(NativeAudioHandle handle)
{
	auto *osc = reinterpret_cast<OscillatorState *>(handle);
	if (!owns(osc) || !osc->inUse) return nullptr;
	return osc;
}

JNIEnv *currentEnv()
{
	if (!g_java_vm) return nullptr;
	JNIEnv *env = nullptr;
	if (g_java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
	if (g_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
	return env;
}

void playTone(OscillatorType type, double frequencyHz, int durationMs, int delayMs)
{
	if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0 || durationMs <= 0) return;
	if (durationMs > kMaxToneMs) durationMs = kMaxToneMs;
	if (delayMs < 0) delayMs = 0;
	frequencyHz = std::max(40.0, std::min(4000.0, frequencyHz));

	JNIEnv *env = currentEnv();
	std::scoped_lock guard(g_jni_mutex);
	if (!env || !g_bridge_class || !g_play_tone_method) return;
	env->CallStaticVoidMethod(g_bridge_class,
	                          g_play_tone_method,
	                          static_cast<jint>(type),
	                          static_cast<jdouble>(frequencyHz),
	                          static_cast<jint>(durationMs),
	                          static_cast<jint>(delayMs),
	                          static_cast<jint>(g_volume));
	if (env->ExceptionCheck()) env->ExceptionClear();
}

}  // namespace

AudioParam::AudioParam(NativeAudioHandle oscillator) : oscillator_(oscillator) {}

double AudioParam::value() const
{
	const auto *osc = oscillator(oscillator_);
	return osc ? osc->frequencyHz : 0.0;
}

void AudioParam::setValue(double value)
{
	auto *osc = oscillator(oscillator_);
	if (!osc || !std::isfinite(value)) return;
	osc->frequencyHz = value;
}

void AudioParam::setValueAtTime(double value, double)
{
	setValue(value);
}

AudioNode::AudioNode(NativeAudioHandle native) : native_(native) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }
AudioDestinationNode::AudioDestinationNode(NativeAudioHandle native) : AudioNode(native) {}
OscillatorNode::OscillatorNode(NativeAudioHandle native) : AudioNode(native), frequency(native) {}

OscillatorType OscillatorNode::type() const
{
	const auto *osc = oscillator(nativeId());
	return osc ? osc->type : OscillatorType::Sine;
}

void OscillatorNode::setType(OscillatorType type)
{
	auto *osc = oscillator(nativeId());
	if (!osc) return;
	osc->type = normalizeType(type);
}

void OscillatorNode::connect(const AudioDestinationNode &)
{
	auto *osc = oscillator(nativeId());
	if (!osc) return;
	osc->connected = true;
}

void OscillatorNode::start(double when)
{
	auto *osc = oscillator(nativeId());
	if (!osc) return;
	if (when <= 0.0) when = steadyNowSeconds();
	osc->startTime = when;
	osc->started = true;
}

void OscillatorNode::stop(double when)
{
	auto *osc = oscillator(nativeId());
	if (!osc || !osc->connected) return;
	const double now = steadyNowSeconds();
	const double startTime = osc->started ? osc->startTime : now;
	if (when <= 0.0) when = now;
	if (when < startTime) when = startTime;
	const int durationMs = static_cast<int>((when - startTime) * 1000.0 + 0.5);
	const int delayMs = startTime > now ? static_cast<int>((startTime - now) * 1000.0 + 0.5) : 0;
	playTone(osc->type, osc->frequencyHz, durationMs, delayMs);
}

double AudioContext::currentTime() const { return steadyNowSeconds(); }
AudioDestinationNode AudioContext::destination() const { return AudioDestinationNode(0); }

OscillatorNode AudioContext::createOscillator() const
{
	const int slot = g_next_oscillator;
	g_next_oscillator = (g_next_oscillator + 1) % kMaxOscillators;
	auto &osc = g_oscillators[slot];
	osc = OscillatorState{};
	osc.inUse = true;
	return OscillatorNode(nativeHandle(osc));
}

AudioContext AudioSystem::sharedContext() { return AudioContext{}; }
int AudioSystem::volume() { return g_volume; }

void AudioSystem::setVolume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	g_volume = v;
}

bool AudioSystem::playFile(const std::string &) { return false; }
bool AudioSystem::playPcm(const std::int16_t *, std::size_t, int, int) { return false; }
void AudioSystem::stopPlayback() {}

}  // namespace gea::platform::audio

extern "C" void gea_android_audio_set_bridge_class(JNIEnv *env, jclass bridgeClass)
{
	if (!env || !bridgeClass) return;
	std::scoped_lock guard(gea::platform::audio::g_jni_mutex);
	if (!gea::platform::audio::g_java_vm) env->GetJavaVM(&gea::platform::audio::g_java_vm);
	if (!gea::platform::audio::g_bridge_class) {
		gea::platform::audio::g_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridgeClass));
	}
	if (!gea::platform::audio::g_play_tone_method && gea::platform::audio::g_bridge_class) {
		gea::platform::audio::g_play_tone_method =
		    env->GetStaticMethodID(gea::platform::audio::g_bridge_class, "playTone", "(IDIII)V");
		if (!gea::platform::audio::g_play_tone_method) env->ExceptionClear();
	}
}
