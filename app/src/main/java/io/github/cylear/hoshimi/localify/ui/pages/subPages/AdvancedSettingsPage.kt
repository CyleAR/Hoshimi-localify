package io.github.cylear.hoshimi.localify.ui.pages.subPages

import io.github.cylear.hoshimi.localify.ui.components.IPGroupBox
import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.getConfigState
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.ui.components.IPSwitch


@Composable
fun AdvanceSettingsPage(modifier: Modifier = Modifier,
             context: MainActivity? = null,
             previewData: IdolyprideConfig? = null,
             bottomSpacerHeight: Dp = 120.dp,
             screenH: Dp = 1080.dp) {
    val config = getConfigState(context, previewData)
    LazyColumn(modifier = modifier
        .sizeIn(maxHeight = screenH)
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
