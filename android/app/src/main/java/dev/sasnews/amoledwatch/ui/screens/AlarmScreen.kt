package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.AlarmEntry
import dev.sasnews.amoledwatch.ui.WatchViewModel

private val DOW_LABELS = listOf("日", "月", "火", "水", "木", "金", "土")

/** dow ビット (bit0=日..bit6=土, 0=毎日) を「月火金」のような表記にする。 */
fun dowLabel(dow: Int): String {
    if (dow == 0) return "毎日"
    return (0..6).filter { dow and (1 shl it) != 0 }.joinToString("") { DOW_LABELS[it] }
}

@Composable
fun AlarmScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val link by vm.linkState.collectAsState()
    val alarms by vm.alarms.collectAsState()
    AlarmContent(
        connected = link is LinkState.Connected,
        alarms = alarms,
        onRefresh = vm::refreshAlarms,
        onSave = vm::alarmSave,
        onDelete = vm::alarmDelete,
        modifier = modifier,
    )
}

/** 時計のアラーム (alarm.list/alarm.set/alarm.delete) を編集する画面。 */
@Composable
fun AlarmContent(
    connected: Boolean,
    alarms: List<AlarmEntry>?,
    onRefresh: () -> Unit,
    onSave: (id: Int, hour: Int, min: Int, dow: Int, on: Boolean) -> Unit,
    onDelete: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    // 編集中のアラーム id (0=新規, null=閉じている)
    var editingId by remember { mutableStateOf<Int?>(null) }
    var eHour by remember { mutableIntStateOf(7) }
    var eMin by remember { mutableIntStateOf(0) }
    var eDow by remember { mutableIntStateOf(0) }
    var eOn by remember { mutableStateOf(true) }

    fun openEditor(entry: AlarmEntry?) {
        editingId = entry?.id ?: 0
        eHour = entry?.hour ?: 7
        eMin = entry?.min ?: 0
        eDow = entry?.dow ?: 0
        eOn = entry?.on ?: true
    }

    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!connected) {
            Text(
                stringResource(R.string.alarm_not_connected),
                style = MaterialTheme.typography.bodyMedium,
            )
            return@Column
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                stringResource(R.string.alarm_list_title),
                style = MaterialTheme.typography.titleMedium,
            )
            OutlinedButton(onClick = onRefresh) {
                Text(stringResource(R.string.alarm_refresh))
            }
        }

        if (alarms == null) {
            Text(
                stringResource(R.string.alarm_loading),
                style = MaterialTheme.typography.bodyMedium,
            )
        } else if (alarms.isEmpty() && editingId == null) {
            Text(
                stringResource(R.string.alarm_list_empty),
                style = MaterialTheme.typography.bodyMedium,
            )
        }

        alarms?.forEach { a ->
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column {
                            Text(
                                "%d:%02d".format(a.hour, a.min),
                                style = MaterialTheme.typography.headlineMedium,
                            )
                            Text(
                                dowLabel(a.dow),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                        Switch(
                            checked = a.on,
                            onCheckedChange = { onSave(a.id, a.hour, a.min, a.dow, it) },
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = { openEditor(a) }) {
                            Text(stringResource(R.string.alarm_edit))
                        }
                        OutlinedButton(onClick = { onDelete(a.id) }) {
                            Text(stringResource(R.string.alarm_delete))
                        }
                    }
                }
            }
        }

        Button(
            onClick = { openEditor(null) },
            enabled = editingId == null && (alarms?.size ?: 0) < MAX_ALARMS,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.alarm_add))
        }

        editingId?.let { editId ->
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(
                        stringResource(
                            if (editId == 0) R.string.alarm_add_title
                            else R.string.alarm_edit_title,
                        ),
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        NumberField(
                            label = stringResource(R.string.alarm_hour),
                            value = eHour,
                            range = 0..23,
                            onChange = { eHour = it },
                        )
                        Text(":", style = MaterialTheme.typography.headlineMedium)
                        NumberField(
                            label = stringResource(R.string.alarm_minute),
                            value = eMin,
                            range = 0..59,
                            onChange = { eMin = it },
                        )
                    }
                    DowPicker(dow = eDow, onChange = { eDow = it })
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Switch(checked = eOn, onCheckedChange = { eOn = it })
                        Text(
                            stringResource(
                                if (eOn) R.string.alarm_enabled else R.string.alarm_disabled,
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = {
                            onSave(editId, eHour, eMin, eDow, eOn)
                            editingId = null
                        }) {
                            Text(stringResource(R.string.alarm_save))
                        }
                        OutlinedButton(onClick = { editingId = null }) {
                            Text(stringResource(R.string.alarm_cancel))
                        }
                    }
                }
            }
        }
    }
}

private const val MAX_ALARMS = 5

/** 数値入力 (範囲外は入らない)。 */
@Composable
private fun NumberField(
    label: String,
    value: Int,
    range: IntRange,
    onChange: (Int) -> Unit,
) {
    var text by remember(value) { mutableStateOf(value.toString()) }
    OutlinedTextField(
        value = text,
        onValueChange = { s ->
            text = s.filter { it.isDigit() }.take(2)
            text.toIntOrNull()?.let { if (it in range) onChange(it) }
        },
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = Modifier.width(96.dp),
    )
}

/** 曜日繰り返しの選択。全部OFF = 毎日 (dow=0)。 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun DowPicker(dow: Int, onChange: (Int) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(
            stringResource(R.string.alarm_dow_hint),
            style = MaterialTheme.typography.bodySmall,
        )
        FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            DOW_LABELS.forEachIndexed { i, d ->
                val bit = 1 shl i
                FilterChip(
                    selected = dow and bit != 0,
                    onClick = { onChange(dow xor bit) },
                    label = { Text(d) },
                )
            }
        }
    }
}
