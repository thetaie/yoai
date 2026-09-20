package ai.bodyclock.app

import android.os.Build
import java.io.File

/**
 * What the chip says about itself.
 *
 * This is here because of one measured result in design/voice.md: a cross build
 * with no `-march` lands on baseline `armv8-a` and costs 2.9x prefill and 2.2x
 * decode, because CMake's feature probes compile *and run* a test binary and
 * cannot run one for the phone. So the feature set has to be asserted by the
 * build rather than detected, and **this screen is how the assertion gets
 * checked against the device it was made for.**
 *
 * Everything is read best-effort: a missing sysfs node is a blank, never a
 * crash. Nothing here is a measurement -- it is the label on the thing being
 * measured.
 */
data class DeviceInfo(
    val soc: String,
    val board: String,
    val abis: String,
    val cores: Int,
    /** Max clock per core in MHz, in core order. Empty when sysfs is closed. */
    val coreMhz: List<Int>,
    /** The arm64 feature words from /proc/cpuinfo, deduplicated. */
    val features: List<String>,
    val androidRelease: String,
) {
    /**
     * Cores grouped by identical max clock, in descending order: "2x3.8 + 6x3.5".
     * The shape a thread-count sweep is read against -- design/voice.md's
     * "four is where it stops paying" is a statement about a cluster boundary.
     */
    val clusters: String
        get() {
            if (coreMhz.isEmpty()) return ""
            return coreMhz.groupingBy { it }.eachCount()
                .entries.sortedByDescending { it.key }
                .joinToString(" + ") { (mhz, n) -> "%dx%.2fGHz".format(n, mhz / 1000.0) }
        }

    /**
     * The features that change code generation for this project's kernels, in
     * the order they matter. `i8mm` and `bf16` are what the HarmonyOS lane's
     * `-march=armv8.6-a+i8mm+bf16+dotprod+fp16` asks for; `sve2` is what an
     * armv9 core adds and nothing here has been built against yet.
     */
    val relevantFeatures: List<String>
        get() = listOf("asimd", "asimdhp", "asimddp", "i8mm", "bf16", "sve", "sve2", "fphp")
            .filter { it in features }

    companion object {
        fun read(): DeviceInfo {
            val cpuinfo = runCatching { File("/proc/cpuinfo").readText() }.getOrDefault("")

            // Every core prints its own Features line and they are normally
            // identical; a big.LITTLE part where they are not would show the
            // union, which is the honest answer to "what can this chip do".
            val features = Regex("""^Features\s*:\s*(.+)$""", RegexOption.MULTILINE)
                .findAll(cpuinfo)
                .flatMap { it.groupValues[1].trim().split(Regex("\\s+")) }
                .filter { it.isNotEmpty() }
                .distinct()
                .toList()

            val cores = Runtime.getRuntime().availableProcessors()
            val coreMhz = (0 until cores).map { i ->
                runCatching {
                    File("/sys/devices/system/cpu/cpu$i/cpufreq/cpuinfo_max_freq")
                        .readText().trim().toInt() / 1000
                }.getOrDefault(0)
            }.takeIf { list -> list.any { it > 0 } } ?: emptyList()

            // SOC_MODEL is API 31+ and is the only place a Qualcomm part number
            // appears without parsing a marketing string. HARDWARE is the
            // fallback and on Qualcomm is usually "qcom", which is why it is
            // not the first choice.
            val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                listOf(Build.SOC_MANUFACTURER, Build.SOC_MODEL)
                    .filter { it.isNotBlank() && it != Build.UNKNOWN }
                    .joinToString(" ")
                    .ifBlank { Build.HARDWARE }
            } else {
                Build.HARDWARE
            }

            return DeviceInfo(
                soc = soc,
                board = "${Build.MANUFACTURER} ${Build.MODEL} (${Build.BOARD})",
                abis = Build.SUPPORTED_ABIS.joinToString(", "),
                cores = cores,
                coreMhz = coreMhz,
                features = features,
                androidRelease = "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
            )
        }
    }
}
