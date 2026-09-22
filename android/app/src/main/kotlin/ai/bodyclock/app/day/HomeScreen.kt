package ai.bodyclock.app.day

import ai.bodyclock.app.R
import ai.bodyclock.app.ui.LocalPalette
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.delay
import java.text.DateFormat
import java.util.Date

/**
 * Home: what state you are in, how long you have been in it, and nine cells to
 * change it.
 *
 * The one thing to keep straight about this screen is that **a cell is not a
 * log button**. Tapping one switches the current state: the previous entry ends
 * at that same instant and its duration is written for you. That is what makes
 * a day continuous without anybody back-filling it, and it is why there is no
 * "add entry" affordance anywhere on this screen.
 *
 * Ending is the separate gesture, and it leaves a hole on purpose. Nothing here
 * fills a gap in later, and the idle screen says so out loud rather than
 * offering to.
 *
 * **There is no bottom navigation bar**, here or on any other screen. The
 * floating button at the lower right is the only way off this screen.
 */
@Composable
fun HomeScreen(vm: HomeViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()
    val palette = LocalPalette.current
    val snackbars = remember { SnackbarHostState() }

    // A refusal is ordinary -- "an end in the future is refused" is something
    // the person should see -- so it is shown and dismissed, never logged away.
    LaunchedEffect(state.notice) {
        state.notice?.let {
            snackbars.showSnackbar(it)
            vm.dismissNotice()
        }
    }

    Scaffold(
        containerColor = palette.bgPage,
        contentWindowInsets = WindowInsets.safeDrawing,
        snackbarHost = { SnackbarHost(snackbars) },
    ) { insets ->
        when {
            state.fatal != null -> Fatal(state.fatal!!, Modifier.padding(insets))
            !state.ready -> Box(Modifier.fillMaxSize())   // no spinner: the open is milliseconds
            else -> Box(Modifier.fillMaxSize()) {
                HomeContent(state, vm, insets)
                NavPills(
                    Modifier
                        .align(Alignment.BottomEnd)
                        .padding(insets)
                        .padding(end = 20.dp, bottom = 20.dp)
                )
            }
        }
    }
}

@Composable
private fun HomeContent(
    state: HomeViewModel.UiState,
    vm: HomeViewModel,
    insets: PaddingValues,
) {
    val palette = LocalPalette.current

    // One clock for the whole screen, ticking once a second, and only while
    // something is actually running. An idle screen has nothing that changes, so
    // it recomposes zero times per second rather than sixty times a minute.
    var nowMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val running = state.current != null
    LaunchedEffect(running) {
        while (running) {
            nowMs = System.currentTimeMillis()
            delay(1_000)
        }
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 20.dp,
            end = 20.dp,
            top = insets.calculateTopPadding() + 16.dp,
            // Clears the floating button, which overlaps this list.
            bottom = insets.calculateBottomPadding() + 96.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            val current = state.current
            if (current != null) {
                CurrentCard(current, nowMs, onEnd = vm::end, onNote = vm::addNote)
            } else {
                IdleCard(state.lastEnded, onUndo = vm::undo)
            }
        }

        item {
            Text(
                text = stringResource(R.string.home_hint),
                color = palette.textTertiary,
                fontSize = 12.sp,
                lineHeight = 17.sp,
            )
        }

        item {
            StateGrid(
                currentId = state.current?.stateId,
                onPick = vm::switchTo,
            )
        }

        item {
            Text(
                text = stringResource(R.string.home_recent),
                color = palette.textTertiary,
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.padding(top = 4.dp),
            )
        }

        if (state.recent.isEmpty()) {
            item {
                Text(
                    text = stringResource(R.string.home_recent_empty),
                    color = palette.textTertiary,
                    fontSize = 13.sp,
                )
            }
        } else {
            items(state.recent, key = { it.id }) { entry ->
                RecentRow(entry, nowMs)
            }
        }
    }
}

/**
 * The header while something is running.
 *
 * Drawn on the inverse surface, which is what makes it read as the state of the
 * app rather than as the first row of a list.
 */
@Composable
private fun CurrentCard(
    entry: Entry,
    nowMs: Long,
    onEnd: () -> Unit,
    onNote: (String) -> Unit,
) {
    val palette = LocalPalette.current
    val spec = stateSpec(entry.stateId)

    // Reset when the entry changes: a half-typed note belongs to the state it
    // was being written about, and carrying it into the next one would attach it
    // to something the person never said it about.
    var draft by remember(entry.id) { mutableStateOf("") }
    var writing by remember(entry.id) { mutableStateOf(false) }

    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(palette.surfaceInverse)
            .padding(20.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = stringResource(R.string.home_current),
                color = palette.mutedOnInverse,
                fontSize = 12.sp,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = stringResource(R.string.home_end),
                color = palette.accentOnInverse,
                fontSize = 13.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(onClick = onEnd)
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            )
        }
        Spacer(Modifier.height(10.dp))
        Text(
            text = spec?.let { stringResource(it.name) } ?: entry.stateId,
            color = palette.onBrand,
            fontSize = 26.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(6.dp))
        LtrText(
            text = runningDuration(entry.elapsedMs(nowMs)),
            color = palette.accentOnInverse,
            fontSize = 15.sp,
            fontFamily = FontFamily.Monospace,
        )
        if (entry.notes.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            entry.notes.forEach { note ->
                Text(
                    text = note.text,
                    color = palette.mutedOnInverse,
                    fontSize = 13.sp,
                    modifier = Modifier.padding(top = 3.dp),
                )
            }
        }

        Spacer(Modifier.height(14.dp))
        if (writing) {
            BasicTextField(
                value = draft,
                onValueChange = { draft = it },
                singleLine = true,
                textStyle = TextStyle(color = palette.onBrand, fontSize = 14.sp),
                cursorBrush = SolidColor(palette.accentOnInverse),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(
                    onDone = {
                        onNote(draft)
                        draft = ""
                        writing = false
                    },
                ),
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(10.dp))
                    .background(palette.bgOverlay.copy(alpha = 0.12f))
                    .padding(horizontal = 12.dp, vertical = 10.dp),
            )
        } else {
            Text(
                text = stringResource(R.string.home_note_add),
                color = palette.mutedOnInverse,
                fontSize = 13.sp,
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable { writing = true }
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            )
        }
    }
}

/**
 * The header with nothing running -- design round 5h.
 *
 * It offers undo and nothing else. There is deliberately no "fill this in"
 * affordance: the gap that is accruing right now is a real fact about the day,
 * and the screen's job is to say so, not to help paper over it.
 */
@Composable
private fun IdleCard(lastEnded: Entry?, onUndo: () -> Unit) {
    val palette = LocalPalette.current

    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(palette.bgSurface)
            .border(1.dp, palette.border, RoundedCornerShape(20.dp))
            .padding(20.dp),
    ) {
        Text(
            text = stringResource(R.string.home_idle_title),
            color = palette.textPrimary,
            fontSize = 22.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(8.dp))
        Text(
            text = stringResource(R.string.home_idle_hint),
            color = palette.textTertiary,
            fontSize = 13.sp,
            lineHeight = 19.sp,
        )

        if (lastEnded != null) {
            val name = stateSpec(lastEnded.stateId)?.let { stringResource(it.name) }
                ?: lastEnded.stateId
            Spacer(Modifier.height(16.dp))
            Text(
                text = stringResource(
                    R.string.home_just_ended,
                    name,
                    settledDuration(lastEnded.closedMs),
                ),
                color = palette.textSecondary,
                fontSize = 13.sp,
            )
            Spacer(Modifier.height(10.dp))
            Text(
                text = stringResource(R.string.home_undo, name),
                color = palette.onBrand,
                fontSize = 13.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier
                    .clip(RoundedCornerShape(10.dp))
                    .background(palette.brand)
                    .clickable(onClick = onUndo)
                    .padding(horizontal = 14.dp, vertical = 9.dp),
            )
        }
    }
}

/**
 * The nine cells, three across.
 *
 * A plain Column of Rows rather than a LazyVerticalGrid: nine fixed cells do
 * not need recycling, and a lazy grid nested in a lazy column needs a fixed
 * height, which would mean picking a cell height in dp instead of letting the
 * longest label decide it. German runs about 60% longer than Chinese and two
 * lines have to fit.
 */
@Composable
private fun StateGrid(currentId: String?, onPick: (String) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        STATES.chunked(COLUMNS).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { spec ->
                    StateCell(
                        spec = spec,
                        selected = spec.id == currentId,
                        onClick = { onPick(spec.id) },
                        modifier = Modifier.weight(1f),
                    )
                }
                // The nine built-ins fill every row exactly. Custom states are
                // on the design's list and will not, and a short last row of
                // weighted cells stretches to fill the width instead of lining
                // up with the rows above it.
                repeat(COLUMNS - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }
    }
}

/** Three across, which is what the design's nine-cell grid is. */
private const val COLUMNS = 3

@Composable
private fun StateCell(
    spec: StateSpec,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val palette = LocalPalette.current
    val shape = RoundedCornerShape(14.dp)

    Column(
        modifier
            .clip(shape)
            .background(if (selected) palette.surfaceInverse else palette.bgSurface)
            .border(1.dp, if (selected) palette.surfaceInverse else palette.border, shape)
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 13.dp),
    ) {
        // A color bar, not an icon. design/icons.md calls this out by name: it
        // is a palette value carrying which state this is, and a glyph in its
        // place loses that.
        Box(
            Modifier
                .width(22.dp)
                .height(3.dp)
                .clip(RoundedCornerShape(2.dp))
                .background(stateColor(spec.id, palette)),
        )
        Spacer(Modifier.height(10.dp))
        Text(
            text = stringResource(spec.name),
            color = if (selected) palette.onBrand else palette.textPrimary,
            fontSize = 14.sp,
            lineHeight = 18.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 2,
        )
        Spacer(Modifier.height(3.dp))
        Text(
            text = stringResource(spec.hint),
            color = if (selected) palette.mutedOnInverse else palette.textTertiary,
            fontSize = 11.sp,
            lineHeight = 15.sp,
            maxLines = 2,
        )
    }
}

@Composable
private fun RecentRow(entry: Entry, nowMs: Long) {
    val palette = LocalPalette.current
    val spec = stateSpec(entry.stateId)

    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(palette.bgSurface)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(width = 3.dp, height = 26.dp)
                .clip(RoundedCornerShape(2.dp))
                .background(stateColor(entry.stateId, palette)),
        )
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = spec?.let { stringResource(it.name) } ?: entry.stateId,
                color = palette.textPrimary,
                fontSize = 14.sp,
                fontWeight = FontWeight.Medium,
            )
            LtrText(
                text = clockTime(entry.startedAt),
                color = palette.textTertiary,
                fontSize = 12.sp,
            )
        }
        LtrText(
            text = settledDuration(entry.elapsedMs(nowMs)),
            color = if (entry.open) palette.brand else palette.textSecondary,
            fontSize = 13.sp,
            fontFamily = FontFamily.Monospace,
        )
    }
}

/**
 * The floating button, and the pill stack it opens.
 *
 * Collapsed it is a dot; open it is Ask / Now / Today / Settings, with Ask in
 * the accent because it is the one people reach for without a destination in
 * mind. Every screen in the app carries this same control at the same corner,
 * which is why no screen needs a back arrow of its own.
 *
 * Nothing behind the pills is built yet -- this is the home screen, and Ask,
 * Today and Settings are separate rounds -- so they open nothing for now rather
 * than navigating somewhere empty.
 */
@Composable
private fun NavPills(modifier: Modifier = Modifier) {
    val palette = LocalPalette.current
    var open by remember { mutableStateOf(false) }

    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.End,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (open) {
            Pill(stringResource(R.string.nav_ask), palette.brand, palette.onBrand) {}
            Pill(stringResource(R.string.nav_now), palette.bgSurface, palette.textPrimary) {}
            Pill(stringResource(R.string.nav_today), palette.bgSurface, palette.textPrimary) {}
            Pill(stringResource(R.string.nav_settings), palette.bgSurface, palette.textPrimary) {}
        }
        Box(
            Modifier
                .size(52.dp)
                .clip(RoundedCornerShape(26.dp))
                .background(palette.surfaceInverse)
                .clickable { open = !open },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = stringResource(if (open) R.string.nav_close else R.string.nav_open),
                color = palette.onBrand,
                fontSize = 12.sp,
                textAlign = TextAlign.Center,
            )
        }
    }
}

@Composable
private fun Pill(label: String, background: Color, foreground: Color, onClick: () -> Unit) {
    val palette = LocalPalette.current
    Text(
        text = label,
        color = foreground,
        fontSize = 13.sp,
        fontWeight = FontWeight.Medium,
        modifier = Modifier
            .clip(RoundedCornerShape(18.dp))
            .background(background)
            .border(1.dp, palette.border, RoundedCornerShape(18.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
    )
}

@Composable
private fun Fatal(message: String, modifier: Modifier = Modifier) {
    val palette = LocalPalette.current
    Box(modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        Text(text = message, color = palette.danger, fontSize = 14.sp, textAlign = TextAlign.Center)
    }
}

/**
 * Text that stays left-to-right whatever the interface language is.
 *
 * Clocks, timers and timestamps are in round 5f's do-not-mirror list, along
 * with the logo and photographs. `1:22:47` is not a sentence and reversing it
 * in Arabic or Hebrew produces a time that does not exist.
 */
@Composable
private fun LtrText(
    text: String,
    color: Color,
    fontSize: TextUnit,
    fontFamily: FontFamily? = null,
) {
    CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
        Text(text = text, color = color, fontSize = fontSize, fontFamily = fontFamily)
    }
}

/**
 * A wall-clock time in the device's format, so 24-hour locales get 24-hour and
 * the rest get what they expect. Not the entry's own offset: this is the recent
 * window, which is about what just happened here.
 */
@Composable
private fun clockTime(atMs: Long): String =
    remember(atMs) { DateFormat.getTimeInstance(DateFormat.SHORT).format(Date(atMs)) }
