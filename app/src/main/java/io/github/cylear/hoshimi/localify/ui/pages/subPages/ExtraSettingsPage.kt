package io.github.cylear.hoshimi.localify.ui.pages.subPages

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.getConfigState
import io.github.cylear.hoshimi.localify.models.BreastCollapsibleBoxViewModel
import io.github.cylear.hoshimi.localify.models.BreastCollapsibleBoxViewModelFactory
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.ui.components.IPGroupBox
import io.github.cylear.hoshimi.localify.ui.components.IPSwitch
import io.github.cylear.hoshimi.localify.ui.components.base.CollapsibleBox
import kotlin.math.roundToInt

@Composable
fun ExtraSettingsPage(
    modifier: Modifier = Modifier,
    context: MainActivity? = null,
    previewData: IdolyprideConfig? = null,
    screenH: Dp = 1080.dp
) {
    val config by getConfigState(context, previewData)
    val breastParamViewModel: BreastCollapsibleBoxViewModel =
        viewModel(factory = BreastCollapsibleBoxViewModelFactory(initiallyExpanded = false))

    LazyColumn(
        modifier = modifier.sizeIn(maxHeight = screenH).fillMaxWidth(),
        contentPadding = PaddingValues(bottom = 120.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        item {
            IPGroupBox(modifier, stringResource(R.string.camera_settings)) {
                IPSwitch(modifier, stringResource(R.string.enable_free_camera), checked = config.enableFreeCamera) {
                    context?.onEnableFreeCameraChanged(it)
                }
            }
        }

        item {
            IPGroupBox(
                modifier,
                stringResource(R.string.breast_param),
                contentPadding = 0.dp,
                onHeadClick = { breastParamViewModel.expanded = !breastParamViewModel.expanded }
            ) {
                CollapsibleBox(modifier = modifier, viewModel = breastParamViewModel) {
                    Column(
                        modifier = modifier.padding(8.dp).fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        val size = config.bScale.takeIf { config.bUseScale && it.isFinite() } ?: 1f
                        val percent = (size.coerceIn(0.75f, 1.75f) * 100).roundToInt()
                        IPSwitch(
                            modifier = modifier,
                            checked = config.enableBreastParam,
                            text = stringResource(R.string.enable_breast_param)
                        ) { context?.onEnableBreastParamChanged(it) }
                        Text(stringResource(R.string.breast_size_percent, percent))
                        Slider(
                            value = percent.toFloat(),
                            onValueChange = { context?.onBreastSizeChanged(it.roundToInt()) },
                            enabled = config.enableBreastParam,
                            valueRange = 75f..175f,
                            steps = 19
                        )
                        val intensity = config.breastSwingIntensity
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
                            enabled = config.enableBreastParam,
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

        item {
            IPGroupBox(modifier, stringResource(R.string.test_mode_live)) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    IPSwitch(modifier, stringResource(R.string.skip_live_to_result), checked = config.skipLiveToResult) {
                        context?.onSkipLiveToResultChanged(it)
                    }
                    IPSwitch(modifier, stringResource(R.string.unlockAllLiveCostume), checked = config.unlockAllLiveCostume) {
                        context?.onUnlockAllLiveCostumeChanged(it)
                    }
                    AnimatedVisibility(
                        visible = config.unlockAllLiveCostume,
                        enter = expandVertically() + fadeIn(),
                        exit = shrinkVertically() + fadeOut()
                    ) {
                        Column(
                            modifier = Modifier.fillMaxWidth().padding(start = 16.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp)
                        ) {
                            IPSwitch(
                                modifier = modifier,
                                text = stringResource(R.string.unlockCrossCharacterCostume),
                                checked = config.unlockCrossCharacterCostume
                            ) {
                                context?.onUnlockCrossCharacterCostumeChanged(it)
                            }
                            IPSwitch(
                                modifier = modifier,
                                text = stringResource(R.string.unlockCrossCharacterHair),
                                checked = config.unlockCrossCharacterHair
                            ) {
                                context?.onUnlockCrossCharacterHairChanged(it)
                            }
                        }
                    }
                    IPSwitch(modifier, stringResource(R.string.unlockAllPhotoPose), checked = config.unlockAllPhotoPose) {
                        context?.onUnlockAllPhotoPoseChanged(it)
                    }
                }
            }
        }
    }
}
