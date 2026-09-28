package com.yoai.bodyclock.day

import com.yoai.bodyclock.R
import com.yoai.bodyclock.ui.Palette
import androidx.annotation.StringRes
import androidx.compose.runtime.Immutable
import androidx.compose.ui.graphics.Color

/**
 * How the nine states are DRAWN. Not what they are.
 *
 * The ids are the core's -- they are seeded by `sql/sqlite/001_day_log.sql` and
 * are stable tokens that are **never shown**. Everything a person reads is a
 * string resource, so the names follow the interface language rather than the
 * data. That split is why this table exists at all: a name, an order and a color
 * are host decisions, and the id is the only thing the two sides share.
 *
 * **This list duplicates the seed, and that is a temporary arrangement.** Custom
 * states are on the design's list and will be rows in `states`, at which point
 * membership and order have to be read from the table instead and this becomes
 * a lookup keyed by id rather than the list itself. Until then the two agree by
 * being short, and they disagree loudly rather than quietly: an id here that is
 * not in the table gets the switch refused by the core with "unknown state", not
 * written as something odd.
 */
@Immutable
data class StateSpec(
    /** The core's id. Never displayed. */
    val id: String,
    @param:StringRes val name: Int,
    /** The second line on the cell -- examples, not a definition. */
    @param:StringRes val hint: Int,
)

val STATES: List<StateSpec> = listOf(
    StateSpec("work", R.string.state_work, R.string.state_work_hint),
    StateSpec("eat", R.string.state_eat, R.string.state_eat_hint),
    StateSpec("nap", R.string.state_nap, R.string.state_nap_hint),
    StateSpec("commute", R.string.state_commute, R.string.state_commute_hint),
    StateSpec("exercise", R.string.state_exercise, R.string.state_exercise_hint),
    StateSpec("read", R.string.state_read, R.string.state_read_hint),
    StateSpec("social", R.string.state_social, R.string.state_social_hint),
    StateSpec("rest", R.string.state_rest, R.string.state_rest_hint),
    StateSpec("childcare", R.string.state_childcare, R.string.state_childcare_hint),
)

/** The spec for an id from the core, or null for one this build does not draw. */
fun stateSpec(id: String): StateSpec? = STATES.firstOrNull { it.id == id }

/**
 * The color bar at the top of a state's cell.
 *
 * A bar, not an icon -- design/icons.md says so explicitly, because it is a
 * palette value rather than a glyph and substituting a mark for it loses the
 * one thing it carries.
 *
 * The ramp is four colors for nine states, so it cycles. `rest` is the
 * exception and takes a neutral grey instead: it is the one state that means
 * "nothing in particular was happening", and giving it an accent of its own put
 * it forward on a grid where it should recede.
 */
fun stateColor(id: String, palette: Palette): Color {
    val i = STATES.indexOfFirst { it.id == id }
    if (i < 0) return palette.textTertiary
    if (id == "rest") return palette.textDisabled
    return palette.stateRamp[i % palette.stateRamp.size]
}
