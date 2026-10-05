package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.ui.WatchViewModel

@Composable
fun NotifyScreen(
    vm: WatchViewModel,
    onOpenNotifSettings: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val hasAccess by vm.notifAccess.collectAsState()
    val enabled by vm.enabledPackages.collectAsState()
    val nowPlaying by vm.nowPlaying.collectAsState()
    val apps = remember { vm.installedApps() }
    NotifyContent(
        hasAccess = hasAccess,
        enabledPackages = enabled,
        apps = apps,
        nowPlaying = nowPlaying,
        onToggle = vm::togglePackage,
        onOpenNotifSettings = onOpenNotifSettings,
        modifier = modifier,
    )
}

data class NotifyUiState(
    val hasAccess: Boolean,
    val enabledPackages: Set<String>,
)

/** 通知転送とメディア連携の設定画面。 */
@Composable
fun NotifyContent(
    hasAccess: Boolean,
    enabledPackages: Set<String>,
    apps: List<Pair<String, String>>,
    nowPlaying: MediaBridge.NowPlaying?,
    onToggle: (String, Boolean) -> Unit,
    onOpenNotifSettings: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.notif_access_title), style = MaterialTheme.typography.titleMedium)
                if (hasAccess) {
                    Text(stringResource(R.string.notif_access_granted))
                } else {
                    Text(stringResource(R.string.notif_access_body))
                    OutlinedButton(onClick = onOpenNotifSettings, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(R.string.notif_access_open))
                    }
                }
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.media_title), style = MaterialTheme.typography.titleMedium)
                Text(stringResource(R.string.media_body), style = MaterialTheme.typography.bodySmall)
                if (!hasAccess) {
                    Text(stringResource(R.string.media_no_access), style = MaterialTheme.typography.bodySmall)
                } else if (nowPlaying != null) {
                    Text(
                        stringResource(R.string.media_now_playing, nowPlaying.title, nowPlaying.artist),
                    )
                } else {
                    Text(stringResource(R.string.media_idle), style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(R.string.notif_apps_title), style = MaterialTheme.typography.titleMedium)
                if (enabledPackages.isEmpty()) {
                    Text(
                        stringResource(R.string.notif_apps_none),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                apps.forEach { (pkg, label) ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Checkbox(
                            checked = enabledPackages.contains(pkg),
                            onCheckedChange = { onToggle(pkg, it) },
                        )
                        Column {
                            Text(label)
                            Text(pkg, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
    }
}
