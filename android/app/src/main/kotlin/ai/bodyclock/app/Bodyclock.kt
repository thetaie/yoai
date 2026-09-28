package ai.bodyclock.app

/**
 * The whole native surface, in one object.
 *
 * This is the Android counterpart of the HarmonyOS module's `index.d.ts`, and
 * it follows the same rule: nothing here interprets a result. Every `native*`
 * is a direct translation of an entry point in `src/yoai.h`, and the
 * meaning of each argument lives there rather than being restated here.
 *
 * **Everything that loads or generates BLOCKS**, for seconds to minutes, and
 * calling one on the main thread will ANR. The helpers that need it say so; the
 * view model runs all of them on a background dispatcher.
 *
 * A `String?` return is the C ABI's `const char*`: null means the call failed
 * and the reason is in logcat, and for the speech lane also in [nativeTtsLastError].
 */
object Bodyclock {

    /**
     * True when libbodyclock.so loaded. False means every call below would
     * throw, and the UI says so rather than crashing on the first tap -- a
     * device with the wrong ABI, or an APK built before the native lane was
     * configured, should be diagnosable from the screen.
     */
    val loaded: Boolean = runCatching { System.loadLibrary("bodyclock") }.isSuccess

    /** Streaming reply events. Return false to abort the turn. */
    fun interface ChatSink {
        fun onEvent(type: String, content: String): Boolean
    }

    /**
     * Generated audio, 24 kHz mono float. Return false to stop generating.
     *
     * Passing no sink at all is the measurement case: the samples are never
     * copied across the bridge, so a stopwatch does not also pay for a
     * megabyte it would discard.
     */
    fun interface AudioSink {
        fun onAudio(pcm: FloatArray, last: Boolean): Boolean
    }

    // --- Core ---------------------------------------------------------------

    external fun nativeVersion(): String?
    external fun nativeSetLogLevel(level: String)

    /** JSON: the read rate in GB/s and what it took to get it. Blocks. */
    external fun nativeMemoryBandwidth(threads: Int, mib: Int, passes: Int): String?

    // --- The day log --------------------------------------------------------
    //
    // The exception to the blocking rule above: one indexed SQLite query each,
    // so none of these is an ANR risk. They are still disk reads and still go
    // on an IO dispatcher -- see HomeViewModel -- because StrictMode is right
    // that a file read does not belong on the thread that draws.
    //
    // [ai.bodyclock.app.day.Timeline] wraps them; nothing else should call them
    // directly.
    //
    // A write returns 0 on success; a read returns JSON, or null on failure.
    // Either way the reason is in [nativeTimelineLastError].

    external fun nativeTimelineOpen(dir: String): Int
    external fun nativeTimelineOpened(): Boolean
    external fun nativeTimelineClose()

    external fun nativeTimelineSwitchTo(stateId: String): Int
    external fun nativeTimelineEnd(): Int
    external fun nativeTimelineEndAt(at: Long): Int
    external fun nativeTimelineUndo(): Int
    external fun nativeTimelineNoteAdd(text: String): Int

    external fun nativeTimelineNow(): String?
    external fun nativeTimelineRecent(hours: Int): String?
    external fun nativeTimelineDay(localDay: Int): String?

    external fun nativeTimelineToday(): Int
    external fun nativeTimelineDayToIso(localDay: Int): String?
    external fun nativeTimelineIsoToDay(iso: String): Int

    external fun nativeTimelineLastError(): String?

    // --- Recognition --------------------------------------------------------

    external fun nativeVoiceAvailable(): Boolean

    /** Blocks; the weights are hundreds of megabytes. 0 on success. */
    external fun nativeVoiceLoad(modelPath: String): Int
    external fun nativeVoiceLoaded(): Boolean
    external fun nativeVoiceUnload()
    external fun nativeVoiceSetThreads(threads: Int): Int
    external fun nativeVoiceSetItn(on: Boolean): Int

    /** Blocks for the length of the analysis. JSON: transcript plus prosody. */
    external fun nativeVoiceAnalyze(pcm: ShortArray, sampleRate: Int): String?

    // --- Speech output ------------------------------------------------------

    external fun nativeTtsAvailable(): Boolean

    /** Blocks. 0 on success; the reason for a failure is in [nativeTtsLastError]. */
    external fun nativeTtsLoad(talkerPath: String, codecPath: String): Int
    external fun nativeTtsLoaded(): Boolean
    external fun nativeTtsUnload()
    external fun nativeTtsModelType(): String?
    external fun nativeTtsSpeakers(): String?
    external fun nativeTtsSetSpeaker(name: String)
    external fun nativeTtsSetVoice(instruct: String)
    external fun nativeTtsSetSeed(seed: Long)
    external fun nativeTtsSetTopK(topK: Int)
    external fun nativeTtsSetReference(wavPath: String, transcript: String): Int
    external fun nativeTtsReferenceSeconds(): Float
    external fun nativeTtsSetStreaming(on: Boolean)

    /** Takes effect on the NEXT load. */
    external fun nativeTtsSetThreads(threads: Int)

    /**
     * Blocks for the length of the synthesis. JSON of per-stage timings and RTF.
     *
     * @param sink null to measure without carrying the samples back.
     */
    external fun nativeTtsSay(text: String, sink: AudioSink?): String?
    external fun nativeTtsLastError(): String?

    // --- On-device LLM ------------------------------------------------------

    external fun nativeLocalAvailable(): Boolean
    external fun nativeLocalSet(id: String, modelPath: String, optionsJson: String): Int

    /** Blocks. */
    external fun nativeLocalLoad(id: String): Int
    external fun nativeLocalSetThreads(id: String, threads: Int): Int

    /** Blocks until the turn ends. 0 on success. */
    external fun nativeChat(
        providerId: String,
        messagesJson: String,
        systemPrompt: String,
        grammar: String,
        sink: ChatSink,
    ): Int
}
