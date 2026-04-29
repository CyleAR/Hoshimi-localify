package io.github.cylear.hoshimi.localify

import android.app.Activity
import android.content.Intent
import android.widget.Toast
import androidx.core.content.FileProvider
import io.github.cylear.hoshimi.localify.mainUtils.json
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.models.ProgramConfig
import io.github.cylear.hoshimi.localify.models.ProgramConfigSerializer
import kotlinx.serialization.SerializationException
import java.io.File


interface IHasConfigItems {
    var config: IdolyprideConfig
    var programConfig: ProgramConfig

    fun saveConfig() {}  // do nothing
}

interface IConfigurableActivity<T : Activity> : IHasConfigItems


fun <T> T.getConfigContent(): String where T : Activity {
    val configFile = File(filesDir, "ipr-config.json")
    return if (configFile.exists()) {
        configFile.readText()
    } else {
        Toast.makeText(this, "First launch detected, initialising configuration file...", Toast.LENGTH_SHORT).show()
        configFile.writeText("{}")
        "{}"
    }
}

fun <T> T.getProgramConfigContent(
    excludes: List<String> = emptyList(),
    origProgramConfig: ProgramConfig? = null
): String where T : Activity {
    val configFile = File(filesDir, "localify-config.json")
    if (excludes.isEmpty()) {
        return if (configFile.exists()) {
            configFile.readText()
        } else {
            "{}"
        }
    } else {
        return if (origProgramConfig == null) {
            if (configFile.exists()) {
                val parsedConfig = json.decodeFromString<ProgramConfig>(configFile.readText())
                json.encodeToString(ProgramConfigSerializer(excludes), parsedConfig)
            } else {
                "{}"
            }
        } else {
            json.encodeToString(ProgramConfigSerializer(excludes), origProgramConfig)
        }
    }
}

fun <T> T.loadConfig() where T : Activity, T : IHasConfigItems {
    val configStr = getConfigContent()
    config = try {
        json.decodeFromString<IdolyprideConfig>(configStr)
    } catch (e: SerializationException) {
        Toast.makeText(this, "Configuration file error: $e", Toast.LENGTH_SHORT).show()
        IdolyprideConfig()
    }
    saveConfig()

    val programConfigStr = getProgramConfigContent()
    programConfig = try {
        json.decodeFromString<ProgramConfig>(programConfigStr)
    } catch (e: SerializationException) {
        ProgramConfig()
    }
    if (programConfig.useAPIAssetsURL.isEmpty()) {
        programConfig.useAPIAssetsURL = getString(R.string.default_assets_check_api)
    }
}

fun <T> T.onClickStartGame() where T : Activity, T : IHasConfigItems {
    val lastStartPluginVersionFile = File(filesDir, "lastStartPluginVersion.txt")
    val lastStartPluginVersion = if (lastStartPluginVersionFile.exists()) {
        lastStartPluginVersionFile.readText()
    }
    else {
        "null"
    }
    val packInfo = packageManager.getPackageInfo(packageName, 0)
    val version = packInfo.versionName
    val versionCode = packInfo.longVersionCode
    val currentPluginVersion = "$version ($versionCode)"
    if (lastStartPluginVersion != currentPluginVersion) {  // 插件版本更新，强制启用资源更新检查
        lastStartPluginVersionFile.writeText(currentPluginVersion)
        programConfig.checkBuiltInAssets = true
    }

    val intent = Intent().apply {
        setClassName(
            "game.qualiarts.idolypride",
            "com.google.firebase.MessagingUnityPlayerActivity"
        )
        putExtra("iprData", getConfigContent())
        putExtra(
            "localData",
            getProgramConfigContent(listOf("transRemoteZipUrl", "useAPIAssetsURL",
                "localAPIAssetsVersion", "p"), programConfig)
        )
        putExtra("lVerName", version)
        flags = Intent.FLAG_ACTIVITY_NEW_TASK
    }

    val updateFile = File(filesDir, "update_trans.zip")
    val updateAPIFile = File(filesDir, "remote_files/remote.zip")
    val targetFile = if (programConfig.useAPIAssets && updateAPIFile.exists()) {
        updateAPIFile
    }
    else if (programConfig.useRemoteAssets && updateFile.exists()) {
        updateFile
    }
    else {
        null
    }

    if (targetFile != null) {
        val dirUri = FileProvider.getUriForFile(
            this,
            "io.gith.fileprovider",
            File(targetFile.absolutePath)
        )
        // intent.setDataAndType(dirUri, "resource/file")

        grantUriPermission(
            "game.qualiarts.idolypride",
            dirUri,
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        )
        intent.putExtra("resource_file", dirUri)
        // intent.clipData = ClipData.newRawUri("resource_file", dirUri)
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
    }

    startActivity(intent)
}
