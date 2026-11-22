package io.github.cylear.hoshimi.localify.ui.components

import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.text
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import androidx.compose.material3.Surface
import androidx.compose.material3.TabRowDefaults.tabIndicatorOffset
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.runtime.*
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.pager.rememberPagerState


@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
fun IPTabRow(
    modifier: Modifier = Modifier,
    pagerState: PagerState,
    tabs: List<String>,
    onTabSelected: (index: Int) -> Unit
) {
    if (tabs.isEmpty()) return
    val selectedIndex = pagerState.currentPage.coerceIn(tabs.indices)
    val coroutineScope = rememberCoroutineScope()
    LaunchedEffect(selectedIndex) {
        onTabSelected(selectedIndex)
    }

    Box(
        modifier = Modifier
            .shadow(4.dp, RoundedCornerShape(16.dp))
    ) {
        Surface(
            modifier = modifier
                .clip(RoundedCornerShape(16.dp))
                .shadow(4.dp),
            shape = RoundedCornerShape(16.dp),
        ) {
            Column {
                TabRow(
                    modifier = modifier.background(Color.Transparent),
                    containerColor = Color.Transparent,
                    selectedTabIndex = selectedIndex,
                    indicator = @Composable { tabPositions ->
                        Box(
                            Modifier
                                .tabIndicatorOffset(tabPositions[selectedIndex])
                                .height(4.dp)
                                .background(Color(0xFF1428ff))
                                .padding(horizontal = 4.dp)
                        )
                    }
                ) {
                    tabs.forEachIndexed { index, title ->
                        Tab(
                            selected = selectedIndex == index,
                            onClick = {
                                coroutineScope.launch {
                                    pagerState.scrollToPage(index)
//                                    pagerState.animateScrollToPage(
//                                        page = index,
//                                        animationSpec = tween(durationMillis = 250)
//                                    )
                                }
                            }
                        ) {
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .heightIn(min = 48.dp)
                                    .padding(horizontal = 6.dp, vertical = 4.dp),
                                contentAlignment = Alignment.Center
                            ) {
                                FlowRow(
                                    modifier = Modifier.clearAndSetSemantics { text = AnnotatedString(title) },
                                    horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterHorizontally)
                                ) {
                                    title.split(' ').filter { it.isNotEmpty() }.forEach { word ->
                                        Text(
                                            text = word,
                                            style = MaterialTheme.typography.titleSmall,
                                            color = if (selectedIndex == index) Color(0xFF1428ff) else Color.Black,
                                            maxLines = 1,
                                            softWrap = false,
                                            overflow = TextOverflow.Ellipsis
                                        )
                                    }
                                }
                            }
                        }
                    }
                }


            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO)
@Composable
fun IPTabRowPreview(modifier: Modifier = Modifier) {
    val pagerState = rememberPagerState(initialPage = 1, pageCount = { 3 })
    IPTabRow(modifier, pagerState, listOf("TAB 1", "TAB 2", "TAB 3")) { _ -> }
}
