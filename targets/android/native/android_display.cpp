#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "host/display_orientation.h"
#include "pixel.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

gea::framework::graphics::Canvas g_canvas;
std::uint32_t *g_framebuffer = nullptr;
std::uint8_t g_alpha = 255;
int g_brightness = 100;
int g_canvas_width = gea::platform::display::kWidth;
int g_canvas_height = gea::platform::display::kHeight;
int g_flush_rows = 32;
int g_flush_depth = 2;
int g_flush_calls = 0;
int g_flush_pixels = 0;
JNIEnv *g_live_env = nullptr;
jobject g_live_canvas = nullptr;
bool g_direct_canvas_presented = false;

namespace pixel = gea::framework::graphics::pixel;

struct AndroidCanvasBindings {
	bool ready = false;
	jclass canvasClass = nullptr;
	jclass paintClass = nullptr;
	jclass pathClass = nullptr;
	jclass rectClass = nullptr;
	jclass rectFClass = nullptr;
	jclass bitmapClass = nullptr;
	jmethodID canvasDrawColor = nullptr;
	jmethodID canvasDrawRect = nullptr;
	jmethodID canvasDrawCircle = nullptr;
	jmethodID canvasDrawPath = nullptr;
	jmethodID canvasDrawBitmapAt = nullptr;
	jmethodID canvasDrawBitmapRectF = nullptr;
	jmethodID canvasSave = nullptr;
	jmethodID canvasRestoreToCount = nullptr;
	jmethodID canvasClipRect = nullptr;
	jmethodID paintCtor = nullptr;
	jmethodID paintSetColor = nullptr;
	jmethodID paintSetStyle = nullptr;
	jmethodID paintSetStrokeWidth = nullptr;
	jmethodID paintSetAlpha = nullptr;
	jmethodID paintSetAntiAlias = nullptr;
	jmethodID paintSetFilterBitmap = nullptr;
	jmethodID paintSetDither = nullptr;
	jmethodID pathCtor = nullptr;
	jmethodID pathReset = nullptr;
	jmethodID pathMoveTo = nullptr;
	jmethodID pathLineTo = nullptr;
	jmethodID pathClose = nullptr;
	jmethodID rectCtor = nullptr;
	jmethodID rectSet = nullptr;
	jmethodID rectFCtor = nullptr;
	jmethodID rectFSet = nullptr;
	jmethodID bitmapCreateBitmap = nullptr;
	jobject paint = nullptr;
	jobject path = nullptr;
	jobject rect = nullptr;
	jobject rectF = nullptr;
	jobject paintFillStyle = nullptr;
	jobject paintStrokeStyle = nullptr;
	jobject bitmapConfigArgb8888 = nullptr;
};

struct CachedAndroidBitmap {
	const pixel::native_t *pixels = nullptr;
	const std::uint8_t *alpha = nullptr;
	int width = 0;
	int height = 0;
	bool rotated90CW = false;
	jobject bitmap = nullptr;
};

AndroidCanvasBindings g_android_canvas;
std::vector<CachedAndroidBitmap> g_android_bitmaps;

void ensureCanvas()
{
	if (g_framebuffer && g_canvas.width() == g_canvas_width && g_canvas.height() == g_canvas_height) return;
	std::free(g_framebuffer);
	g_framebuffer = static_cast<std::uint32_t *>(std::calloc(static_cast<std::size_t>(g_canvas_width) * g_canvas_height,
	                                                        sizeof(std::uint32_t)));
	if (!g_framebuffer) {
		g_canvas.bindPixels(nullptr, 0, 0);
		return;
	}
	g_canvas.bindPixels(g_framebuffer, g_canvas_width, g_canvas_height);
}

void countRect(int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= g_canvas_width) x1 = g_canvas_width - 1;
	if (y1 >= g_canvas_height) y1 = g_canvas_height - 1;
	if (x0 > x1 || y0 > y1) return;
	g_flush_calls++;
	g_flush_pixels += (x1 - x0 + 1) * (y1 - y0 + 1);
}

bool clearPendingJavaException(JNIEnv *env)
{
	if (!env || !env->ExceptionCheck()) return false;
	env->ExceptionClear();
	return true;
}

jclass findGlobalClass(JNIEnv *env, const char *name)
{
	jclass local = env ? env->FindClass(name) : nullptr;
	const bool hadException = clearPendingJavaException(env);
	if (!local || hadException) return nullptr;
	jclass global = static_cast<jclass>(env->NewGlobalRef(local));
	env->DeleteLocalRef(local);
	return global;
}

bool method(JNIEnv *env, jclass cls, jmethodID *out, const char *name, const char *sig)
{
	*out = cls ? env->GetMethodID(cls, name, sig) : nullptr;
	const bool hadException = clearPendingJavaException(env);
	return *out && !hadException;
}

bool staticMethod(JNIEnv *env, jclass cls, jmethodID *out, const char *name, const char *sig)
{
	*out = cls ? env->GetStaticMethodID(cls, name, sig) : nullptr;
	const bool hadException = clearPendingJavaException(env);
	return *out && !hadException;
}

bool ensureAndroidCanvasBindings(JNIEnv *env)
{
	auto &b = g_android_canvas;
	if (b.ready) return true;
	if (!env) return false;

	b.canvasClass = findGlobalClass(env, "android/graphics/Canvas");
	b.paintClass = findGlobalClass(env, "android/graphics/Paint");
	jclass paintStyleClass = findGlobalClass(env, "android/graphics/Paint$Style");
	b.pathClass = findGlobalClass(env, "android/graphics/Path");
	b.rectClass = findGlobalClass(env, "android/graphics/Rect");
	b.rectFClass = findGlobalClass(env, "android/graphics/RectF");
	b.bitmapClass = findGlobalClass(env, "android/graphics/Bitmap");
	jclass bitmapConfigClass = findGlobalClass(env, "android/graphics/Bitmap$Config");
	if (!b.canvasClass || !b.paintClass || !paintStyleClass || !b.pathClass || !b.rectClass ||
	    !b.rectFClass || !b.bitmapClass || !bitmapConfigClass) {
		return false;
	}

	if (!method(env, b.canvasClass, &b.canvasDrawColor, "drawColor", "(I)V") ||
	    !method(env, b.canvasClass, &b.canvasDrawRect, "drawRect", "(FFFFLandroid/graphics/Paint;)V") ||
	    !method(env, b.canvasClass, &b.canvasDrawCircle, "drawCircle", "(FFFLandroid/graphics/Paint;)V") ||
	    !method(env, b.canvasClass, &b.canvasDrawPath, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V") ||
	    !method(env, b.canvasClass, &b.canvasDrawBitmapAt, "drawBitmap", "(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V") ||
	    !method(env, b.canvasClass, &b.canvasDrawBitmapRectF, "drawBitmap", "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V") ||
	    !method(env, b.canvasClass, &b.canvasSave, "save", "()I") ||
	    !method(env, b.canvasClass, &b.canvasRestoreToCount, "restoreToCount", "(I)V") ||
	    !method(env, b.canvasClass, &b.canvasClipRect, "clipRect", "(FFFF)Z") ||
	    !method(env, b.paintClass, &b.paintCtor, "<init>", "()V") ||
	    !method(env, b.paintClass, &b.paintSetColor, "setColor", "(I)V") ||
	    !method(env, b.paintClass, &b.paintSetStyle, "setStyle", "(Landroid/graphics/Paint$Style;)V") ||
	    !method(env, b.paintClass, &b.paintSetStrokeWidth, "setStrokeWidth", "(F)V") ||
	    !method(env, b.paintClass, &b.paintSetAlpha, "setAlpha", "(I)V") ||
	    !method(env, b.paintClass, &b.paintSetAntiAlias, "setAntiAlias", "(Z)V") ||
	    !method(env, b.paintClass, &b.paintSetFilterBitmap, "setFilterBitmap", "(Z)V") ||
	    !method(env, b.paintClass, &b.paintSetDither, "setDither", "(Z)V") ||
	    !method(env, b.pathClass, &b.pathCtor, "<init>", "()V") ||
	    !method(env, b.pathClass, &b.pathReset, "reset", "()V") ||
	    !method(env, b.pathClass, &b.pathMoveTo, "moveTo", "(FF)V") ||
	    !method(env, b.pathClass, &b.pathLineTo, "lineTo", "(FF)V") ||
	    !method(env, b.pathClass, &b.pathClose, "close", "()V") ||
	    !method(env, b.rectClass, &b.rectCtor, "<init>", "()V") ||
	    !method(env, b.rectClass, &b.rectSet, "set", "(IIII)V") ||
	    !method(env, b.rectFClass, &b.rectFCtor, "<init>", "()V") ||
	    !method(env, b.rectFClass, &b.rectFSet, "set", "(FFFF)V") ||
	    !staticMethod(env, b.bitmapClass, &b.bitmapCreateBitmap, "createBitmap", "([IIIIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;")) {
		return false;
	}

	jfieldID fillField = env->GetStaticFieldID(paintStyleClass, "FILL", "Landroid/graphics/Paint$Style;");
	jfieldID strokeField = env->GetStaticFieldID(paintStyleClass, "STROKE", "Landroid/graphics/Paint$Style;");
	jfieldID argbField = env->GetStaticFieldID(bitmapConfigClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
	const bool fieldException = clearPendingJavaException(env);
	if (!fillField || !strokeField || !argbField || fieldException) return false;
	jobject fillStyle = env->GetStaticObjectField(paintStyleClass, fillField);
	jobject strokeStyle = env->GetStaticObjectField(paintStyleClass, strokeField);
	jobject argbConfig = env->GetStaticObjectField(bitmapConfigClass, argbField);
	b.paintFillStyle = fillStyle ? env->NewGlobalRef(fillStyle) : nullptr;
	b.paintStrokeStyle = strokeStyle ? env->NewGlobalRef(strokeStyle) : nullptr;
	b.bitmapConfigArgb8888 = argbConfig ? env->NewGlobalRef(argbConfig) : nullptr;
	if (fillStyle) env->DeleteLocalRef(fillStyle);
	if (strokeStyle) env->DeleteLocalRef(strokeStyle);
	if (argbConfig) env->DeleteLocalRef(argbConfig);

	jobject paint = env->NewObject(b.paintClass, b.paintCtor);
	jobject path = env->NewObject(b.pathClass, b.pathCtor);
	jobject rect = env->NewObject(b.rectClass, b.rectCtor);
	jobject rectF = env->NewObject(b.rectFClass, b.rectFCtor);
	if (!paint || !path || !rect || !rectF || clearPendingJavaException(env)) return false;
	b.paint = env->NewGlobalRef(paint);
	b.path = env->NewGlobalRef(path);
	b.rect = env->NewGlobalRef(rect);
	b.rectF = env->NewGlobalRef(rectF);
	env->DeleteLocalRef(paint);
	env->DeleteLocalRef(path);
	env->DeleteLocalRef(rect);
	env->DeleteLocalRef(rectF);
	if (!b.paint || !b.path || !b.rect || !b.rectF || !b.paintFillStyle || !b.paintStrokeStyle ||
	    !b.bitmapConfigArgb8888) {
		return false;
	}

	env->CallVoidMethod(b.paint, b.paintSetAntiAlias, JNI_FALSE);
	env->CallVoidMethod(b.paint, b.paintSetFilterBitmap, JNI_FALSE);
	env->CallVoidMethod(b.paint, b.paintSetDither, JNI_FALSE);
	b.ready = !clearPendingJavaException(env);
	return b.ready;
}

jint androidCanvasColor(pixel::native_t color, std::uint8_t alpha)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	std::uint32_t argb = static_cast<std::uint32_t>(color);
	const int baseAlpha = static_cast<int>((argb >> 24) & 0xffu);
	const int outAlpha = (baseAlpha * static_cast<int>(alpha) + 127) / 255;
	return static_cast<jint>((argb & 0x00ffffffu) | (static_cast<std::uint32_t>(outAlpha) << 24));
#else
	int r = 0;
	int g = 0;
	int bl = 0;
	int a = 255;
	pixel::unpackNative8(color, &r, &g, &bl, &a);
	a = (a * static_cast<int>(alpha) + 127) / 255;
	return static_cast<jint>(((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (bl & 0xff));
#endif
}

bool setPaintFill(JNIEnv *env, pixel::native_t color, std::uint8_t alpha)
{
	if (!ensureAndroidCanvasBindings(env)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(b.paint, b.paintSetStyle, b.paintFillStyle);
	env->CallVoidMethod(b.paint, b.paintSetStrokeWidth, 1.0f);
	env->CallVoidMethod(b.paint, b.paintSetAlpha, 255);
	env->CallVoidMethod(b.paint, b.paintSetColor, androidCanvasColor(color, alpha));
	return !clearPendingJavaException(env);
}

bool setPaintStroke(JNIEnv *env, pixel::native_t color, std::uint8_t alpha)
{
	if (!ensureAndroidCanvasBindings(env)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(b.paint, b.paintSetStyle, b.paintStrokeStyle);
	env->CallVoidMethod(b.paint, b.paintSetStrokeWidth, 1.0f);
	env->CallVoidMethod(b.paint, b.paintSetAlpha, 255);
	env->CallVoidMethod(b.paint, b.paintSetColor, androidCanvasColor(color, alpha));
	return !clearPendingJavaException(env);
}

bool setBitmapPaint(JNIEnv *env, std::uint8_t alpha)
{
	if (!ensureAndroidCanvasBindings(env)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(b.paint, b.paintSetAlpha, static_cast<jint>(alpha));
	env->CallVoidMethod(b.paint, b.paintSetFilterBitmap, JNI_FALSE);
	env->CallVoidMethod(b.paint, b.paintSetDither, JNI_FALSE);
	return !clearPendingJavaException(env);
}

bool drawCanvasRect(JNIEnv *env, float x, float y, float w, float h)
{
	if (w <= 0.0f || h <= 0.0f) return true;
	auto &b = g_android_canvas;
	env->CallVoidMethod(g_live_canvas, b.canvasDrawRect, x, y, x + w, y + h, b.paint);
	return !clearPendingJavaException(env);
}

jobject bitmapForPixels(JNIEnv *env, const pixel::native_t *pixels, const std::uint8_t *alpha, int width, int height,
                        bool rotated90CW = false)
{
	if (!pixels || width <= 0 || height <= 0 || !ensureAndroidCanvasBindings(env)) return nullptr;
	for (const auto &entry : g_android_bitmaps) {
		if (entry.pixels == pixels && entry.alpha == alpha && entry.width == width && entry.height == height &&
		    entry.rotated90CW == rotated90CW) {
			return entry.bitmap;
		}
	}

	const int bitmapWidth = rotated90CW ? height : width;
	const int bitmapHeight = rotated90CW ? width : height;
	const int pixelCount = bitmapWidth * bitmapHeight;
	jintArray array = env->NewIntArray(static_cast<jsize>(pixelCount));
	if (!array) return nullptr;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	if (!alpha && !rotated90CW) {
		env->SetIntArrayRegion(array, 0, static_cast<jsize>(pixelCount), reinterpret_cast<const jint *>(pixels));
	} else
#endif
	{
		std::vector<jint> converted(static_cast<std::size_t>(pixelCount));
		for (int outY = 0; outY < bitmapHeight; outY++) {
			for (int outX = 0; outX < bitmapWidth; outX++) {
				const int srcX = rotated90CW ? outY : outX;
				const int srcY = rotated90CW ? height - 1 - outX : outY;
				const int srcIndex = srcY * width + srcX;
				const int outIndex = outY * bitmapWidth + outX;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
				std::uint32_t argb = static_cast<std::uint32_t>(pixels[srcIndex]);
				if (alpha) argb = (argb & 0x00ffffffu) | (static_cast<std::uint32_t>(alpha[srcIndex]) << 24);
				converted[static_cast<std::size_t>(outIndex)] = static_cast<jint>(argb);
#else
				int r = 0;
				int g = 0;
				int b = 0;
				int a = 255;
				pixel::unpackNative8(pixels[srcIndex], &r, &g, &b, &a);
				if (alpha) a = alpha[srcIndex];
				converted[static_cast<std::size_t>(outIndex)] =
				    static_cast<jint>(((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
#endif
			}
		}
		env->SetIntArrayRegion(array, 0, static_cast<jsize>(pixelCount), converted.data());
	}
	if (clearPendingJavaException(env)) {
		env->DeleteLocalRef(array);
		return nullptr;
	}

	auto &b = g_android_canvas;
	jobject localBitmap = env->CallStaticObjectMethod(b.bitmapClass, b.bitmapCreateBitmap, array, 0,
	                                                  static_cast<jint>(bitmapWidth), static_cast<jint>(bitmapWidth),
	                                                  static_cast<jint>(bitmapHeight), b.bitmapConfigArgb8888);
	env->DeleteLocalRef(array);
	if (!localBitmap || clearPendingJavaException(env)) return nullptr;
	jobject bitmap = env->NewGlobalRef(localBitmap);
	env->DeleteLocalRef(localBitmap);
	if (!bitmap) return nullptr;
	g_android_bitmaps.push_back({pixels, alpha, width, height, rotated90CW, bitmap});
	return bitmap;
}

bool drawBitmapAt(JNIEnv *env, const pixel::native_t *pixels, const std::uint8_t *alpha, int width, int height,
                  int x, int y, std::uint8_t globalAlpha)
{
	jobject bitmap = bitmapForPixels(env, pixels, alpha, width, height);
	if (!bitmap || !setBitmapPaint(env, globalAlpha)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(g_live_canvas, b.canvasDrawBitmapAt, bitmap, static_cast<float>(x), static_cast<float>(y), b.paint);
	return !clearPendingJavaException(env);
}

bool drawBitmapScaled(JNIEnv *env, const pixel::native_t *pixels, const std::uint8_t *alpha, int srcWidth,
                      int srcHeight, int x, int y, int w, int h, std::uint8_t globalAlpha)
{
	if (w <= 0 || h <= 0) return true;
	jobject bitmap = bitmapForPixels(env, pixels, alpha, srcWidth, srcHeight);
	if (!bitmap || !setBitmapPaint(env, globalAlpha)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(b.rectF, b.rectFSet, static_cast<float>(x), static_cast<float>(y),
	                    static_cast<float>(x + w), static_cast<float>(y + h));
	env->CallVoidMethod(g_live_canvas, b.canvasDrawBitmapRectF, bitmap, nullptr, b.rectF, b.paint);
	return !clearPendingJavaException(env);
}

bool drawBitmapRotated90CW(JNIEnv *env, const pixel::native_t *pixels, const std::uint8_t *alpha, int srcWidth,
                           int srcHeight, int x, int y, int w, int h, std::uint8_t globalAlpha)
{
	if (w <= 0 || h <= 0) return true;
	jobject bitmap = bitmapForPixels(env, pixels, alpha, srcWidth, srcHeight, true);
	if (!bitmap || !setBitmapPaint(env, globalAlpha)) return false;
	auto &b = g_android_canvas;
	env->CallVoidMethod(b.rectF, b.rectFSet, static_cast<float>(x), static_cast<float>(y),
	                    static_cast<float>(x + w), static_cast<float>(y + h));
	env->CallVoidMethod(g_live_canvas, b.canvasDrawBitmapRectF, bitmap, nullptr, b.rectF, b.paint);
	return !clearPendingJavaException(env);
}

bool drawBitmapTiledX(JNIEnv *env, const pixel::native_t *pixels, const std::uint8_t *alpha, int srcWidth,
                      int srcHeight, int x, int y, int w, std::uint8_t globalAlpha)
{
	if (srcWidth <= 0 || srcHeight <= 0 || w <= 0) return true;
	jobject bitmap = bitmapForPixels(env, pixels, alpha, srcWidth, srcHeight);
	if (!bitmap || !setBitmapPaint(env, globalAlpha)) return false;
	auto &b = g_android_canvas;
	const jint saveCount = env->CallIntMethod(g_live_canvas, b.canvasSave);
	env->CallBooleanMethod(g_live_canvas, b.canvasClipRect, static_cast<float>(x), static_cast<float>(y),
	                       static_cast<float>(x + w), static_cast<float>(y + srcHeight));
	int tileX = x;
	if (tileX + srcWidth <= 0) {
		const int skip = (-tileX) / srcWidth;
		tileX += skip * srcWidth;
		while (tileX + srcWidth <= 0) tileX += srcWidth;
	}
	for (; tileX < x + w; tileX += srcWidth) {
		env->CallVoidMethod(g_live_canvas, b.canvasDrawBitmapAt, bitmap, static_cast<float>(tileX),
		                    static_cast<float>(y), b.paint);
		if (clearPendingJavaException(env)) return false;
	}
	env->CallVoidMethod(g_live_canvas, b.canvasRestoreToCount, saveCount);
	return !clearPendingJavaException(env);
}

bool drawBitmapFontText(JNIEnv *env, const char *text, int x, int y, pixel::native_t color, float scale,
                        std::uint8_t alpha)
{
	if (!text) return true;
	if (scale < 0.1f) scale = 1.0f;
	int glyphW = static_cast<int>(8.0f * scale + 0.5f);
	int glyphH = static_cast<int>(16.0f * scale + 0.5f);
	if (glyphW < 1) glyphW = 1;
	if (glyphH < 1) glyphH = 1;
	if (!setPaintFill(env, color, alpha)) return false;

	const auto &font = gea::framework::graphics::FontRegistry::bitmap8x16();
	int penX = x;
	for (const char *p = text; *p; ++p) {
		if (*p == '\n') {
			penX = x;
			y += glyphH;
			continue;
		}
		const std::uint8_t *glyph = font.glyphRows(*p);
		for (int row = 0; row < 16; row++) {
			const std::uint8_t bits = glyph[row];
			if (!bits) continue;
			const int y0 = y + row * glyphH / 16;
			const int y1 = y + (row + 1) * glyphH / 16;
			if (y1 <= y0) continue;
			int runStart = -1;
			for (int col = 0; col <= 8; col++) {
				const bool lit = col < 8 && (bits & (0x80 >> col));
				if (lit) {
					if (runStart < 0) runStart = col;
					continue;
				}
				if (runStart < 0) continue;
				const int x0 = penX + runStart * glyphW / 8;
				const int x1 = penX + col * glyphW / 8;
				runStart = -1;
				if (x1 <= x0) continue;
				if (!drawCanvasRect(env, static_cast<float>(x0), static_cast<float>(y0),
				                    static_cast<float>(x1 - x0), static_cast<float>(y1 - y0))) {
					return false;
				}
			}
		}
		penX += glyphW;
	}
	return true;
}

bool presentToNativeCanvas(const gea::platform::display::DisplayPresentCommand *commands, int command_count)
{
	JNIEnv *env = g_live_env;
	if (!env || !g_live_canvas || !commands || command_count <= 0 || !ensureAndroidCanvasBindings(env)) return false;
	auto &b = g_android_canvas;
	for (int i = 0; i < command_count; i++) {
		const auto &command = commands[i];
		switch (command.type) {
		case gea::platform::display::DisplayPresentCommandType::Clear:
			{
				jint clearColor = androidCanvasColor(command.clear.color, 255);
				if ((static_cast<std::uint32_t>(clearColor) & 0xff000000u) == 0) clearColor = static_cast<jint>(0xff000000u);
				env->CallVoidMethod(g_live_canvas, b.canvasDrawColor, clearColor);
			}
			if (clearPendingJavaException(env)) return false;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillRectRgb565:
			if (!setPaintFill(env, command.fillRectRgb565.color, command.fillRectRgb565.alpha) ||
			    !drawCanvasRect(env, static_cast<float>(command.fillRectRgb565.x),
			                    static_cast<float>(command.fillRectRgb565.y),
			                    static_cast<float>(command.fillRectRgb565.w),
			                    static_cast<float>(command.fillRectRgb565.h))) {
				return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::StrokeRectRgb565: {
			const auto &r = command.strokeRectRgb565;
			if (!setPaintFill(env, r.color, r.alpha)) return false;
			if (!drawCanvasRect(env, static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), 1.0f) ||
			    !drawCanvasRect(env, static_cast<float>(r.x), static_cast<float>(r.y + r.h - 1), static_cast<float>(r.w), 1.0f) ||
			    !drawCanvasRect(env, static_cast<float>(r.x), static_cast<float>(r.y), 1.0f, static_cast<float>(r.h)) ||
			    !drawCanvasRect(env, static_cast<float>(r.x + r.w - 1), static_cast<float>(r.y), 1.0f, static_cast<float>(r.h))) {
				return false;
			}
			break;
		}
		case gea::platform::display::DisplayPresentCommandType::FillTriangleRgb565:
			if (!setPaintFill(env, command.fillTriangleRgb565.color, command.fillTriangleRgb565.alpha)) return false;
			env->CallVoidMethod(b.path, b.pathReset);
			env->CallVoidMethod(b.path, b.pathMoveTo, static_cast<float>(command.fillTriangleRgb565.x0),
			                    static_cast<float>(command.fillTriangleRgb565.y0));
			env->CallVoidMethod(b.path, b.pathLineTo, static_cast<float>(command.fillTriangleRgb565.x1),
			                    static_cast<float>(command.fillTriangleRgb565.y1));
			env->CallVoidMethod(b.path, b.pathLineTo, static_cast<float>(command.fillTriangleRgb565.x2),
			                    static_cast<float>(command.fillTriangleRgb565.y2));
			env->CallVoidMethod(b.path, b.pathClose);
			env->CallVoidMethod(g_live_canvas, b.canvasDrawPath, b.path, b.paint);
			if (clearPendingJavaException(env)) return false;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillCircleRgb565:
			if (!setPaintFill(env, command.fillCircleRgb565.color, command.fillCircleRgb565.alpha)) return false;
			env->CallVoidMethod(g_live_canvas, b.canvasDrawCircle, static_cast<float>(command.fillCircleRgb565.x),
			                    static_cast<float>(command.fillCircleRgb565.y),
			                    static_cast<float>(command.fillCircleRgb565.radius), b.paint);
			if (clearPendingJavaException(env)) return false;
			break;
		case gea::platform::display::DisplayPresentCommandType::StrokeCircleRgb565:
			if (!setPaintStroke(env, command.strokeCircleRgb565.color, command.strokeCircleRgb565.alpha)) return false;
			env->CallVoidMethod(g_live_canvas, b.canvasDrawCircle, static_cast<float>(command.strokeCircleRgb565.x),
			                    static_cast<float>(command.strokeCircleRgb565.y),
			                    static_cast<float>(command.strokeCircleRgb565.radius), b.paint);
			if (clearPendingJavaException(env)) return false;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillCirclesRgb565:
			for (int circle = 0; circle < command.fillCirclesRgb565.count; circle++) {
				const pixel::native_t color = command.fillCirclesRgb565.colors
				                                  ? command.fillCirclesRgb565.colors[circle]
				                                  : pixel::nativeColor(255, 255, 255);
				if (!setPaintFill(env, color, command.fillCirclesRgb565.alpha)) return false;
				env->CallVoidMethod(g_live_canvas, b.canvasDrawCircle,
				                    static_cast<float>(command.fillCirclesRgb565.xs[circle]),
				                    static_cast<float>(command.fillCirclesRgb565.ys[circle]),
				                    static_cast<float>(command.fillCirclesRgb565.radius), b.paint);
				if (clearPendingJavaException(env)) return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImage:
			if (!drawBitmapAt(env, command.drawImage.pixels, command.drawImage.alphaPixels,
			                  command.drawImage.srcWidth, command.drawImage.srcHeight,
			                  command.drawImage.x, command.drawImage.y, command.drawImage.alpha)) {
				return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageScaled:
			if (!drawBitmapScaled(env, command.drawImageScaled.pixels, command.drawImageScaled.alphaPixels,
			                      command.drawImageScaled.srcWidth, command.drawImageScaled.srcHeight,
			                      command.drawImageScaled.x, command.drawImageScaled.y,
			                      command.drawImageScaled.w, command.drawImageScaled.h,
			                      command.drawImageScaled.alpha)) {
				return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageRotated90CW:
			if (!drawBitmapRotated90CW(env, command.drawImageRotated90CW.pixels,
			                           command.drawImageRotated90CW.alphaPixels,
			                           command.drawImageRotated90CW.srcWidth,
			                           command.drawImageRotated90CW.srcHeight,
			                           command.drawImageRotated90CW.x, command.drawImageRotated90CW.y,
			                           command.drawImageRotated90CW.w, command.drawImageRotated90CW.h,
			                           command.drawImageRotated90CW.alpha)) {
				return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageTiledX:
			if (!drawBitmapTiledX(env, command.drawImageTiledX.pixels, command.drawImageTiledX.alphaPixels,
			                      command.drawImageTiledX.srcWidth, command.drawImageTiledX.srcHeight,
			                      command.drawImageTiledX.x, command.drawImageTiledX.y,
			                      command.drawImageTiledX.w, command.drawImageTiledX.alpha)) {
				return false;
			}
			break;
		case gea::platform::display::DisplayPresentCommandType::FillText:
			if (!drawBitmapFontText(env, command.fillText.text, command.fillText.x, command.fillText.y,
			                        command.fillText.color,
			                        command.fillText.fontFamilyId >= 0 && command.fillText.fontSizePx > 0
			                            ? static_cast<float>(command.fillText.fontSizePx) / 16.0f
			                            : command.fillText.scale,
			                        command.fillText.alpha)) {
				return false;
			}
			break;
		}
	}
	g_flush_calls++;
	g_flush_pixels += g_canvas_width * g_canvas_height;
	return true;
}

}  // namespace

namespace gea::platform::display {

bool Display::init()
{
	ensureCanvas();
	return true;
}

bool Display::start() { return true; }

gea::framework::graphics::Canvas *Display::canvas()
{
	ensureCanvas();
	return &g_canvas;
}

bool Display::framebufferIsPanelDirect() { return true; }
bool Display::panelScanoutSurface(uint16_t **out_buffer, int *out_width, int *out_height)
{
	if (out_buffer) *out_buffer = nullptr;
	if (out_width) *out_width = 0;
	if (out_height) *out_height = 0;
	return false;
}
bool Display::panelDirectTarget(int, int, int, int, uint16_t **out_buffer, int *out_buffer_w, int *out_buffer_h,
                                int *out_panel_x, int *out_panel_y, int *out_panel_w, int *out_panel_h,
                                int *out_rot_steps, int *out_flip)
{
	if (out_buffer) *out_buffer = nullptr;
	if (out_buffer_w) *out_buffer_w = 0;
	if (out_buffer_h) *out_buffer_h = 0;
	if (out_panel_x) *out_panel_x = 0;
	if (out_panel_y) *out_panel_y = 0;
	if (out_panel_w) *out_panel_w = 0;
	if (out_panel_h) *out_panel_h = 0;
	if (out_rot_steps) *out_rot_steps = 0;
	if (out_flip) *out_flip = 0;
	return false;
}
void Display::flipPanelToBack() {}

bool Display::copySnapshotRgb565(uint16_t *dst, int pixel_capacity, int *width, int *height, bool)
{
	ensureCanvas();
	if (width) *width = g_canvas_width;
	if (height) *height = g_canvas_height;
	if (!dst || pixel_capacity < g_canvas_width * g_canvas_height || !g_framebuffer) return false;
	for (int i = 0; i < g_canvas_width * g_canvas_height; i++) dst[i] = gea::framework::graphics::pixel::fromNative(g_framebuffer[i]);
	return true;
}

int Display::countNonBlackPixels(bool)
{
	ensureCanvas();
	if (!g_framebuffer) return 0;
	int count = 0;
	for (int i = 0; i < g_canvas_width * g_canvas_height; i++) {
		if ((g_framebuffer[i] & 0x00ffffffu) != 0) count++;
	}
	return count;
}

void Display::clear() { canvas()->clear(0); }
void Display::clearNoFlush() { canvas()->clear(0); }
void Display::print(const char *) {}

void Display::flush()
{
	ensureCanvas();
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (g_canvas.dirty(&x0, &y0, &x1, &y1)) countRect(x0, y0, x1, y1);
	g_canvas.resetDirty();
}

void Display::flushRects(const DisplayFlushRect *rects, int count, bool)
{
	ensureCanvas();
	if (!rects || count <= 0) return;
	for (int i = 0; i < count; i++) countRect(rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1);
	g_canvas.resetDirty();
}

void Display::flushRectsRasterized(const DisplayFlushRect *rects, int count, DisplayStreamRasterFn raster, void *user, bool)
{
	if (!rects || count <= 0 || !raster) return;
	for (int i = 0; i < count; i++) {
		streamRect(rects[i].x0, rects[i].y0, rects[i].x1 - rects[i].x0 + 1, rects[i].y1 - rects[i].y0 + 1, raster, user);
	}
}

void Display::rebindCanvasToFramebuffer() { ensureCanvas(); }

bool Display::streamRect(int x, int y, int w, int h, DisplayStreamRasterFn raster, void *user)
{
	ensureCanvas();
	if (!raster || !g_framebuffer || w <= 0 || h <= 0) return false;
	const int x0 = std::max(0, x);
	const int y0 = std::max(0, y);
	const int x1 = std::min(g_canvas_width - 1, x + w - 1);
	const int y1 = std::min(g_canvas_height - 1, y + h - 1);
	if (x0 > x1 || y0 > y1) return true;
	const int rw = x1 - x0 + 1;
	const int rh = y1 - y0 + 1;
	// The raster callback fills NATIVE pixels (display.h): ARGB8888 here, so the
	// rows land in the framebuffer as they are, with no RGB565 round-trip.
	std::vector<gea::framework::graphics::pixel::native_t> pixels(static_cast<std::size_t>(rw * rh));
	raster(pixels.data(), rw, rh, x0, y0, user);
	for (int row = 0; row < rh; row++) {
		for (int col = 0; col < rw; col++) {
			g_framebuffer[static_cast<std::size_t>(y0 + row) * g_canvas_width + x0 + col] =
			    pixels[static_cast<std::size_t>(row) * rw + col];
		}
	}
	g_canvas.markDirty(x0, y0, x1, y1);
	flush();
	return true;
}

bool Display::present(const DisplayPresentCommand *commands, int command_count)
{
	if (g_live_env && g_live_canvas && presentToNativeCanvas(commands, command_count)) {
		g_direct_canvas_presented = true;
		return true;
	}
	ensureCanvas();
	if (!commands || command_count <= 0) return false;
	for (int i = 0; i < command_count; i++) {
		const auto &command = commands[i];
		switch (command.type) {
		case DisplayPresentCommandType::Clear:
			g_canvas.clear(command.clear.color);
			break;
		case DisplayPresentCommandType::FillRectRgb565:
			g_canvas.setGlobalAlpha(command.fillRectRgb565.alpha);
			g_canvas.fillRect(command.fillRectRgb565.x, command.fillRectRgb565.y, command.fillRectRgb565.w, command.fillRectRgb565.h, command.fillRectRgb565.color);
			break;
		case DisplayPresentCommandType::StrokeRectRgb565:
			g_canvas.setGlobalAlpha(command.strokeRectRgb565.alpha);
			g_canvas.strokeRect(command.strokeRectRgb565.x, command.strokeRectRgb565.y, command.strokeRectRgb565.w, command.strokeRectRgb565.h, command.strokeRectRgb565.color);
			break;
		case DisplayPresentCommandType::FillTriangleRgb565:
			g_canvas.setGlobalAlpha(command.fillTriangleRgb565.alpha);
			g_canvas.fillTriangle(command.fillTriangleRgb565.x0, command.fillTriangleRgb565.y0, command.fillTriangleRgb565.x1, command.fillTriangleRgb565.y1, command.fillTriangleRgb565.x2, command.fillTriangleRgb565.y2, command.fillTriangleRgb565.color);
			break;
		case DisplayPresentCommandType::FillCircleRgb565:
			g_canvas.setGlobalAlpha(command.fillCircleRgb565.alpha);
			g_canvas.fillCircle(command.fillCircleRgb565.x, command.fillCircleRgb565.y, command.fillCircleRgb565.radius, command.fillCircleRgb565.color);
			break;
		case DisplayPresentCommandType::StrokeCircleRgb565:
			g_canvas.setGlobalAlpha(command.strokeCircleRgb565.alpha);
			g_canvas.strokeCircle(command.strokeCircleRgb565.x, command.strokeCircleRgb565.y, command.strokeCircleRgb565.radius, command.strokeCircleRgb565.color);
			break;
		case DisplayPresentCommandType::FillCirclesRgb565:
			g_canvas.setGlobalAlpha(command.fillCirclesRgb565.alpha);
			g_canvas.fillCirclesRgb565(command.fillCirclesRgb565.xs, command.fillCirclesRgb565.ys, command.fillCirclesRgb565.count, command.fillCirclesRgb565.radius, command.fillCirclesRgb565.colors);
			break;
		case DisplayPresentCommandType::DrawImage:
			g_canvas.setGlobalAlpha(command.drawImage.alpha);
			g_canvas.drawImage(command.drawImage.pixels, command.drawImage.alphaPixels, command.drawImage.srcWidth, command.drawImage.srcHeight, command.drawImage.x, command.drawImage.y);
			break;
		case DisplayPresentCommandType::DrawImageScaled:
			g_canvas.setGlobalAlpha(command.drawImageScaled.alpha);
			g_canvas.drawImage(command.drawImageScaled.pixels, command.drawImageScaled.alphaPixels, command.drawImageScaled.srcWidth, command.drawImageScaled.srcHeight, command.drawImageScaled.x, command.drawImageScaled.y, command.drawImageScaled.w, command.drawImageScaled.h);
			break;
		case DisplayPresentCommandType::DrawImageRotated90CW:
			g_canvas.setGlobalAlpha(command.drawImageRotated90CW.alpha);
			g_canvas.drawImageRotated90CW(command.drawImageRotated90CW.pixels, command.drawImageRotated90CW.alphaPixels, command.drawImageRotated90CW.srcWidth, command.drawImageRotated90CW.srcHeight, command.drawImageRotated90CW.x, command.drawImageRotated90CW.y, command.drawImageRotated90CW.w, command.drawImageRotated90CW.h);
			break;
		case DisplayPresentCommandType::DrawImageTiledX:
			g_canvas.setGlobalAlpha(command.drawImageTiledX.alpha);
			g_canvas.drawImageTiledX(command.drawImageTiledX.pixels, command.drawImageTiledX.alphaPixels, command.drawImageTiledX.srcWidth, command.drawImageTiledX.srcHeight, command.drawImageTiledX.x, command.drawImageTiledX.y, command.drawImageTiledX.w);
			break;
		case DisplayPresentCommandType::FillText:
			g_canvas.setGlobalAlpha(command.fillText.alpha);
			if (command.fillText.fontFamilyId >= 0) {
				g_canvas.drawTextFontFamily(command.fillText.text, command.fillText.x, command.fillText.y, command.fillText.color, command.fillText.fontFamilyId, command.fillText.fontSizePx);
			} else {
				g_canvas.drawText(command.fillText.text, command.fillText.x, command.fillText.y, command.fillText.color, command.fillText.scale);
			}
			break;
		}
	}
	g_canvas.setGlobalAlpha(255);
	flush();
	return true;
}

void Display::setFlushConfig(int chunk_rows, int queue_depth)
{
	g_flush_rows = chunk_rows;
	g_flush_depth = queue_depth;
}
void Display::reserveInternal(std::size_t) {}
void Display::applyPendingInternalReserve() {}
int Display::flushChunkRows() { return g_flush_rows; }
int Display::flushQueueDepth() { return g_flush_depth; }
int Display::flushBufferBytes() { return g_canvas_width * g_flush_rows * 4 * g_flush_depth; }
void Display::pushClip(int x, int y, int w, int h) { canvas()->pushClip(x, y, w, h); }
void Display::popClip() { canvas()->popClip(); }
void Display::resetClip() { canvas()->resetClip(); }
void Display::setAlpha(std::uint8_t alpha) { g_alpha = alpha; canvas()->setGlobalAlpha(alpha); }
std::uint8_t Display::alpha() { return g_alpha; }
int Display::brightness() { return g_brightness; }
void Display::setBrightness(int brightness_percent) { g_brightness = std::clamp(brightness_percent, 0, 100); }
void Display::setVSync(bool) {}
bool Display::vsyncEnabled() { return false; }
void Display::invalidate() {}
void Display::vsyncWaitForFrame() {}
void Display::clip(int *x0, int *y0, int *x1, int *y1) { canvas()->currentClip(x0, y0, x1, y1); }
void Display::fillRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color) { canvas()->fillRect(x, y, w, h, color); }
void Display::scrollRect(int x, int y, int w, int h, int dx, int dy) { canvas()->scrollRect(x, y, w, h, dx, dy); }
void Display::resetScrollRegion() { canvas()->setScrollRegion(0, 0, 0); }
void Display::strokeRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color) { canvas()->strokeRect(x, y, w, h, color); }
void Display::fillCircle(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color) { canvas()->fillCircle(cx, cy, r, color); }
void Display::strokeCircle(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color) { canvas()->strokeCircle(cx, cy, r, color); }
void Display::drawLine(int x0, int y0, int x1, int y1, gea::framework::graphics::pixel::native_t color) { canvas()->drawLine(x0, y0, x1, y1, color); }
void Display::drawArc(int cx, int cy, int r, int start_deg, int end_deg, gea::framework::graphics::pixel::native_t color) { canvas()->drawArc(cx, cy, r, start_deg, end_deg, color); }
void Display::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, gea::framework::graphics::pixel::native_t color) { canvas()->fillTriangle(x0, y0, x1, y1, x2, y2, color); }
void Display::drawText(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, float scale) { canvas()->drawText(text, x, y, color, scale); }
void Display::drawTextFont(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, int font_id) { canvas()->drawTextFont(text, x, y, color, font_id); }
void Display::drawTextFontFamily(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, int family_id, int size_px) { canvas()->drawTextFontFamily(text, x, y, color, family_id, size_px); }
void Display::setPixel(int x, int y, gea::framework::graphics::pixel::native_t color) { canvas()->fillRect(x, y, 1, 1, color); }
void Display::fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, gea::framework::graphics::pixel::native_t color) { canvas()->fillRoundedRect(x, y, w, h, tl, tr, br, bl, color); }
void Display::fillRoundedRectBoxesRgb565(const std::int16_t *xs, const std::int16_t *ys, int count, int w, int h, int tl, int tr, int br, int bl, const gea::framework::graphics::pixel::native_t *colors)
{
	canvas()->fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
}
void Display::strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, gea::framework::graphics::pixel::native_t color) { canvas()->strokeRoundedRect(x, y, w, h, tl, tr, br, bl, lw, color); }
void Display::blitImage(const gea::framework::graphics::pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h, int dx, int dy) { canvas()->drawImage(src, alpha, src_w, src_h, dx, dy); }
void Display::blitImageScaled(const gea::framework::graphics::pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h, int dx, int dy, int dst_w, int dst_h) { canvas()->drawImage(src, alpha, src_w, src_h, dx, dy, dst_w, dst_h); }
void Display::setWorldOverlay(const std::uint16_t *, int, int, int, int) {}
void Display::setWorldScroll(int) {}
void Display::flushStatsRead(std::int64_t *total_us, int *call_count, int *pixel_count)
{
	if (total_us) *total_us = 0;
	if (call_count) *call_count = g_flush_calls;
	if (pixel_count) *pixel_count = g_flush_pixels;
}
DisplayFlushPerfStats Display::flushPerfStatsRead()
{
	DisplayFlushPerfStats stats;
	stats.callCount = g_flush_calls;
	stats.pixelCount = g_flush_pixels;
	return stats;
}
void Display::presentPathDebug(int *calls, int *direct, int *general, int *rejected, int *tileShapeFailKind, int *tileShapeFailType, int *tileShapeFailCount)
{
	if (calls) *calls = 0;
	if (direct) *direct = 0;
	if (general) *general = 0;
	if (rejected) *rejected = 0;
	if (tileShapeFailKind) *tileShapeFailKind = 0;
	if (tileShapeFailType) *tileShapeFailType = 0;
	if (tileShapeFailCount) *tileShapeFailCount = 0;
}
void Display::landFrameDebug(int *total, int *align, int *raster, int *text, int *flip)
{
	if (total) *total = 0;
	if (align) *align = 0;
	if (raster) *raster = 0;
	if (text) *text = 0;
	if (flip) *flip = 0;
}
void Display::landPanDebug(int *detect, int *kick, int *strips, int *wait, int *interior)
{
	if (detect) *detect = 0;
	if (kick) *kick = 0;
	if (strips) *strips = 0;
	if (wait) *wait = 0;
	if (interior) *interior = 0;
}
void Display::flushStatsReset()
{
	g_flush_calls = 0;
	g_flush_pixels = 0;
}
const char *Display::flushStageName() { return "idle"; }
int Display::flushStageChunk() { return 0; }
DisplayFlushStageDetail Display::flushStageDetail() { return {}; }

void applyOrientation(gea::framework::display::DisplayOrientation) {}

}  // namespace gea::platform::display

extern "C" void gea_android_display_set_viewport_size(int width, int height)
{
	if (width <= 0 || height <= 0) return;
	g_canvas_width = width;
	g_canvas_height = height;
	ensureCanvas();
}

extern "C" int gea_android_display_copy_argb8888(std::uint32_t *dst, int pixel_capacity, int *width, int *height)
{
	ensureCanvas();
	if (width) *width = g_canvas_width;
	if (height) *height = g_canvas_height;
	if (!dst || pixel_capacity < g_canvas_width * g_canvas_height || !g_framebuffer) return 0;
	const int pixel_count = g_canvas_width * g_canvas_height;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	std::memcpy(dst, g_framebuffer, static_cast<std::size_t>(pixel_count) * sizeof(std::uint32_t));
#else
	for (int i = 0; i < pixel_count; i++) {
		int r = 0;
		int g = 0;
		int b = 0;
		int a = 255;
		gea::framework::graphics::pixel::unpackNative8(g_framebuffer[i], &r, &g, &b, &a);
		dst[i] = static_cast<std::uint32_t>(((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
	}
#endif
	return 1;
}

// Direct SurfaceView presentation: the composed native framebuffer is posted
// straight to a BufferQueue buffer (SurfaceFlinger composites it on the GPU),
// bypassing the Bitmap.setPixels + HWUI-texture-upload path that costs more
// than a whole vsync budget on watch-class GPUs. All calls run on the UI
// thread (surfaceChanged/surfaceDestroyed and the frame callback), so no
// locking is needed.
namespace {
ANativeWindow *g_native_window = nullptr;
int g_native_window_width = 0;
int g_native_window_height = 0;
}  // namespace

extern "C" void gea_android_display_set_surface(JNIEnv *env, jobject surface)
{
	if (g_native_window) {
		ANativeWindow_release(g_native_window);
		g_native_window = nullptr;
		g_native_window_width = 0;
		g_native_window_height = 0;
	}
	if (!surface) return;
	g_native_window = ANativeWindow_fromSurface(env, surface);
}

extern "C" int gea_android_display_blit_to_surface()
{
	if (!g_native_window) return 0;
	ensureCanvas();
	const int width = g_canvas_width;
	const int height = g_canvas_height;
	if (!g_framebuffer || width <= 0 || height <= 0) return 0;

	// Engine ARGB ints are BGRA in byte order: ask for a BGRA buffer so the
	// blit is a straight memcpy. Composers that reject it get RGBA_8888 and a
	// R<->B swizzle copy — the actual format granted is read back from the
	// locked buffer, so a silent downgrade still renders correct colors.
	constexpr int kHalPixelFormatBgra8888 = 5;
	if (g_native_window_width != width || g_native_window_height != height) {
		if (ANativeWindow_setBuffersGeometry(g_native_window, width, height, kHalPixelFormatBgra8888) != 0 &&
		    ANativeWindow_setBuffersGeometry(g_native_window, width, height, WINDOW_FORMAT_RGBA_8888) != 0)
			return 0;
		g_native_window_width = width;
		g_native_window_height = height;
	}

	ANativeWindow_Buffer buffer;
	if (ANativeWindow_lock(g_native_window, &buffer, nullptr) != 0) return 0;

	const int copyWidth = std::min(width, buffer.width);
	const int copyHeight = std::min(height, buffer.height);
	const bool bufferIsBgra = buffer.format == kHalPixelFormatBgra8888;
	auto *dstBase = static_cast<std::uint32_t *>(buffer.bits);
	for (int y = 0; y < copyHeight; y++) {
		const std::uint32_t *src = g_framebuffer + static_cast<std::size_t>(y) * width;
		std::uint32_t *dst = dstBase + static_cast<std::size_t>(y) * buffer.stride;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
		if (bufferIsBgra) {
			std::memcpy(dst, src, static_cast<std::size_t>(copyWidth) * sizeof(std::uint32_t));
		} else {
			for (int x = 0; x < copyWidth; x++) {
				const std::uint32_t argb = src[x];
				dst[x] = (argb & 0xFF00FF00u) | ((argb & 0x00FF0000u) >> 16) | ((argb & 0x000000FFu) << 16);
			}
		}
#else
		(void)bufferIsBgra;
		std::memcpy(dst, src, static_cast<std::size_t>(copyWidth) * sizeof(std::uint32_t));
#endif
	}

	ANativeWindow_unlockAndPost(g_native_window);
	return 1;
}

extern "C" void gea_android_display_begin_native_canvas(JNIEnv *env, jobject canvas)
{
	g_live_env = env;
	g_live_canvas = canvas;
	g_direct_canvas_presented = false;
}

extern "C" void gea_android_display_end_native_canvas()
{
	g_live_canvas = nullptr;
	g_live_env = nullptr;
}

extern "C" int gea_android_display_direct_canvas_presented()
{
	return g_direct_canvas_presented ? 1 : 0;
}
