package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.ui.WatchViewModel

/** 時計の Wi-Fi 設定画面 (BLE wifi.set → 時計の NVS に保存)。 */

data class WifiUiState(
    val connected: Boolean,
    val savedSsid: String?,
)

private val WIFI_MIN_TAP = 48.dp

@Composable
fun WifiScreen(vm: WatchViewModel, onBack: () -> Unit, modifier: Modifier = Modifier) {
    val linkState by vm.linkState.collectAsState()
    val wifiInfo by vm.wifiInfo.collectAsState()

    LaunchedEffect(Unit) { vm.refreshWifiInfo() }

    WifiContent(
        state = WifiUiState(
            connected = linkState is dev.sasnews.amoledwatch.connection.LinkState.Connected,
            savedSsid = wifiInfo?.takeIf { it.configured }?.ssid,
        ),
        onSave = { ssid, pass -> vm.wifiSet(ssid, pass) },
        onBack = onBack,
        modifier = modifier,
    )
}

@Composable
fun WifiContent(
    state: WifiUiState,
    onSave: (String, String) -> Unit,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var ssid by remember { mutableStateOf("") }
    var pass by remember { mutableStateOf("") }
    val passOk = pass.isEmpty() || pass.length in 8..63
    val canSave = state.connected && ssid.isNotEmpty() && passOk

    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    stringResource(R.string.wifi_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(R.string.wifi_body),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    stringResource(
                        R.string.wifi_current,
                        state.savedSsid ?: stringResource(R.string.wifi_not_set),
                    ),
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedTextField(
                    value = ssid,
                    onValueChange = { if (it.length <= 32) ssid = it },
                    label = { Text(stringResource(R.string.wifi_ssid)) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = pass,
                    onValueChange = { if (it.length <= 63) pass = it },
                    label = { Text(stringResource(R.string.wifi_pass)) },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    stringResource(R.string.wifi_pass_hint),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Button(
                    onClick = { onSave(ssid, pass) },
                    enabled = canSave,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = WIFI_MIN_TAP),
                ) {
                    Text(stringResource(R.string.wifi_save))
                }
            }
        }

        OutlinedButton(
            onClick = onBack,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = WIFI_MIN_TAP),
        ) {
            Text(stringResource(R.string.wifi_back))
        }
    }
}
