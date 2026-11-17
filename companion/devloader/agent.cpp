// libglassagent.so: the companion's dev loader, for the userdebug test AVD.
//
// A JVMTI agent attached to a running app with
//     cmd activity attach-agent <process> <this .so>=<companion base.apk>
// It loads assets/devloader.dex out of the APK, which runs the module's Xposed
// entry points against a small Xposed API backed by this file: a hook is a
// JVMTI breakpoint at the first instruction of the method, and when it is hit
// the arguments are read out of their registers, handed to the Java side, and
// any that a callback replaced are written back before the method runs.
//
// ART allows this in a process that is not Java-debuggable as long as JDWP is
// allowed (userdebug + persist.debug.dalvik.vm.jdwp.enabled=1, read by zygote
// at start), through the restricted kArtTiVersion environment. That
// environment has breakpoints and local-variable access but not class
// redefinition, early return or popping frames, which is why the dev loader
// only does before-hooks. Release builds strip local-variable tables; ART
// infers the argument types from the verifier instead, so reads and writes of
// the argument registers work on stripped SystemUI and launcher builds.
//
// LSPosed never loads this file. It is inert unless someone attaches it.
#include <jni.h>
#include "jvmti.h"

#include <android/log.h>
#include <pthread.h>
#include <string.h>

#define TAG "StockGlassDev"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr jint kArtTiVersion = JVMTI_VERSION_1_2 | 0x40000000;
constexpr int kMaxHooks = 32;
constexpr int kMaxArgs = 48;

struct Hook {
  jmethodID method;
  bool isStatic;
  int nargs;
  char shape[kMaxArgs + 1];  // one char per parameter: L Z B C S I J F D
  int firstSlot;             // register of `this`, or of the first argument
};

struct Box {
  char code;
  const char* cls;
  const char* valueOfSig;
  const char* unboxName;
  const char* unboxSig;
  jclass c;
  jmethodID valueOf;
  jmethodID unbox;
};

Box gBoxes[] = {
    {'Z', "java/lang/Boolean", "(Z)Ljava/lang/Boolean;", "booleanValue", "()Z", nullptr, nullptr, nullptr},
    {'B', "java/lang/Byte", "(B)Ljava/lang/Byte;", "byteValue", "()B", nullptr, nullptr, nullptr},
    {'C', "java/lang/Character", "(C)Ljava/lang/Character;", "charValue", "()C", nullptr, nullptr, nullptr},
    {'S', "java/lang/Short", "(S)Ljava/lang/Short;", "shortValue", "()S", nullptr, nullptr, nullptr},
    {'I', "java/lang/Integer", "(I)Ljava/lang/Integer;", "intValue", "()I", nullptr, nullptr, nullptr},
    {'J', "java/lang/Long", "(J)Ljava/lang/Long;", "longValue", "()J", nullptr, nullptr, nullptr},
    {'F', "java/lang/Float", "(F)Ljava/lang/Float;", "floatValue", "()F", nullptr, nullptr, nullptr},
    {'D', "java/lang/Double", "(D)Ljava/lang/Double;", "doubleValue", "()D", nullptr, nullptr, nullptr},
};

jvmtiEnv* gTi;
Hook gHooks[kMaxHooks];
int gHookCount;
pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
jclass gObjectClass;
jclass gLoaderClass;
jmethodID gDispatch;
bool gAttached;

Box* boxFor(char code) {
  for (Box& b : gBoxes) {
    if (b.code == code) return &b;
  }
  return nullptr;
}

bool clearException(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) return false;
  LOGE("%s: Java exception", what);
  env->ExceptionDescribe();
  env->ExceptionClear();
  return true;
}

// Reads argument i into a boxed object. Advances *slot past it.
jvmtiError readArg(jvmtiEnv* ti, JNIEnv* env, jthread thread, char code, int* slot, jobject* out) {
  jvmtiError e;
  *out = nullptr;
  switch (code) {
    case 'L':
      e = ti->GetLocalObject(thread, 0, *slot, out);
      *slot += 1;
      return e;
    case 'J': {
      jlong v = 0;
      e = ti->GetLocalLong(thread, 0, *slot, &v);
      *slot += 2;
      if (!e) *out = env->CallStaticObjectMethod(boxFor('J')->c, boxFor('J')->valueOf, v);
      return e;
    }
    case 'D': {
      jdouble v = 0;
      e = ti->GetLocalDouble(thread, 0, *slot, &v);
      *slot += 2;
      if (!e) *out = env->CallStaticObjectMethod(boxFor('D')->c, boxFor('D')->valueOf, v);
      return e;
    }
    case 'F': {
      jfloat v = 0;
      e = ti->GetLocalFloat(thread, 0, *slot, &v);
      *slot += 1;
      if (!e) *out = env->CallStaticObjectMethod(boxFor('F')->c, boxFor('F')->valueOf, v);
      return e;
    }
    default: {
      jint v = 0;
      e = ti->GetLocalInt(thread, 0, *slot, &v);
      *slot += 1;
      if (e) return e;
      Box* b = boxFor(code);
      switch (code) {
        case 'Z': *out = env->CallStaticObjectMethod(b->c, b->valueOf, (jboolean)(v != 0)); break;
        case 'B': *out = env->CallStaticObjectMethod(b->c, b->valueOf, (jbyte)v); break;
        case 'C': *out = env->CallStaticObjectMethod(b->c, b->valueOf, (jchar)v); break;
        case 'S': *out = env->CallStaticObjectMethod(b->c, b->valueOf, (jshort)v); break;
        default: *out = env->CallStaticObjectMethod(b->c, b->valueOf, v); break;
      }
      return e;
    }
  }
}

jvmtiError writeArg(jvmtiEnv* ti, JNIEnv* env, jthread thread, char code, int slot, jobject v) {
  if (code == 'L') return ti->SetLocalObject(thread, 0, slot, v);
  if (v == nullptr) return JVMTI_ERROR_ILLEGAL_ARGUMENT;  // null for a primitive
  Box* b = boxFor(code);
  switch (code) {
    case 'J': return ti->SetLocalLong(thread, 0, slot, env->CallLongMethod(v, b->unbox));
    case 'D': return ti->SetLocalDouble(thread, 0, slot, env->CallDoubleMethod(v, b->unbox));
    case 'F': return ti->SetLocalFloat(thread, 0, slot, env->CallFloatMethod(v, b->unbox));
    case 'Z': return ti->SetLocalInt(thread, 0, slot, env->CallBooleanMethod(v, b->unbox) ? 1 : 0);
    case 'B': return ti->SetLocalInt(thread, 0, slot, env->CallByteMethod(v, b->unbox));
    case 'C': return ti->SetLocalInt(thread, 0, slot, env->CallCharMethod(v, b->unbox));
    case 'S': return ti->SetLocalInt(thread, 0, slot, env->CallShortMethod(v, b->unbox));
    default: return ti->SetLocalInt(thread, 0, slot, env->CallIntMethod(v, b->unbox));
  }
}

void JNICALL onBreakpoint(jvmtiEnv* ti, JNIEnv* env, jthread thread, jmethodID method, jlocation) {
  Hook h;
  int id = -1;
  pthread_mutex_lock(&gLock);
  for (int i = 0; i < gHookCount; ++i) {
    if (gHooks[i].method == method) {
      h = gHooks[i];
      id = i;
      break;
    }
  }
  pthread_mutex_unlock(&gLock);
  if (id < 0) return;

  if (env->PushLocalFrame(2 * h.nargs + 16) != JNI_OK) {
    clearException(env, "PushLocalFrame");
    return;
  }
  int slots[kMaxArgs];
  jobject self = nullptr;
  int slot = h.firstSlot;
  if (!h.isStatic) {
    jvmtiError e = ti->GetLocalInstance(thread, 0, &self);
    if (e) {
      LOGE("hook %d: GetLocalInstance: jvmtiError %d", id, e);
      env->PopLocalFrame(nullptr);
      return;
    }
    slot += 1;
  }
  jobjectArray args = env->NewObjectArray(h.nargs, gObjectClass, nullptr);
  for (int i = 0; i < h.nargs; ++i) {
    slots[i] = slot;
    jobject v = nullptr;
    jvmtiError e = readArg(ti, env, thread, h.shape[i], &slot, &v);
    if (e || clearException(env, "boxing")) {
      // Never dispatch with an argument we could not read: a callback would see
      // null where the method will see a real value.
      LOGE("hook %d: reading argument %d (%c, register v%d): jvmtiError %d", id, i, h.shape[i],
           slots[i], e);
      env->PopLocalFrame(nullptr);
      return;
    }
    env->SetObjectArrayElement(args, i, v);
  }

  jobjectArray out =
      (jobjectArray)env->CallStaticObjectMethod(gLoaderClass, gDispatch, id, self, args);
  if (clearException(env, "dispatch")) out = nullptr;
  if (out != nullptr && env->GetArrayLength(out) == h.nargs) {
    for (int i = 0; i < h.nargs; ++i) {
      jobject now = env->GetObjectArrayElement(out, i);
      jobject was = env->GetObjectArrayElement(args, i);
      if (env->IsSameObject(now, was)) continue;
      jvmtiError e = writeArg(ti, env, thread, h.shape[i], slots[i], now);
      if (clearException(env, "unboxing") || e) {
        LOGE("hook %d: writing argument %d (%c, register v%d): jvmtiError %d", id, i, h.shape[i],
             slots[i], e);
      }
    }
  }
  env->PopLocalFrame(nullptr);
}

jint JNICALL nativeHook(JNIEnv* env, jclass, jobject member, jstring shape, jboolean isStatic) {
  jmethodID m = env->FromReflectedMethod(member);
  if (m == nullptr) {
    clearException(env, "FromReflectedMethod");
    return -JVMTI_ERROR_INVALID_METHODID;
  }
  const char* s = env->GetStringUTFChars(shape, nullptr);
  int n = (int)strlen(s);
  if (n > kMaxArgs) {
    env->ReleaseStringUTFChars(shape, s);
    return -JVMTI_ERROR_ILLEGAL_ARGUMENT;
  }
  jint maxLocals = 0, argsSize = 0;
  jvmtiError e = gTi->GetMaxLocals(m, &maxLocals);
  if (!e) e = gTi->GetArgumentsSize(m, &argsSize);
  if (!e) e = gTi->SetBreakpoint(m, 0);
  if (e) {
    env->ReleaseStringUTFChars(shape, s);
    return -e;
  }
  pthread_mutex_lock(&gLock);
  int id = -1;
  if (gHookCount < kMaxHooks) {
    id = gHookCount++;
    Hook& h = gHooks[id];
    h.method = m;
    h.isStatic = isStatic;
    h.nargs = n;
    memcpy(h.shape, s, n + 1);
    // ART puts the incoming arguments (with `this` first) in the last
    // `ins` registers of the frame.
    h.firstSlot = maxLocals - argsSize;
  }
  pthread_mutex_unlock(&gLock);
  env->ReleaseStringUTFChars(shape, s);
  if (id < 0) {
    gTi->ClearBreakpoint(m, 0);
    return -JVMTI_ERROR_OUT_OF_MEMORY;
  }
  return id;
}

// Reads assets/devloader.dex out of the APK into an InMemoryDexClassLoader
// whose parent is the boot class path, so the module (loaded by Loader with a
// PathClassLoader on top of it) sees the Xposed API and nothing of the host app.
jclass loadLoaderClass(JNIEnv* env, const char* apk) {
  jclass zipC = env->FindClass("java/util/zip/ZipFile");
  jobject zip = env->NewObject(zipC, env->GetMethodID(zipC, "<init>", "(Ljava/lang/String;)V"),
                               env->NewStringUTF(apk));
  if (clearException(env, "open APK")) return nullptr;
  jobject entry = env->CallObjectMethod(
      zip, env->GetMethodID(zipC, "getEntry", "(Ljava/lang/String;)Ljava/util/zip/ZipEntry;"),
      env->NewStringUTF("assets/devloader.dex"));
  if (clearException(env, "getEntry") || entry == nullptr) {
    LOGE("no assets/devloader.dex in %s", apk);
    return nullptr;
  }
  jobject in = env->CallObjectMethod(
      zip,
      env->GetMethodID(zipC, "getInputStream", "(Ljava/util/zip/ZipEntry;)Ljava/io/InputStream;"),
      entry);
  jclass isC = env->FindClass("java/io/InputStream");
  jbyteArray bytes =
      (jbyteArray)env->CallObjectMethod(in, env->GetMethodID(isC, "readAllBytes", "()[B"));
  env->CallVoidMethod(zip, env->GetMethodID(zipC, "close", "()V"));
  if (clearException(env, "read devloader.dex")) return nullptr;
  jclass bbC = env->FindClass("java/nio/ByteBuffer");
  jobject buf = env->CallStaticObjectMethod(
      bbC, env->GetStaticMethodID(bbC, "wrap", "([B)Ljava/nio/ByteBuffer;"), bytes);
  jclass imC = env->FindClass("dalvik/system/InMemoryDexClassLoader");
  jobject loader = env->NewObject(
      imC, env->GetMethodID(imC, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"), buf,
      (jobject) nullptr);
  if (clearException(env, "InMemoryDexClassLoader")) return nullptr;
  jclass clC = env->FindClass("java/lang/ClassLoader");
  jclass cls = (jclass)env->CallObjectMethod(
      loader, env->GetMethodID(clC, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
      env->NewStringUTF("dev.stockglass.devloader.Loader"));
  if (clearException(env, "load Loader")) return nullptr;
  return cls;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void*) {
  if (gAttached) {
    LOGI("already attached; restart the process to load another build");
    return JNI_OK;
  }
  if (options == nullptr || options[0] == '\0') {
    LOGE("usage: attach-agent <process> <libglassagent.so>=<companion base.apk>");
    return JNI_OK;
  }
  if (vm->GetEnv((void**)&gTi, kArtTiVersion) != JNI_OK &&
      vm->GetEnv((void**)&gTi, JVMTI_VERSION_1_2) != JNI_OK) {
    LOGE("no JVMTI environment");
    return JNI_OK;
  }
  jvmtiCapabilities caps;
  memset(&caps, 0, sizeof caps);
  caps.can_generate_breakpoint_events = 1;
  caps.can_access_local_variables = 1;
  jvmtiError e = gTi->AddCapabilities(&caps);
  if (e) {
    LOGE("breakpoints/local variables unavailable: jvmtiError %d", e);
    return JNI_OK;
  }
  jvmtiEventCallbacks cb;
  memset(&cb, 0, sizeof cb);
  cb.Breakpoint = onBreakpoint;
  gTi->SetEventCallbacks(&cb, sizeof cb);
  gTi->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, nullptr);

  JNIEnv* env = nullptr;
  vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  gObjectClass = (jclass)env->NewGlobalRef(env->FindClass("java/lang/Object"));
  for (Box& b : gBoxes) {
    b.c = (jclass)env->NewGlobalRef(env->FindClass(b.cls));
    b.valueOf = env->GetStaticMethodID(b.c, "valueOf", b.valueOfSig);
    b.unbox = env->GetMethodID(b.c, b.unboxName, b.unboxSig);
  }
  if (clearException(env, "boxing setup")) return JNI_OK;

  jclass cls = loadLoaderClass(env, options);
  if (cls == nullptr) return JNI_OK;
  JNINativeMethod natives[] = {
      {(char*)"nativeHook", (char*)"(Ljava/lang/reflect/Member;Ljava/lang/String;Z)I",
       (void*)nativeHook},
  };
  if (env->RegisterNatives(cls, natives, 1) != JNI_OK) {
    clearException(env, "RegisterNatives");
    return JNI_OK;
  }
  gLoaderClass = (jclass)env->NewGlobalRef(cls);
  gDispatch = env->GetStaticMethodID(cls, "dispatch",
                                     "(ILjava/lang/Object;[Ljava/lang/Object;)[Ljava/lang/Object;");
  jmethodID attach = env->GetStaticMethodID(cls, "attach", "(Ljava/lang/String;)V");
  if (clearException(env, "Loader methods")) return JNI_OK;
  gAttached = true;
  env->CallStaticVoidMethod(cls, attach, env->NewStringUTF(options));
  clearException(env, "Loader.attach");
  return JNI_OK;
}
