package io.github.cylear.hoshimi.localify.ui.pages

import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.annotation.StringRes
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.cylear.hoshimi.localify.MainActivity
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.onClickStartGame
import io.github.cylear.hoshimi.localify.ui.components.IPTabRow
import io.github.cylear.hoshimi.localify.ui.pages.subPages.AboutPage
import io.github.cylear.hoshimi.localify.ui.pages.subPages.AdvanceSettingsPage
import io.github.cylear.hoshimi.localify.ui.pages.subPages.ExtraSettingsPage
import io.github.cylear.hoshimi.localify.ui.pages.subPages.HomePage
import io.github.cylear.hoshimi.localify.ui.pages.subPages.PrivateServerPage

private enum class SettingsPage(@StringRes val title: Int) {
    About(R.string.about),
    Home(R.string.home),
    Advanced(R.string.advanced_settings),
    Extras(R.string.extra_settings),
    PrivateServer(R.string.private_server)
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun SettingsTabs(modifier: Modifier = Modifier,
                 showHiddenPages: Boolean = false,
                 context: MainActivity? = null,
                 previewData: IdolyprideConfig? = null,
                 screenH: Dp = 1080.dp
) {

    val pages = if (showHiddenPages) SettingsPage.entries
        else listOf(SettingsPage.About, SettingsPage.Home, SettingsPage.Advanced)
    val titles = pages.map { stringResource(it.title) }
    val pagerState = rememberPagerState(initialPage = SettingsPage.Home.ordinal, pageCount = { pages.size })

    LaunchedEffect(pages.size) {
        if (pagerState.currentPage >= pages.size) {
            pagerState.scrollToPage(pages.lastIndex)
        }
    }

    Box {
        HorizontalPager(
            state = pagerState,
            modifier = modifier.fillMaxSize(),
            pageSpacing = 10.dp
        ) { page ->
            val settingsPage = pages.getOrNull(page) ?: return@HorizontalPager
            Column(modifier = modifier
                .padding(5.dp)
                .fillMaxHeight(),
                verticalArrangement = Arrangement.Top,
                horizontalAlignment = Alignment.CenterHorizontally) {
                when (settingsPage) {
                    SettingsPage.About -> AboutPage(modifier, context = context, previewData = previewData, screenH = screenH)
                    SettingsPage.Home -> HomePage(modifier, context = context, previewData = previewData, screenH = screenH)
                    SettingsPage.Advanced -> AdvanceSettingsPage(modifier, context = context, previewData = previewData, screenH = screenH)
                    SettingsPage.Extras -> ExtraSettingsPage(modifier, context = context, previewData = previewData, screenH = screenH)
                    SettingsPage.PrivateServer -> PrivateServerPage(modifier, context = context)
                }
            }
        }

        Box(
            Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 6.dp)) {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End) {
                    FloatingActionButton(
                        onClick = { context?.onClickStartGame() },
                        //modifier = Modifier.align(Alignment.End),
                        containerColor = MaterialTheme.colorScheme.primary,
                        shape = CircleShape
                    ) {
                        Icon(imageVector = Icons.AutoMirrored.Filled.KeyboardArrowRight,
                            contentDescription = "StartGame")
                    }
                }

                IPTabRow(modifier, pagerState, titles) { }
            }
        }
    }
}


@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO, heightDp = 760)
@Composable
fun SettingTabsPreview(modifier: Modifier = Modifier) {
    SettingsTabs(modifier, showHiddenPages = true, previewData = IdolyprideConfig())
}
