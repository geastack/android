#pragma once

#include <jni.h>
#include <string>
#include "host/websocket.h"

namespace gea::android {
class JniEnv {
public:
  JniEnv();
  ~JniEnv();
  JniEnv(const JniEnv&) = delete;
  JniEnv& operator=(const JniEnv&) = delete;
  JNIEnv *get() const { return env_; }
private:
  JNIEnv *env_ = nullptr;
  bool attached_ = false;
};

jclass findAppClass(JNIEnv *env, const char *name);
jbyteArray utf8Bytes(JNIEnv *env, const std::string &text);
std::string fromUtf8Bytes(JNIEnv *env, jbyteArray bytes);
bool isLoopbackHttpRequest();
bool sendBinaryBase64(const host::WebSocket &socket, const std::string &text);
}
