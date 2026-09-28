// The JNI bridge. Everything here is a thin translation of src/yoai.h into
// what Kotlin can call; no policy lives at this layer.
//
// **Threading.** yoai_chat() and yoai_tts_say() both BLOCK and both
// invoke their handler ON THE CALLING THREAD, so the JNIEnv of the call frame
// is valid throughout and no AttachCurrentThread dance is needed. The Kotlin
// side is responsible for not calling them on the main thread; see Yoai.kt.
//
// **Strings.** The core returns buffers owned by it and valid until that
// thread's next call, so every one is copied into a jstring before returning.
// A NULL return means failure and becomes a null jstring -- the reason is in
// the log, and for the TTS lane also in nativeTtsLastError().

#include <jni.h>

#include <string>
#include <vector>

#include "yoai.h"

namespace {

// A jstring as UTF-8. Kotlin's null becomes an empty string, which every entry
// point below treats the same way the C ABI does: as "not given".
std::string to_utf8(JNIEnv* env, jstring s) {
    if (s == nullptr) { return {}; }
    const char* raw = env->GetStringUTFChars(s, nullptr);
    if (raw == nullptr) { return {}; }
    std::string out(raw);
    env->ReleaseStringUTFChars(s, raw);
    return out;
}

// NULL becomes a null jstring rather than an empty one. The distinction is the
// return contract of every `const char*` entry point in the C ABI: NULL is
// failure, "" is a successful empty answer.
jstring from_utf8(JNIEnv* env, const char* s) {
    return s == nullptr ? nullptr : env->NewStringUTF(s);
}

// An empty std::string means the caller passed nothing, which the C ABI spells
// as NULL for its optional arguments.
const char* or_null(const std::string& s) {
    return s.empty() ? nullptr : s.c_str();
}

//------------------------------------------------------------------------------
// Callback plumbing
//------------------------------------------------------------------------------

// What the two streaming entry points forward to their handler. Holds the call
// frame's env, which is valid because the core calls back on this same thread.
struct CallbackCtx {
    JNIEnv*  env;
    jobject  sink;      // the Kotlin listener, a local ref owned by the caller
    jmethodID method;
};

// Chat: (String, String) -> Boolean, true to keep streaming.
int chat_handler(const char* event_type, const char* content, void* user_data) {
    auto* ctx = static_cast<CallbackCtx*>(user_data);
    JNIEnv* env = ctx->env;

    jstring j_type = from_utf8(env, event_type);
    jstring j_content = from_utf8(env, content);
    jboolean keep = env->CallBooleanMethod(ctx->sink, ctx->method, j_type, j_content);
    env->DeleteLocalRef(j_type);
    env->DeleteLocalRef(j_content);

    // An exception thrown out of Kotlin cannot propagate through the C core, so
    // it is cleared here and reported as an abort. The turn stops, which is the
    // same thing the handler returning false does.
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return 0;
    }
    return keep == JNI_TRUE ? 1 : 0;
}

// TTS: (FloatArray, Boolean) -> Boolean, true to keep generating.
int audio_handler(const float* pcm, int n_samples, int last, void* user_data) {
    auto* ctx = static_cast<CallbackCtx*>(user_data);
    JNIEnv* env = ctx->env;

    jfloatArray arr = env->NewFloatArray(n_samples);
    if (arr == nullptr) { return 0; }     // OOM; stop rather than drop silently
    if (n_samples > 0) {
        env->SetFloatArrayRegion(arr, 0, n_samples, pcm);
    }
    jboolean keep = env->CallBooleanMethod(ctx->sink, ctx->method, arr,
                                           last != 0 ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(arr);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return 0;
    }
    return keep == JNI_TRUE ? 1 : 0;
}

}  // namespace

// Every entry point below is named for com.yoai.bodyclock.Yoai, which is the
// single Kotlin object that owns the library. See Yoai.kt.
#define BC(name) Java_com_yoai_bodyclock_Yoai_##name

extern "C" {

//------------------------------------------------------------------------------
// Core
//------------------------------------------------------------------------------

JNIEXPORT jstring JNICALL BC(nativeVersion)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_version());
}

JNIEXPORT void JNICALL BC(nativeSetLogLevel)(JNIEnv* env, jobject, jstring level) {
    const std::string s = to_utf8(env, level);
    yoai_set_log_level(s.c_str());
}

JNIEXPORT jstring JNICALL BC(nativeMemoryBandwidth)(JNIEnv* env, jobject,
                                                    jint threads, jint mib, jint passes) {
    return from_utf8(env, yoai_memory_bandwidth(threads, mib, passes));
}

//------------------------------------------------------------------------------
// The day log
//------------------------------------------------------------------------------
//
// Unlike everything else in this file these are FAST -- one indexed SQLite
// query each -- so Kotlin calls them straight from the main thread. That is the
// only reason the home screen can read the current state during composition
// instead of through a coroutine and a loading state.
//
// The reads return JSON whose shape is documented in src/yoai.h. Nothing
// here parses it: the Kotlin side does, once, in Timeline.kt.

JNIEXPORT jint JNICALL BC(nativeTimelineOpen)(JNIEnv* env, jobject, jstring dir) {
    const std::string s = to_utf8(env, dir);
    return yoai_timeline_open(s.c_str());
}

JNIEXPORT jboolean JNICALL BC(nativeTimelineOpened)(JNIEnv*, jobject) {
    return yoai_timeline_opened() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL BC(nativeTimelineClose)(JNIEnv*, jobject) {
    yoai_timeline_close();
}

JNIEXPORT jint JNICALL BC(nativeTimelineSwitchTo)(JNIEnv* env, jobject, jstring state_id) {
    const std::string s = to_utf8(env, state_id);
    return yoai_timeline_switch_to(s.c_str());
}

JNIEXPORT jint JNICALL BC(nativeTimelineEnd)(JNIEnv*, jobject) {
    return yoai_timeline_end();
}

JNIEXPORT jint JNICALL BC(nativeTimelineEndAt)(JNIEnv*, jobject, jlong at) {
    return yoai_timeline_end_at(at);
}

JNIEXPORT jint JNICALL BC(nativeTimelineUndo)(JNIEnv*, jobject) {
    return yoai_timeline_undo();
}

JNIEXPORT jint JNICALL BC(nativeTimelineNoteAdd)(JNIEnv* env, jobject, jstring text) {
    const std::string s = to_utf8(env, text);
    return yoai_timeline_note_add(s.c_str());
}

JNIEXPORT jstring JNICALL BC(nativeTimelineNow)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_timeline_now());
}

JNIEXPORT jstring JNICALL BC(nativeTimelineRecent)(JNIEnv* env, jobject, jint hours) {
    return from_utf8(env, yoai_timeline_recent(hours));
}

JNIEXPORT jstring JNICALL BC(nativeTimelineDay)(JNIEnv* env, jobject, jint local_day) {
    return from_utf8(env, yoai_timeline_day(local_day));
}

JNIEXPORT jint JNICALL BC(nativeTimelineToday)(JNIEnv*, jobject) {
    return yoai_timeline_today();
}

JNIEXPORT jstring JNICALL BC(nativeTimelineDayToIso)(JNIEnv* env, jobject, jint local_day) {
    return from_utf8(env, yoai_timeline_day_to_iso(local_day));
}

JNIEXPORT jint JNICALL BC(nativeTimelineIsoToDay)(JNIEnv* env, jobject, jstring iso) {
    const std::string s = to_utf8(env, iso);
    return yoai_timeline_iso_to_day(s.c_str());
}

JNIEXPORT jstring JNICALL BC(nativeTimelineLastError)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_timeline_last_error());
}

//------------------------------------------------------------------------------
// Recognition (SenseVoice) and the prosody measurements
//------------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL BC(nativeVoiceAvailable)(JNIEnv*, jobject) {
    return yoai_voice_available() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL BC(nativeVoiceLoad)(JNIEnv* env, jobject, jstring model_path) {
    const std::string p = to_utf8(env, model_path);
    return yoai_voice_load(p.c_str());
}

JNIEXPORT jboolean JNICALL BC(nativeVoiceLoaded)(JNIEnv*, jobject) {
    return yoai_voice_loaded() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL BC(nativeVoiceUnload)(JNIEnv*, jobject) {
    yoai_voice_unload();
}

JNIEXPORT jint JNICALL BC(nativeVoiceSetThreads)(JNIEnv*, jobject, jint threads) {
    return yoai_voice_set_threads(threads);
}

JNIEXPORT jint JNICALL BC(nativeVoiceSetItn)(JNIEnv*, jobject, jboolean on) {
    return yoai_voice_set_itn(on == JNI_TRUE ? 1 : 0);
}

// The samples are copied out of the JVM heap rather than pinned: analysis holds
// them across a call that runs for a second or more, and a pinned array blocks
// the collector for all of it.
JNIEXPORT jstring JNICALL BC(nativeVoiceAnalyze)(JNIEnv* env, jobject,
                                                 jshortArray pcm, jint sample_rate) {
    if (pcm == nullptr) { return nullptr; }
    const jsize n = env->GetArrayLength(pcm);
    std::vector<short> buf(static_cast<size_t>(n));
    if (n > 0) {
        env->GetShortArrayRegion(pcm, 0, n, buf.data());
    }
    return from_utf8(env, yoai_voice_analyze(buf.data(), static_cast<int>(n), sample_rate));
}

//------------------------------------------------------------------------------
// Speech output (qwentts.cpp)
//------------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL BC(nativeTtsAvailable)(JNIEnv*, jobject) {
    return yoai_tts_available() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL BC(nativeTtsLoad)(JNIEnv* env, jobject,
                                         jstring talker_path, jstring codec_path) {
    const std::string t = to_utf8(env, talker_path);
    const std::string c = to_utf8(env, codec_path);
    return yoai_tts_load(t.c_str(), c.c_str());
}

JNIEXPORT jboolean JNICALL BC(nativeTtsLoaded)(JNIEnv*, jobject) {
    return yoai_tts_loaded() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL BC(nativeTtsUnload)(JNIEnv*, jobject) {
    yoai_tts_unload();
}

JNIEXPORT jstring JNICALL BC(nativeTtsModelType)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_tts_model_type());
}

JNIEXPORT jstring JNICALL BC(nativeTtsSpeakers)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_tts_speakers());
}

JNIEXPORT void JNICALL BC(nativeTtsSetSpeaker)(JNIEnv* env, jobject, jstring name) {
    const std::string s = to_utf8(env, name);
    yoai_tts_set_speaker(or_null(s));
}

JNIEXPORT void JNICALL BC(nativeTtsSetVoice)(JNIEnv* env, jobject, jstring instruct) {
    const std::string s = to_utf8(env, instruct);
    yoai_tts_set_voice(or_null(s));
}

JNIEXPORT void JNICALL BC(nativeTtsSetSeed)(JNIEnv*, jobject, jlong seed) {
    yoai_tts_set_seed(static_cast<long long>(seed));
}

JNIEXPORT void JNICALL BC(nativeTtsSetTopK)(JNIEnv*, jobject, jint top_k) {
    yoai_tts_set_top_k(top_k);
}

JNIEXPORT jint JNICALL BC(nativeTtsSetReference)(JNIEnv* env, jobject,
                                                 jstring wav_path, jstring transcript) {
    const std::string w = to_utf8(env, wav_path);
    const std::string t = to_utf8(env, transcript);
    return yoai_tts_set_reference(or_null(w), or_null(t));
}

JNIEXPORT jfloat JNICALL BC(nativeTtsReferenceSeconds)(JNIEnv*, jobject) {
    return yoai_tts_reference_seconds();
}

JNIEXPORT void JNICALL BC(nativeTtsSetStreaming)(JNIEnv*, jobject, jboolean on) {
    yoai_tts_set_streaming(on == JNI_TRUE ? 1 : 0);
}

JNIEXPORT void JNICALL BC(nativeTtsSetThreads)(JNIEnv*, jobject, jint threads) {
    yoai_tts_set_threads(threads);
}

// `sink` may be null, which is the measurement case: the timings come back and
// the samples are never copied across the bridge. Passing a sink is what lets
// the app play what it generated.
JNIEXPORT jstring JNICALL BC(nativeTtsSay)(JNIEnv* env, jobject, jstring text, jobject sink) {
    const std::string t = to_utf8(env, text);

    if (sink == nullptr) {
        return from_utf8(env, yoai_tts_say(t.c_str(), nullptr, nullptr));
    }

    jclass cls = env->GetObjectClass(sink);
    jmethodID m = env->GetMethodID(cls, "onAudio", "([FZ)Z");
    env->DeleteLocalRef(cls);
    if (m == nullptr) {
        // GetMethodID has already thrown NoSuchMethodError; let it reach Kotlin.
        return nullptr;
    }

    CallbackCtx ctx{env, sink, m};
    return from_utf8(env, yoai_tts_say(t.c_str(), audio_handler, &ctx));
}

JNIEXPORT jstring JNICALL BC(nativeTtsLastError)(JNIEnv* env, jobject) {
    return from_utf8(env, yoai_tts_last_error());
}

//------------------------------------------------------------------------------
// On-device LLM
//------------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL BC(nativeLocalAvailable)(JNIEnv*, jobject) {
    return yoai_local_available() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL BC(nativeLocalSet)(JNIEnv* env, jobject, jstring id,
                                          jstring model_path, jstring options_json) {
    const std::string i = to_utf8(env, id);
    const std::string p = to_utf8(env, model_path);
    const std::string o = to_utf8(env, options_json);
    return yoai_local_set(i.c_str(), p.c_str(), or_null(o));
}

JNIEXPORT jint JNICALL BC(nativeLocalLoad)(JNIEnv* env, jobject, jstring id) {
    const std::string i = to_utf8(env, id);
    return yoai_local_load(i.c_str());
}

JNIEXPORT jint JNICALL BC(nativeLocalSetThreads)(JNIEnv* env, jobject,
                                                 jstring id, jint threads) {
    const std::string i = to_utf8(env, id);
    return yoai_local_set_threads(i.c_str(), threads);
}

JNIEXPORT jint JNICALL BC(nativeChat)(JNIEnv* env, jobject, jstring provider_id,
                                      jstring messages_json, jstring system_prompt,
                                      jstring grammar, jobject sink) {
    const std::string p = to_utf8(env, provider_id);
    const std::string m = to_utf8(env, messages_json);
    const std::string s = to_utf8(env, system_prompt);
    const std::string g = to_utf8(env, grammar);

    if (sink == nullptr) { return 1; }    // the handler is required by the C ABI

    jclass cls = env->GetObjectClass(sink);
    jmethodID method =
        env->GetMethodID(cls, "onEvent", "(Ljava/lang/String;Ljava/lang/String;)Z");
    env->DeleteLocalRef(cls);
    if (method == nullptr) { return 1; }

    CallbackCtx ctx{env, sink, method};
    return yoai_chat(p.c_str(), m.c_str(), or_null(s), or_null(g), chat_handler, &ctx);
}

}  // extern "C"
