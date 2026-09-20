package ai.bodyclock.app

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Just enough RIFF to read a recording off the device.
 *
 * Deliberately narrow: 16-bit PCM, and the channel count is folded to mono by
 * averaging. That is what the recognition lane takes and what every clip in
 * design/voice.md is. A file this cannot read is a wrong file rather than a
 * missing feature, so it says which field disagreed instead of guessing.
 */
object Wav {

    data class Clip(val pcm: ShortArray, val sampleRate: Int) {
        val seconds: Double get() = pcm.size.toDouble() / sampleRate

        // Generated because the class holds an array; identity is the array's
        // contents, not its reference.
        override fun equals(other: Any?): Boolean =
            this === other ||
                (other is Clip && sampleRate == other.sampleRate && pcm.contentEquals(other.pcm))

        override fun hashCode(): Int = 31 * pcm.contentHashCode() + sampleRate
    }

    /** @throws IllegalArgumentException with the field that disagreed. */
    fun read(file: File): Clip {
        val bytes = file.readBytes()
        require(bytes.size >= 44) { "${file.name}: ${bytes.size} bytes, too short for a WAV header" }

        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        require(tag(bytes, 0) == "RIFF" && tag(bytes, 8) == "WAVE") {
            "${file.name}: not a RIFF/WAVE file"
        }

        // Walk the chunks rather than assuming fmt at 12 and data at 36: a file
        // written by a phone recorder often carries a LIST chunk in between.
        var pos = 12
        var channels = 0
        var sampleRate = 0
        var bits = 0
        var dataOffset = -1
        var dataSize = 0

        while (pos + 8 <= bytes.size) {
            val id = tag(bytes, pos)
            val size = buf.getInt(pos + 4)
            val body = pos + 8
            if (size < 0 || body + size > bytes.size) {
                // A truncated final chunk: take what is actually there. Files
                // copied off a device mid-write do this.
                if (id == "data" && body < bytes.size) {
                    dataOffset = body
                    dataSize = bytes.size - body
                }
                break
            }
            when (id) {
                "fmt " -> {
                    channels = buf.getShort(body + 2).toInt()
                    sampleRate = buf.getInt(body + 4)
                    bits = buf.getShort(body + 14).toInt()
                }
                "data" -> {
                    dataOffset = body
                    dataSize = size
                }
            }
            // Chunks are word-aligned; an odd size is followed by a pad byte.
            pos = body + size + (size and 1)
        }

        require(bits == 16) { "${file.name}: ${bits}-bit samples, expected 16" }
        require(channels in 1..8) { "${file.name}: $channels channels" }
        require(dataOffset >= 0) { "${file.name}: no data chunk" }

        val frames = dataSize / 2 / channels
        val out = ShortArray(frames)
        if (channels == 1) {
            buf.position(dataOffset)
            buf.asShortBuffer().get(out, 0, frames)
        } else {
            for (f in 0 until frames) {
                var sum = 0
                for (c in 0 until channels) {
                    sum += buf.getShort(dataOffset + (f * channels + c) * 2)
                }
                out[f] = (sum / channels).toShort()
            }
        }
        return Clip(out, sampleRate)
    }

    private fun tag(b: ByteArray, at: Int): String =
        String(b, at, 4, Charsets.US_ASCII)
}
