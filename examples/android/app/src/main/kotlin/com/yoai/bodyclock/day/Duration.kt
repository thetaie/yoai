package com.yoai.bodyclock.day

import com.yoai.bodyclock.R
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource

/**
 * Durations, in words, in the interface language.
 *
 * Two shapes on purpose. A running timer counts seconds, because watching the
 * seconds move is how the header says "this is live"; anything already finished
 * drops them, because "1 小时 23 分 47 秒" for something that ended yesterday is
 * false precision about a number the person eyeballed anyway.
 *
 * **Whatever this returns is rendered LTR**, in every language. A duration is a
 * clock reading, and design/ui round 5f puts clocks and timers in the list that
 * does not mirror -- see [LtrText] at the use sites.
 */
@Composable
fun runningDuration(ms: Long): String {
    val total = (ms / 1000).coerceAtLeast(0)
    val h = total / 3600
    val m = (total % 3600) / 60
    val s = total % 60
    return if (h > 0) {
        stringResource(R.string.dur_hms, h, m, s)
    } else {
        stringResource(R.string.dur_ms, m, s)
    }
}

@Composable
fun settledDuration(ms: Long): String {
    val minutes = (ms / 60_000).coerceAtLeast(0)
    val h = minutes / 60
    val m = minutes % 60
    return if (h > 0) stringResource(R.string.dur_hm, h, m) else stringResource(R.string.dur_m, m)
}
