#include "camera.h"
#include "imu.h"
#include "touch.h"

namespace gea::platform::sensors {

void Accelerometer::init() {}
void Accelerometer::close() {}
void Accelerometer::calibrateBias() {}
int Accelerometer::tiltX() { return 0; }
int Accelerometer::tiltY() { return 0; }
double Accelerometer::accelerationX() { return 0.0; }
double Accelerometer::accelerationY() { return 0.0; }
double Accelerometer::accelerationZ() { return 0.0; }
double Accelerometer::gyroscopeX() { return 0.0; }
double Accelerometer::gyroscopeY() { return 0.0; }
double Accelerometer::gyroscopeZ() { return 0.0; }
void Accelerometer::setWebTilt(int, int) {}

}  // namespace gea::platform::sensors

namespace gea::platform::touch {

namespace {
bool g_touching = false;
int g_touch_x = 0;
int g_touch_y = 0;
Touchscreen::Observer g_observer = nullptr;
}  // namespace

void Touchscreen::setObserver(Observer observer) { g_observer = observer; }
bool Touchscreen::init() { return true; }
int Touchscreen::read(int *x, int *y)
{
	if (x) *x = g_touch_x;
	if (y) *y = g_touch_y;
	return g_touching ? 1 : 0;
}
int Touchscreen::readCached(int *x, int *y)
{
	if (x) *x = g_touch_x;
	if (y) *y = g_touch_y;
	return g_touching ? 1 : 0;
}
void Touchscreen::consumeLatestMove(int *x, int *y)
{
	if (x) *x = g_touch_x;
	if (y) *y = g_touch_y;
}
void Touchscreen::injectEvent(Phase phase, bool touching, int x, int y)
{
	g_touching = touching;
	g_touch_x = x;
	g_touch_y = y;
	if (g_observer) g_observer(phase, touching, x, y);
}

extern "C" void gea_android_touch_set_state(bool touching, int x, int y)
{
	g_touching = touching;
	g_touch_x = x;
	g_touch_y = y;
}

}  // namespace gea::platform::touch

namespace gea::platform::camera {

bool Camera::isAvailable() { return false; }
bool Camera::hasPermission() { return false; }
bool Camera::requestPermission() { return false; }
bool Camera::open(const std::string &, int, int) { return false; }
void Camera::close() {}
bool Camera::isOpen() { return false; }
int Camera::width() { return 0; }
int Camera::height() { return 0; }
int Camera::orientation() { return 0; }
Facing Camera::currentFacing() { return Facing::Back; }
std::string Camera::currentFacingString() { return "back"; }
int Camera::deviceCount() { return 0; }
DeviceInfo Camera::deviceAt(int) { return DeviceInfo{}; }
void Camera::drawPreview(int, int, int, int) {}
int Camera::previewMode() { return 0; }
bool Camera::fillPreview(gea::framework::graphics::pixel::native_t *, int, int, int, bool) { return false; }
void Camera::positionPreviewLayer(int, int, int, int) {}
void Camera::hidePreviewLayer() {}
void Camera::presentNativeOverlay() {}
int Camera::capture(bool) { return -1; }
bool Camera::startRecording(const std::string &, double) { return false; }
double Camera::stopRecording() { return -1.0; }
bool Camera::isRecording() { return false; }
void Camera::setFlash(const std::string &) {}
void Camera::setZoom(double) {}
void Camera::setMirror(bool) {}
void Camera::setExposure(const std::string &, double, double, double) {}
void Camera::setWhiteBalance(const std::string &, double, double) {}
void Camera::setFocus(const std::string &, double, double) {}
void Camera::setTorch(const std::string &, double) {}

}  // namespace gea::platform::camera
