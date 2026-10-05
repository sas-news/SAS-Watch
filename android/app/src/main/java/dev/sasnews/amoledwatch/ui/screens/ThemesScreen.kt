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
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.protocol.ThemePackage
import dev.sasnews.amoledwatch.protocol.text
import dev.sasnews.amoledwatch.ui.WatchViewModel

/** 時計のテーマ（内蔵の切替え + ZIP パッケージの BULK 転送）画面。 */

/** テーマ転送の進行状態。 */
sealed interface ThemeTransferState {
    data object Idle : ThemeTransferState
    data class Sending(val sent: Int, val total: Int) : ThemeTransferState
    data class Failed(val reason: String) : ThemeTransferState
    data object Applied : ThemeTransferState
}

/** テーマ1件（内蔵テーマ or 転送するパッケージ）の表示用データ。 */
data class ThemeItemUi(
    val id: String,
    val label: String,
    val applied: Boolean = false,
)

data class ThemesUiState(
    val connected: Boolean,
    val currentTheme: String?,
    val builtins: List<ThemeItemUi>,
    val bundled: ThemePackage.ThemePkgInfo?,
    val picked: ThemePackage.ThemePkgInfo?,
    val pickError: String?,
    val transfer: ThemeTransferState,
)

private val MIN_TAP = 48.dp

@Composable
fun ThemesScreen(vm: WatchViewModel, modifier: Modifier = Modifier) {
    val linkState by vm.linkState.collectAsState()
    val settings by vm.settings.collectAsState()
    val bundled by vm.bundledTheme.collectAsState()
    val picked by vm.pickedTheme.collectAsState()
    val pickError by vm.themePickError.collectAsState()
    val transfer by vm.themeTransfer.collectAsState()

    LaunchedEffect(Unit) { vm.loadBundledTheme() }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) vm.pickTheme(uri)
    }

    val current = settings?.get(SettingsKeys.THEME)?.text
    ThemesContent(
        state = ThemesUiState(
            connected = linkState is LinkState.Connected,
            currentTheme = current,
            builtins = listOf(
                ThemeItemUi(
                    id = "standard",
                    label = stringResource(R.string.themes_builtin_standard),
                    applied = current == "standard",
                ),
                ThemeItemUi(
                    id = "light",
                    label = stringResource(R.string.themes_builtin_light),
                    applied = current == "light",
                ),
            ),
            bundled = bundled,
            picked = picked,
            pickError = pickError,
            transfer = transfer,
        ),
        onApplyBuiltin = vm::applyBuiltinTheme,
        onSendBundled = vm::sendBundledTheme,
        onPickZip = { picker.launch(arrayOf("application/zip")) },
        onSendPicked = vm::sendPickedTheme,
        onRetry = vm::retryThemeSend,
        modifier = modifier,
    )
}

@Composable
fun ThemesContent(
    state: ThemesUiState,
    onApplyBuiltin: (String) -> Unit,
    onSendBundled: () -> Unit,
    onPickZip: () -> Unit,
    onSendPicked: () -> Unit,
    onRetry: () -> Unit,
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
                    stringResource(R.string.themes_not_connected),
                    modifier = Modifier.padding(16.dp),
                )
            }
        }

        // 内蔵テーマの切替え（settings.set theme:<id>）
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(R.string.themes_builtin_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(
                        R.string.themes_current,
                        state.currentTheme ?: stringResource(R.string.themes_unknown),
                    ),
                    style = MaterialTheme.typography.bodyMedium,
                )
                state.builtins.forEach { item ->
                    Text(item.label)
                    Button(
                        onClick = { onApplyBuiltin(item.id) },
                        enabled = state.connected && !item.applied,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.themes_apply))
                    }
                }
            }
        }

        // 同梱サンプル (assets/themes/mame.zip)
        if (state.bundled != null) {
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        stringResource(R.string.themes_sample_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        stringResource(R.string.themes_sample_body, state.bundled.name, state.bundled.id),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(
                        onClick = onSendBundled,
                        enabled = state.connected && state.transfer !is ThemeTransferState.Sending,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.themes_send))
                    }
                }
            }
        }

        // ZIP ファイルから転送
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(R.string.themes_zip_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                OutlinedButton(
                    onClick = onPickZip,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = MIN_TAP),
                ) {
                    Text(stringResource(R.string.themes_pick))
                }
                if (state.picked != null) {
                    Text(
                        stringResource(R.string.themes_picked, state.picked.name, state.picked.id),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(
                        onClick = onSendPicked,
                        enabled = state.connected && state.transfer !is ThemeTransferState.Sending,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.themes_send))
                    }
                }
                if (state.pickError != null) {
                    Text(
                        stringResource(R.string.themes_pick_error, state.pickError),
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        // 転送状態
        when (val t = state.transfer) {
            is ThemeTransferState.Sending -> {
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        val fraction =
                            if (t.total > 0) (t.sent.toFloat() / t.total).coerceIn(0f, 1f) else 0f
                        LinearProgressIndicator(
                            progress = { fraction },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Text(
                            stringResource(
                                R.string.themes_sending,
                                (fraction * 100).toInt(),
                            ),
                        )
                    }
                }
            }
            is ThemeTransferState.Failed -> {
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Text(
                            stringResource(R.string.themes_failed, t.reason),
                            color = MaterialTheme.colorScheme.error,
                        )
                        OutlinedButton(
                            onClick = onRetry,
                            enabled = state.connected,
                            modifier = Modifier
                                .fillMaxWidth()
                                .heightIn(min = MIN_TAP),
                        ) {
                            Text(stringResource(R.string.themes_retry))
                        }
                    }
                }
            }
            ThemeTransferState.Applied -> {
                Card(Modifier.fillMaxWidth()) {
                    Text(
                        stringResource(R.string.themes_applied),
                        modifier = Modifier.padding(16.dp),
                    )
                }
            }
            ThemeTransferState.Idle -> Unit
        }
    }
}
