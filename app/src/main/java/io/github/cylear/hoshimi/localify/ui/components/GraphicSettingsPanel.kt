package io.github.cylear.hoshimi.localify.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.cylear.hoshimi.localify.ConfigListener
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.models.GraphicPresets
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig

@Composable
@OptIn(ExperimentalMaterial3Api::class)
fun GraphicSettingsPanel(config: IdolyprideConfig, listener: ConfigListener?) {
    var showHelp by rememberSaveable { mutableStateOf(false) }
    var presetRevision by remember { mutableIntStateOf(0) }
    val labels = listOf(
        R.string.graphic_quality_low, R.string.graphic_quality_middle,
        R.string.graphic_quality_high, R.string.graphic_quality_very_high,
        R.string.graphic_quality_ultra
    )
    val presetsDescending = GraphicPresets.all.asReversed()
    Column(
        Modifier.fillMaxWidth().padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        IPTextInput(
            value = config.targetFrameRate.toString(),
            onValueChange = { listener?.onTargetFpsChanged(it, 0, 0, 0) },
            modifier = Modifier.fillMaxWidth().height(45.dp),
            fontSize = 14f,
            label = { Text(stringResource(R.string.setFpsTitle), fontSize = 12.sp) },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        Text(stringResource(R.string.orientation_lock), style = MaterialTheme.typography.bodyMedium)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            listOf(
                0 to R.string.orientation_orig,
                1 to R.string.orientation_portrait,
                2 to R.string.orientation_landscape
            ).forEach { (orientation, label) ->
                IPRadio(
                    text = stringResource(label),
                    selected = config.gameOrientation == orientation,
                    modifier = Modifier.weight(1f).height(40.dp),
                    onClick = { listener?.onGameOrientationChanged(orientation) }
                )
            }
        }
        HorizontalDivider()
        IPSwitch(
            text = stringResource(R.string.graphic_quality_toggle),
            checked = config.useCustomeGraphicSettings
        ) { listener?.onUseCustomeGraphicSettingsChanged(it) }
        if (!config.useCustomeGraphicSettings) return@Column
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val columns = if (maxWidth >= 340.dp) 5 else 3
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                GraphicPresets.all.indices.reversed().toList().chunked(columns).forEach { row ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        row.forEach { index ->
                            IPButton(
                                text = stringResource(labels[index]),
                                modifier = Modifier.weight(1f).height(40.dp),
                                onClick = {
                                    listener?.onChangePresetQuality(index)
                                    presetRevision++
                                }
                            )
                        }
                    }
                }
            }
        }
        val matchedPreset = GraphicPresets.all.indexOfFirst { it.matches(config) }
        val selectedLabel = when {
            matchedPreset >= 0 -> labels[matchedPreset]
            else -> R.string.graphic_custom
        }
        Text(
            "${stringResource(R.string.graphic_quality_title)}: ${stringResource(selectedLabel)}",
            style = MaterialTheme.typography.bodySmall
        )
        key(presetRevision) {
            Row(
                Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.Bottom
            ) {
                IPTextInput(
                    value = config.renderScale.toString(),
                    onValueChange = { listener?.onRenderScaleChanged(it, 0, 0, 0) },
                    modifier = Modifier.weight(1f).height(45.dp),
                    fontSize = 14f,
                    label = {
                        Text(
                            "${stringResource(R.string.graphic_scale_short)}\n(${presetsDescending.joinToString("/") { it.renderScale.toString() }})",
                            fontSize = 10.sp, lineHeight = 11.sp
                        )
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
                IPTextInput(
                    value = config.maxBufferPixel.toString(),
                    onValueChange = { listener?.onMaxBufferPixelChanged(it, 0, 0, 0) },
                    modifier = Modifier.weight(1f).height(45.dp),
                    fontSize = 14f,
                    label = {
                        Text(
                            "${stringResource(R.string.graphic_max_short)}\n(${presetsDescending.joinToString("/") { it.maxBufferPixel.toString() }})",
                            fontSize = 10.sp, lineHeight = 11.sp
                        )
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
            }
            Row(
                Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.Bottom
            ) {
                val qualityOptions = listOf(0 to R.string.graphic_quality_original) +
                    GraphicPresets.all.indices.reversed().map { GraphicPresets.all[it].quality to labels[it] }
                var qualityMenuExpanded by remember { mutableStateOf(false) }
                ExposedDropdownMenuBox(
                    expanded = qualityMenuExpanded,
                    onExpandedChange = { qualityMenuExpanded = it },
                    modifier = Modifier.weight(1f)
                ) {
                    val qualityLabel = qualityOptions.firstOrNull { it.first == config.graphicQuality }?.second
                    OutlinedTextFieldNoPadding(
                        value = qualityLabel?.let { stringResource(it) } ?: config.graphicQuality.toString(),
                        onValueChange = {},
                        readOnly = true,
                        singleLine = true,
                        modifier = Modifier.menuAnchor().fillMaxWidth().height(45.dp),
                        textStyle = TextStyle(fontSize = 14.sp),
                        shape = RoundedCornerShape(topStart = 4.dp, topEnd = 16.dp, bottomEnd = 4.dp, bottomStart = 16.dp),
                        label = {
                            Text(
                                stringResource(R.string.graphic_quality_input),
                                fontSize = 10.sp, lineHeight = 11.sp
                            )
                        },
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = qualityMenuExpanded) }
                    )
                    ExposedDropdownMenu(
                        expanded = qualityMenuExpanded,
                        onDismissRequest = { qualityMenuExpanded = false }
                    ) {
                        qualityOptions.forEach { (quality, label) ->
                            DropdownMenuItem(
                                text = { Text(if (quality == 0) stringResource(label) else "${stringResource(label)} ($quality)") },
                                onClick = {
                                    listener?.onGraphicQualityChanged(quality)
                                    qualityMenuExpanded = false
                                }
                            )
                        }
                    }
                }
                var shadowMenuExpanded by remember { mutableStateOf(false) }
                ExposedDropdownMenuBox(
                    expanded = shadowMenuExpanded,
                    onExpandedChange = { shadowMenuExpanded = it },
                    modifier = Modifier.weight(1f)
                ) {
                    OutlinedTextFieldNoPadding(
                        value = if (config.shadowResolution == 0) stringResource(R.string.graphic_quality_original)
                                else config.shadowResolution.toString(),
                        onValueChange = {},
                        readOnly = true,
                        singleLine = true,
                        modifier = Modifier.menuAnchor().fillMaxWidth().height(45.dp),
                        textStyle = TextStyle(fontSize = 14.sp),
                        shape = RoundedCornerShape(topStart = 4.dp, topEnd = 16.dp, bottomEnd = 4.dp, bottomStart = 16.dp),
                        label = { Text(stringResource(R.string.graphic_shadow_title), fontSize = 10.sp) },
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = shadowMenuExpanded) }
                    )
                    ExposedDropdownMenu(
                        expanded = shadowMenuExpanded,
                        onDismissRequest = { shadowMenuExpanded = false }
                    ) {
                        listOf(0, 512, 1024, 2048, 4096).forEach { resolution ->
                            DropdownMenuItem(
                                text = { Text(if (resolution == 0) stringResource(R.string.graphic_quality_original) else resolution.toString()) },
                                onClick = {
                                    listener?.onShadowResolutionChanged(resolution)
                                    shadowMenuExpanded = false
                                }
                            )
                        }
                    }
                }
            }
        }
        if (config.graphicQuality == 100) {
            Text(
                stringResource(R.string.graphic_ultra_short),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
        val aaOptions = listOf(
            -1 to R.string.graphic_aa_default,
            0 to R.string.graphic_aa_off,
            1 to R.string.graphic_aa_fxaa,
            2 to R.string.graphic_aa_smaa
        )
        Text(stringResource(R.string.graphic_aa_title), style = MaterialTheme.typography.bodyMedium)
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val columns = if (maxWidth >= 340.dp) 4 else 2
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                aaOptions.chunked(columns).forEach { options ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        options.forEach { (mode, label) ->
                            IPRadio(
                                text = stringResource(if (mode == 2 && columns == 4) R.string.graphic_aa_smaa_short else label),
                                selected = config.antiAliasingMode == mode,
                                compact = columns == 4,
                                fontSize = if (columns == 4) 12.sp else 14.sp,
                                modifier = Modifier.weight(1f).height(40.dp),
                                onClick = { listener?.onAntiAliasingModeChanged(mode) }
                            )
                        }
                    }
                }
            }
        }
        IPButton(
            text = stringResource(if (showHelp) R.string.graphic_help_hide else R.string.graphic_help_show),
            modifier = Modifier.fillMaxWidth().height(40.dp),
            onClick = { showHelp = !showHelp }
        )
        if (showHelp) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.graphic_quality_hint), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.render_resolution_scale), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.render_resolution_max), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.graphic_aa_hint), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.graphic_shadow_hint), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.render_resolution_hint), style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
