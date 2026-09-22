#include <jni.h>

#include "host/fetch.h"
#include "wifi.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

JavaVM *g_java_vm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_fetch_method = nullptr;
jmethodID g_connected_method = nullptr;
std::mutex g_jni_mutex;

// A thread this file attaches to the VM has to detach before it exits, or ART
// aborts the process ("native thread exited without detaching"). Every fetch runs
// on its own detached std::thread (host/host/fetch.cpp), so the guard lives in
// that thread's storage and detaches while the thread unwinds. Threads the VM
// already knows -- the UI thread, which GetEnv finds attached -- are never armed,
// because detaching one we did not attach would tear down its JNI state.
class JniThreadAttachment {
public:
	~JniThreadAttachment()
	{
		if (attached_ && g_java_vm) g_java_vm->DetachCurrentThread();
	}

	void arm() { attached_ = true; }

private:
	bool attached_ = false;
};

JNIEnv *currentEnv()
{
	if (!g_java_vm) return nullptr;
	thread_local JniThreadAttachment attachment;
	JNIEnv *env = nullptr;
	if (g_java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
	if (g_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
	attachment.arm();
	return env;
}

struct BridgeRefs {
	jclass clazz = nullptr;
	jmethodID fetchMethod = nullptr;
	jmethodID connectedMethod = nullptr;
};

// The globals are written once, by gea_android_network_set_bridge_class. Copy
// them under the lock and let it go before calling into Java: a fetch parks its
// thread inside HttpURLConnection for up to 35s, and the frame thread asks
// wifi().connected() every tick, so holding the lock across the call would stall
// the UI for the whole request.
BridgeRefs bridgeRefs()
{
	std::scoped_lock guard(g_jni_mutex);
	return BridgeRefs{g_bridge_class, g_fetch_method, g_connected_method};
}

std::int32_t readInt32LE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
	if (offset + 4 > bytes.size()) return 0;
	return static_cast<std::int32_t>(bytes[offset]) |
	       (static_cast<std::int32_t>(bytes[offset + 1]) << 8) |
	       (static_cast<std::int32_t>(bytes[offset + 2]) << 16) |
	       (static_cast<std::int32_t>(bytes[offset + 3]) << 24);
}

void trimTrailingCarriageReturn(std::string &line)
{
	if (!line.empty() && line.back() == '\r') line.pop_back();
}

void parseHeaders(const std::string &headerBlock, gea::host::FetchResponse &result)
{
	std::istringstream stream(headerBlock);
	std::string line;
	while (std::getline(stream, line)) {
		trimTrailingCarriageReturn(line);
		const std::size_t colon = line.find(':');
		if (colon == std::string::npos) continue;
		std::string name = line.substr(0, colon);
		std::string value = line.substr(colon + 1);
		while (!value.empty() && value.front() == ' ') value.erase(value.begin());
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (!name.empty()) result.headers[name] = value;
	}
}

void parseHeaderBlock(const std::string &header, gea::host::FetchResponse &result)
{
	const std::size_t statusEnd = header.find('\n');
	if (statusEnd == std::string::npos) {
		result.status = std::strtod(header.c_str(), nullptr);
		return;
	}
	result.status = std::strtod(header.substr(0, statusEnd).c_str(), nullptr);

	const std::size_t textEnd = header.find('\n', statusEnd + 1);
	if (textEnd == std::string::npos) {
		result.status_text = header.substr(statusEnd + 1);
		trimTrailingCarriageReturn(result.status_text);
		return;
	}
	result.status_text = header.substr(statusEnd + 1, textEnd - statusEnd - 1);
	trimTrailingCarriageReturn(result.status_text);
	parseHeaders(header.substr(textEnd + 1), result);
}

gea::host::FetchResponse parsePackedFetch(const std::vector<std::uint8_t> &packed)
{
	gea::host::FetchResponse result{};
	if (packed.size() < 8) return result;
	const std::int32_t headerLen = readInt32LE(packed, 0);
	const std::int32_t bodyLen = readInt32LE(packed, 4);
	if (headerLen < 0 || bodyLen < 0) return result;
	const std::size_t headerSize = static_cast<std::size_t>(headerLen);
	const std::size_t bodySize = static_cast<std::size_t>(bodyLen);
	if (8 + headerSize > packed.size() || 8 + headerSize + bodySize > packed.size()) return result;
	std::string header(reinterpret_cast<const char *>(packed.data() + 8), headerSize);
	result.body.assign(packed.begin() + static_cast<std::ptrdiff_t>(8 + headerSize),
	                   packed.begin() + static_cast<std::ptrdiff_t>(8 + headerSize + bodySize));
	parseHeaderBlock(header, result);
	result.ok = result.status >= 200.0 && result.status < 300.0;
	return result;
}

bool androidConnected()
{
	JNIEnv *env = currentEnv();
	const BridgeRefs refs = bridgeRefs();
	if (!env || !refs.clazz || !refs.connectedMethod) return true;
	const jboolean connected = env->CallStaticBooleanMethod(refs.clazz, refs.connectedMethod);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return connected == JNI_TRUE;
}

gea::host::FetchResponse androidFetch(const std::string &url)
{
	JNIEnv *env = currentEnv();
	const BridgeRefs refs = bridgeRefs();
	if (!env || !refs.clazz || !refs.fetchMethod) return {};
	jstring jUrl = env->NewStringUTF(url.c_str());
	if (!jUrl) return {};
	jobject resultObject = env->CallStaticObjectMethod(refs.clazz, refs.fetchMethod, jUrl);
	env->DeleteLocalRef(jUrl);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return {};
	}
	jbyteArray resultArray = static_cast<jbyteArray>(resultObject);
	if (!resultArray) return {};
	const jsize length = env->GetArrayLength(resultArray);
	if (length <= 0) {
		env->DeleteLocalRef(resultArray);
		return {};
	}
	std::vector<std::uint8_t> packed(static_cast<std::size_t>(length));
	env->GetByteArrayRegion(resultArray, 0, length, reinterpret_cast<jbyte *>(packed.data()));
	env->DeleteLocalRef(resultArray);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return {};
	}
	return parsePackedFetch(packed);
}

class AndroidWifiDriver final : public gea::framework::network::WifiDriver {
public:
	bool init() override { return true; }
	bool enabled() const override { return enabled_; }
	void setEnabled(bool enabled) override { enabled_ = enabled; }
	bool connected() const override { return enabled_ && androidConnected(); }
	int rssi() override { return connected() ? -50 : 0; }
	std::string ssid() override { return connected() ? "android" : ""; }
	std::string ip() const override { return ""; }
	std::string mac() override { return ""; }
	void configure(const std::string &, const std::string &) override {}
	void scan() override {}
	bool scanning() const override { return false; }
	int scanCount() const override { return 0; }
	gea::framework::network::WifiNetwork networkAt(int) const override { return {}; }
	std::vector<gea::framework::network::WifiNetwork> scanResults() const override { return {}; }

private:
	bool enabled_ = true;
};

AndroidWifiDriver &androidWifiDriver()
{
	static AndroidWifiDriver driver;
	return driver;
}

}  // namespace

extern "C" void gea_android_network_set_bridge_class(JNIEnv *env, jclass bridgeClass)
{
	if (!env || !bridgeClass) return;
	std::scoped_lock guard(g_jni_mutex);
	if (!g_java_vm) env->GetJavaVM(&g_java_vm);
	if (!g_bridge_class) g_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridgeClass));
	if (g_bridge_class && !g_fetch_method) {
		g_fetch_method = env->GetStaticMethodID(g_bridge_class, "fetchUrlForNative", "(Ljava/lang/String;)[B");
		if (!g_fetch_method) env->ExceptionClear();
	}
	if (g_bridge_class && !g_connected_method) {
		g_connected_method = env->GetStaticMethodID(g_bridge_class, "networkConnectedForNative", "()Z");
		if (!g_connected_method) env->ExceptionClear();
	}
	gea::framework::network::WifiAdapter::setDriver(&androidWifiDriver());
}

namespace gea::framework::host {

gea::host::FetchResponse test_canned_response(const std::string &url)
{
	return androidFetch(url);
}

}  // namespace gea::framework::host
