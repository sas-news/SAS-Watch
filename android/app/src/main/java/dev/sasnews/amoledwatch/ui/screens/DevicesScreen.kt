package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.BuildConfig
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.DeviceInfo
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.HelloResult
import dev.sasnews.amoledwatch.protocol.MediaCmd
import dev.sasnews.amoledwatch.ui.WatchViewModel

/** スキャンで見つかったデバイスの表示用データ。 */
data class FoundUi(val name: String?, val address: String)

/** DevicesContent が描画に使う全部の状態（スクショテストしやすいよう分離）。 */
data class DevicesUiState(
    val permissionsGranted: Boolean,
    val linkState: LinkState,
    val scanning: Boolean,
    val found: List<FoundUi>,
    val hello: HelloResult?,
    val deviceInfo: DeviceInfo?,
    val events: List<String>,
    val log: List<String>,
    val isFake: Boolean,
    val debugBuild: Boolean,
)

@Composable
fun DevicesScreen(
    vm: WatchViewModel,
    permissionsGranted: Boolean,
    onRequestPermissions: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val linkState by vm.linkState.collectAsState()
    val scanning by vm.scanning.collectAsState()
    val found by vm.found.collectAsState()
    val hello by vm.hello.collectAsState()
    val deviceInfo by vm.deviceInfo.collectAsState()
    val events by vm.recentEvents.collectAsState()
    val log by vm.log.collectAsState()
    val isFake by vm.isFakeLink.collectAsState()

    DevicesContent(
        state = DevicesUiState(
            permissionsGranted = permissionsGranted,
            linkState = linkState,
            scanning = scanning,
            found = found.map { FoundUi(it.name, it.device.address) },
            hello = hello,
            deviceInfo = deviceInfo,
            events = events,
            log = log,
            isFake = isFake,
            debugBuild = BuildConfig.DEBUG,
        ),
        onRequestPermissions = onRequestPermissions,
        onScanToggle = { if (scanning) vm.stopScan() else vm.startScan() },
        onConnect = { addr ->
            found.firstOrNull { it.device.address == addr }?.let { vm.connect(it.device) }
        },
        onConnectFake = vm::connectFake,
        onDisconnect = vm::disconnect,
        onRefresh = vm::refresh,
        onTimerStart = vm::timerStart,
        onTimerStop = vm::timerStop,
        onMemoSend = vm::memoSend,
        onFakeBattery = vm::fakeBattery,
        onFakeMediaCmd = vm::fakeMediaCmd,
        modifier = modifier,
    )
}

@Composable
fun DevicesContent(
    state: DevicesUiState,
    onRequestPermissions: () -> Unit,
    onScanToggle: () -> Unit,
    onConnect: (String) -> Unit,
    onConnectFake: () -> Unit,
    onDisconnect: () -> Unit,
    onRefresh: () -> Unit,
    onTimerStart: (Int) -> Unit,
    onTimerStop: () -> Unit,
    onMemoSend: (String) -> Unit,
    onFakeBattery: () -> Unit,
    onFakeMediaCmd: (MediaCmd) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!state.permissionsGranted) {
            PermissionCard(onRequestPermissions)
        }

        ConnectionCard(state, onDisconnect)

        if (state.linkState is LinkState.Disconnected) {
            ScanCard(state, onScanToggle, onConnect, onConnectFake)
        }

        if (state.linkState is LinkState.Connected) {
            HelloCard(state.hello)
            DeviceInfoCard(state.deviceInfo, onRefresh)
            TimerCard(onTimerStart, onTimerStop)
            MemoCard(onMemoSend)
            if (state.isFake) {
                FakeDemoCard(onFakeBattery, onFakeMediaCmd)
            }
            EventsCard(state.events)
        }

        LogCard(state.log)
    }
}

@Composable
private fun PermissionCard(onRequestPermissions: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.perm_needed_title), style = MaterialTheme.typography.titleMedium)
            Text(stringResource(R.string.perm_needed_body))
            Text(stringResource(R.string.perm_notification), style = MaterialTheme.typography.bodySmall)
            Button(onClick = onRequestPermissions, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.perm_grant))
            }
        }
    }
}

@Composable
private fun ConnectionCard(state: DevicesUiState, onDisconnect: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            val text = when (val s = state.linkState) {
                is LinkState.Connected -> stringResource(R.string.connected_to)
                is LinkState.Connecting -> stringResource(R.string.connecting_to, s.attempt)
                is LinkState.Bonding -> stringResource(R.string.bonding)
                is LinkState.Disconnected -> stringResource(R.string.disconnected)
            }
            Text(text, style = MaterialTheme.typography.titleMedium)
            if (state.linkState !is LinkState.Disconnected) {
                OutlinedButton(onClick = onDisconnect, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.disconnect))
                }
            }
        }
    }
}

@Composable
private fun ScanCard(
    state: DevicesUiState,
    onScanToggle: () -> Unit,
    onConnect: (String) -> Unit,
    onConnectFake: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = onScanToggle,
                enabled = state.permissionsGranted,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(if (state.scanning) R.string.scan_stop else R.string.scan_start))
            }
            if (state.scanning) {
                Text(stringResource(R.string.scanning), style = MaterialTheme.typography.bodySmall)
            }
            if (!state.scanning && state.found.isEmpty()) {
                Text(stringResource(R.string.no_devices), style = MaterialTheme.typography.bodySmall)
            }
            state.found.forEach { dev ->
                HorizontalDivider()
                Column(Modifier.fillMaxWidth()) {
                    Text(dev.name ?: dev.address, style = MaterialTheme.typography.titleSmall)
                    Text(dev.address, style = MaterialTheme.typography.bodySmall)
                    Button(
                        onClick = { onConnect(dev.address) },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(stringResource(R.string.connect))
                    }
                }
            }
            if (state.debugBuild) {
                HorizontalDivider()
                Text(stringResource(R.string.fake_watch_card), style = MaterialTheme.typography.titleSmall)
                Text(stringResource(R.string.fake_watch_body), style = MaterialTheme.typography.bodySmall)
                OutlinedButton(onClick = onConnectFake, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.fake_connect))
                }
            }
        }
    }
}

@Composable
private fun HelloCard(hello: HelloResult?) {
    if (hello == null) return
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(stringResource(R.string.hello_fw, hello.fw, hello.proto))
            Text(
                stringResource(R.string.hello_caps, hello.caps.joinToString(", ")),
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun DeviceInfoCard(info: DeviceInfo?, onRefresh: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.device_info_title), style = MaterialTheme.typography.titleMedium)
            if (info != null) {
                Text(
                    stringResource(
                        R.string.device_info_battery,
                        info.battery,
                        stringResource(if (info.charging) R.string.charging else R.string.not_charging),
                    ),
                )
                Text(
                    stringResource(R.string.device_info_heap, info.freeHeap, info.freePsram),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            OutlinedButton(onClick = onRefresh, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.refresh_info))
            }
        }
    }
}

@Composable
private fun TimerCard(onStart: (Int) -> Unit, onStop: () -> Unit) {
    var seconds by remember { mutableStateOf("60") }
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.timer_title), style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = seconds,
                onValueChange = { seconds = it.filter(Char::isDigit).take(6) },
                label = { Text(stringResource(R.string.timer_seconds_label)) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = { seconds.toIntOrNull()?.let { if (it > 0) onStart(it) } },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.timer_start))
            }
            OutlinedButton(onClick = onStop, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.timer_stop))
            }
        }
    }
}

@Composable
private fun MemoCard(onSend: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.memo_title), style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = text,
                onValueChange = { text = it.take(200) },
                label = { Text(stringResource(R.string.memo_hint)) },
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = { if (text.isNotBlank()) onSend(text) },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.memo_send))
            }
        }
    }
}

@Composable
private fun FakeDemoCard(onBattery: () -> Unit, onMediaCmd: (MediaCmd) -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.fake_demo_title), style = MaterialTheme.typography.titleMedium)
            OutlinedButton(onClick = onBattery, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.fake_demo_battery))
            }
            OutlinedButton(
                onClick = { onMediaCmd(MediaCmd.PLAY_PAUSE) },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.fake_demo_media))
            }
        }
    }
}

@Composable
private fun EventsCard(events: List<String>) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(stringResource(R.string.events_title), style = MaterialTheme.typography.titleMedium)
            if (events.isEmpty()) {
                Text(stringResource(R.string.no_events), style = MaterialTheme.typography.bodySmall)
            } else {
                events.takeLast(10).reversed().forEach {
                    Text(it, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
private fun LogCard(log: List<String>) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("ログ", style = MaterialTheme.typography.titleMedium)
            log.takeLast(8).reversed().forEach {
                Text(it, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
