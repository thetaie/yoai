package com.yoai.bodyclock.day

import com.yoai.bodyclock.Yoai
import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/**
 * The day log, as Kotlin sees it.
 *
 * A thin reader over the C ABI in `src/yoai.h`: it parses the JSON and
 * nothing else. **No rule about what a day is lives here** -- not "a switch
 * closes the open entry", not "a gap is the absence of a row", not how a local
 * day is numbered. All of that is in `src/store/timeline.hpp`, once, because it
 * is the same on every platform and was not going to survive being written
 * twice. Anything on this side that starts to look like a rule belongs down
 * there instead.
 *
 * Every call is fast -- one indexed query -- so none of them is an ANR risk.
 * They are disk reads all the same, and [HomeViewModel] runs them on an IO
 * dispatcher: cheap is not the same as belonging on the thread that draws.
 */
object Timeline {

    /** A refusal from the core, carrying the core's own sentence. */
    class Refused(message: String) : Exception(message)

    /**
     * Open the log under the app's internal storage.
     *
     * `filesDir`, not `getExternalFilesDir` -- the bench pushes models to the
     * external directory because `adb push` needs no permission to write there,
     * and that is exactly the wrong property for the day log. This is the
     * user's own record and it belongs where nothing else can read it.
     *
     * Safe to call more than once; the second call is a no-op.
     */
    fun open(context: Context): Result<Unit> {
        if (!Yoai.loaded) {
            return Result.failure(Refused("libyoai.so did not load"))
        }
        val dir = context.filesDir
        if (!dir.exists() && !dir.mkdirs()) {
            return Result.failure(Refused("cannot create ${dir.path}"))
        }
        return wrote(Yoai.nativeTimelineOpen(dir.path))
    }

    val isOpen: Boolean get() = Yoai.loaded && Yoai.nativeTimelineOpened()

    // --- Writes -------------------------------------------------------------

    /**
     * Make [stateId] the current state. This is what tapping a cell does: a
     * switch, not an event. The previous state ends at the same instant and its
     * duration falls out of that, so the day needs no back-filling.
     */
    fun switchTo(stateId: String): Result<Unit> = wrote(Yoai.nativeTimelineSwitchTo(stateId))

    /** Close the current state and open nothing. What follows is a gap, and stays one. */
    fun end(): Result<Unit> = wrote(Yoai.nativeTimelineEnd())

    /**
     * End as of an instant that has already passed -- the way out of a state
     * nobody switched away from. Refused if [at] precedes the start or is in
     * the future, and the refusal is worth showing rather than swallowing.
     */
    fun endAt(at: Long): Result<Unit> = wrote(Yoai.nativeTimelineEndAt(at))

    /** Reopen what was closed most recently, absorbing the gap since. Idle only. */
    fun undo(): Result<Unit> = wrote(Yoai.nativeTimelineUndo())

    /** Attach a note to the current state. Refused while idle. */
    fun addNote(text: String): Result<Unit> = wrote(Yoai.nativeTimelineNoteAdd(text))

    // --- Reads --------------------------------------------------------------

    /** What is running and what stopped last. Both null on a fresh install. */
    fun now(): Result<Now> = read(Yoai.nativeTimelineNow()).map { json ->
        val o = JSONObject(json)
        Now(current = o.entryOrNull("current"), lastEnded = o.entryOrNull("lastEnded"))
    }

    /**
     * Everything still going or finished within the last [hours], newest first.
     *
     * A rolling window, not a day, and the home screen is the one place that
     * wants it: "what have I been doing" has no reason to stop at midnight.
     * Everything that COMPARES uses [day] -- two windows overlap, so they
     * cannot be compared to each other and a calendar of them means nothing.
     */
    fun recent(hours: Int = 24): Result<List<Entry>> =
        read(Yoai.nativeTimelineRecent(hours)).map { it.toEntries() }

    /** Every entry that touches [localDay], oldest first. */
    fun day(localDay: Int): Result<List<Entry>> =
        read(Yoai.nativeTimelineDay(localDay)).map { it.toEntries() }

    /** Today by the device's clock. The one call that reads the current timezone. */
    fun today(): Int = Yoai.nativeTimelineToday()

    fun dayToIso(localDay: Int): String = Yoai.nativeTimelineDayToIso(localDay).orEmpty()

    /** -1 when [iso] does not parse. */
    fun isoToDay(iso: String): Int = Yoai.nativeTimelineIsoToDay(iso)

    // --- Marshalling --------------------------------------------------------

    private fun lastError(): String =
        Yoai.nativeTimelineLastError()?.takeIf { it.isNotEmpty() } ?: "the day log refused, without saying why"

    private fun wrote(rc: Int): Result<Unit> =
        if (rc == 0) Result.success(Unit) else Result.failure(Refused(lastError()))

    private fun read(json: String?): Result<String> =
        if (json != null) Result.success(json) else Result.failure(Refused(lastError()))

    private fun String.toEntries(): List<Entry> {
        val arr = JSONArray(this)
        return List(arr.length()) { arr.getJSONObject(it).toEntry() }
    }

    private fun JSONObject.entryOrNull(key: String): Entry? =
        if (isNull(key)) null else getJSONObject(key).toEntry()

    private fun JSONObject.toEntry(): Entry {
        val notes = getJSONArray("notes")
        return Entry(
            id = getLong("id"),
            stateId = getString("stateId"),
            open = getBoolean("open"),
            startedAt = getLong("startedAt"),
            startedTz = getInt("startedTz"),
            endedAt = getLong("endedAt"),
            endedTz = getInt("endedTz"),
            localDay = getInt("localDay"),
            localDayEnd = getInt("localDayEnd"),
            notes = List(notes.length()) { i ->
                val n = notes.getJSONObject(i)
                Note(id = n.getLong("id"), text = n.getString("text"), at = n.getLong("at"))
            },
        )
    }
}

/** One note hung off an entry. */
data class Note(val id: Long, val text: String, val at: Long)

/**
 * One stretch of time in one state.
 *
 * [startedTz] and [endedTz] are minutes east of UTC **at that endpoint**, and
 * they can differ from each other -- across a flight, or a daylight-saving
 * switch. Placing an entry on a 00-24 bar of local time means converting each
 * end with its own offset; one shared offset misplaces the other end by the
 * whole difference.
 *
 * [endedAt], [endedTz] and [localDayEnd] are 0 and meaningless while [open].
 */
data class Entry(
    val id: Long,
    val stateId: String,
    val open: Boolean,
    val startedAt: Long,
    val startedTz: Int,
    val endedAt: Long,
    val endedTz: Int,
    val localDay: Int,
    val localDayEnd: Int,
    val notes: List<Note>,
) {
    /**
     * How long this entry has covered, as of [nowMs].
     *
     * An open entry is still running, so its length depends on when you ask --
     * which is why this takes the instant rather than reading the clock itself.
     * A composable that ticks passes its own `now` and re-reads nothing. For one
     * that has already closed the argument is ignored; [closedMs] says that at
     * the call site instead of leaving a reader to work it out.
     */
    fun elapsedMs(nowMs: Long): Long = (if (open) nowMs else endedAt) - startedAt

    /** How long a finished entry ran. 0 while [open], where the question has no answer yet. */
    val closedMs: Long get() = if (open) 0L else endedAt - startedAt
}

/** What the home screen reads in one shot. */
data class Now(val current: Entry?, val lastEnded: Entry?)
