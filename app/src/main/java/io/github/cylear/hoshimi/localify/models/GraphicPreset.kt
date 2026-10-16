package io.github.cylear.hoshimi.localify.models

data class GraphicPreset(val quality: Int, val renderScale: Float, val maxBufferPixel: Int, val shadowResolution: Int, val antiAliasingMode: Int = -1) {
    fun matches(config: IdolyprideConfig): Boolean =
        config.antiAliasingMode == antiAliasingMode && config.shadowResolution == shadowResolution && config.graphicQuality == quality && config.renderScale == renderScale &&
            config.maxBufferPixel == maxBufferPixel

    fun applyTo(config: IdolyprideConfig) {
        config.graphicQuality = quality
        config.antiAliasingMode = antiAliasingMode
        config.shadowResolution = shadowResolution
        config.renderScale = renderScale
        config.maxBufferPixel = maxBufferPixel
        config.useCustomeGraphicSettings = true
    }
}

object GraphicPresets {
    val all = listOf(
        GraphicPreset(10, 0.5f, 1024, 512),
        GraphicPreset(20, 0.67f, 1440, 1024),
        GraphicPreset(30, 0.85f, 2160, 1024),
        GraphicPreset(40, 1.0f, 4096, 2048),
        GraphicPreset(100, 1.5f, 4096, 4096, antiAliasingMode = 2)
    )
}
