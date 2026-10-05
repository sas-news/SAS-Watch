package dev.sasnews.amoledwatch.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.core.content.ContextCompat
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.ui.screens.DevicesScreen
import dev.sasnews.amoledwatch.ui.screens.NotifyScreen
import dev.sasnews.amoledwatch.ui.screens.SettingsScreen
import dev.sasnews.amoledwatch.ui.theme.AmoledWatchTheme

class MainActivity : ComponentActivity() {

    private val vm: WatchViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            AmoledWatchTheme {
                App(vm)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        vm.refreshNotifAccess()
    }
}

enum class Tab { DEVICES, SETTINGS, NOTIFY }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun App(vm: WatchViewModel) {
    val context = LocalContext.current
    var tab by remember { mutableIntStateOf(Tab.DEVICES.ordinal) }
    val snackbar = remember { SnackbarHostState() }
    val notice by vm.notice.collectAsState()

    LaunchedEffect(notice) {
        notice?.let {
            snackbar.showSnackbar(it)
            vm.consumeNotice()
        }
    }

    // ---- 権限 ----
    val required = remember {
        buildList {
            if (Build.VERSION.SDK_INT >= 31) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            }
            if (Build.VERSION.SDK_INT >= 33) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }.toTypedArray()
    }
    var permissionsGranted by remember {
        mutableStateOf(
            required.all {
                ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
            },
        )
    }
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { result ->
        permissionsGranted = result.values.all { it }
    }

    Scaffold(
        topBar = {
            TopAppBar(title = { Text(stringResource(R.string.app_name)) })
        },
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == Tab.DEVICES.ordinal,
                    onClick = { tab = Tab.DEVICES.ordinal },
                    label = { Text(stringResource(R.string.tab_devices)) },
                    icon = {},
                )
                NavigationBarItem(
                    selected = tab == Tab.SETTINGS.ordinal,
                    onClick = { tab = Tab.SETTINGS.ordinal },
                    label = { Text(stringResource(R.string.tab_settings)) },
                    icon = {},
                )
                NavigationBarItem(
                    selected = tab == Tab.NOTIFY.ordinal,
                    onClick = { tab = Tab.NOTIFY.ordinal },
                    label = { Text(stringResource(R.string.tab_notify)) },
                    icon = {},
                )
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        when (Tab.entries[tab]) {
            Tab.DEVICES -> DevicesScreen(
                vm = vm,
                permissionsGranted = permissionsGranted,
                onRequestPermissions = { permLauncher.launch(required) },
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
            )
            Tab.SETTINGS -> SettingsScreen(
                vm = vm,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
            )
            Tab.NOTIFY -> NotifyScreen(
                vm = vm,
                onOpenNotifSettings = {
                    context.startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                },
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
            )
        }
    }
}
