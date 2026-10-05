package dev.sasnews.amoledwatch

import android.content.Context
import android.content.SharedPreferences
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** アプリ設定（時計側の settings とは別物）。 */
class Prefs(context: Context) {

    private val sp: SharedPreferences =
        context.getSharedPreferences("amoledwatch", Context.MODE_PRIVATE)

    private val _enabledPackages = MutableStateFlow(enabledPackages())
    /** 通知転送の対象に選ばれたアプリのパッケージ名。 */
    val enabledPackages: StateFlow<Set<String>> = _enabledPackages

    fun enabledPackages(): Set<String> =
        sp.getStringSet(KEY_ENABLED_PACKAGES, emptySet())?.toSet() ?: emptySet()

    fun setPackageEnabled(pkg: String, enabled: Boolean) {
        val set = enabledPackages().toMutableSet()
        if (enabled) set.add(pkg) else set.remove(pkg)
        sp.edit().putStringSet(KEY_ENABLED_PACKAGES, set).apply()
        _enabledPackages.value = set
    }

    companion object {
        private const val KEY_ENABLED_PACKAGES = "enabled_packages"
    }
}
