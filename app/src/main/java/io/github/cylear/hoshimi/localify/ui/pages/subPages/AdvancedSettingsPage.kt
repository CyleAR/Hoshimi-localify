package io.github.cylear.hoshimi.localify.ui.pages.subPages

import io.github.cylear.hoshimi.localify.ui.components.IPGroupBox
import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.getConfigState
import io.github.cylear.hoshimi.localify.models.BreastCollapsibleBoxViewModel
import io.github.cylear.hoshimi.localify.models.BreastCollapsibleBoxViewModelFactory
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.ui.components.base.CollapsibleBox
import io.github.cylear.hoshimi.localify.ui.components.IPSwitch
import io.github.cylear.hoshimi.localify.ui.components.IPTextInput
import kotlin.math.roundToInt


@Composable
fun AdvanceSettingsPage(modifier: Modifier = Modifier,
             context: MainActivity? = null,
             previewData: IdolyprideConfig? = null,
             bottomSpacerHeight: Dp = 120.dp,
             screenH: Dp = 1080.dp) {
    val config = getConfigState(context, previewData)
    // val scrollState = rememberScrollState()

    val breastParamViewModel: BreastCollapsibleBoxViewModel =
        viewModel(factory = BreastCollapsibleBoxViewModelFactory(initiallyExpanded = false))

    LazyColumn(modifier = modifier
        .sizeIn(maxHeight = screenH)
        // .fillMaxHeight()
        // .verticalScroll(scrollState)
        .fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        item {
            IPGroupBox(modifier, stringResource(R.string.debug_settings)) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    IPSwitch(modifier, stringResource(R.string.text_hook_test_mode), checked = config.value.textTest) {
                            v -> context?.onTextTestChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.export_text), checked = config.value.dumpText) {
                            v -> context?.onDumpTextChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.debug_image_resource_log), checked = config.value.debugImageResourceLog) {
                            v -> context?.onDebugImageResourceLogChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.force_image_preserve_aspect), checked = config.value.forceImagePreserveAspect) {
                            v -> context?.onForceImagePreserveAspectChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.debug_masterdb_log), checked = config.value.debugMasterDbLog) {
                            v -> context?.onDebugMasterDbLogChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.debug_audio_log), checked = config.value.debugAudioLog) {
                            v -> context?.onDebugAudioLogChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.force_export_resource), checked = config.value.forceExportResource) {
                            v -> context?.onForceExportResourceChanged(v)
                    }

                }
            }

            Spacer(Modifier.height(6.dp))
        }


        item {
            if (config.value.dbgMode) {
                IPGroupBox(modifier, stringResource(R.string.camera_settings)) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        IPSwitch(modifier, stringResource(R.string.enable_free_camera), checked = config.value.enableFreeCamera) {
                                v -> context?.onEnableFreeCameraChanged(v)
                        }
                    }
                }

                Spacer(Modifier.height(6.dp))
            }
        }

        item {
            if (config.value.dbgMode) {
                IPGroupBox(modifier, stringResource(R.string.breast_param),
                contentPadding = 0.dp,
                onHeadClick = {
                    breastParamViewModel.expanded = !breastParamViewModel.expanded
                }) {
                CollapsibleBox(modifier = modifier,
                    viewModel = breastParamViewModel
                ) {
                    Column(
                        modifier = modifier.padding(8.dp).fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        val size = config.value.bScale
                            .takeIf { config.value.bUseScale && it.isFinite() } ?: 1f
                        val percent = (size.coerceIn(0.75f, 1.75f) * 100).roundToInt()
                        IPSwitch(
                            modifier = modifier,
                            checked = config.value.enableBreastParam,
                            text = stringResource(R.string.enable_breast_param)
                        ) { context?.onEnableBreastParamChanged(it) }
                        Text(stringResource(R.string.breast_size_percent, percent))
                        Slider(
                            value = percent.toFloat(),
                            onValueChange = { context?.onBreastSizeChanged(it.roundToInt()) },
                            enabled = config.value.enableBreastParam,
                            valueRange = 75f..175f,
                            steps = 19
                        )
                        val intensity = config.value.breastSwingIntensity
                        val intensityLabels = listOf(
                            R.string.breast_swing_game_default,
                            R.string.breast_swing_light,
                            R.string.breast_swing_strong,
                            R.string.breast_swing_stronger,
                            R.string.breast_swing_maximum
                        )
                        Text(stringResource(
                            R.string.breast_swing_level,
                            intensity + 1,
                            stringResource(intensityLabels[intensity])
                        ))
                        Slider(
                            value = intensity.toFloat(),
                            onValueChange = { context?.onBreastSwingLevelChanged(it.roundToInt()) },
                            enabled = config.value.enableBreastParam,
                            valueRange = 0f..4f,
                            steps = 3
                        )
                        Text(
                            stringResource(R.string.breast_auto_hint),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }
        }

        }

        item {
            if (config.value.dbgMode) {
                Spacer(Modifier.height(6.dp))

                IPGroupBox(modifier, stringResource(R.string.test_mode_live)) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        IPSwitch(modifier, stringResource(R.string.unlockAllLive),
                            checked = config.value.unlockAllLive) {
                                v -> context?.onUnlockAllLiveChanged(v)
                        }
                        IPSwitch(modifier, stringResource(R.string.skip_live_to_result),
                            checked = config.value.skipLiveToResult) {
                                v -> context?.onSkipLiveToResultChanged(v)
                        }
                        IPSwitch(modifier, stringResource(R.string.unlockAllLiveCostume),
                            checked = config.value.unlockAllLiveCostume) {
                                v -> context?.onUnlockAllLiveCostumeChanged(v)
                        }
                        IPSwitch(modifier, stringResource(R.string.unlockAllPhotoPose),
                            checked = config.value.unlockAllPhotoPose) {
                                v -> context?.onUnlockAllPhotoPoseChanged(v)
                        }

                        /*
                        HorizontalDivider(
                            thickness = 1.dp,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f)
                        )
                        IPSwitch(modifier, stringResource(R.string.liveUseCustomeDress),
                            checked = config.value.enableLiveCustomeDress) {
                                v -> context?.onLiveCustomeDressChanged(v)
                        }
                        IPTextInput(modifier = modifier
                            .height(45.dp)
                            .fillMaxWidth(),
                            fontSize = 14f,
                            value = config.value.liveCustomeHeadId,
                            onValueChange = { c -> context?.onLiveCustomeHeadIdChanged(c, 0, 0, 0)},
                            label = { Text(stringResource(R.string.live_costume_head_id),
                                fontSize = 12.sp) }
                        )
                        IPTextInput(modifier = modifier
                            .height(45.dp)
                            .fillMaxWidth(),
                            fontSize = 14f,
                            value = config.value.liveCustomeCostumeId,
                            onValueChange = { c -> context?.onLiveCustomeCostumeIdChanged(c, 0, 0, 0)},
                            label = { Text(stringResource(R.string.live_custome_dress_id)) }
                        )*/
                    }
                }
            }
        }

        item {
            Spacer(modifier = modifier.height(bottomSpacerHeight))
        }
    }
}


@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO)
@Composable
fun AdvanceSettingsPagePreview(modifier: Modifier = Modifier, data: IdolyprideConfig = IdolyprideConfig()) {
    AdvanceSettingsPage(modifier, previewData = data)
}
