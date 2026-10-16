package io.github.cylear.hoshimi.localify.mainUtils

import android.content.Context
import io.github.cylear.hoshimi.localify.BuildConfig
import io.github.cylear.hoshimi.localify.TargetGamePackages
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

object BuiltInResourceVersion {
    private const val versionPath = "assets/hoshimi-local/version.txt"

    fun get(context: Context): String? {
        val gamePackage = TargetGamePackages.findInstalled(context)
        val gameInfo = gamePackage?.let { context.packageManager.getPackageInfo(it, 0) }
        val managerInfo = context.packageManager.getPackageInfo(context.packageName, 0)
        val gamePath = gameInfo?.applicationInfo?.sourceDir
        val cacheKey = listOf(
            gamePackage, gamePath, gameInfo?.lastUpdateTime,
            managerInfo.applicationInfo?.sourceDir, managerInfo.lastUpdateTime
        ).joinToString("|")
        val preferences = context.getSharedPreferences("built_in_resource_version", Context.MODE_PRIVATE)
        if (preferences.getString("source", null) == cacheKey) {
            return preferences.getString("version", null)?.ifEmpty { null }
        }

        val version = gamePath?.let { readEmbeddedVersion(it) }
            ?: runCatching {
                context.assets.open("hoshimi-local/version.txt").bufferedReader().use {
                    it.readText().trim().ifEmpty { null }
                }
            }.getOrNull()
        preferences.edit().putString("source", cacheKey).putString("version", version.orEmpty()).apply()
        return version
    }

    internal fun readEmbeddedVersion(apkPath: String): String? {
        ZipFile(apkPath).use { gameApk ->
            val patchConfig = gameApk.getEntry("assets/lspatch/config.json") ?: return null
            val useManager = gameApk.getInputStream(patchConfig).bufferedReader().use {
                json.parseToJsonElement(it.readText()).jsonObject["useManager"]?.jsonPrimitive?.booleanOrNull
            }
            if (useManager != false) return null

            val module = gameApk.getEntry("assets/lspatch/modules/${BuildConfig.APPLICATION_ID}.apk")
                ?: return null
            ZipInputStream(gameApk.getInputStream(module).buffered()).use { moduleApk ->
                while (true) {
                    val entry = moduleApk.nextEntry ?: return null
                    if (entry.name == versionPath) {
                        return moduleApk.bufferedReader().readText().trim().ifEmpty { null }
                    }
                }
            }
        }
        return null
    }
}
