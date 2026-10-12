package com.yoai.bodyclock

import com.yoai.bodyclock.ui.YoaiBenchTheme
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel

/**
 * One screen, one column, one measurement at a time.
 *
 * Not an app -- a bench. Every panel prints the engine's own JSON under the one
 * derived number worth reading at a glance, so nothing on screen is a figure
 * whose origin has to be taken on trust.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            YoaiBenchTheme {
                Surface(Modifier.fillMaxSize()) { MeasureScreen() }
            }
        }
    }
}

@Composable
private fun MeasureScreen(vm: MeasureViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()

    Scaffold { insets ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(insets)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Header(state)
            DevicePanel(state)
            BandwidthPanel(state, vm)
            TtsPanel(state, vm)
            AsrPanel(state, vm)
            LlmPanel(state, vm)
            FilesPanel(state, vm)
            LogPanel(state)
            Spacer(Modifier.height(24.dp))
        }
    }
}

//------------------------------------------------------------------------------

@Composable
private fun Header(state: MeasureViewModel.UiState) {
    Column {
        Text("YoAI — bench", style = MaterialTheme.typography.headlineSmall)
        Text(
            if (state.libLoaded) "core ${state.coreVersion}"
            else "libyoai.so did not load — wrong ABI, or the native lane was never built",
            style = MaterialTheme.typography.bodySmall,
        )
        if (state.busy != null) {
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.height(16.dp))
                Spacer(Modifier.fillMaxWidth(0.03f))
                Text("running ${state.busy}…", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable
private fun DevicePanel(state: MeasureViewModel.UiState) = Panel("Device") {
    val d = state.device
    Mono(
        buildString {
            appendLine("soc       ${d.soc}")
            appendLine("board     ${d.board}")
            appendLine("os        ${d.androidRelease}")
            appendLine("abis      ${d.abis}")
            appendLine("cores     ${d.cores}${if (d.clusters.isNotEmpty()) "  (${d.clusters})" else ""}")
            appendLine("features  ${d.relevantFeatures.joinToString(" ").ifEmpty { "(none read)" }}")
            append("lanes     ")
            append(
                listOf(
                    "asr" to state.asrAvailable,
                    "tts" to state.ttsAvailable,
                    "llm" to state.llmAvailable,
                ).joinToString("  ") { (name, on) -> "$name=${if (on) "on" else "off"}" }
            )
        }
    )
    Text(
        "A lane reads off when its SDK was not built. See examples/android/build.py.",
        style = MaterialTheme.typography.bodySmall,
    )
}

@Composable
private fun BandwidthPanel(state: MeasureViewModel.UiState, vm: MeasureViewModel) =
    Panel("Memory bandwidth") {
        Text(
            "The only reading here that is about the chip rather than about an " +
                "engine running on it, and the only one that needs no model. " +
                "A Kirin 9020 reads 24.4 GB/s.",
            style = MaterialTheme.typography.bodySmall,
        )
        Actions {
            Button(onClick = { vm.runBandwidthSweep() }, enabled = state.busy == null) {
                Text("Sweep 1…${state.device.cores} threads")
            }
        }
        if (state.bandwidth.isNotEmpty()) {
            Mono(
                buildString {
                    appendLine("threads   GB/s")
                    state.bandwidth.forEach {
                        appendLine("%7d   %.1f".format(it.threads, it.gbPerSecond))
                    }
                }.trimEnd()
            )
        }
    }

@Composable
private fun TtsPanel(state: MeasureViewModel.UiState, vm: MeasureViewModel) =
    Panel("Speech out") {
        var talker by rememberSaveable { mutableStateOf("talker.gguf") }
        var codec by rememberSaveable { mutableStateOf("codec.gguf") }
        var text by rememberSaveable { mutableStateOf("先把灯调暗一点吧。") }
        var threads by rememberSaveable { mutableStateOf("4") }
        var seed by rememberSaveable { mutableStateOf("42") }

        Text(
            "File names inside the model directory, which the Models panel below lists. " +
                "The three checkpoint kinds take different parameters -- the type the " +
                "engine read is printed with the result.",
            style = MaterialTheme.typography.bodySmall,
        )
        Field(talker, { talker = it }, "talker gguf")
        Field(codec, { codec = it }, "codec gguf")
        Field(text, { text = it }, "text")
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Field(threads, { threads = it }, "threads", Modifier.fillMaxWidth(0.5f))
            Field(seed, { seed = it }, "seed")
        }
        Actions {
            Button(
                onClick = {
                    vm.runTts(talker, codec, text, threads.toIntOrNull() ?: 4, seed.toLongOrNull() ?: 42L)
                },
                enabled = state.busy == null && state.ttsAvailable,
            ) { Text("Say") }
            OutlinedButton(onClick = vm::unloadTts, enabled = state.busy == null) { Text("Unload") }
        }
        Result(state.ttsHeadline, state.ttsJson)
    }

@Composable
private fun AsrPanel(state: MeasureViewModel.UiState, vm: MeasureViewModel) =
    Panel("Recognition") {
        var model by rememberSaveable { mutableStateOf("sensevoice.gguf") }
        var wav by rememberSaveable { mutableStateOf("clip.wav") }
        var threads by rememberSaveable { mutableStateOf("4") }

        Field(model, { model = it }, "model gguf")
        Field(wav, { wav = it }, "wav (16-bit PCM)")
        Field(threads, { threads = it }, "threads")
        Actions {
            Button(
                onClick = { vm.runAsr(model, wav, threads.toIntOrNull() ?: 4) },
                enabled = state.busy == null && state.asrAvailable,
            ) { Text("Analyze") }
            OutlinedButton(onClick = vm::unloadAsr, enabled = state.busy == null) { Text("Unload") }
        }
        Result(state.asrHeadline, state.asrJson)
    }

@Composable
private fun LlmPanel(state: MeasureViewModel.UiState, vm: MeasureViewModel) =
    Panel("On-device LLM") {
        var model by rememberSaveable { mutableStateOf("chat.gguf") }
        var prompt by rememberSaveable { mutableStateOf("用一句话说说今天适合做什么。") }
        var threads by rememberSaveable { mutableStateOf("4") }

        Field(model, { model = it }, "model gguf")
        Field(prompt, { prompt = it }, "prompt")
        Field(threads, { threads = it }, "threads")
        Actions {
            Button(
                onClick = { vm.runLlm(model, prompt, threads.toIntOrNull() ?: 4) },
                enabled = state.busy == null && state.llmAvailable,
            ) { Text("One turn") }
        }
        Result(state.llmHeadline, state.llmJson)
    }

@Composable
private fun FilesPanel(state: MeasureViewModel.UiState, vm: MeasureViewModel) =
    Panel("Models") {
        Text(
            "Pushed by hand, and needing no permission:\n" +
                "adb push <file> ${state.modelDir}/",
            style = MaterialTheme.typography.bodySmall,
        )
        Actions {
            OutlinedButton(onClick = vm::refreshFiles) { Text("Refresh") }
        }
        Mono(state.files.joinToString("\n").ifEmpty { "(empty)" })
    }

@Composable
private fun LogPanel(state: MeasureViewModel.UiState) = Panel("Log") {
    Mono(state.log.takeLast(30).joinToString("\n").ifEmpty { "(nothing yet)" })
}

//------------------------------------------------------------------------------
// Small shared pieces
//------------------------------------------------------------------------------

@Composable
private fun Panel(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            HorizontalDivider()
            content()
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Actions(content: @Composable () -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) { content() }
}

@Composable
private fun Field(
    value: String,
    onChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        modifier = modifier.fillMaxWidth(),
        textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
    )
}

/** The engine's own object under the one figure worth reading at a glance. */
@Composable
private fun Result(headline: String, json: String) {
    if (headline.isEmpty() && json.isEmpty()) return
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (headline.isNotEmpty()) {
            Text(headline, style = MaterialTheme.typography.titleSmall)
        }
        if (json.isNotEmpty()) Mono(json)
    }
}

@Composable
private fun Mono(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
    )
}
