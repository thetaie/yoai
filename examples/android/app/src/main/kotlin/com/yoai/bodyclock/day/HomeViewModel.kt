package com.yoai.bodyclock.day

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The home screen's state, and the only thing that writes to the day log.
 *
 * Every read after a write, in the same call, rather than a local edit of the
 * state it just changed. The core decides what a switch did -- whether it closed
 * something, at what instant, whether it was refused for being a re-tap of the
 * state already running -- and guessing that here would give the screen a second
 * opinion about the day. One extra indexed query is a cheap price for there
 * being exactly one answer.
 *
 * Timeline calls go on [Dispatchers.IO]. They are fast enough not to risk an ANR
 * -- one indexed query each -- but they are still disk reads, and StrictMode is
 * right to say those do not belong on the thread that draws.
 */
class HomeViewModel(app: Application) : AndroidViewModel(app) {

    data class UiState(
        /** False until the log is open; the screen shows nothing rather than an empty day. */
        val ready: Boolean = false,
        /**
         * Why the log could not be opened. Fatal for this screen: without a
         * store there is no home screen, only a reason.
         */
        val fatal: String? = null,
        /**
         * The last refusal, for a transient message. Refusals are ordinary here
         * -- "an end in the future is refused" is something the UI should say
         * rather than swallow -- so this is not an error state, just a notice.
         */
        val notice: String? = null,
        val now: Now? = null,
        val recent: List<Entry> = emptyList(),
    ) {
        val current: Entry? get() = now?.current
        val lastEnded: Entry? get() = now?.lastEnded
    }

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    init {
        viewModelScope.launch {
            val opened = withContext(Dispatchers.IO) { Timeline.open(getApplication()) }
            opened
                .onSuccess {
                    _state.update { it.copy(ready = true) }
                    refresh()
                }
                .onFailure { e ->
                    _state.update { it.copy(fatal = e.message ?: "the day log did not open") }
                }
        }
    }

    fun switchTo(stateId: String) = write { Timeline.switchTo(stateId) }

    fun end() = write { Timeline.end() }

    fun undo() = write { Timeline.undo() }

    fun addNote(text: String) {
        if (text.isBlank()) return
        write { Timeline.addNote(text.trim()) }
    }

    fun dismissNotice() = _state.update { it.copy(notice = null) }

    fun refresh() {
        if (!_state.value.ready) return
        viewModelScope.launch { reload() }
    }

    private fun write(action: () -> Result<Unit>) {
        if (!_state.value.ready) return
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { action() }
            result.onFailure { e -> _state.update { it.copy(notice = e.message) } }
            reload()
        }
    }

    /**
     * Both reads in one hop to the IO thread, then one state update.
     *
     * Two updates would let the screen draw a current state from after the write
     * beside a recent list from before it, for a frame. Nobody would file that
     * bug and it would still be wrong.
     */
    private suspend fun reload() {
        val snapshot = withContext(Dispatchers.IO) {
            val now = Timeline.now().getOrNull()
            val recent = Timeline.recent(RECENT_HOURS).getOrDefault(emptyList())
            now to recent
        }
        _state.update { it.copy(now = snapshot.first, recent = snapshot.second) }
    }

    private companion object {
        /**
         * A rolling window, deliberately not "today".
         *
         * Opening the app at 00:30 under a day-shaped answer shows one entry
         * while the evening it belongs to has just been cut away by a boundary
         * that has nothing to do with what the person was doing. Everything that
         * COMPARES -- the day bar, the month calendar -- uses `Timeline.day`
         * instead, because two windows overlap and cannot be compared.
         */
        const val RECENT_HOURS = 24
    }
}
