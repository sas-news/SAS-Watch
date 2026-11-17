package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedButton
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.agent.AgentConfig
import dev.sasnews.amoledwatch.agent.AgentEngine
import dev.sasnews.amoledwatch.agent.AgentPrefs
import dev.sasnews.amoledwatch.agent.SttModes
import dev.sasnews.amoledwatch.agent.VoskModelStore
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.ActionNames
import dev.sasnews.amoledwatch.protocol.ActionSpec
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.ClockFontNames
import dev.sasnews.amoledwatch.protocol.FaceNames
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.protocol.StepsInfo
import dev.sasnews.amoledwatch.protocol.UsbKeepAwakeModes
import dev.sasnews.amoledwatch.protocol.bool
import dev.sasnews.amoledwatch.protocol.int
import dev.sasnews.amoledwatch.protocol.text
import dev.sasnews.amoledwatch.ui.WatchViewModel
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun SettingsScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val settings by vm.settings.collectAsState()
    val steps by vm.steps.collectAsState()
    val linkState by vm.linkState.collectAsState()
    val agentConfig by vm.agentConfig.collectAsState()
    val agentHistory by vm.agentHistory.collectAsState()
    val agentBusy by vm.agentBusy.collectAsState()
    val voskState by vm.voskState.collectAsState()
    val advancedAi by vm.advancedAi.collectAsState()
    SettingsContent(
        settings = settings,
        steps = steps,
        connected = linkState is LinkState.Connected,
        onLoad = vm::refresh,
        onSave = vm::saveSettings,
        onRefreshSteps = vm::refreshSteps,
        agentConfig = agentConfig,
        agentHistory = agentHistory,
        agentBusy = agentBusy,
        onSaveAgent = vm::saveAgentConfig,
        onClearAgentHistory = vm::clearAgentHistory,
        voskState = voskState,
        onDownloadVosk = vm::downloadVoskModel,
        advancedAi = advancedAi,
        onToggleAdvanced = vm::setAdvancedAi,
        modifier = modifier,
    )
}

/** settings.get の値を編集して settings.set で保存する画面。 */
@Composable
fun SettingsContent(
    settings: Map<String, Cbor>?,
    steps: StepsInfo?,
    connected: Boolean,
    onLoad: () -> Unit,
    onSave: (Map<String, Cbor>) -> Unit,
    onRefreshSteps: () -> Unit,
    modifier: Modifier = Modifier,
    agentConfig: AgentConfig = AgentConfig(),
    agentHistory: List<AgentEngine.Turn> = emptyList(),
    agentBusy: Int = 0,
    onSaveAgent: (AgentConfig) -> Unit = {},
    onClearAgentHistory: () -> Unit = {},
    voskState: VoskModelStore.State = VoskModelStore.State.NotDownloaded,
    onDownloadVosk: () -> Unit = {},
    advancedAi: Boolean = false,
    onToggleAdvanced: (Boolean) -> Unit = {},
) {
    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!connected) {
            Card(Modifier.fillMaxWidth()) {
                Text(
                    stringResource(R.string.settings_not_connected),
                    modifier = Modifier.padding(16.dp),
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onLoad, enabled = connected, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.settings_load))
                }
                Text(
                    stringResource(R.string.settings_raw_hint),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        // 今日の歩数 (steps.get)。設定ではなく表示なのでエディタの外。
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                val s = steps
                if (s != null) {
                    val pct = if (s.goal > 0) s.steps * 100 / s.goal else 0
                    Text(stringResource(R.string.settings_steps_today))
                    Text(
                        "${s.steps} / ${s.goal} 歩 ($pct%)",
                        style = MaterialTheme.typography.titleMedium,
                    )
                } else {
                    Text(stringResource(R.string.settings_steps_today))
                    Text("-")
                }
                OutlinedButton(
                    onClick = onRefreshSteps,
                    enabled = connected,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(stringResource(R.string.settings_steps_refresh))
                }
            }
        }

        VoskModelCard(state = voskState, onDownload = onDownloadVosk)

        AgentConfigCard(
            config = agentConfig,
            busy = agentBusy,
            advanced = advancedAi,
            onToggleAdvanced = onToggleAdvanced,
            onSave = onSaveAgent,
        )


        if (settings != null) {
            SettingsEditor(settings = settings, onSave = onSave)
        }

        AgentHistoryCard(history = agentHistory, onClear = onClearAgentHistory)
    }
}

@Composable
private fun SettingsEditor(
    settings: Map<String, Cbor>,
    onSave: (Map<String, Cbor>) -> Unit,
) {
    var brightness by remember(settings) {
        mutableFloatStateOf(settings[SettingsKeys.BRIGHTNESS]?.int?.toFloat() ?: 50f)
    }
    var dimAfter by remember(settings) {
        mutableFloatStateOf(settings[SettingsKeys.DIM_AFTER_S]?.int?.toFloat() ?: 8f)
    }
    var screenOff by remember(settings) {
        mutableFloatStateOf(settings[SettingsKeys.SCREEN_OFF_AFTER_S]?.int?.toFloat() ?: 12f)
    }
    val buttonKeys = listOf(
        SettingsKeys.BUTTON_BOOT_SHORT to R.string.settings_button_boot_short,
        SettingsKeys.BUTTON_BOOT_LONG to R.string.settings_button_boot_long,
        SettingsKeys.BUTTON_BOOT_DOUBLE to R.string.settings_button_boot_double,
        SettingsKeys.BUTTON_PWR_SHORT to R.string.settings_button_pwr_short,
        SettingsKeys.BUTTON_PWR_LONG to R.string.settings_button_pwr_long,
        SettingsKeys.BUTTON_PWR_DOUBLE to R.string.settings_button_pwr_double,
    )
    var buttons by remember(settings) {
        mutableStateOf(
            buttonKeys.associate { (key, _) ->
                key to (settings[key]?.text ?: "none")
            },
        )
    }
    var theme by remember(settings) {
        mutableStateOf(settings[SettingsKeys.THEME]?.text ?: "standard")
    }
    var raiseToWake by remember(settings) {
        mutableStateOf((settings[SettingsKeys.RAISE_TO_WAKE]?.int ?: 1L) != 0L)
    }
    var usbKeepAwake by remember(settings) {
        mutableStateOf((settings[SettingsKeys.USB_KEEP_AWAKE]?.int ?: 1L).toString())
    }
    var stepsGoal by remember(settings) {
        mutableFloatStateOf(settings[SettingsKeys.STEPS_GOAL]?.int?.toFloat() ?: 8000f)
    }
    var face by remember(settings) {
        mutableStateOf(settings[SettingsKeys.FACE]?.text ?: "bold")
    }
    var clockFont by remember(settings) {
        mutableStateOf(settings[SettingsKeys.CLOCK_FONT]?.text ?: "auto")
    }
    var agentQ1 by remember(settings) {
        mutableStateOf(settings[SettingsKeys.AGENT_Q1]?.text ?: "")
    }
    var agentQ2 by remember(settings) {
        mutableStateOf(settings[SettingsKeys.AGENT_Q2]?.text ?: "")
    }
    var agentQ3 by remember(settings) {
        mutableStateOf(settings[SettingsKeys.AGENT_Q3]?.text ?: "")
    }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SliderRow(
                label = stringResource(R.string.settings_brightness),
                value = brightness,
                range = 0f..100f,
                onChange = { brightness = it },
            )
            SliderRow(
                label = stringResource(R.string.settings_dim_after),
                value = dimAfter,
                range = 3f..60f,
                onChange = { dimAfter = it },
            )
            SliderRow(
                label = stringResource(R.string.settings_screen_off),
                value = screenOff,
                range = 5f..120f,
                onChange = { screenOff = it },
            )

            buttonKeys.forEach { (key, labelRes) ->
                ActionDropdown(
                    label = stringResource(labelRes),
                    selected = buttons[key] ?: "none",
                    options = ActionNames.ALL,
                    onSelect = { buttons = buttons + (key to it) },
                )
            }

            ActionDropdown(
                label = stringResource(R.string.settings_face),
                selected = face,
                options = FaceNames.ALL,
                labelOf = FaceNames::label,
                onSelect = { face = it },
            )
            ActionDropdown(
                label = stringResource(R.string.settings_clock_font),
                selected = clockFont,
                options = ClockFontNames.ALL,
                labelOf = ClockFontNames::label,
                onSelect = { clockFont = it },
            )

            OutlinedTextField(
                value = theme,
                onValueChange = { theme = it },
                label = { Text(stringResource(R.string.settings_theme)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            ) {
                Text(
                    stringResource(R.string.settings_raise_to_wake),
                    modifier = Modifier.weight(1f),
                )
                Switch(checked = raiseToWake, onCheckedChange = { raiseToWake = it })
            }
            ActionDropdown(
                label = stringResource(R.string.settings_usb_keep_awake),
                selected = usbKeepAwake,
                options = UsbKeepAwakeModes.ALL,
                labelOf = UsbKeepAwakeModes::label,
                onSelect = { usbKeepAwake = it },
            )
            SliderRow(
                label = stringResource(R.string.settings_steps_goal),
                value = stepsGoal,
                range = 1000f..30000f,
                steps = 58,  // 500 刻み
                onChange = { stepsGoal = it },
            )

            OutlinedTextField(
                value = agentQ1,
                onValueChange = { agentQ1 = it },
                label = { Text(stringResource(R.string.agent_q1)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = agentQ2,
                onValueChange = { agentQ2 = it },
                label = { Text(stringResource(R.string.agent_q2)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = agentQ3,
                onValueChange = { agentQ3 = it },
                label = { Text(stringResource(R.string.agent_q3)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                stringResource(R.string.agent_questions_hint),
                style = MaterialTheme.typography.bodySmall,
            )


            Button(
                onClick = {
                    onSave(
                        mapOf(
                            SettingsKeys.BRIGHTNESS to Cbor.Cint(brightness.roundToInt().toLong()),
                            SettingsKeys.DIM_AFTER_S to Cbor.Cint(dimAfter.roundToInt().toLong()),
                            SettingsKeys.SCREEN_OFF_AFTER_S to Cbor.Cint(screenOff.roundToInt().toLong()),
                            SettingsKeys.THEME to Cbor.Ctext(theme),
                            SettingsKeys.RAISE_TO_WAKE to Cbor.Cint(if (raiseToWake) 1 else 0),
                            SettingsKeys.USB_KEEP_AWAKE to Cbor.Cint(usbKeepAwake.toLong()),
                            SettingsKeys.STEPS_GOAL to Cbor.Cint(stepsGoal.roundToInt().toLong()),
                            SettingsKeys.FACE to Cbor.Ctext(face),
                            SettingsKeys.CLOCK_FONT to Cbor.Ctext(clockFont),
                            SettingsKeys.AGENT_Q1 to Cbor.Ctext(agentQ1),
                            SettingsKeys.AGENT_Q2 to Cbor.Ctext(agentQ2),
                            SettingsKeys.AGENT_Q3 to Cbor.Ctext(agentQ3),
                        ) + buttons.mapValues { Cbor.Ctext(it.value) },
                    )
                },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.settings_save))
            }
        }
    }
}

/** スマホ内 Vosk のモデルをダウンロード/展開するカード。 */
@Composable
private fun VoskModelCard(state: VoskModelStore.State, onDownload: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                stringResource(R.string.vosk_title),
                style = MaterialTheme.typography.titleMedium,
            )
            Text(
                stringResource(R.string.vosk_body),
                style = MaterialTheme.typography.bodySmall,
            )
            when (state) {
                is VoskModelStore.State.NotDownloaded -> {
                    Text(stringResource(R.string.vosk_not_downloaded))
                    Button(onClick = onDownload, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(R.string.vosk_download))
                    }
                }
                is VoskModelStore.State.Downloading -> {
                    val mb = state.received / (1024.0 * 1024.0)
                    if (state.total > 0) {
                        val totalMb = state.total / (1024.0 * 1024.0)
                        val pct = (state.received * 100 / state.total).toInt()
                        Text(
                            stringResource(
                                R.string.vosk_downloading_pct,
                                String.format(Locale.US, "%.1f / %.1f MB", mb, totalMb),
                                pct,
                            ),
                        )
                    } else {
                        Text(
                            stringResource(
                                R.string.vosk_downloading,
                                String.format(Locale.US, "%.1f MB", mb),
                                "?",
                            ),
                        )
                    }
                }
                is VoskModelStore.State.Extracting -> {
                    Text(stringResource(R.string.vosk_extracting))
                }
                is VoskModelStore.State.Ready -> {
                    Text(stringResource(R.string.vosk_ready))
                }
                is VoskModelStore.State.Failed -> {
                    Text(
                        stringResource(R.string.vosk_failed, state.message),
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall,
                    )
                    Button(onClick = onDownload, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(R.string.vosk_redownload))
                    }
                }
            }
        }
    }
}

/**
 * AI 設定カード。
 * OpenAI 互換 (muse2api) が主経路。Muse (useapi.net) / Meta realtime ASR は
 * 「上級者向け設定（有料）」を ON にしたときだけ表示する。
 * 秘密値は AgentPrefs (EncryptedSharedPreferences) に保存し、画面ではパスワード表示でマスクする。
 */
@Composable
private fun AgentConfigCard(
    config: AgentConfig,
    busy: Int,
    advanced: Boolean,
    onToggleAdvanced: (Boolean) -> Unit,
    onSave: (AgentConfig) -> Unit,
) {
    var baseUrl by remember(config) { mutableStateOf(config.baseUrl) }
    var apiKey by remember(config) { mutableStateOf(config.apiKey) }
    var chatModel by remember(config) { mutableStateOf(config.chatModel) }
    var sttModel by remember(config) { mutableStateOf(config.sttModel) }
    var museEnabled by remember(config) { mutableStateOf(config.museEnabled) }
    var museToken by remember(config) { mutableStateOf(config.museToken) }
    var museChatId by remember(config) { mutableStateOf(config.museChatId) }
    var museEmail by remember(config) { mutableStateOf(config.museEmail) }
    var metaApiKey by remember(config) { mutableStateOf(config.metaApiKey) }
    var sttMode by remember(config) { mutableStateOf(config.sttMode) }
    var languageBias by remember(config) {
        mutableStateOf(AgentPrefs.encodeList(config.languageBias))
    }
    var keywords by remember(config) { mutableStateOf(AgentPrefs.encodeList(config.asrKeywords)) }

    val urlError = baseUrl.isNotBlank() &&
        !(baseUrl.startsWith("http://") || baseUrl.startsWith("https://"))
    val emailError = museEmail.isNotBlank() && !museEmail.contains('@')

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(
                stringResource(R.string.agent_title),
                style = MaterialTheme.typography.titleMedium,
            )
            Text(
                stringResource(R.string.agent_audio_privacy),
                style = MaterialTheme.typography.bodySmall,
            )
            Text(
                stringResource(R.string.agent_muse_hint),
                style = MaterialTheme.typography.bodySmall,
            )

            // ---- AI 応答 (muse2api / OpenAI 互換) ----
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            ) {
                Text(
                    stringResource(R.string.agent_advanced_toggle),
                    modifier = Modifier.weight(1f),
                )
                Switch(checked = advanced, onCheckedChange = onToggleAdvanced)
            }
            if (advanced) {
                Text(
                    stringResource(R.string.agent_advanced_hint),
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            Text(
                stringResource(R.string.agent_openai_section),
                style = MaterialTheme.typography.titleSmall,
            )
            OutlinedTextField(
                value = baseUrl,
                onValueChange = { baseUrl = it },
                label = { Text(stringResource(R.string.agent_base_url)) },
                placeholder = { Text("https://sas.tail85c8c5.ts.net:8445/v1") },
                singleLine = true,
                isError = urlError,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth(),
            )
            if (urlError) {
                Text(
                    stringResource(R.string.agent_invalid_url),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            OutlinedTextField(
                value = apiKey,
                onValueChange = { apiKey = it },
                label = { Text(stringResource(R.string.agent_api_key)) },
                placeholder = { Text("sk-…") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                stringResource(R.string.agent_key_hint),
                style = MaterialTheme.typography.bodySmall,
            )
            OutlinedTextField(
                value = chatModel,
                onValueChange = { chatModel = it },
                label = { Text(stringResource(R.string.agent_chat_model)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = sttModel,
                onValueChange = { sttModel = it },
                label = { Text(stringResource(R.string.agent_stt_model)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                stringResource(R.string.agent_fallback_hint),
                style = MaterialTheme.typography.bodySmall,
            )

            if (advanced) {
            // ---- Muse (useapi.net) ----
            Text(
                stringResource(R.string.agent_muse_section),
                style = MaterialTheme.typography.titleSmall,
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            ) {
                Text(
                    stringResource(R.string.agent_muse_enabled),
                    modifier = Modifier.weight(1f),
                )
                Switch(checked = museEnabled, onCheckedChange = { museEnabled = it })
            }
            OutlinedTextField(
                value = museToken,
                onValueChange = { museToken = it },
                label = { Text(stringResource(R.string.agent_muse_token)) },
                placeholder = { Text("user:12345-…") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = museChatId,
                onValueChange = { museChatId = it },
                label = { Text(stringResource(R.string.agent_muse_chat_id)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = museEmail,
                onValueChange = { museEmail = it },
                label = { Text(stringResource(R.string.agent_muse_email)) },
                placeholder = { Text("you@example.com") },
                singleLine = true,
                isError = emailError,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                modifier = Modifier.fillMaxWidth(),
            )
            if (emailError) {
                Text(
                    stringResource(R.string.agent_invalid_email),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            // ---- Meta realtime ASR ----
            Text(
                stringResource(R.string.agent_dictation_title),
                style = MaterialTheme.typography.titleSmall,
            )
            Text(
                stringResource(R.string.agent_dictation_body),
                style = MaterialTheme.typography.bodySmall,
            )
            OutlinedTextField(
                value = metaApiKey,
                onValueChange = { metaApiKey = it },
                label = { Text(stringResource(R.string.agent_meta_api_key)) },
                placeholder = { Text("LLM_…") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = languageBias,
                onValueChange = { languageBias = it },
                label = { Text(stringResource(R.string.agent_language_bias)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = keywords,
                onValueChange = { keywords = it },
                label = { Text(stringResource(R.string.agent_keywords)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            }  // if (advanced)

            // STT 方式は無料構成でも選べる (既定: 自動 = スマホ内 Vosk)。
            ActionDropdown(
                label = stringResource(R.string.agent_stt_mode),
                selected = sttMode,
                options = SttModes.ALL,
                labelOf = SttModes::label,
                onSelect = { sttMode = it },
            )

            if (busy > 0) {
                Text(
                    stringResource(R.string.agent_busy),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Button(
                onClick = {
                    onSave(
                        AgentConfig(
                            baseUrl = baseUrl.trim(),
                            apiKey = apiKey.trim(),
                            chatModel = chatModel.trim(),
                            sttModel = sttModel.trim(),
                            museEnabled = museEnabled,
                            museToken = museToken.trim(),
                            museChatId = museChatId.trim(),
                            museEmail = museEmail.trim(),
                            metaApiKey = metaApiKey.trim(),
                            sttMode = sttMode,
                            languageBias = AgentPrefs.decodeList(
                                languageBias,
                                default = listOf("Japanese"),
                            ),
                            asrKeywords = AgentPrefs.decodeList(keywords),
                        ),
                    )
                },
                enabled = !urlError && !emailError,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.agent_save))
            }
        }
    }
}

/** 会話履歴カード (直近10往復、新しいものが下)。 */
@Composable
private fun AgentHistoryCard(
    history: List<AgentEngine.Turn>,
    onClear: () -> Unit,
) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                stringResource(R.string.agent_history_title),
                style = MaterialTheme.typography.titleMedium,
            )
            if (history.isEmpty()) {
                Text(
                    stringResource(R.string.agent_history_empty),
                    style = MaterialTheme.typography.bodySmall,
                )
            } else {
                for (t in history) {
                    Text("Q: ${t.question}", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        "A: ${t.answer}",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(bottom = 4.dp),
                    )
                }
                TextButton(onClick = onClear, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.agent_history_clear))
                }
            }
        }
    }
}

@Composable
private fun SliderRow(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit,
    steps: Int = 0,
) {
    Column {
        Text("$label: ${value.roundToInt()}")
        Slider(value = value, onValueChange = onChange, valueRange = range, steps = steps)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ActionDropdown(
    label: String,
    selected: String,
    options: List<ActionSpec>,
    onSelect: (String) -> Unit,
    labelOf: (String) -> String = ActionNames::label,
) {
    var expanded by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { expanded = it }) {
        OutlinedTextField(
            value = labelOf(selected),
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
            modifier = Modifier
                .fillMaxWidth()
                .menuAnchor(MenuAnchorType.PrimaryNotEditable),
        )
        ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { opt ->
                DropdownMenuItem(
                    text = { Text(opt.labelJa) },
                    onClick = {
                        onSelect(opt.name)
                        expanded = false
                    },
                )
            }
        }
    }
}
