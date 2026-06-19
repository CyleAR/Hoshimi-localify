package io.github.cylear.hoshimi.localify.ui.pages

import android.content.res.Configuration.UI_MODE_NIGHT_NO
import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.TAG
import io.github.cylear.hoshimi.localify.getMainUIConfirmState
import io.github.cylear.hoshimi.localify.getProgramConfigState
import io.github.cylear.hoshimi.localify.getProgramLocalAPIResourceVersionState
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.ui.components.IPGroupConfirm
import io.github.cylear.hoshimi.localify.ui.theme.HoshimiLocalifyTheme


@Composable
fun MainUI(modifier: Modifier = Modifier, context: MainActivity? = null,
           previewData: IdolyprideConfig? = null) {
    var versionInfo by remember {
        mutableStateOf(context?.getVersion() ?: listOf("", "Unknown"))
    }
    // val config = getConfigState(context, previewData)
    val confirmState by getMainUIConfirmState(context, null)
    val programConfig by getProgramConfigState(context)
    val localAPIResourceVersion by getProgramLocalAPIResourceVersionState(context)

    LaunchedEffect(programConfig, localAPIResourceVersion) {
        versionInfo = context?.getVersion() ?: listOf("", "Unknown")
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFFFDFDFD))
    ) {
        val screenH = 1080.dp

        Column(
            modifier = modifier
                .fillMaxSize()
                .padding(10.dp, 10.dp, 10.dp, 0.dp),
            verticalArrangement = Arrangement.Top
        ) {
            Text(text = "Hoshimi Localify ${versionInfo[0]}", fontSize = 18.sp)
            Text(text = "${stringResource(R.string.current_resource_version)}: ${versionInfo[1]}", fontSize = 13.sp)

            SettingsTabs(modifier, listOf(stringResource(R.string.about), stringResource(R.string.home),
                stringResource(R.string.advanced_settings)),
                context = context, previewData = previewData, screenH = screenH)
        }

        if (confirmState.isShow) {
            IPGroupConfirm(
                title = confirmState.title,
                onCancel = { confirmState.onCancel() },
                onConfirm = { confirmState.onConfirm() },
                contentHeightForAnimation = screenH.value * 1.8f
            ) {
                LazyColumn(modifier =
                Modifier.sizeIn(maxHeight = (screenH.value * 0.45f).dp)
                    .fillMaxWidth()) {
                    item {
                        Text(confirmState.content)
                    }
                }
            }
        }
    }
}


@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO, widthDp = 380)
@Composable
fun MainUIPreview(modifier: Modifier = Modifier) {
    val previewConfig = IdolyprideConfig()
    previewConfig.enabled = true

    HoshimiLocalifyTheme {
        MainUI(previewData = previewConfig)
    }
}
