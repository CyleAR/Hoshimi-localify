package io.github.cylear.hoshimi.localify.ui.pages.subPages

import io.github.cylear.hoshimi.localify.ui.components.IPGroupBox
import android.content.res.Configuration.UI_MODE_NIGHT_NO
import android.util.Log
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.TAG
import io.github.cylear.hoshimi.localify.getConfigState
import io.github.cylear.hoshimi.localify.getProgramConfigState
import io.github.cylear.hoshimi.localify.getProgramDownloadAbleState
import io.github.cylear.hoshimi.localify.getProgramDownloadErrorStringState
import io.github.cylear.hoshimi.localify.getProgramDownloadState
import io.github.cylear.hoshimi.localify.getProgramLocalAPIResourceVersionState
import io.github.cylear.hoshimi.localify.mainUtils.FileDownloader
import io.github.cylear.hoshimi.localify.mainUtils.RemoteAPIFilesChecker
import io.github.cylear.hoshimi.localify.mainUtils.TimeUtils
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.models.ResourceCollapsibleBoxViewModel
import io.github.cylear.hoshimi.localify.models.ResourceCollapsibleBoxViewModelFactory
import io.github.cylear.hoshimi.localify.ui.components.base.CollapsibleBox
import io.github.cylear.hoshimi.localify.ui.components.IPButton
import io.github.cylear.hoshimi.localify.ui.components.IPProgressBar
import io.github.cylear.hoshimi.localify.ui.components.IPRadio
import io.github.cylear.hoshimi.localify.ui.components.IPSwitch
import io.github.cylear.hoshimi.localify.ui.components.IPTextInput


@Composable
fun HomePage(modifier: Modifier = Modifier,
             context: MainActivity? = null,
             previewData: IdolyprideConfig? = null,
             bottomSpacerHeight: Dp = 120.dp,
             screenH: Dp = 1080.dp) {
    val config = getConfigState(context, previewData)
    val programConfig = getProgramConfigState(context)

    val downloadProgress by getProgramDownloadState(context)
    val downloadAble by getProgramDownloadAbleState(context)
    val localAPIResourceVersion by getProgramLocalAPIResourceVersionState(context)
    val downloadErrorString by getProgramDownloadErrorStringState(context)
    var isFirstTimeInThisPage by rememberSaveable { mutableStateOf(true) }

    // val scrollState = rememberScrollState()
    val keyboardOptionsNumber = remember {
        KeyboardOptions(keyboardType = KeyboardType.Number)
    }

    val resourceSettingsViewModel: ResourceCollapsibleBoxViewModel =
        viewModel(factory = ResourceCollapsibleBoxViewModelFactory(initiallyExpanded = false))

    fun onClickDownload(isHumanClick: Boolean = true) {
        context?.mainPageAssetsViewDataUpdate(
            downloadAbleState = false,
            errorString = "",
            downloadProgressState = -1f
        )
        RemoteAPIFilesChecker.checkUpdateLocalAssets(context!!,
            programConfig.value.useAPIAssetsURL,
            onFailed = { _, reason ->
                context.mainPageAssetsViewDataUpdate(
                    downloadAbleState = true,
                    errorString = "",
                    downloadProgressState = -1f
                )
                context.mainUIConfirmStatUpdate(true, "Error", reason)
            },
            onResult = { data, localVersion ->
                if (!isHumanClick) {
                    if (data.tag_name == localVersion) {
                        context.mainPageAssetsViewDataUpdate(
                            downloadAbleState = true,
                            errorString = "",
                            downloadProgressState = -1f
                        )
                        return@checkUpdateLocalAssets
                    }
                }
                context.mainUIConfirmStatUpdate(true, context.getString(R.string.translation_resource_update),
                    "${data.name}\n$localVersion -> ${data.tag_name}\n${data.body}\n\n${TimeUtils.convertIsoToLocalTime(data.published_at)}",
                    onConfirm = {
                        resourceSettingsViewModel.expanded = true
                        RemoteAPIFilesChecker.updateLocalAssets(context, programConfig.value.useAPIAssetsURL,
                            onDownload = { progress, _, _ ->
                                context.mainPageAssetsViewDataUpdate(downloadProgressState = progress)
                            },
                            onFailed = { _, reason -> context.mainPageAssetsViewDataUpdate(
                                downloadAbleState = true,
                                errorString = reason,
                            )},
                            onSuccess = { saveFile, releaseVersion ->
                                context.mainPageAssetsViewDataUpdate(
                                    downloadAbleState = true,
                                    errorString = "",
                                    downloadProgressState = -1f
                                )
                                context.mainPageAssetsViewDataUpdate(
                                    localAPIResourceVersion = RemoteAPIFilesChecker.getLocalVersion(context)
                                )
                                context.saveProgramConfig()
                                Log.d(TAG, "saved: $releaseVersion $saveFile")
                            })
                    },
                    onCancel = {
                        context.mainPageAssetsViewDataUpdate(
                            downloadAbleState = true,
                            errorString = "",
                            downloadProgressState = -1f
                        )
                    }
                    )
            })
    }

    LaunchedEffect(Unit) {
        try {
            if (context == null) return@LaunchedEffect
            val localAPIResVer = RemoteAPIFilesChecker.getLocalVersion(context)
            context.mainPageAssetsViewDataUpdate(
                localAPIResourceVersion = localAPIResVer
            )
            if (isFirstTimeInThisPage) {
                if (programConfig.value.useAPIAssets && programConfig.value.useAPIAssetsURL.isNotEmpty()) {
                    onClickDownload(false)
                }
            }
        }
        finally {
            isFirstTimeInThisPage = false
        }
    }

    LazyColumn(modifier = modifier
        .sizeIn(maxHeight = screenH)
        // .fillMaxHeight()
        // .verticalScroll(scrollState)
        // .width(IntrinsicSize.Max)
        .fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        item {
            IPGroupBox(modifier = modifier, stringResource(R.string.basic_settings)) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    IPSwitch(modifier, stringResource(R.string.enable_plugin), checked = config.value.enabled) {
                            v -> context?.onEnabledChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.lazy_init), checked = config.value.lazyInit) {
                            v -> context?.onLazyInitChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.replace_font), checked = config.value.replaceFont) {
                            v -> context?.onReplaceFontChanged(v)
                    }

                    IPSwitch(
                        modifier,
                        if (config.value.useRuntimeKoreanFont) {
                            stringResource(R.string.font_mode_runtime)
                        } else {
                            stringResource(R.string.font_mode_builtin)
                        },
                        checked = config.value.useRuntimeKoreanFont
                    ) { v -> context?.onUseRuntimeKoreanFontChanged(v) }

                    IPSwitch(modifier, stringResource(R.string.useMasterDBTrans), checked = config.value.useMasterTrans) {
                            v -> context?.onUseMasterTransChanged(v)
                    }



                    IPSwitch(modifier, stringResource(R.string.replace_images), checked = config.value.replaceImages) {
                            v -> context?.onReplaceImagesChanged(v)
                    }

                    IPSwitch(modifier, stringResource(R.string.use_phone_subtitles), checked = config.value.usePhoneSubtitles) {
                            v -> context?.onUsePhoneSubtitlesChanged(v)
                    }

                    IPTextInput(
                        modifier = modifier
                            .height(45.dp)
                            .fillMaxWidth(),
                        fontSize = 14f,
                        value = config.value.displayUserName,
                        onValueChange = { value -> context?.onDisplayUserNameChanged(value, 0, 0, 0) },
                        label = { Text(stringResource(R.string.display_user_name), fontSize = 12.sp) }
                    )

                }
            }
            Spacer(Modifier.height(6.dp))
        }

        item {
            IPGroupBox(modifier, stringResource(R.string.resource_settings),
                contentPadding = 0.dp,
                onHeadClick = {
                    resourceSettingsViewModel.expanded = !resourceSettingsViewModel.expanded
                }) {
                CollapsibleBox(modifier = modifier,
                    viewModel = resourceSettingsViewModel
                ) {
                    LazyColumn(modifier = modifier
                        // .padding(8.dp)
                        .sizeIn(maxHeight = screenH),
                        // verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        item {
                            Column(
                                modifier = modifier
                                    .fillMaxWidth()
                                    .padding(start = 8.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
                                verticalArrangement = Arrangement.spacedBy(2.dp)
                            ) {
                                Text(
                                    text = "${stringResource(R.string.built_in_resource_version)}: ${context?.getBuiltInResourceVersion() ?: "—"}",
                                    fontSize = 13.sp,
                                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                                )
                                Text(
                                    text = "${stringResource(R.string.api_resource_version)}: ${context?.getAPIResourceVersion() ?: "—"}",
                                    fontSize = 13.sp,
                                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                                )
                            }
                        }

                        item {
                            IPSwitch(modifier = modifier.padding(start = 8.dp, end = 8.dp),
                                checked = programConfig.value.useBuiltInAssets,
                                text = stringResource(id = R.string.use_built_in_resource)
                            ) { v -> context?.onPUseBuiltInAssetsChanged(v) }
                        }
                        item {
                            IPSwitch(modifier = modifier.padding(start = 8.dp, end = 8.dp),
                                checked = programConfig.value.cleanLocalAssets,
                                text = stringResource(id = R.string.delete_plugin_resource)
                            ) { v -> context?.onPCleanLocalAssetsChanged(v) }
                        }

                        item {
                            HorizontalDivider(
                                thickness = 1.dp,
                                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f)
                            )
                        }

                        item {
                            IPSwitch(modifier = modifier.padding(start = 8.dp, end = 8.dp),
                                checked = programConfig.value.useAPIAssets,
                                text = stringResource(R.string.check_resource_from_api)
                            ) { v -> context?.onPUseAPIAssetsChanged(v) }

                            CollapsibleBox(modifier = modifier.graphicsLayer(clip = false),
                                expandState = programConfig.value.useAPIAssets,
                                collapsedHeight = 0.dp,
                                innerPaddingLeftRight = 8.dp,
                                showExpand = false
                            ) {
                                LazyColumn(modifier = modifier
                                    // .padding(8.dp)
                                    .sizeIn(maxHeight = screenH),
                                    verticalArrangement = Arrangement.spacedBy(12.dp)
                                ) {
                                    item {
                                        Row(modifier = modifier.fillMaxWidth(),
                                            horizontalArrangement = Arrangement.spacedBy(2.dp),
                                            verticalAlignment = Alignment.CenterVertically) {

                                            IPTextInput(modifier = modifier
                                                .height(45.dp)
                                                .padding(end = 8.dp)
                                                .fillMaxWidth()
                                                .weight(1f),
                                                fontSize = 14f,
                                                value = programConfig.value.useAPIAssetsURL,
                                                onValueChange = { c -> context?.onPUseAPIAssetsURLChanged(c, 0, 0, 0)},
                                                label = { Text(stringResource(R.string.api_addr)) }
                                            )

                                            if (downloadAble) {
                                                IPButton(modifier = modifier
                                                    .height(40.dp)
                                                    .sizeIn(minWidth = 80.dp),
                                                    text = stringResource(R.string.check_update),
                                                    onClick = { onClickDownload() })
                                            }
                                            else {
                                                IPButton(modifier = modifier
                                                    .height(40.dp)
                                                    .sizeIn(minWidth = 80.dp),
                                                    text = stringResource(id = R.string.cancel), onClick = {
                                                        FileDownloader.cancel()
                                                    })
                                            }

                                        }
                                    }

                                    if (downloadProgress >= 0) {
                                        item {
                                            IPProgressBar(progress = downloadProgress, isError = downloadErrorString.isNotEmpty())
                                        }
                                    }

                                    if (downloadErrorString.isNotEmpty()) {
                                        item {
                                            Text(text = downloadErrorString, color = Color(0xFFE2041B))
                                        }
                                    }

                                    item {
                                        Text(modifier = Modifier
                                            .fillMaxWidth()
                                            .clickable {
                                                context?.mainPageAssetsViewDataUpdate(
                                                    localAPIResourceVersion = RemoteAPIFilesChecker.getLocalVersion(
                                                        context
                                                    )
                                                )
                                            }, text = "${stringResource(R.string.downloaded_resource_version)}: $localAPIResourceVersion")
                                    }

                                    item {
                                        Spacer(Modifier.height(0.dp))
                                    }

                                }

                            }
                        }

                    }
                }
            }

            Spacer(Modifier.height(6.dp))
        }

        item {
            IPGroupBox(modifier = modifier, contentPadding = 0.dp, title = stringResource(R.string.graphic_settings)) {
                LazyColumn(modifier = Modifier
                    .sizeIn(maxHeight = screenH),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    item {
                        Spacer(modifier = Modifier.height(8.dp))
                        IPTextInput(modifier = modifier
                            .padding(start = 4.dp, end = 4.dp)
                            .height(45.dp)
                            .fillMaxWidth(),
                            fontSize = 14f,
                            value = config.value.targetFrameRate.toString(),
                            onValueChange = { c -> context?.onTargetFpsChanged(c, 0, 0, 0)},
                            label = { Text(stringResource(R.string.setFpsTitle)) },
                            keyboardOptions = keyboardOptionsNumber)
                    }

                    item {
                        Column(modifier = Modifier.padding(start = 8.dp, end = 8.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(stringResource(R.string.orientation_lock))
                            Row(modifier = modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                val radioModifier = remember {
                                    modifier
                                        .height(40.dp)
                                        .weight(1f)
                                }

                                IPRadio(modifier = radioModifier,
                                    text = stringResource(R.string.orientation_orig), selected = config.value.gameOrientation == 0,
                                    onClick = { context?.onGameOrientationChanged(0) })

                                IPRadio(modifier = radioModifier,
                                    text = stringResource(R.string.orientation_portrait), selected = config.value.gameOrientation == 1,
                                    onClick = { context?.onGameOrientationChanged(1) })

                                IPRadio(modifier = radioModifier,
                                    text = stringResource(R.string.orientation_landscape), selected = config.value.gameOrientation == 2,
                                    onClick = { context?.onGameOrientationChanged(2) })
                            }
                        }
                    }

                }

            }

            Spacer(Modifier.height(6.dp))
        }

        item {
            IPButton(
                modifier = modifier
                    .height(40.dp)
                    .fillMaxWidth(),
                text = stringResource(R.string.reset_settings),
                onClick = {
                    context?.mainUIConfirmStatUpdate(
                        true,
                        context.getString(R.string.reset_settings),
                        context.getString(R.string.reset_settings_confirm),
                        onConfirm = { context.resetSettings() }
                    )
                }
            )
        }

        item {
            Spacer(modifier = modifier.height(bottomSpacerHeight))
        }
    }
}


@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO)
@Composable
fun HomePagePreview(modifier: Modifier = Modifier, data: IdolyprideConfig = IdolyprideConfig()) {
    HomePage(modifier, previewData = data)
}
