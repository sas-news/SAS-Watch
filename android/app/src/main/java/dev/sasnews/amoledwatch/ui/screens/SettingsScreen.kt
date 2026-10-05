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
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.ActionNames
import dev.sasnews.amoledwatch.protocol.ActionSpec
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.ClockFontNames
import dev.sasnews.amoledwatch.protocol.FaceNames
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.protocol.StepsInfo
import dev.sasnews.amoledwatch.protocol.bool
import dev.sasnews.amoledwatch.protocol.int
import dev.sasnews.amoledwatch.protocol.text
import dev.sasnews.amoledwatch.ui.WatchViewModel
import kotlin.math.roundToInt

@Composable
fun SettingsScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val settings by vm.settings.collectAsState()
    val steps by vm.steps.collectAsState()
    val linkState by vm.linkState.collectAsState()
    SettingsContent(
        settings = settings,
        steps = steps,
        connected = linkState is LinkState.Connected,
        onLoad = vm::refresh,
        onSave = vm::saveSettings,
        onRefreshSteps = vm::refreshSteps,
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

        if (settings != null) {
            SettingsEditor(settings = settings, onSave = onSave)
        }
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
    var stepsGoal by remember(settings) {
        mutableFloatStateOf(settings[SettingsKeys.STEPS_GOAL]?.int?.toFloat() ?: 8000f)
    }
    var face by remember(settings) {
        mutableStateOf(settings[SettingsKeys.FACE]?.text ?: "bold")
    }
    var clockFont by remember(settings) {
        mutableStateOf(settings[SettingsKeys.CLOCK_FONT]?.text ?: "auto")
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
            SliderRow(
                label = stringResource(R.string.settings_steps_goal),
                value = stepsGoal,
                range = 1000f..30000f,
                steps = 58,  // 500 刻み
                onChange = { stepsGoal = it },
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
                            SettingsKeys.STEPS_GOAL to Cbor.Cint(stepsGoal.roundToInt().toLong()),
                            SettingsKeys.FACE to Cbor.Ctext(face),
                            SettingsKeys.CLOCK_FONT to Cbor.Ctext(clockFont),
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
