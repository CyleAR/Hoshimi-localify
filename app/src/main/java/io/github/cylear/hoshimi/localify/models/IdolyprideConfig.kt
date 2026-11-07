package io.github.cylear.hoshimi.localify.models

import kotlinx.serialization.Serializable

@Serializable
data class IdolyprideConfig (
    var dbgMode: Boolean = false,
    var enabled: Boolean = true,
    var lazyInit: Boolean = true,
    var replaceFont: Boolean = true,
    var useRuntimeKoreanFont: Boolean = false,
    var replaceImages: Boolean = true,
    var usePhoneSubtitles: Boolean = true,
    var displayUserName: String = "",
    var textTest: Boolean = false,
    var useMasterTrans: Boolean = true,
    var dumpText: Boolean = false,
    var debugImageResourceLog: Boolean = false,
    var forceImagePreserveAspect: Boolean = true,
    var debugMasterDbLog: Boolean = false,
    var debugAudioLog: Boolean = false,
    var gameOrientation: Int = 0,
    var forceExportResource: Boolean = false,
    var enableFreeCamera: Boolean = false,
    var targetFrameRate: Int = 0,
    var unlockAllLive: Boolean = false,
    var skipLiveToResult: Boolean = false,
    var unlockAllLiveCostume: Boolean = false,
    var unlockAllPhotoPose: Boolean = false,
    var enableLiveCustomeDress: Boolean = false,
    var liveCustomeHeadId: String = "",
    var liveCustomeCostumeId: String = "",


    var useCustomeGraphicSettings: Boolean = false,
    var graphicQuality: Int = 0,
    var antiAliasingMode: Int = -1,
    var shadowResolution: Int = 0,
    var renderScale: Float = 0.77f,
    var qualitySettingsLevel: Int = 3,
    var volumeIndex: Int = 3,
    var maxBufferPixel: Int = 3384,
    var reflectionQualityLevel: Int = 4,  // 0~5
    var lodQualityLevel: Int = 4,  // 0~5

    var enableBreastParam: Boolean = false,
    var bUseScale: Boolean = false,
    var bScale: Float = 1.0f,
    var bSwingLevel: Int = 1, // Legacy three-stage setting.
    var bSwingIntensity: Int? = null,

    var pf: Boolean = false
) {
    val breastSwingIntensity: Int
        get() = (bSwingIntensity ?: (bSwingLevel.coerceIn(0, 2) * 2)).coerceIn(0, 4)
}
