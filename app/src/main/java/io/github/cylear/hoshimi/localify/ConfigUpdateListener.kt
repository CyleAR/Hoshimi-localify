package io.github.cylear.hoshimi.localify

import android.util.Log
import android.view.KeyEvent
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.models.GraphicPresets
import io.github.cylear.hoshimi.localify.models.ProgramConfig
import io.github.cylear.hoshimi.localify.models.ProgramConfigViewModel
import io.github.cylear.hoshimi.localify.models.ProgramConfigViewModelFactory
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.runBlocking


interface ConfigListener {
    fun onEnabledChanged(value: Boolean)
    fun onForceExportResourceChanged(value: Boolean)
    fun onTextTestChanged(value: Boolean)
    fun onUseMasterTransChanged(value: Boolean)
    fun onReplaceFontChanged(value: Boolean)
    fun onUseRuntimeKoreanFontChanged(value: Boolean)
    fun onReplaceImagesChanged(value: Boolean)
    fun onUsePhoneSubtitlesChanged(value: Boolean)
    fun onDisplayUserNameChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onLazyInitChanged(value: Boolean)
    fun onEnableFreeCameraChanged(value: Boolean)
    fun onTargetFpsChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onUnlockAllLiveChanged(value: Boolean)
    fun onSkipLiveToResultChanged(value: Boolean)
    fun onUnlockAllLiveCostumeChanged(value: Boolean)
    fun onUnlockAllPhotoPoseChanged(value: Boolean)
    fun onLiveCustomeDressChanged(value: Boolean)
    fun onLiveCustomeHeadIdChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onLiveCustomeCostumeIdChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onUseCustomeGraphicSettingsChanged(value: Boolean)
    fun onGraphicQualityChanged(value: Int)
    fun onAntiAliasingModeChanged(value: Int)
    fun onShadowResolutionChanged(value: Int)
    fun onRenderScaleChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onQualitySettingsLevelChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onVolumeIndexChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onMaxBufferPixelChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onChangePresetQuality(level: Int)
    fun onReflectionQualityLevelChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onLodQualityLevelChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun onGameOrientationChanged(checkedId: Int)
    fun onDumpTextChanged(value: Boolean)
    fun onDebugImageResourceLogChanged(value: Boolean)
    fun onForceImagePreserveAspectChanged(value: Boolean)
    fun onDebugMasterDbLogChanged(value: Boolean)
    fun onDebugAudioLogChanged(value: Boolean)

    fun onEnableBreastParamChanged(value: Boolean)
    fun onBreastSizeChanged(percent: Int)
    fun onBreastSwingLevelChanged(level: Int)
    fun onPUseBuiltInAssetsChanged(value: Boolean)
    fun onPCleanLocalAssetsChanged(value: Boolean)
    fun mainPageAssetsViewDataUpdate(downloadAbleState: Boolean? = null,
                                     downloadProgressState: Float? = null,
                                     localResourceVersionState: String? = null,
                                     errorString: String? = null,
                                     localAPIResourceVersion: String? = null)
    fun onPUseAPIAssetsChanged(value: Boolean)
    fun onPUseAPIAssetsURLChanged(s: CharSequence, start: Int, before: Int, count: Int)
    fun mainUIConfirmStatUpdate(isShow: Boolean? = null, title: String? = null,
                                content: String? = null,
                                onConfirm: (() -> Unit)? = { mainUIConfirmStatUpdate(isShow = false) },
                                onCancel: (() -> Unit)? = { mainUIConfirmStatUpdate(isShow = false) })
}

class UserConfigViewModelFactory(private val initialValue: IdolyprideConfig) : ViewModelProvider.Factory {
    override fun <T : ViewModel> create(modelClass: Class<T>): T {
        if (modelClass.isAssignableFrom(UserConfigViewModel::class.java)) {
            @Suppress("UNCHECKED_CAST")
            return UserConfigViewModel(initialValue) as T
        }
        throw IllegalArgumentException("Unknown ViewModel class")
    }
}

class UserConfigViewModel(initValue: IdolyprideConfig) : ViewModel() {
    val configState = MutableStateFlow(initValue)
    val config: StateFlow<IdolyprideConfig> = configState.asStateFlow()
}


interface ConfigUpdateListener: ConfigListener, IHasConfigItems {
    var factory: UserConfigViewModelFactory
    var viewModel: UserConfigViewModel

    var programConfigFactory: ProgramConfigViewModelFactory
    var programConfigViewModel: ProgramConfigViewModel

    fun pushKeyEvent(event: KeyEvent): Boolean
    fun checkConfigAndUpdateView() {}  // do nothing
    // fun saveConfig()
    fun saveProgramConfig()


    override fun onEnabledChanged(value: Boolean) {
        config.enabled = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 29))
    }

    override fun onForceExportResourceChanged(value: Boolean) {
        config.forceExportResource = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 30))
    }

    override fun onReplaceFontChanged(value: Boolean) {
        config.replaceFont = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 30))
    }

    override fun onUseRuntimeKoreanFontChanged(value: Boolean) {
        config.useRuntimeKoreanFont = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 30))
    }

    override fun onReplaceImagesChanged(value: Boolean) {
        config.replaceImages = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 30))
    }

    override fun onUsePhoneSubtitlesChanged(value: Boolean) {
        config.usePhoneSubtitles = value
        saveConfig()
        pushKeyEvent(KeyEvent(1145, 30))
    }

    override fun onDisplayUserNameChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.displayUserName = s.toString()
        saveConfig()
    }

    override fun onLazyInitChanged(value: Boolean) {
        config.lazyInit = value
        saveConfig()
    }

    override fun onTextTestChanged(value: Boolean) {
        config.textTest = value
        saveConfig()
    }

    override fun onUseMasterTransChanged(value: Boolean) {
        config.useMasterTrans = value
        saveConfig()
    }

    override fun onDumpTextChanged(value: Boolean) {
        config.dumpText = value
        saveConfig()
    }

    override fun onDebugImageResourceLogChanged(value: Boolean) {
        config.debugImageResourceLog = value
        saveConfig()
    }

    override fun onForceImagePreserveAspectChanged(value: Boolean) {
        config.forceImagePreserveAspect = value
        saveConfig()
    }

    override fun onDebugMasterDbLogChanged(value: Boolean) {
        config.debugMasterDbLog = value
        saveConfig()
    }

    override fun onDebugAudioLogChanged(value: Boolean) {
        config.debugAudioLog = value
        saveConfig()
    }

    override fun onEnableFreeCameraChanged(value: Boolean) {
        config.enableFreeCamera = value
        saveConfig()
    }

    override fun onUnlockAllLiveChanged(value: Boolean) {
        config.unlockAllLive = value
        saveConfig()
    }

    
    override fun onSkipLiveToResultChanged(value: Boolean) {
        config.skipLiveToResult = value
        saveConfig()
    }
override fun onUnlockAllLiveCostumeChanged(value: Boolean) {
        config.unlockAllLiveCostume = value
        saveConfig()
    }

    override fun onUnlockAllPhotoPoseChanged(value: Boolean) {
        config.unlockAllPhotoPose = value
        saveConfig()
    }

    override fun onTargetFpsChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        try {
            val valueStr = s.toString()

            val value = if (valueStr == "") {
                0
            } else {
                valueStr.toInt()
            }
            config.targetFrameRate = value
            saveConfig()
        }
        catch (e: Exception) {
            return
        }
    }

    override fun onLiveCustomeDressChanged(value: Boolean) {
        config.enableLiveCustomeDress = value
        saveConfig()
    }

    override fun onLiveCustomeCostumeIdChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.liveCustomeCostumeId = s.toString()
        saveConfig()
    }

    override fun onUseCustomeGraphicSettingsChanged(value: Boolean) {
        config.useCustomeGraphicSettings = value
        saveConfig()
    }

    override fun onGraphicQualityChanged(value: Int) {
        if (value !in listOf(0, 10, 20, 30, 40, 100)) return
        config.graphicQuality = value
        saveConfig()
    }

    override fun onShadowResolutionChanged(value: Int) {
        if (value !in listOf(0, 512, 1024, 2048, 4096)) return
        config.shadowResolution = value
        saveConfig()
    }

    override fun onAntiAliasingModeChanged(value: Int) {
        if (value !in -1..2) return
        config.antiAliasingMode = value
        saveConfig()
    }

    override fun onRenderScaleChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        val scale = s.toString().toFloatOrNull() ?: return
        if (!scale.isFinite() || scale !in 0.25f..2.0f) return
        config.renderScale = scale
        saveConfig()
    }

    override fun onQualitySettingsLevelChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.qualitySettingsLevel = try {
            s.toString().toInt()
        }
        catch (e: Exception) {
            0
        }
        saveConfig()
    }

    override fun onVolumeIndexChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.volumeIndex = try {
            s.toString().toInt()
        }
        catch (e: Exception) {
            0
        }
        saveConfig()
    }

    override fun onMaxBufferPixelChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        val maxPixels = s.toString().toIntOrNull() ?: return
        if (maxPixels !in 256..4096) return
        config.maxBufferPixel = maxPixels
        saveConfig()
    }

    override fun onLiveCustomeHeadIdChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.liveCustomeHeadId = s.toString()
        saveConfig()
    }

    override fun onReflectionQualityLevelChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.reflectionQualityLevel = try {
            val value = s.toString().toInt()
            if (value > 5) 5 else value
        }
        catch (e: Exception) {
            0
        }
        saveConfig()
    }

    override fun onLodQualityLevelChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        config.lodQualityLevel = try {
            val value = s.toString().toInt()
            if (value > 5) 5 else value
        }
        catch (e: Exception) {
            0
        }
        saveConfig()
    }

    override fun onChangePresetQuality(level: Int) {
        val preset = GraphicPresets.all.getOrNull(level) ?: return
        preset.applyTo(config)
        saveConfig()
    }

    override fun onGameOrientationChanged(checkedId: Int) {
        if (checkedId in listOf(0, 1, 2)) {
            config.gameOrientation = checkedId
        }
        saveConfig()
    }

    override fun onEnableBreastParamChanged(value: Boolean) {
        config.enableBreastParam = value
        saveConfig()
        checkConfigAndUpdateView()
    }

    override fun onBreastSizeChanged(percent: Int) {
        val size = percent.coerceIn(75, 175) / 100f
        if (config.bUseScale && config.bScale == size) return
        config.bUseScale = true
        config.bScale = size
        saveConfig()
    }

    override fun onBreastSwingLevelChanged(level: Int) {
        val swingIntensity = level.coerceIn(0, 4)
        if (config.breastSwingIntensity == swingIntensity) return
        config.bSwingIntensity = swingIntensity
        saveConfig()
    }

    override fun onPUseBuiltInAssetsChanged(value: Boolean) {
        programConfig.useBuiltInAssets = value
        if (value) {
            programConfig.cleanLocalAssets = false
        }
        saveProgramConfig()
    }

    override fun onPCleanLocalAssetsChanged(value: Boolean) {
        programConfig.cleanLocalAssets = value
        if (value) {
            programConfig.useAPIAssets = false
            programConfig.useBuiltInAssets = false
        }
        saveProgramConfig()
    }

    override fun mainPageAssetsViewDataUpdate(downloadAbleState: Boolean?, downloadProgressState: Float?,
                                              localResourceVersionState: String?, errorString: String?,
                                              localAPIResourceVersion: String?) {
        downloadAbleState?.let { programConfigViewModel.downloadAbleState.value = it }
        downloadProgressState?.let{ programConfigViewModel.downloadProgressState.value = it }
        localResourceVersionState?.let{ programConfigViewModel.localResourceVersionState.value = it }
        errorString?.let{ programConfigViewModel.errorStringState.value = it }
        localAPIResourceVersion?.let{ programConfigViewModel.localAPIResourceVersionState.value = it }
    }

    override fun onPUseAPIAssetsChanged(value: Boolean) {
        programConfig.useAPIAssets = value
        if (value) {
            programConfig.useBuiltInAssets = false
            programConfig.cleanLocalAssets = false
        }
        saveProgramConfig()
    }

    override fun onPUseAPIAssetsURLChanged(s: CharSequence, start: Int, before: Int, count: Int) {
        programConfig.useAPIAssetsURL = s.toString()
        saveProgramConfig()
    }

    override fun mainUIConfirmStatUpdate(isShow: Boolean?, title: String?, content: String?,
        onConfirm: (() -> Unit)?, onCancel: (() -> Unit)?
    ) {
        val orig = programConfigViewModel.mainUIConfirmState.value
        isShow?.let {
            if (orig.isShow && it) {
                Log.e(TAG, "Duplicate mainUIConfirmStat")
            }
            orig.isShow = it
        }
        title?.let { orig.title = it }
        content?.let { orig.content = it }
        onConfirm?.let { orig.onConfirm = {
            try {
                it()
            }
            finally {
                mainUIConfirmStatUpdate(isShow = false)
            }
        } }
        onCancel?.let { orig.onCancel = {
            try {
                it()
            }
            finally {
                mainUIConfirmStatUpdate(isShow = false)
            }
        } }
        orig.p = false
        programConfigViewModel.mainUIConfirmState.value = orig.copy(p = true)
    }
}
