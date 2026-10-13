package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.DebugInfo
import dev.sasnews.amoledwatch.protocol.SettingsEditor
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.ui.WatchViewModel
import kotlinx.coroutines.delay

/**
 * デバッグ画面 (T13)。
 * debug.info を 5 秒ごとに更新 + settings.get の全キー編集。
 */
@Composable
fun DebugScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val debug by vm.debugInfo.collectAsState()
    val settings by vm.settings.collectAsState()

    // 画面を開いている間だけ 5 秒ポーリング。
    LaunchedEffect(Unit) {
        while (true) {
            vm.refreshDebug()
            delay(5_000)
        }
    }

    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        DebugInfoCard(debug)
        SettingsEditorCard(
            settings = settings ?: emptyMap(),
            onSave = { k, v -> vm.saveSettings(mapOf(k to v)) },
        )
    }
}

@Composable
private fun DebugInfoCard(d: DebugInfo?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text("デバッグ情報", style = MaterialTheme.typography.titleMedium)
            if (d == null) {
                Text("未取得 (未接続)", style = MaterialTheme.typography.bodySmall)
                return@Column
            }
            val rows = buildList {
                add("内蔵 heap" to fmtKb(d.heapInt) + " (min " + fmtKb(d.heapIntMin) + ")")
                add("PSRAM" to fmtKb(d.psram) + " (min " + fmtKb(d.psramMin) + ")")
                add("稼働時間" to fmtUptime(d.uptimeS))
                add("リセット理由" to (d.resetReason?.toString() ?: "-"))
                add("FW" to (d.fw ?: "-"))
                add("画面" to (d.screen ?: "-"))
                add("電池" to (d.battery?.let { "$it%" } ?: "-") +
                    (d.battMv?.let { " (${it}mV)" } ?: ""))
                add("VBUS" to (d.vbusMv?.let { "${it}mV" } ?: "-"))
                add("BLE RSSI" to (d.bleRssi?.let { "${it}dBm" } ?: "-"))
                add("BLE 間隔" to (d.bleItvlMs?.let { "${it}ms" } ?: "-"))
            }
            for ((k, v) in rows) {
                Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Text(k, modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall)
                    Text(v, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

/** 設定の汎用エディタ (T13)。既知キーは日本語ラベル+選択肢、未知は生の値。 */
@Composable
private fun SettingsEditorCard(
    settings: Map<String, Cbor>,
    onSave: (String, Cbor) -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text("設定エディタ", style = MaterialTheme.typography.titleMedium)
            // settings.get の全キー + SettingsKeys.ALL の既知キー (未設定も出す)。
            val keys = (settings.keys + SettingsKeys.ALL).distinct().sorted()
            for (key in keys) {
                SettingRow(
                    key = key,
                    value = settings[key]
                        ?: SettingsKeys.DEFAULTS[key],
                    onSave = { onSave(key, it) },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SettingRow(key: String, value: Cbor?, onSave: (Cbor) -> Unit) {
    if (value == null) return
    val label = SettingsEditor.labelOf(key) ?: key
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodySmall)
            Text(key, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        when (SettingsEditor.kindOf(key, value)) {
            SettingsEditor.Kind.Bool -> {
                val on = (value as? Cbor.Cint)?.value != 0L
                Switch(checked = on, onCheckedChange = {
                    onSave(Cbor.Cint(if (it) 1 else 0))
                })
            }
            SettingsEditor.Kind.Choice -> {
                val choices = SettingsEditor.choices(key) ?: return
                val cur = when (value) {
                    is Cbor.Ctext -> value.value
                    is Cbor.Cint -> value.value.toString()
                    else -> ""
                }
                var expanded by remember { mutableStateOf(false) }
                ExposedDropdownMenuBox(
                    expanded = expanded,
                    onExpandedChange = { expanded = it },
                ) {
                    OutlinedTextField(
                        value = choices.find { it.name == cur }?.labelJa ?: cur,
                        onValueChange = {},
                        readOnly = true,
                        modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable),
                        textStyle = MaterialTheme.typography.bodySmall,
                    )
                    ExposedDropdownMenu(
                        expanded = expanded,
                        onDismissRequest = { expanded = false },
                    ) {
                        for (c in choices) {
                            DropdownMenuItem(
                                text = { Text(c.labelJa) },
                                onClick = {
                                    expanded = false
                                    onSave(
                                        if (value is Cbor.Cint) {
                                            Cbor.Cint(c.name.toLongOrNull() ?: 0)
                                        } else {
                                            Cbor.Ctext(c.name)
                                        },
                                    )
                                },
                            )
                        }
                    }
                }
            }
            SettingsEditor.Kind.Int -> {
                var text by remember(key) {
                    mutableStateOf((value as? Cbor.Cint)?.value?.toString() ?: "")
                }
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    modifier = Modifier.fillMaxWidth(0.45f),
                    textStyle = MaterialTheme.typography.bodySmall,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    trailingIcon = {
                        TextButton(onClick = {
                            text.toLongOrNull()?.let { onSave(Cbor.Cint(it)) }
                        }) { Text("保存") }
                    },
                )
            }
            SettingsEditor.Kind.Text -> {
                var text by remember(key) {
                    mutableStateOf((value as? Cbor.Ctext)?.value ?: "")
                }
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    modifier = Modifier.fillMaxWidth(0.6f),
                    textStyle = MaterialTheme.typography.bodySmall,
                    trailingIcon = {
                        TextButton(onClick = { onSave(Cbor.Ctext(text)) }) {
                            Text("保存")
                        }
                    },
                )
            }
        }
    }
}

private fun fmtKb(v: Long?): String =
    if (v == null) "-" else String.format("%.0fKB", v / 1024.0)

private fun fmtUptime(v: Long?): String {
    if (v == null) return "-"
    val h = v / 3600; val m = (v % 3600) / 60
    return "${h}h ${m}m"
}
