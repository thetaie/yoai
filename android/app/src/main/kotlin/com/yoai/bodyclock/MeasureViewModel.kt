package com.yoai.bodyclock

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject
import java.io.File

/**
 * Every measurement this app can take, and nothing else.
 *
 * The rule the whole screen follows: **it reports what the core returned and
 * does not interpret it.** Each run prints the engine's own JSON alongside the
 * one derived figure worth reading at a glance, so a number on the screen can
 * always be traced to the object it came from. design/voice.md is full of
 * conclusions drawn from figures that turned out to be something else; the
 * cheapest defence is not hiding the original.
 *
 * Models are read from [modelDir], which needs no permission and is where
 * `adb push` puts them. Nothing here downloads or manages a model.
 */
class MeasureViewModel(app: Application) : AndroidViewModel(app) {

    data class BandwidthRow(val threads: Int, val gbPerSecond: Double, val json: String)

    data class UiState(
        val device: DeviceInfo = DeviceInfo.read(),
        val libLoaded: Boolean = Yoai.loaded,
        val coreVersion: String = "",
        val asrAvailable: Boolean = false,
        val ttsAvailable: Boolean = false,
        val llmAvailable: Boolean = false,
        val modelDir: String = "",
        val files: List<String> = emptyList(),
        /** What is running, for the progress line. Null when idle. */
        val busy: String? = null,
        val bandwidth: List<BandwidthRow> = emptyList(),
        val ttsHeadline: String = "",
        val ttsJson: String = "",
        val asrHeadline: String = "",
        val asrJson: String = "",
        val llmHeadline: String = "",
        val llmJson: String = "",
        val log: List<String> = emptyList(),
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** The one place models are looked for. Shown in the UI so it is pasteable. */
    private val modelDir: File =
        app.getExternalFilesDir(null) ?: app.filesDir

    private var running: Job? = null

    init {
        modelDir.mkdirs()
        if (Yoai.loaded) {
            Yoai.nativeSetLogLevel("info")
        }
        _state.update {
            it.copy(
                coreVersion = if (Yoai.loaded) Yoai.nativeVersion().orEmpty() else "",
                asrAvailable = Yoai.loaded && Yoai.nativeVoiceAvailable(),
                ttsAvailable = Yoai.loaded && Yoai.nativeTtsAvailable(),
                llmAvailable = Yoai.loaded && Yoai.nativeLocalAvailable(),
                modelDir = modelDir.absolutePath,
            )
        }
        refreshFiles()
    }

    fun refreshFiles() {
        val names = modelDir.listFiles()
            ?.sortedBy { it.name }
            ?.map { "${it.name}  ${humanBytes(it.length())}" }
            .orEmpty()
        _state.update { it.copy(files = names) }
    }

    fun cancel() {
        running?.cancel()
        running = null
        _state.update { it.copy(busy = null) }
        log("cancelled")
    }

    //--------------------------------------------------------------------------
    // Memory bandwidth
    //--------------------------------------------------------------------------

    /**
     * Sweeps one thread at a time up to the core count.
     *
     * The reason this is the first panel and needs no model: it is the only
     * measurement here that is about the chip rather than about an engine
     * running on it. design/voice.md reads 24.4 GB/s on a Kirin 9020 and uses
     * it to bound decode, so the figure has a job beyond being interesting.
     */
    fun runBandwidthSweep(mib: Int = 256, passes: Int = 4) = run("memory bandwidth") {
        val rows = mutableListOf<BandwidthRow>()
        _state.update { it.copy(bandwidth = emptyList()) }
        for (threads in 1..state.value.device.cores) {
            val json = Yoai.nativeMemoryBandwidth(threads, mib, passes)
            if (json == null) {
                log("bandwidth: $threads threads returned nothing; see logcat")
                continue
            }
            val gb = runCatching { JSONObject(json).getDouble("gbPerSecond") }.getOrDefault(0.0)
            rows += BandwidthRow(threads, gb, json)
            _state.update { it.copy(bandwidth = rows.toList()) }
        }
        val best = rows.maxByOrNull { it.gbPerSecond }
        if (best != null) {
            log("bandwidth: peak %.1f GB/s at %d threads".format(best.gbPerSecond, best.threads))
        }
    }

    //--------------------------------------------------------------------------
    // Speech output
    //--------------------------------------------------------------------------

    /**
     * Loads a talker and its codec, then says one line and reports the cost.
     *
     * `sink` is left null: this is a stopwatch, and copying a megabyte of
     * samples across the bridge to discard them would be part of what it
     * measured. Playback is a separate concern and not in this cut.
     */
    fun runTts(
        talker: String,
        codec: String,
        text: String,
        threads: Int,
        seed: Long,
    ) = run("speech out") {
        val talkerFile = modelDir.resolve(talker)
        val codecFile = modelDir.resolve(codec)
        if (!talkerFile.isFile || !codecFile.isFile) {
            log("tts: need both files under $modelDir")
            return@run
        }

        if (!Yoai.nativeTtsLoaded()) {
            Yoai.nativeTtsSetThreads(threads)   // takes effect on the next load
            val began = System.nanoTime()
            val rc = Yoai.nativeTtsLoad(talkerFile.absolutePath, codecFile.absolutePath)
            if (rc != 0) {
                log("tts: load failed -- ${Yoai.nativeTtsLastError().orEmpty()}")
                return@run
            }
            log("tts: loaded in %.1f s, type=%s".format(
                (System.nanoTime() - began) / 1e9, Yoai.nativeTtsModelType().orEmpty()))
        }

        // Pinned, so two runs of one configuration are the same utterance and a
        // difference between them is the device rather than the draw.
        Yoai.nativeTtsSetSeed(seed)

        val json = Yoai.nativeTtsSay(text, null)
        if (json == null) {
            log("tts: ${Yoai.nativeTtsLastError().orEmpty()}")
            return@run
        }
        val o = runCatching { JSONObject(json) }.getOrNull()
        val headline = o?.let {
            "RTF %.2f  ·  %.2f s of audio in %.2f s".format(
                it.optDouble("rtf"), it.optDouble("audioSeconds"),
                it.optDouble("totalMs") / 1000.0)
        }.orEmpty()
        _state.update { it.copy(ttsHeadline = headline, ttsJson = pretty(json)) }
    }

    fun unloadTts() = run("unload") { Yoai.nativeTtsUnload() }

    //--------------------------------------------------------------------------
    // Recognition
    //--------------------------------------------------------------------------

    /** Transcribes a WAV from [modelDir] and reports RTF against its duration. */
    fun runAsr(model: String, wav: String, threads: Int) = run("recognition") {
        val modelFile = modelDir.resolve(model)
        val wavFile = modelDir.resolve(wav)
        if (!modelFile.isFile || !wavFile.isFile) {
            log("asr: need both files under $modelDir")
            return@run
        }

        if (!Yoai.nativeVoiceLoaded()) {
            val began = System.nanoTime()
            if (Yoai.nativeVoiceLoad(modelFile.absolutePath) != 0) {
                log("asr: load failed; see logcat")
                return@run
            }
            log("asr: loaded in %.1f s".format((System.nanoTime() - began) / 1e9))
        }
        Yoai.nativeVoiceSetThreads(threads)

        val clip = runCatching { Wav.read(wavFile) }.getOrElse {
            log("asr: ${it.message}")
            return@run
        }

        val began = System.nanoTime()
        val json = Yoai.nativeVoiceAnalyze(clip.pcm, clip.sampleRate)
        val elapsed = (System.nanoTime() - began) / 1e9

        if (json == null) {
            log("asr: analyze returned nothing; see logcat")
            return@run
        }
        // RTF is timed here rather than taken from the core, because the core
        // does not report one for this lane -- compute over audio duration, the
        // same definition design/voice.md uses everywhere.
        val headline = "RTF %.3f  ·  %.2f s of audio at %d Hz in %.2f s, %d threads".format(
            elapsed / clip.seconds, clip.seconds, clip.sampleRate, elapsed, threads)
        _state.update { it.copy(asrHeadline = headline, asrJson = pretty(json)) }
    }

    fun unloadAsr() = run("unload") { Yoai.nativeVoiceUnload() }

    //--------------------------------------------------------------------------
    // On-device LLM
    //--------------------------------------------------------------------------

    /**
     * One turn, timed into its two halves.
     *
     * Time to the first `reply` event is prefill plus one token and is read as
     * prefill; everything after it is decode, divided by the token count the
     * `usage` event reports. That split is the one design/voice.md keeps
     * returning to -- prefill is compute-bound and decode is memory-bound, and
     * they scale differently between a desktop and a phone.
     */
    fun runLlm(model: String, prompt: String, threads: Int) = run("on-device LLM") {
        val modelFile = modelDir.resolve(model)
        if (!modelFile.isFile) {
            log("llm: no $model under $modelDir")
            return@run
        }

        val id = "measure"
        if (Yoai.nativeLocalSet(id, modelFile.absolutePath, "") != 0) {
            log("llm: could not register the model; see logcat")
            return@run
        }
        Yoai.nativeLocalSetThreads(id, threads)

        var began = System.nanoTime()
        if (Yoai.nativeLocalLoad(id) != 0) {
            log("llm: load failed; see logcat")
            return@run
        }
        log("llm: loaded in %.1f s".format((System.nanoTime() - began) / 1e9))

        val reply = StringBuilder()
        var usage = ""
        var firstReplyNs = 0L
        began = System.nanoTime()

        val messages = """[{"role":"user","content":${JSONObject.quote(prompt)}}]"""
        val rc = Yoai.nativeChat(id, messages, "", "") { type, content ->
            when (type) {
                "reply" -> {
                    if (firstReplyNs == 0L) firstReplyNs = System.nanoTime()
                    reply.append(content)
                }
                "usage" -> usage = content
                "error" -> log("llm: $content")
            }
            true
        }
        val totalS = (System.nanoTime() - began) / 1e9
        if (rc != 0) {
            log("llm: turn failed ($rc)")
            return@run
        }

        val prefillS = if (firstReplyNs == 0L) totalS else (firstReplyNs - began) / 1e9
        val u = runCatching { JSONObject(usage) }.getOrNull()
        val inTok = u?.optInt("inputTokens") ?: 0
        val outTok = u?.optInt("outputTokens") ?: 0
        val decodeS = (totalS - prefillS).coerceAtLeast(1e-9)

        val headline = buildString {
            append("prefill %d tok in %.2f s".format(inTok, prefillS))
            if (inTok > 0) append(" (%.1f tok/s)".format(inTok / prefillS))
            append("  ·  decode %d tok in %.2f s".format(outTok, decodeS))
            if (outTok > 1) append(" (%.1f tok/s)".format((outTok - 1) / decodeS))
        }
        _state.update {
            it.copy(
                llmHeadline = headline,
                llmJson = buildString {
                    appendLine("usage: ${usage.ifBlank { "(none)" }}")
                    appendLine()
                    append(reply.toString().trim())
                },
            )
        }
    }

    //--------------------------------------------------------------------------

    /**
     * One measurement at a time, on a background thread.
     *
     * Serialised deliberately: two of these running together would each be
     * measuring the other's thermal and memory load, and the throttle step
     * design/voice.md documents makes that not a small effect.
     */
    private fun run(what: String, block: suspend () -> Unit) {
        if (running?.isActive == true) {
            log("busy with ${state.value.busy}")
            return
        }
        if (!Yoai.loaded) {
            log("libyoai.so did not load; nothing can run")
            return
        }
        running = viewModelScope.launch {
            _state.update { it.copy(busy = what) }
            try {
                withContext(Dispatchers.Default) { block() }
            } catch (t: Throwable) {
                log("$what: ${t::class.simpleName}: ${t.message}")
            } finally {
                _state.update { it.copy(busy = null) }
                refreshFiles()
            }
        }
    }

    private fun log(line: String) {
        _state.update { it.copy(log = (it.log + line).takeLast(200)) }
    }

    private fun pretty(json: String): String =
        runCatching { JSONObject(json).toString(2) }.getOrDefault(json)

    private fun humanBytes(n: Long): String = when {
        n >= 1L shl 30 -> "%.2f GB".format(n / (1L shl 30).toDouble())
        n >= 1L shl 20 -> "%.1f MB".format(n / (1L shl 20).toDouble())
        else -> "%d KB".format(n / 1024)
    }
}
