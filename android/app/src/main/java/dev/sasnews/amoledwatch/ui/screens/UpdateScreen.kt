package dev.sasnews.amoledwatch.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.OtaStatusInfo
import dev.sasnews.amoledwatch.ui.WatchViewModel

/** ファーム更新画面 (GitHub Releases → HTTPS OTA、またはファイル選択 → BLE BULK)。 */

/** BLE ファーム転送の進行状態。 */
sealed interface FirmwareSendState {
    data object Idle : FirmwareSendState
    data class Sending(val sent: Int, val total: Int) : FirmwareSendState
    data object Done : FirmwareSendState
    data object Failed : FirmwareSendState
}

/** 確認済みリリースの表示用 (URL/sha256 は ViewModel 側に持たせる)。 */
data class ReleaseUi(val tag: String, val binUrl: String)

data class UpdateUiState(
    val connected: Boolean,
    val fw: String?,
    val release: ReleaseUi?,
    val releaseLoading: Boolean,
    val releaseError: Boolean,
    val ota: OtaStatusInfo?,
    val send: FirmwareSendState,
    val pickedName: String?,
)

private val MIN_TAP = 48.dp

@Composable
fun UpdateScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val linkState by vm.linkState.collectAsState()
    val deviceInfo by vm.deviceInfo.collectAsState()
    val release by vm.release.collectAsState()
    val releaseLoading by vm.releaseLoading.collectAsState()
    val releaseError by vm.releaseError.collectAsState()
    val ota by vm.otaStatus.collectAsState()
    val send by vm.firmwareSend.collectAsState()
    val picked by vm.pickedFirmware.collectAsState()

    var showWifi by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) { vm.refreshOtaStatus() }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) {
            val name = uri.lastPathSegment?.substringAfterLast('/')
            vm.pickFirmware(uri, name)
        }
    }

    if (showWifi) {
        WifiScreen(vm = vm, onBack = { showWifi = false }, modifier = modifier)
        return
    }

    UpdateContent(
        state = UpdateUiState(
            connected = linkState is LinkState.Connected,
            fw = deviceInfo?.fw,
            release = release,
            releaseLoading = releaseLoading,
            releaseError = releaseError,
            ota = ota,
            send = send,
            pickedName = picked,
        ),
        onOpenWifi = { showWifi = true },
        onFetchRelease = vm::fetchLatestRelease,
        onStartHttps = vm::startHttpsOta,
        onPickFirmware = { picker.launch(arrayOf("application/octet-stream")) },
        onSendFirmware = vm::sendPickedFirmware,
        modifier = modifier,
    )
}

/** stage の日本語表示 (protocol-v1.md)。 */
@Composable
fun stageLabel(stage: String): String = when (stage) {
    "wifi" -> stringResource(R.string.update_stage_wifi)
    "download" -> stringResource(R.string.update_stage_download)
    "verify" -> stringResource(R.string.update_stage_verify)
    "done" -> stringResource(R.string.update_stage_done)
    "reboot" -> stringResource(R.string.update_stage_reboot)
    "fail" -> stringResource(R.string.update_stage_fail)
    else -> stringResource(R.string.update_stage_idle)
}

@Composable
fun UpdateContent(
    state: UpdateUiState,
    onOpenWifi: () -> Unit,
    onFetchRelease: () -> Unit,
    onStartHttps: () -> Unit,
    onPickFirmware: () -> Unit,
    onSendFirmware: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!state.connected) {
            Card(Modifier.fillMaxWidth()) {
                Text(
                    stringResource(R.string.update_not_connected),
                    modifier = Modifier.padding(16.dp),
                )
            }
        }

        // 現在のバージョン + Wi-Fi 設定への導線
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(
                        R.string.update_current,
                        state.fw ?: stringResource(R.string.update_unknown),
                    ),
                    style = MaterialTheme.typography.titleMedium,
                )
                OutlinedButton(
                    onClick = onOpenWifi,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = MIN_TAP),
                ) {
                    Text(stringResource(R.string.update_wifi_open))
                }
            }
        }

        // GitHub Releases → HTTPS OTA
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(R.string.update_release_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(R.string.update_release_body),
                    style = MaterialTheme.typography.bodyMedium,
                )
                OutlinedButton(
                    onClick = onFetchRelease,
                    enabled = !state.releaseLoading,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = MIN_TAP),
                ) {
                    Text(
                        if (state.releaseLoading) {
                            stringResource(R.string.update_checking)
                        } else {
                            stringResource(R.string.update_check)
                        },
                    )
                }
                if (state.release != null) {
                    Text(
                        stringResource(R.string.update_latest, state.release.tag),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(
                        onClick = onStartHttps,
                        enabled = state.connected && state.ota?.active != true,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.update_start_https))
                    }
                }
                if (state.releaseError) {
                    Text(
                        stringResource(R.string.update_release_error),
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        // ファイル選択 → BLE BULK
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(R.string.update_ble_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(R.string.update_ble_body),
                    style = MaterialTheme.typography.bodyMedium,
                )
                OutlinedButton(
                    onClick = onPickFirmware,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = MIN_TAP),
                ) {
                    Text(stringResource(R.string.update_pick))
                }
                if (state.pickedName != null) {
                    Text(
                        stringResource(R.string.update_picked, state.pickedName),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(
                        onClick = onSendFirmware,
                        enabled = state.connected &&
                            state.send !is FirmwareSendState.Sending,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.update_send_ble))
                    }
                }
                when (val s = state.send) {
                    is FirmwareSendState.Sending -> {
                        val fraction =
                            if (s.total > 0) (s.sent.toFloat() / s.total).coerceIn(0f, 1f) else 0f
                        LinearProgressIndicator(
                            progress = { fraction },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Text(
                            stringResource(
                                R.string.update_sending,
                                (fraction * 100).toInt(),
                            ),
                        )
                    }
                    FirmwareSendState.Done -> Text(
                        stringResource(R.string.update_ble_done),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    FirmwareSendState.Failed -> Text(
                        stringResource(R.string.update_ble_failed),
                        color = MaterialTheme.colorScheme.error,
                    )
                    FirmwareSendState.Idle -> Unit
                }
            }
        }

        // 時計側の OTA セッション進捗
        if (state.ota != null && state.ota.stage != "idle") {
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        stringResource(R.string.update_progress_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(stageLabel(state.ota.stage))
                    LinearProgressIndicator(
                        progress = { (state.ota.pct / 100f).coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text(
                        if (state.ota.version.isNotEmpty()) {
                            stringResource(
                                R.string.update_progress_pct,
                                state.ota.version,
                                state.ota.pct,
                            )
                        } else {
                            stringResource(R.string.update_progress_pct_nover, state.ota.pct)
                        },
                    )
                    if (state.ota.msg.isNotEmpty()) {
                        Text(
                            state.ota.msg,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
        }
    }
}
