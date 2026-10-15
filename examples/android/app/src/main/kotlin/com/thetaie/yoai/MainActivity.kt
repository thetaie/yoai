package com.thetaie.yoai

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.UUID
import kotlin.time.Duration.Companion.milliseconds

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            YoaiTheme {
                YoaiApp()
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Design System / Palette (Aligned with HarmonyOS Palettes.ets)
// -----------------------------------------------------------------------------
object YoaiPalette {
    val bgPage = Color(0xFFF8F9F8)
    val bgSurface = Color(0xFFFFFFFF)
    val bgBubble = Color(0xFFE9ECEA)
    val bgOverlay = Color(0xFFF0F2F1)
    val textPrimary = Color(0xFF22262A)
    val textSecondary = Color(0xFF464C51)
    val textTertiary = Color(0xFF6E7479)
    val textDisabled = Color(0xFF9EA4A8)
    val brand = Color(0xFF0072E8) // Arctic Blue
    val onBrand = Color(0xFFFFFFFF)
    val border = Color(0xFFDFE2E0)
    val divider = Color(0x1F6E7479)
    val danger = Color(0xFFC8372F)
}

@Composable
fun YoaiTheme(content: @Composable () -> Unit) {
    val colorScheme = lightColorScheme(
        primary = YoaiPalette.brand,
        onPrimary = YoaiPalette.onBrand,
        background = YoaiPalette.bgPage,
        surface = YoaiPalette.bgSurface,
        onBackground = YoaiPalette.textPrimary,
        onSurface = YoaiPalette.textPrimary
    )
    MaterialTheme(
        colorScheme = colorScheme,
        content = content
    )
}

// -----------------------------------------------------------------------------
// Data Models & Navigation Enum
// -----------------------------------------------------------------------------
enum class ScreenState {
    CHAT, SETTINGS
}

enum class SettingSubSheet {
    MODELS, PROMPT, THEME
}

data class ChatMessage(
    val id: String = UUID.randomUUID().toString(),
    val role: String, // "user" or "assistant"
    val content: String,
    val thinking: String? = null,
    val model: String? = null,
    val transient: Boolean = false,
    val usageInfo: String? = null,
    val ts: Long = System.currentTimeMillis()
)

data class SessionMeta(
    val id: String = UUID.randomUUID().toString(),
    val title: String,
    val count: Int,
    val updatedAt: Long = System.currentTimeMillis()
)

data class ModelConfig(
    val id: String,
    val label: String
)

// -----------------------------------------------------------------------------
// Main Application Screen
// -----------------------------------------------------------------------------
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun YoaiApp() {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val listState = rememberLazyListState()

    // Screen State
    var currentScreen by remember { mutableStateOf(ScreenState.CHAT) }
    var activeSettingSubSheet by remember { mutableStateOf<SettingSubSheet?>(null) }
    var showClearHistoryDialog by remember { mutableStateOf(false) }

    // Settings Data State
    var customPrompt by remember { mutableStateOf("你是一个表达清晰、有帮助的 AI 助手。") }
    var activeThemeName by remember { mutableStateOf("Daylight Arctic (默认蓝)") }

    // Models State
    val models = remember {
        mutableStateListOf(
            ModelConfig("yoai-ondevice", "Yoai On-Device 0.5B"),
            ModelConfig("gpt-4o", "GPT-4o"),
            ModelConfig("claude-3-5", "Claude 3.5 Sonnet"),
            ModelConfig("deepseek-r1", "DeepSeek R1")
        )
    }
    var activeModel by remember { mutableStateOf(models.first()) }
    var input by remember { mutableStateOf("") }
    var streaming by remember { mutableStateOf(false) }
    var streamText by remember { mutableStateOf("") }
    var streamThinking by remember { mutableStateOf("") }
    var speakingMessageId by remember { mutableStateOf<String?>(null) }

    // Session Management
    val sessions = remember {
        mutableStateListOf(
            SessionMeta("s1", "天气与出行计划", 4),
            SessionMeta("s2", "Android Compose 最佳实践", 2)
        )
    }
    var currentSessionId by remember { mutableStateOf("s1") }
    var messages by remember {
        mutableStateOf(
            listOf(
                ChatMessage(
                    role = "user",
                    content = "你好，请告诉我今天天气如何？"
                ),
                ChatMessage(
                    role = "assistant",
                    content = "你好！今天天气晴朗，气温约 18°C ~ 26°C，非常适合户外活动和出行。",
                    thinking = "用户询问天气情况，给出晴朗宜人的回答。",
                    model = "Yoai On-Device 0.5B",
                    usageInfo = "输入 18 · 输出 42 tokens"
                )
            )
        )
    }

    var showDrawer by remember { mutableStateOf(false) }

    fun scrollToBottom() {
        coroutineScope.launch {
            if (messages.isNotEmpty() || streaming) {
                listState.animateScrollToItem((messages.size + if (streaming) 1 else 0) - 1)
            }
        }
    }

    fun handleSend() {
        val text = input.trim()
        if (text.isEmpty() || streaming) return

        messages = messages + ChatMessage(role = "user", content = text)
        input = ""
        streaming = true
        streamText = ""
        streamThinking = ""
        scrollToBottom()

        // Simulate streaming turn
        coroutineScope.launch {
            streamThinking = "正在分析问题并生成回复..."
            delay(800.milliseconds)
            streamThinking = "分析完成，准备生成回答。"
            delay(400.milliseconds)

            val fullReply = "这是由 ${activeModel.label} 生成的回复：关于“$text”，我已经为您整理好了相关要点与建议。"
            for (i in 1..fullReply.length) {
                streamText = fullReply.substring(0, i)
                delay(30.milliseconds)
                scrollToBottom()
            }

            // Finish turn
            messages = messages + ChatMessage(
                role = "assistant",
                content = streamText,
                thinking = streamThinking,
                model = activeModel.label,
                usageInfo = "输入 ${text.length * 2} · 输出 ${streamText.length * 3} tokens"
            )
            streaming = false
            streamText = ""
            streamThinking = ""
            scrollToBottom()
        }
    }

    fun handleStop() {
        streaming = false
        streamText = ""
        streamThinking = ""
        speakingMessageId = null
    }

    fun newChat() {
        handleStop()
        val newSession = SessionMeta(title = "新对话", count = 0)
        sessions.add(0, newSession)
        currentSessionId = newSession.id
        messages = emptyList()
        showDrawer = false
    }

    if (currentScreen == ScreenState.SETTINGS) {
        // ---------------------------------------------------------------------
        // Settings Main Screen
        // ---------------------------------------------------------------------
        SettingsScreen(
            activeModelLabel = activeModel.label,
            customPrompt = customPrompt,
            activeThemeName = activeThemeName,
            onBack = { currentScreen = ScreenState.CHAT },
            onOpenModels = { activeSettingSubSheet = SettingSubSheet.MODELS },
            onOpenPrompt = { activeSettingSubSheet = SettingSubSheet.PROMPT },
            onOpenSpeech = { Toast.makeText(context, "QwenTTS 离线语音合成引擎运行中", Toast.LENGTH_SHORT).show() },
            onOpenTheme = { activeSettingSubSheet = SettingSubSheet.THEME },
            onClearHistory = { showClearHistoryDialog = true }
        )
    } else {
        // ---------------------------------------------------------------------
        // Chat Main Screen
        // ---------------------------------------------------------------------
        Scaffold(
            topBar = {
                Header(
                    activeModel = activeModel,
                    models = models,
                    onSelectModel = { activeModel = it },
                    onOpenDrawer = { showDrawer = true },
                    onOpenSettings = { currentScreen = ScreenState.SETTINGS }
                )
            },
            bottomBar = {
                Composer(
                    input = input,
                    streaming = streaming,
                    speaking = speakingMessageId != null,
                    onInputChange = { input = it },
                    onSend = { handleSend() },
                    onStop = { handleStop() }
                )
            },
            containerColor = YoaiPalette.bgPage
        ) { innerPadding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
            ) {
                if (messages.isEmpty() && !streaming) {
                    EmptyState(modifier = Modifier.align(Alignment.Center))
                } else {
                    LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(vertical = 8.dp)
                    ) {
                        itemsIndexed(messages) { index, msg ->
                            if (msg.role == "user") {
                                UserBubble(message = msg)
                            } else {
                                AssistantBubble(
                                    message = msg,
                                    isLast = index == messages.lastIndex,
                                    speakingId = speakingMessageId,
                                    onCopy = {
                                        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                                        clipboard.setPrimaryClip(ClipData.newPlainText("chat", msg.content))
                                        Toast.makeText(context, "已复制到剪贴板", Toast.LENGTH_SHORT).show()
                                    },
                                    onToggleSpeak = {
                                        speakingMessageId = if (speakingMessageId == msg.id) null else msg.id
                                    }
                                )
                            }
                        }

                        if (streaming) {
                            item {
                                StreamingBubble(
                                    streamText = streamText,
                                    streamThinking = streamThinking
                                )
                            }
                        }
                    }
                }
            }
        }

        // Drawer / History Bottom Sheet
        if (showDrawer) {
            ModalBottomSheet(
                onDismissRequest = { showDrawer = false },
                containerColor = YoaiPalette.bgSurface,
                shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp)
            ) {
                DrawerContent(
                    sessions = sessions,
                    currentSessionId = currentSessionId,
                    onNewChat = { newChat() },
                    onSelectSession = { id ->
                        currentSessionId = id
                        showDrawer = false
                        Toast.makeText(context, "切换对话", Toast.LENGTH_SHORT).show()
                    },
                    onDeleteSession = { id ->
                        sessions.removeIf { it.id == id }
                        if (currentSessionId == id) {
                            newChat()
                        }
                    }
                )
            }
        }
    }

    // Settings Sub-Sheets
    activeSettingSubSheet?.let { subSheet ->
        ModalBottomSheet(
            onDismissRequest = { activeSettingSubSheet = null },
            containerColor = YoaiPalette.bgSurface,
            shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp)
        ) {
            when (subSheet) {
                SettingSubSheet.MODELS -> {
                    ModelsSubSheet(
                        models = models,
                        activeModel = activeModel,
                        onSelectModel = {
                            activeModel = it
                            activeSettingSubSheet = null
                        },
                        onAddModel = { name ->
                            val newM = ModelConfig(UUID.randomUUID().toString(), name)
                            models.add(newM)
                            activeModel = newM
                            activeSettingSubSheet = null
                        }
                    )
                }
                SettingSubSheet.PROMPT -> {
                    PromptSubSheet(
                        currentPrompt = customPrompt,
                        onSavePrompt = { newPrompt ->
                            customPrompt = newPrompt
                            activeSettingSubSheet = null
                            Toast.makeText(context, "提示词已保存", Toast.LENGTH_SHORT).show()
                        }
                    )
                }
                SettingSubSheet.THEME -> {
                    ThemeSubSheet(
                        currentTheme = activeThemeName,
                        onSelectTheme = { themeName ->
                            activeThemeName = themeName
                            activeSettingSubSheet = null
                            Toast.makeText(context, "已应用主题 $themeName", Toast.LENGTH_SHORT).show()
                        }
                    )
                }
            }
        }
    }

    // Clear History Dialog
    if (showClearHistoryDialog) {
        AlertDialog(
            onDismissRequest = { showClearHistoryDialog = false },
            title = { Text("清除历史记录", fontWeight = FontWeight.Bold) },
            text = { Text("确定要清除所有本地聊天历史记录吗？此操作不可撤销。") },
            confirmButton = {
                TextButton(
                    onClick = {
                        messages = emptyList()
                        sessions.clear()
                        showClearHistoryDialog = false
                        Toast.makeText(context, "已清除所有对话历史", Toast.LENGTH_SHORT).show()
                    }
                ) {
                    Text("清除", color = YoaiPalette.danger, fontWeight = FontWeight.Bold)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearHistoryDialog = false }) {
                    Text("取消", color = YoaiPalette.textSecondary)
                }
            },
            containerColor = YoaiPalette.bgSurface
        )
    }
}

// -----------------------------------------------------------------------------
// Settings Main Screen (Matching HarmonyOS Settings.ets)
// -----------------------------------------------------------------------------
@Composable
fun SettingsScreen(
    activeModelLabel: String,
    customPrompt: String,
    activeThemeName: String,
    onBack: () -> Unit,
    onOpenModels: () -> Unit,
    onOpenPrompt: () -> Unit,
    onOpenSpeech: () -> Unit,
    onOpenTheme: () -> Unit,
    onClearHistory: () -> Unit
) {
    Scaffold(
        topBar = {
            Column(modifier = Modifier.fillMaxWidth().background(YoaiPalette.bgPage)) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(56.dp)
                        .padding(horizontal = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = onBack, modifier = Modifier.size(40.dp)) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "返回",
                            tint = YoaiPalette.textSecondary
                        )
                    }
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "设置",
                        fontSize = 18.sp,
                        fontWeight = FontWeight.Medium,
                        color = YoaiPalette.textPrimary
                    )
                }
                HorizontalDivider(color = YoaiPalette.divider)
            }
        },
        containerColor = YoaiPalette.bgPage
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // Options Surface Card
            Surface(
                color = YoaiPalette.bgSurface,
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column {
                    SettingsRow(
                        title = "模型设置",
                        subtitle = activeModelLabel,
                        onClick = onOpenModels
                    )
                    HorizontalDivider(color = YoaiPalette.divider, modifier = Modifier.padding(horizontal = 16.dp))
                    SettingsRow(
                        title = "系统提示词",
                        subtitle = if (customPrompt.isNotBlank()) "自定义提示词" else "默认提示词",
                        onClick = onOpenPrompt
                    )
                    HorizontalDivider(color = YoaiPalette.divider, modifier = Modifier.padding(horizontal = 16.dp))
                    SettingsRow(
                        title = "语音朗读与 TTS",
                        subtitle = "QwenTTS 离线语音引擎 (已就绪)",
                        onClick = onOpenSpeech
                    )
                    HorizontalDivider(color = YoaiPalette.divider, modifier = Modifier.padding(horizontal = 16.dp))
                    SettingsRow(
                        title = "界面与主题",
                        subtitle = activeThemeName,
                        onClick = onOpenTheme
                    )
                }
            }

            // Core & Storage Section
            Text(
                text = "内核与存储",
                fontSize = 13.sp,
                color = YoaiPalette.textTertiary,
                modifier = Modifier.padding(start = 4.dp)
            )

            Surface(
                color = YoaiPalette.bgSurface,
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = "YoAI Native Core v1.0.0 (llama.cpp / qwentts)",
                        fontSize = 12.sp,
                        color = YoaiPalette.textTertiary
                    )
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(
                        text = "清除所有聊天历史记录",
                        fontSize = 15.sp,
                        color = YoaiPalette.danger,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onClearHistory() }
                    )
                }
            }
        }
    }
}

@Composable
fun SettingsRow(
    title: String,
    subtitle: String,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clickable { onClick() }
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                fontSize = 16.sp,
                color = YoaiPalette.textPrimary
            )
            Spacer(modifier = Modifier.height(2.dp))
            Text(
                text = subtitle,
                fontSize = 12.sp,
                color = YoaiPalette.textTertiary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
        Icon(
            imageVector = Icons.Default.ChevronRight,
            contentDescription = null,
            tint = YoaiPalette.textTertiary,
            modifier = Modifier.size(20.dp)
        )
    }
}

// -----------------------------------------------------------------------------
// Settings Sub-Sheets
// -----------------------------------------------------------------------------
@Composable
fun ModelsSubSheet(
    models: List<ModelConfig>,
    activeModel: ModelConfig,
    onSelectModel: (ModelConfig) -> Unit,
    onAddModel: (String) -> Unit
) {
    var newModelName by remember { mutableStateOf("") }
    var showAddInput by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp)
    ) {
        Text(
            text = "模型管理",
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            color = YoaiPalette.textPrimary
        )
        Spacer(modifier = Modifier.height(12.dp))

        models.forEach { m ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onSelectModel(m) }
                    .padding(vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = m.label,
                    fontSize = 16.sp,
                    color = if (m.id == activeModel.id) YoaiPalette.brand else YoaiPalette.textPrimary,
                    fontWeight = if (m.id == activeModel.id) FontWeight.Bold else FontWeight.Normal
                )
                if (m.id == activeModel.id) {
                    Icon(
                        imageVector = Icons.Default.Check,
                        contentDescription = "已选择",
                        tint = YoaiPalette.brand
                    )
                }
            }
            HorizontalDivider(color = YoaiPalette.divider)
        }

        Spacer(modifier = Modifier.height(16.dp))

        if (showAddInput) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                OutlinedTextField(
                    value = newModelName,
                    onValueChange = { newModelName = it },
                    placeholder = { Text("输入模型名称...") },
                    modifier = Modifier.weight(1f)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Button(
                    onClick = {
                        if (newModelName.isNotBlank()) {
                            onAddModel(newModelName.trim())
                        }
                    }
                ) {
                    Text("添加")
                }
            }
        } else {
            Button(
                onClick = { showAddInput = true },
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(containerColor = YoaiPalette.brand)
            ) {
                Icon(imageVector = Icons.Default.Add, contentDescription = null)
                Spacer(modifier = Modifier.width(4.dp))
                Text("添加自定义模型")
            }
        }
        Spacer(modifier = Modifier.height(16.dp))
    }
}

@Composable
fun PromptSubSheet(
    currentPrompt: String,
    onSavePrompt: (String) -> Unit
) {
    var text by remember { mutableStateOf(currentPrompt) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp)
    ) {
        Text(
            text = "系统提示词 (System Prompt)",
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            color = YoaiPalette.textPrimary
        )
        Spacer(modifier = Modifier.height(12.dp))

        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            modifier = Modifier
                .fillMaxWidth()
                .height(120.dp),
            placeholder = { Text("例如：你是一个助手...") }
        )

        Spacer(modifier = Modifier.height(16.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            OutlinedButton(
                onClick = { text = "你是一个表达清晰、有帮助的 AI 助手。" }
            ) {
                Text("恢复默认")
            }
            Button(
                onClick = { onSavePrompt(text) },
                colors = ButtonDefaults.buttonColors(containerColor = YoaiPalette.brand)
            ) {
                Text("保存设置")
            }
        }
        Spacer(modifier = Modifier.height(16.dp))
    }
}

@Composable
fun ThemeSubSheet(
    currentTheme: String,
    onSelectTheme: (String) -> Unit
) {
    val themeOptions = listOf(
        "Daylight Arctic (默认蓝)",
        "Daylight Amber (琥珀日)",
        "Quiet Day (静谧日)",
        "Dark Mode (夜间深色)"
    )

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp)
    ) {
        Text(
            text = "界面与主题",
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            color = YoaiPalette.textPrimary
        )
        Spacer(modifier = Modifier.height(12.dp))

        themeOptions.forEach { t ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onSelectTheme(t) }
                    .padding(vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = t,
                    fontSize = 16.sp,
                    color = if (t == currentTheme) YoaiPalette.brand else YoaiPalette.textPrimary,
                    fontWeight = if (t == currentTheme) FontWeight.Bold else FontWeight.Normal
                )
                if (t == currentTheme) {
                    Icon(
                        imageVector = Icons.Default.Check,
                        contentDescription = "已选择",
                        tint = YoaiPalette.brand
                    )
                }
            }
            HorizontalDivider(color = YoaiPalette.divider)
        }
        Spacer(modifier = Modifier.height(16.dp))
    }
}

// -----------------------------------------------------------------------------
// Header Component
// -----------------------------------------------------------------------------
@Composable
fun Header(
    activeModel: ModelConfig,
    models: List<ModelConfig>,
    onSelectModel: (ModelConfig) -> Unit,
    onOpenDrawer: () -> Unit,
    onOpenSettings: () -> Unit
) {
    var menuExpanded by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxWidth().background(YoaiPalette.bgPage)) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .height(56.dp)
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // History / Drawer Icon Button
            IconButton(
                onClick = onOpenDrawer,
                modifier = Modifier.size(40.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.History,
                    contentDescription = "对话历史",
                    tint = YoaiPalette.textSecondary
                )
            }

            // Model Selection Dropdown Anchor
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(8.dp))
                    .clickable { menuExpanded = true }
                    .padding(vertical = 6.dp, horizontal = 8.dp),
                contentAlignment = Alignment.Center
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center
                ) {
                    Text(
                        text = activeModel.label,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Medium,
                        color = YoaiPalette.textPrimary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Icon(
                        imageVector = Icons.Default.KeyboardArrowDown,
                        contentDescription = "选择模型",
                        tint = YoaiPalette.textTertiary,
                        modifier = Modifier.size(20.dp)
                    )
                }

                DropdownMenu(
                    expanded = menuExpanded,
                    onDismissRequest = { menuExpanded = false },
                    modifier = Modifier.background(YoaiPalette.bgSurface)
                ) {
                    models.forEach { model ->
                        DropdownMenuItem(
                            text = {
                                Text(
                                    text = model.label,
                                    color = if (model.id == activeModel.id) YoaiPalette.brand else YoaiPalette.textPrimary,
                                    fontWeight = if (model.id == activeModel.id) FontWeight.Bold else FontWeight.Normal
                                )
                            },
                            onClick = {
                                onSelectModel(model)
                                menuExpanded = false
                            }
                        )
                    }
                    HorizontalDivider(color = YoaiPalette.divider)
                    DropdownMenuItem(
                        text = {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Icon(
                                    imageVector = Icons.Default.Add,
                                    contentDescription = null,
                                    tint = YoaiPalette.brand,
                                    modifier = Modifier.size(18.dp)
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text("添加模型", color = YoaiPalette.brand)
                            }
                        },
                        onClick = {
                            menuExpanded = false
                            onOpenSettings()
                        }
                    )
                }
            }

            // Settings Button
            IconButton(
                onClick = onOpenSettings,
                modifier = Modifier.size(40.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.Settings,
                    contentDescription = "设置",
                    tint = YoaiPalette.textSecondary
                )
            }
        }
        HorizontalDivider(color = YoaiPalette.divider, thickness = 1.dp)
    }
}

// -----------------------------------------------------------------------------
// Empty State Component
// -----------------------------------------------------------------------------
@Composable
fun EmptyState(modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = "YoAI",
            fontSize = 22.sp,
            fontWeight = FontWeight.Medium,
            color = YoaiPalette.textPrimary,
            textAlign = TextAlign.Center
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "选择模型并开始对话，体验智能 AI 交互",
            fontSize = 14.sp,
            color = YoaiPalette.textTertiary,
            textAlign = TextAlign.Center
        )
    }
}

// -----------------------------------------------------------------------------
// Chat Bubbles
// -----------------------------------------------------------------------------
@Composable
fun UserBubble(message: ChatMessage) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 48.dp, end = 12.dp, top = 6.dp, bottom = 6.dp),
        contentAlignment = Alignment.CenterEnd
    ) {
        Surface(
            color = YoaiPalette.bgBubble,
            shape = RoundedCornerShape(16.dp)
        ) {
            Text(
                text = message.content,
                fontSize = 16.sp,
                color = YoaiPalette.textPrimary,
                modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp)
            )
        }
    }
}

@Composable
fun AssistantBubble(
    message: ChatMessage,
    isLast: Boolean,
    speakingId: String?,
    onCopy: () -> Unit,
    onToggleSpeak: () -> Unit
) {
    var showThinking by remember { mutableStateOf(false) }
    val isSpeaking = speakingId == message.id

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp)
    ) {
        if (message.transient) {
            Text(
                text = message.content,
                fontSize = 16.sp,
                color = YoaiPalette.danger,
                modifier = Modifier.fillMaxWidth()
            )
        } else {
            // Thinking section if present
            if (!message.thinking.isNull_or_blank()) {
                Surface(
                    color = YoaiPalette.bgOverlay,
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(bottom = 6.dp)
                ) {
                    Column(modifier = Modifier.padding(8.dp)) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { showThinking = !showThinking }
                        ) {
                            Icon(
                                imageVector = if (showThinking) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                                contentDescription = null,
                                tint = YoaiPalette.textTertiary,
                                modifier = Modifier.size(16.dp)
                            )
                            Spacer(modifier = Modifier.width(4.dp))
                            Text(
                                text = "思考过程",
                                fontSize = 12.sp,
                                color = YoaiPalette.textTertiary,
                                fontWeight = FontWeight.Medium
                            )
                        }
                        AnimatedVisibility(visible = showThinking) {
                            Text(
                                text = message.thinking.orEmpty(),
                                fontSize = 13.sp,
                                color = YoaiPalette.textSecondary,
                                modifier = Modifier.padding(top = 4.dp, start = 20.dp)
                            )
                        }
                    }
                }
            }

            // Message Body Text
            Text(
                text = message.content,
                fontSize = 16.sp,
                color = YoaiPalette.textPrimary,
                modifier = Modifier.fillMaxWidth()
            )

            Spacer(modifier = Modifier.height(6.dp))

            // Message Footer Actions
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                if (message.model != null) {
                    Text(
                        text = message.model,
                        fontSize = 11.sp,
                        color = YoaiPalette.textTertiary
                    )
                }

                if (message.usageInfo != null) {
                    Text(
                        text = message.usageInfo,
                        fontSize = 11.sp,
                        color = YoaiPalette.textTertiary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                } else {
                    Spacer(modifier = Modifier.weight(1f))
                }

                // Copy Action
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.clickable { onCopy() }
                ) {
                    Icon(
                        imageVector = Icons.Default.ContentCopy,
                        contentDescription = "复制",
                        tint = YoaiPalette.textTertiary,
                        modifier = Modifier.size(14.dp)
                    )
                    Spacer(modifier = Modifier.width(2.dp))
                    Text(
                        text = "复制",
                        fontSize = 11.sp,
                        color = YoaiPalette.textTertiary
                    )
                }

                // Speak / Stop Action
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.clickable { onToggleSpeak() }
                ) {
                    Icon(
                        imageVector = if (isSpeaking) Icons.Default.Stop else Icons.AutoMirrored.Filled.VolumeUp,
                        contentDescription = if (isSpeaking) "停止" else "朗读",
                        tint = if (isSpeaking) YoaiPalette.brand else YoaiPalette.textTertiary,
                        modifier = Modifier.size(14.dp)
                    )
                    Spacer(modifier = Modifier.width(2.dp))
                    Text(
                        text = if (isSpeaking) "停止" else "朗读",
                        fontSize = 11.sp,
                        color = if (isSpeaking) YoaiPalette.brand else YoaiPalette.textTertiary
                    )
                }
            }
        }
    }
}

private fun String?.isNull_or_blank(): Boolean = this.isNullOrBlank()

@Composable
fun StreamingBubble(
    streamText: String,
    streamThinking: String
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp)
    ) {
        if (streamText.isEmpty() && streamThinking.isEmpty()) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                color = YoaiPalette.brand,
                strokeWidth = 2.dp
            )
        } else {
            if (streamThinking.isNotEmpty()) {
                Surface(
                    color = YoaiPalette.bgOverlay,
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(bottom = 6.dp)
                ) {
                    Column(modifier = Modifier.padding(8.dp)) {
                        Text(
                            text = "思考中...",
                            fontSize = 12.sp,
                            color = YoaiPalette.brand,
                            fontWeight = FontWeight.Medium
                        )
                        Text(
                            text = streamThinking,
                            fontSize = 13.sp,
                            color = YoaiPalette.textSecondary,
                            modifier = Modifier.padding(top = 4.dp)
                        )
                    }
                }
            }

            if (streamText.isNotEmpty()) {
                Text(
                    text = streamText,
                    fontSize = 16.sp,
                    color = YoaiPalette.textPrimary,
                    modifier = Modifier.fillMaxWidth()
                )
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Composer (Input Bar)
// -----------------------------------------------------------------------------
@Composable
fun Composer(
    input: String,
    streaming: Boolean,
    speaking: Boolean,
    onInputChange: (String) -> Unit,
    onSend: () -> Unit,
    onStop: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(YoaiPalette.bgPage)
            .padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Rounded Input Field
            Surface(
                modifier = Modifier
                    .weight(1f)
                    .height(44.dp)
                    .border(1.dp, YoaiPalette.border, CircleShape),
                shape = CircleShape,
                color = YoaiPalette.bgSurface
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(horizontal = 16.dp),
                    contentAlignment = Alignment.CenterStart
                ) {
                    if (input.isEmpty()) {
                        Text(
                            text = "输入消息...",
                            fontSize = 16.sp,
                            color = YoaiPalette.textDisabled
                        )
                    }
                    TextField(
                        value = input,
                        onValueChange = onInputChange,
                        colors = TextFieldDefaults.colors(
                            focusedContainerColor = Color.Transparent,
                            unfocusedContainerColor = Color.Transparent,
                            focusedIndicatorColor = Color.Transparent,
                            unfocusedIndicatorColor = Color.Transparent
                        ),
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            Spacer(modifier = Modifier.width(8.dp))

            // Action Button (Send / Stop)
            Surface(
                modifier = Modifier
                    .size(44.dp)
                    .clip(CircleShape)
                    .clickable {
                        if (streaming || speaking) {
                            onStop()
                        } else {
                            onSend()
                        }
                    },
                shape = CircleShape,
                color = YoaiPalette.brand
            ) {
                Box(contentAlignment = Alignment.Center) {
                    if (streaming || speaking) {
                        Icon(
                            imageVector = Icons.Default.Stop,
                            contentDescription = "停止",
                            tint = YoaiPalette.onBrand,
                            modifier = Modifier.size(20.dp)
                        )
                    } else {
                        Icon(
                            imageVector = Icons.Default.ArrowUpward,
                            contentDescription = "发送",
                            tint = YoaiPalette.onBrand,
                            modifier = Modifier.size(22.dp)
                        )
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(6.dp))

        // Disclaimer
        Text(
            text = "AI 生成内容仅供参考，请谨慎甄别",
            fontSize = 11.sp,
            color = YoaiPalette.textTertiary,
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center
        )
    }
}

// -----------------------------------------------------------------------------
// Drawer / History Content
// -----------------------------------------------------------------------------
@Composable
fun DrawerContent(
    sessions: List<SessionMeta>,
    currentSessionId: String,
    onNewChat: () -> Unit,
    onSelectSession: (String) -> Unit,
    onDeleteSession: (String) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 16.dp)
    ) {
        // Drawer Header
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                text = "对话历史",
                fontSize = 18.sp,
                fontWeight = FontWeight.Medium,
                color = YoaiPalette.textPrimary
            )
            TextButton(onClick = onNewChat) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.Add,
                        contentDescription = null,
                        tint = YoaiPalette.brand,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text(
                        text = "新对话",
                        fontSize = 14.sp,
                        color = YoaiPalette.brand,
                        fontWeight = FontWeight.Medium
                    )
                }
            }
        }

        HorizontalDivider(color = YoaiPalette.divider)

        // Session List
        if (sessions.isEmpty()) {
            Text(
                text = "暂无对话记录",
                fontSize = 14.sp,
                color = YoaiPalette.textTertiary,
                modifier = Modifier.padding(16.dp)
            )
        } else {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f, fill = false)
            ) {
                sessions.forEach { session ->
                    val isSelected = session.id == currentSessionId
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(if (isSelected) YoaiPalette.bgOverlay else Color.Transparent)
                            .clickable { onSelectSession(session.id) }
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = session.title.ifEmpty { "新对话" },
                                fontSize = 15.sp,
                                color = YoaiPalette.textPrimary,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                            Spacer(modifier = Modifier.height(2.dp))
                            Text(
                                text = "${session.count} 条消息",
                                fontSize = 12.sp,
                                color = YoaiPalette.textTertiary
                            )
                        }

                        IconButton(
                            onClick = { onDeleteSession(session.id) },
                            modifier = Modifier.size(32.dp)
                        ) {
                            Icon(
                                imageVector = Icons.Default.DeleteOutline,
                                contentDescription = "删除",
                                tint = YoaiPalette.textTertiary,
                                modifier = Modifier.size(18.dp)
                            )
                        }
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(12.dp))

        Text(
            text = "对话仅保存在本地",
            fontSize = 11.sp,
            color = YoaiPalette.textTertiary,
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center
        )
    }
}

@Preview(showBackground = true)
@Composable
fun PreviewYoaiApp() {
    YoaiTheme {
        YoaiApp()
    }
}

@Preview(showBackground = true)
@Composable
fun PreviewSettingsScreen() {
    YoaiTheme {
        SettingsScreen(
            activeModelLabel = "Yoai On-Device 0.5B",
            customPrompt = "你是一个表达清晰、有帮助的 AI 助手。",
            activeThemeName = "Daylight Arctic (默认蓝)",
            onBack = {},
            onOpenModels = {},
            onOpenPrompt = {},
            onOpenSpeech = {},
            onOpenTheme = {},
            onClearHistory = {}
        )
    }
}